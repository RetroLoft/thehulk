// TheHulk - ESP32-S3 firmware (board v2)
//
// The FPGA runs from the Atari's 5 V only, the ESP32 also from USB. Main loop:
//   1. wait until the Atari powers the FPGA side (PWR_ON),
//   2. configure the FPGA, upload the cartridge image (GEMDRIVE) and enable it,
//   3. verify the image afterwards, then watch the bus until the Atari is switched
//      off (PWR_ON or CDONE low), drive all FPGA lines low and start over.
//
// The cartridge is enabled ~0.3 s after the Atari is switched on; TOS 1.04 on an ST
// without hard disk reads the cartridge first after ~0.67 s (fastest case measured).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "board.h"
#include "bootrec.h"
#include "fpga.h"
#include "hulk.h"
#include "status_led.h"

static const char *TAG = "hulk";

extern const uint8_t bitstream_start[] asm("_binary_thehulk_bin_start");
extern const uint8_t bitstream_end[]   asm("_binary_thehulk_bin_end");
extern const uint8_t rom_start[]       asm("_binary_rom_img_start");
extern const uint8_t rom_end[]         asm("_binary_rom_img_end");

static bootrec_t s_rec;

static uint32_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static size_t rom_len(void)
{
    size_t len = rom_end - rom_start;
    return len > HULK_ROM_SIZE ? HULK_ROM_SIZE : len;
}

// Fill the rest of the 128 KB window with 0xFF. Not on the boot path: for a small
// image this takes far longer than the image itself.
static void fill_rom_rest(size_t len)
{
    static const uint8_t ff[256] = { [0 ... 255] = 0xff };
    for (size_t off = (len + 1) & ~1u; off < HULK_ROM_SIZE; off += sizeof(ff))
        hulk_write(off, ff, off + sizeof(ff) <= HULK_ROM_SIZE ? sizeof(ff) : HULK_ROM_SIZE - off);
}

static int verify_rom(const uint8_t *rom, size_t len)
{
    static uint8_t buf[4096];
    int errors = 0;
    for (size_t off = 0; off < len; off += sizeof(buf)) {
        size_t n = len - off < sizeof(buf) ? len - off : sizeof(buf);
        hulk_read(off, buf, n);
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != rom[off + i] && errors++ < 8)
                ESP_LOGE(TAG, "verify %05x: read %02x, expected %02x",
                         (unsigned)(off + i), buf[i], rom[off + i]);
        }
    }
    return errors;
}

// Command packets from the Atari: /IRQ is low while one is waiting. For now they are
// only logged, nothing answers them yet.
static void handle_packets(void)
{
    static hulk_packet_t pkt;
    while (!gpio_get_level(PIN_FPGA_IRQ)) {
        if (hulk_get_packet(&pkt) != ESP_OK)
            return;
        char line[64] = "";
        size_t words = (pkt.size + 1) / 2, len = 0;
        for (size_t i = 0; i < words && i < 8; i++)
            len += snprintf(line + len, sizeof(line) - len, " %04x", pkt.words[i]);
        ESP_LOGI(TAG, "command %u (0x%04x), %u bytes:%s%s", pkt.cmd, pkt.cmd, pkt.size, line,
                 words > 8 ? " ..." : "");
        hulk_ack_packet();
    }
}

// Wait for the Atari, configure the FPGA and enable the cartridge
static void start_cartridge(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
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
                hulk_write(0, rom_start, rom_len());
                hulk_set_ctrl(HULK_CTRL_ROM_EN);
                s_rec.t_enabled = now_ms();
                hulk_status_t st;
                if (hulk_get_status(&st) == ESP_OK) {
                    s_rec.addr_changes = st.addr_changes;
                    s_rec.early = st.early_count;
                }
                ESP_LOGI(TAG, "cartridge enabled (gateware v%d, image %u bytes)",
                         version, (unsigned)rom_len());
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
    bootrec_start();

    ESP_ERROR_CHECK(fpga_init());
    status_led_init();
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
            vTaskDelay(1);
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
        if (fpga_power_present() && fpga_done()) {
            fill_rom_rest(rom_len());
            int errors = verify_rom(rom_start, rom_len());
            s_rec.verify_errors = errors > 255 ? 255 : errors;
            bootrec_save(&s_rec);
            bootrec_print();
            if (errors) {
                ESP_LOGE(TAG, "cartridge verify: %d errors, reloading", errors);
                status_led_set(STATUS_ERROR);
                hulk_set_ctrl(0);
                hulk_write(0, rom_start, rom_len());
                hulk_set_ctrl(HULK_CTRL_ROM_EN);
                status_led_set(STATUS_ACTIVE);
            }
        }

        // Watch until the Atari is switched off
        hulk_status_t prev = { 0 }, st;
        int64_t last_log = 0;
        while (fpga_power_present() && fpga_done()) {
            vTaskDelay(1);
            handle_packets();
            if (hulk_get_status(&st) != ESP_OK)
                continue;
            int64_t now = esp_timer_get_time();
            if ((st.rom4_count != prev.rom4_count || st.rom3_count != prev.rom3_count) &&
                now - last_log > 1000000) {
                last_log = now;
                ESP_LOGI(TAG, "cartridge reads: ROM4 %lu, ROM3 %lu, last %06lx, early %u, "
                         "packets %u, dropped %u",
                         (unsigned long)st.rom4_count, (unsigned long)st.rom3_count,
                         (unsigned long)st.last_addr, st.early_count, st.pkt_count, st.pkt_drops);
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
