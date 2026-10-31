// Boot records in NVS (namespace "bootrec": blob "recs" + index "next")
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "bootrec.h"

static const char *TAG = "bootrec";

// Survives a brown-out reset, not a power loss
static RTC_NOINIT_ATTR uint32_t s_brownouts;
static uint8_t s_count;

void bootrec_start(void)
{
    if (esp_reset_reason() == ESP_RST_BROWNOUT)
        s_brownouts = s_brownouts < 255 ? s_brownouts + 1 : 255;
    else
        s_brownouts = 0;
    s_count = s_brownouts;
}

uint8_t bootrec_brownouts(void)
{
    return s_count;
}

static bool open_nvs(nvs_handle_t *h)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK)
        err = nvs_open("bootrec", NVS_READWRITE, h);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "NVS not available: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void load(nvs_handle_t h, bootrec_t *recs, uint8_t *next)
{
    size_t len = sizeof(bootrec_t) * BOOTREC_COUNT;
    if (nvs_get_blob(h, "recs", recs, &len) != ESP_OK || len != sizeof(bootrec_t) * BOOTREC_COUNT)
        memset(recs, 0, sizeof(bootrec_t) * BOOTREC_COUNT);
    if (nvs_get_u8(h, "next", next) != ESP_OK || *next >= BOOTREC_COUNT)
        *next = 0;
}

void bootrec_save(const bootrec_t *rec)
{
    static bootrec_t recs[BOOTREC_COUNT];
    nvs_handle_t h;
    uint8_t next;
    if (!open_nvs(&h))
        return;
    load(h, recs, &next);
    recs[next] = *rec;
    nvs_set_blob(h, "recs", recs, sizeof(recs));
    nvs_set_u8(h, "next", (next + 1) % BOOTREC_COUNT);
    nvs_commit(h);
    nvs_close(h);
}

void bootrec_print(void)
{
    static bootrec_t recs[BOOTREC_COUNT];
    nvs_handle_t h;
    uint8_t next;
    if (!open_nvs(&h))
        return;
    load(h, recs, &next);
    nvs_close(h);
    ESP_LOGI(TAG, "last starts, oldest first (ms since the app started):");
    for (int i = 0; i < BOOTREC_COUNT; i++) {
        const bootrec_t *r = &recs[(next + i) % BOOTREC_COUNT];
        if (!r->t_enabled)
            continue;
        ESP_LOGI(TAG, "%s, reset %u, brownouts %u: power %lu, configured %lu, enabled %lu, "
                 "first ROM4 %lu, code running %lu, first ROM3 %lu, early %u, Atari active at enable %s, verify errors %u, "
                 "configuration attempts %u",
                 r->waited ? "USB" : "cold", r->reset_reason, r->brownouts,
                 (unsigned long)r->t_power, (unsigned long)r->t_configured,
                 (unsigned long)r->t_enabled, (unsigned long)r->t_rom4,
                 (unsigned long)r->t_init, (unsigned long)r->t_rom3, r->early, r->addr_changes ? "yes" : "no",
                 r->verify_errors, r->attempts);
    }
}
