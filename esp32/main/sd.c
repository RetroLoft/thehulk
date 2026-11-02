// microSD card, see sd.h. The card may hold other data (it is shared with other
// projects): it is mounted as is and never formatted.
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "board.h"
#include "gemdrv_fs.h"
#include "sd.h"

static const char *TAG = "sd";

#define SD_SPI_HOST SPI3_HOST
#define CD_POLL_MS  100
#define CD_STABLE   3       // polls that agree: a card goes in with some bouncing

static volatile bool s_ready;
static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_lock;

bool sd_ready(void)
{
    return s_ready;
}

void sd_lock(void)
{
    if (s_lock)
        xSemaphoreTake(s_lock, portMAX_DELAY);
}

void sd_unlock(void)
{
    if (s_lock)
        xSemaphoreGive(s_lock);
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
    static bool s_bus_ready;
    if (!s_bus_ready) {
        esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK)
            return err;
        s_bus_ready = true;
    }

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

static void cd_input(void)
{
    gpio_set_direction(PIN_SD_CD, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_SD_CD, GPIO_PULLUP_ONLY);
}

static void card_in(void)
{
    esp_err_t err = mount();
    for (int i = 0; err != ESP_OK && i < 2; i++) {      // the contacts may still settle
        vTaskDelay(pdMS_TO_TICKS(300));
        err = mount();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "card mounted: %s, %llu MB", s_card->cid.name,
                 ((unsigned long long)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
        list_root();
        s_ready = true;
    } else {
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(err));
    }
}

// What was not written yet is gone with the card: files are dropped, not closed
static void card_out(void)
{
    sd_lock();
    if (s_ready) {
        s_ready = false;
        fs_sd_removed();
        esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card);
        s_card = NULL;
        ESP_LOGI(TAG, "card removed");
    }
    sd_unlock();
    cd_input();                 // the driver leaves the pin reset
}

static void sd_task(void *arg)
{
    cd_input();
    vTaskDelay(1);
    bool present = !gpio_get_level(PIN_SD_CD);
    ESP_LOGI(TAG, "card detect (GPIO%d): %d", PIN_SD_CD, !present);
    if (present)
        card_in();
    int same = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CD_POLL_MS));
        same = !gpio_get_level(PIN_SD_CD) != present ? same + 1 : 0;
        if (same < CD_STABLE)
            continue;
        same = 0;
        present = !present;
        if (present)
            card_in();
        else
            card_out();
    }
}

void sd_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    // On the second core: the main loop (core 0) must not be held up on the boot path
    xTaskCreatePinnedToCore(sd_task, "sd", 4096, NULL, 1, NULL, 1);
}
