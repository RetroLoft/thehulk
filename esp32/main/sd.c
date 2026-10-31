// microSD card, see sd.h. The card may hold other data (it is shared with other
// projects): it is mounted as is and never formatted.
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "board.h"
#include "sd.h"

static const char *TAG = "sd";

#define SD_SPI_HOST SPI3_HOST

static volatile bool s_ready;
static sdmmc_card_t *s_card;

bool sd_ready(void)
{
    return s_ready;
}

static esp_err_t mount(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
        return err;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cd = PIN_SD_CD;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 0,
    };
    return esp_vfs_fat_sdspi_mount(SD_ROOT, &host, &slot, &cfg, &s_card);
}

static void list_root(void)
{
    DIR *dir = opendir(SD_ROOT);
    if (!dir)
        return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(dir)) && n++ < 20)
        ESP_LOGI(TAG, "  %s%s", e->d_name, e->d_type == DT_DIR ? "\\" : "");
    closedir(dir);
}

static void sd_task(void *arg)
{
    gpio_set_direction(PIN_SD_CD, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_SD_CD, GPIO_PULLUP_ONLY);
    vTaskDelay(1);
    ESP_LOGI(TAG, "card detect (GPIO%d): %d", PIN_SD_CD, gpio_get_level(PIN_SD_CD));
    esp_err_t err = mount();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "card mounted: %s, %llu MB", s_card->cid.name,
                 ((unsigned long long)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
        list_root();
        s_ready = true;
    } else {
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

void sd_start(void)
{
    // On the second core: the main loop (core 0) must not be held up on the boot path
    xTaskCreatePinnedToCore(sd_task, "sd", 4096, NULL, 1, NULL, 1);
}
