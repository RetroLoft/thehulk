// TheHulk - ESP32-S3 firmware (board v2)
//
// The FPGA runs from the Atari's 5 V only, the ESP32 also from USB. Main loop:
//   1. wait until the Atari powers the FPGA side (PWR_ON),
//   2. configure the FPGA, upload the cartridge image (GEMDRIVE) and enable it, then
//      write the rest of the 128 KB (the GEMDRIVE shared memory in ROM3),
//   3. answer the GEMDRIVE commands until the Atari is switched off (PWR_ON or CDONE
//      low), drive all FPGA lines low and start over.
//
// The cartridge space is kept in a shadow (shadow.c); after each command the changed
// parts are written to the FPGA's SPRAM.
//
// The cartridge is enabled ~0.3 s after the Atari is switched on; TOS 1.04 on an ST
// without hard disk reads the cartridge first after ~0.67 s (fastest case measured).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "board.h"
#include "bootrec.h"
#include "fpga.h"
#include "gemdrv.h"
#include "hulk.h"
#include "sd.h"
#include "settings.h"
#include "shadow.h"
#include "status_led.h"

static const char *TAG = "hulk";

extern const uint8_t bitstream_start[] asm("_binary_thehulk_bin_start");
extern const uint8_t bitstream_end[]   asm("_binary_thehulk_bin_end");
extern const uint8_t rom_start[]       asm("_binary_rom_img_start");
extern const uint8_t rom_end[]         asm("_binary_rom_img_end");

static bootrec_t s_rec;

// The log goes through a buffer to a task on the second core: a UART line takes
// ~9 ms, and GEMDRIVE waits for the answer to each command (on a Falcon it gives up
// after ~1.4 s by default). Lines that do not fit are dropped, never waited for.
static StreamBufferHandle_t s_log_buf;

static int log_vprintf(const char *fmt, va_list args)
{
    char line[256];
    int len = vsnprintf(line, sizeof(line), fmt, args);
    if (len > (int)sizeof(line) - 1)
        len = sizeof(line) - 1;
    if (len > 0)
        xStreamBufferSend(s_log_buf, line, len, 0);
    return len;
}

static void log_task(void *arg)
{
    char buf[256];
    for (;;) {
        size_t n = xStreamBufferReceive(s_log_buf, buf, sizeof(buf), portMAX_DELAY);
        fwrite(buf, 1, n, stdout);
        fflush(stdout);
    }
}

static void log_start(void)
{
    s_log_buf = xStreamBufferCreate(32 * 1024, 1);
    xTaskCreatePinnedToCore(log_task, "log", 3072, NULL, 1, NULL, 1);
    esp_log_set_vprintf(log_vprintf);
}

static uint32_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static size_t rom_len(void)
{
    size_t len = rom_end - rom_start;
    return len > HULK_ROM_SIZE ? HULK_ROM_SIZE : len;
}

// Command packets from the Atari: /IRQ is low while one is waiting. The packet is
// acknowledged first (the FPGA can take the next one), then answered.
static TaskHandle_t s_main_task;
static uint32_t s_slowest_us;   // slowest command since the last log line

static void irq_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_main_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void irq_start(void)
{
    s_main_task = xTaskGetCurrentTaskHandle();
    gpio_install_isr_service(0);
    gpio_set_intr_type(PIN_FPGA_IRQ, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(PIN_FPGA_IRQ, irq_isr, NULL);
}

// Until /IRQ falls, at most a tick: the Atari waits for each answer
static void wait_for_packet(void)
{
    ulTaskNotifyTake(pdTRUE, 1);
}

static void handle_packets(void)
{
    static hulk_packet_t pkt;
    // /IRQ reads low too while the FPGA side is unpowered
    while (!gpio_get_level(PIN_FPGA_IRQ) && fpga_power_present() && fpga_done()) {
        int64_t t0 = esp_timer_get_time();
        if (hulk_get_packet(&pkt) != ESP_OK)
            return;
        hulk_ack_packet();
#ifndef HULK_CART_READONLY
        gemdrv_command(&pkt);
        shadow_sync();
#if HULK_GEMDRIVE_NO_INSTALL
        if (!shadow_is_synced())
            ESP_LOGE(TAG, "command 0x%04x changed the shadow outside the synced areas", pkt.cmd);
#endif
#endif
#ifndef HULK_CART_READONLY
        if (gemdrv_restart_wanted()) {
            vTaskDelay(pdMS_TO_TICKS(20));  // the log
            esp_restart();
        }
#endif
        uint32_t us = esp_timer_get_time() - t0;
        if (us > s_slowest_us)
            s_slowest_us = us;
    }
}

// Diagnostics: the last ROM3 cycles the gateware saw, oldest first, as {A15..A1, decoder
// waiting for a header}. Logged when the Atari polls for an answer to a packet that
// never arrived.
static void log_rom3_ring(void)
{
    static uint8_t b[512];
    uint8_t pos[2];
    if (hulk_read(0x801200, pos, sizeof(pos)) != ESP_OK || hulk_read(0x801000, b, sizeof(b)) != ESP_OK)
        return;
    ESP_LOGW(TAG, "no packet while the Atari polls; last ROM3 cycles:");
    for (int i = 0; i < 256; i += 16) {
        char line[100];
        size_t len = 0;
        for (int k = 0; k < 16; k++) {
            int n = (pos[1] + i + k) & 0xFF;
            len += snprintf(line + len, sizeof(line) - len, " %02x%02x", b[2 * n], b[2 * n + 1]);
        }
        ESP_LOGW(TAG, "%3d:%s", i, line);
    }
}

// Wait for the Atari, configure the FPGA and enable the cartridge
static void start_cartridge(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    shadow_init();
    shadow_load_image(rom_start, rom_len());
#ifndef HULK_CART_READONLY
    gemdrv_init();
#endif
    s_rec.reset_reason = esp_reset_reason();
    s_rec.brownouts = bootrec_brownouts();
    s_rec.waited = !fpga_power_present();
    if (!fpga_power_present()) {
        status_led_set(STATUS_WAIT_POWER);
        ESP_LOGI(TAG, "waiting for Atari power");
        while (!fpga_power_present())
            vTaskDelay(pdMS_TO_TICKS(5));
        vTaskDelay(pdMS_TO_TICKS(10));         // let the 1.2 V / 3.3 V regulators settle
    }
    status_led_set(STATUS_STARTING);
    s_rec.t_power = now_ms();

    for (;;) {
        if (s_rec.attempts < 255)
            s_rec.attempts++;
        if (fpga_configure(bitstream_start, bitstream_end - bitstream_start) == ESP_OK) {
            s_rec.t_configured = now_ms();
            int version = hulk_probe();
            if (version >= 0) {
                hulk_set_ctrl(0);
                // The start of the image first: boot path (a 128 KB upload takes 263 ms,
                // TOS reads the header ~0.5 s after power-on)
                size_t done = (rom_len() + 1) & ~1u;
#ifndef HULK_UPLOAD_ALL
                if (done > 0x4000)
                    done = 0x4000;
#endif
                shadow_write_range(0, done);
#ifdef HULK_CART_OFF
                ESP_LOGW(TAG, "diagnostics build: cartridge stays disabled");
#else
                hulk_set_ctrl(HULK_CTRL_ROM_EN);
#endif
                s_rec.t_enabled = now_ms();
                hulk_status_t st;
                if (hulk_get_status(&st) == ESP_OK) {
                    s_rec.addr_changes = st.addr_changes;
                    s_rec.early = st.early_count;
                }
                // Then the rest, GEMDRIVE reads its shared memory ~2 s after power-on
                shadow_write_range(done, HULK_ROM_SIZE - done);
                ESP_LOGI(TAG, "cartridge enabled (gateware v%d, image %u bytes, %lu ms)",
                         version, (unsigned)rom_len(), (unsigned long)(now_ms() - s_rec.t_enabled));
                status_led_set(STATUS_ACTIVE);
                return;
            }
            ESP_LOGE(TAG, "FPGA does not answer the ID command");
        }
        status_led_set(STATUS_ERROR);
        fpga_safe();
        vTaskDelay(pdMS_TO_TICKS(100));
        while (!fpga_power_present())
            vTaskDelay(pdMS_TO_TICKS(5));
        status_led_set(STATUS_STARTING);
    }
}

void app_main(void)
{
    // The startup log level is WARN (each UART line costs ~5 ms on the boot path)
    esp_log_level_set("*", ESP_LOG_INFO);
    log_start();
    bootrec_start();
    settings_init();

    ESP_ERROR_CHECK(fpga_init());
    irq_start();
    status_led_init();
    sd_start();
    if (!fpga_power_present())
        bootrec_print();               // not on the boot path of a cold start

    for (;;) {
        start_cartridge();

        // Note the first cartridge reads: the header check by TOS, GEMDRIVE running
        // (many /ROM4 reads) and its first command (/ROM3). Verify afterwards, once
        // the Atari has had its chance to read the cartridge.
        while (fpga_power_present() && now_ms() - s_rec.t_enabled < 20000 &&
               !(s_rec.t_rom3 && now_ms() - s_rec.t_enabled >= 3000)) {
            hulk_status_t st;
            wait_for_packet();
            if (hulk_get_status(&st) != ESP_OK)
                continue;
            if (!s_rec.t_rom3 && st.rom3_count)
                s_rec.t_rom3 = now_ms();
            handle_packets();
            if (!s_rec.t_rom4 && st.rom4_count)
                s_rec.t_rom4 = now_ms();
            if (!s_rec.t_init && st.rom4_count > 32)
                s_rec.t_init = now_ms();
        }
        // The verify and the NVS write hold up the command answers: wait until the
        // Atari has sent no command for 0.5 s (at most 30 s)
        uint32_t t_quiet = now_ms(), t_wait = now_ms();
        uint16_t pkts = 0;
        while (fpga_power_present() && fpga_done() && now_ms() - t_quiet < 500 &&
               now_ms() - t_wait < 30000) {
            hulk_status_t st;
            wait_for_packet();
            handle_packets();
            if (hulk_get_status(&st) == ESP_OK && st.pkt_count != pkts) {
                pkts = st.pkt_count;
                t_quiet = now_ms();
            }
        }
        if (fpga_power_present() && fpga_done()) {
            int errors = shadow_verify();
            s_rec.verify_errors = errors > 255 || errors < 0 ? 255 : errors;
            bootrec_save(&s_rec);
            bootrec_print();
            if (errors) {
                ESP_LOGE(TAG, "cartridge verify: %d errors, rewriting", errors);
                status_led_set(STATUS_ERROR);
                shadow_write_range(0, HULK_ROM_SIZE);
                status_led_set(STATUS_ACTIVE);
            }
        }

        // Watch until the Atari is switched off
        hulk_status_t prev = { 0 }, st;
        int64_t last_log = 0;
        uint16_t lost_count = 0;
        uint32_t lost_rom3 = 0;
        int64_t lost_time = 0;
        bool lost_logged = false;
        while (fpga_power_present() && fpga_done()) {
            wait_for_packet();
            handle_packets();
            if (hulk_get_status(&st) != ESP_OK)
                continue;
            int64_t now = esp_timer_get_time();
            // Half a second of token polling (~1 M reads) without a new packet
            if (st.pkt_count != lost_count) {
                lost_count = st.pkt_count;
                lost_rom3 = st.rom3_count;
                lost_logged = false;
            } else if (!lost_logged && st.rom3_count - lost_rom3 > 1000000) {
                lost_logged = true;
                log_rom3_ring();
            } else if (st.rom3_count - lost_rom3 <= 1000000 && now - lost_time > 2000000) {
                lost_time = now;
                lost_rom3 = st.rom3_count;
            }
            if ((st.rom4_count != prev.rom4_count || st.rom3_count != prev.rom3_count) &&
                now - last_log > 1000000) {
                last_log = now;
                ESP_LOGI(TAG, "cartridge reads: ROM4 %lu, ROM3 %lu, last %06lx, early %u, "
                         "packets %u, dropped %u, slowest %lu us",
                         (unsigned long)st.rom4_count, (unsigned long)st.rom3_count,
                         (unsigned long)st.last_addr, st.early_count, st.pkt_count, st.pkt_drops,
                         (unsigned long)s_slowest_us);
                s_slowest_us = 0;
            }
            prev = st;
        }

        fpga_safe();
        status_led_set(STATUS_WAIT_POWER);
        ESP_LOGI(TAG, "Atari switched off: FPGA lines driven low");

        // PWR_ON stays high for a while as the capacitors discharge: do not configure
        // the FPGA on a collapsing supply. Still high after 2 s: the FPGA lost its
        // configuration with the Atari on, so configure it again.
        int64_t off = esp_timer_get_time();
        while (fpga_power_present() && esp_timer_get_time() - off < 2000000)
            vTaskDelay(pdMS_TO_TICKS(5));
    }
}
