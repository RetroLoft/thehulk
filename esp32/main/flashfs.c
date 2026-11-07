// The flash drive, see flashfs.h
#include <stdlib.h>
#include "diskio_wl.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "ff.h"
#include "wear_levelling.h"
#include "flashfs.h"

static const char *TAG = "flashfs";

#define PDRV 1                          // FatFs drive number: "1:". The card takes the
                                        // first free one, 0, also after a hot swap.

static FATFS s_fs;
static bool s_ready, s_tried;

bool flashfs_ready(void)
{
    if (s_tried)
        return s_ready;
    s_tried = true;
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_FAT, "hulkfs");
    if (!part) {
        ESP_LOGW(TAG, "no hulkfs partition: the flash drive only has the built-in files");
        return false;
    }
    wl_handle_t wl;
    esp_err_t err = wl_mount(part, &wl);
    if (err == ESP_OK)
        err = ff_diskio_register_wl_partition(PDRV, wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wear levelling: %s", esp_err_to_name(err));
        return false;
    }
    FRESULT fr = f_mount(&s_fs, FLASHFS_DRIVE, 1);
    if (fr == FR_NO_FILESYSTEM) {
        ESP_LOGW(TAG, "empty: formatting %lu KB", (unsigned long)(part->size / 1024));
        const MKFS_PARM opt = { .fmt = FM_FAT | FM_SFD, .n_fat = 1, .au_size = 0 };
        void *work = malloc(FF_MAX_SS);
        fr = work ? f_mkfs(FLASHFS_DRIVE, &opt, work, FF_MAX_SS) : FR_NOT_ENOUGH_CORE;
        free(work);
        if (fr == FR_OK)
            fr = f_mount(&s_fs, FLASHFS_DRIVE, 1);
    }
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "mount: FatFs error %d", fr);
        return false;
    }
    DWORD clusters;
    FATFS *fs;
    if (f_getfree(FLASHFS_DRIVE, &clusters, &fs) == FR_OK)
        ESP_LOGI(TAG, "mounted, %lu KB free",
                 (unsigned long)((uint64_t)clusters * fs->csize * fs->ssize / 1024));
    s_ready = true;
    return true;
}
