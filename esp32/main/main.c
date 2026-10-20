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
#include "board.h"
#include "fpga.h"
#include "hulk.h"
#include "status_led.h"

static const char *TAG = "hulk";

extern const uint8_t bitstream_start[] asm("_binary_thehulk_bin_start");
extern const uint8_t bitstream_end[]   asm("_binary_thehulk_bin_end");
extern const uint8_t rom_start[]       asm("_binary_rom_img_start");
extern const uint8_t rom_end[]         asm("_binary_rom_img_end");

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

// Wait for the Atari, configure the FPGA and enable the cartridge
static void start_cartridge(void)
{
    if (!fpga_power_present()) {
        status_led_set(STATUS_WAIT_POWER);
        ESP_LOGI(TAG, "waiting for Atari power");
        while (!fpga_power_present())
            vTaskDelay(pdMS_TO_TICKS(5));
        vTaskDelay(pdMS_TO_TICKS(10));         // let the 1.2 V / 3.3 V regulators settle
    }
    status_led_set(STATUS_STARTING);

    for (;;) {
        if (fpga_configure(bitstream_start, bitstream_end - bitstream_start) == ESP_OK) {
            int version = hulk_probe();
            if (version >= 0) {
                hulk_set_ctrl(0);
                hulk_write(0, rom_start, rom_len());
                hulk_set_ctrl(HULK_CTRL_ROM_EN);
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

    ESP_ERROR_CHECK(fpga_init());
    status_led_init();

    for (;;) {
        start_cartridge();

        // Verify once the Atari has had its chance to read the cartridge
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (fpga_power_present() && fpga_done()) {
            fill_rom_rest(rom_len());
            int errors = verify_rom(rom_start, rom_len());
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
            vTaskDelay(pdMS_TO_TICKS(10));
            if (hulk_get_status(&st) != ESP_OK)
                continue;
            int64_t now = esp_timer_get_time();
            if ((st.rom4_count != prev.rom4_count || st.rom3_count != prev.rom3_count) &&
                now - last_log > 1000000) {
                last_log = now;
                ESP_LOGI(TAG, "cartridge reads: ROM4 %lu, ROM3 %lu, last %06lx, early %u",
                         (unsigned long)st.rom4_count, (unsigned long)st.rom3_count,
                         (unsigned long)st.last_addr, st.early_count);
            }
            prev = st;
        }

        fpga_safe();
        status_led_set(STATUS_WAIT_POWER);
        ESP_LOGI(TAG, "Atari switched off: FPGA lines driven low");
    }
}
