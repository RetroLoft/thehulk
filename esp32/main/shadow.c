// Shadow of the cartridge space, see shadow.h
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hulk.h"
#include "shadow.h"

#define BLOCK       512
#define BLOCKS      (HULK_ROM_SIZE / BLOCK)

static const char *TAG = "shadow";

static uint8_t *s_mem;       // the shadow, CPU word order
static uint8_t *s_synced;    // what the SPRAM holds
static uint8_t s_buf[4096];  // big-endian bounce buffer for the SPI

esp_err_t shadow_init(void)
{
    if (!s_mem) {
        s_mem = heap_caps_malloc(HULK_ROM_SIZE, MALLOC_CAP_SPIRAM);
        s_synced = heap_caps_malloc(HULK_ROM_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_mem || !s_synced) {
            ESP_LOGE(TAG, "no memory for the shadow");
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_mem, 0, HULK_ROM_SIZE);
    memset(s_synced, 0, HULK_ROM_SIZE);
    return ESP_OK;
}

uintptr_t shadow_base(void)
{
    return (uintptr_t)s_mem;
}

void shadow_load_image(const uint8_t *img, size_t len)
{
    if (len > HULK_ROM_SIZE)
        len = HULK_ROM_SIZE;
    for (size_t i = 0; i + 1 < len; i += 2) {
        s_mem[i] = img[i + 1];
        s_mem[i + 1] = img[i];
    }
    if (len & 1)
        s_mem[len] = img[len - 1];   // odd length: high byte of the last word
}

// Shadow (CPU order) -> SPRAM (big-endian words); offset and len even
static esp_err_t write_be(uint32_t offset, size_t len)
{
    while (len) {
        size_t n = len > sizeof(s_buf) ? sizeof(s_buf) : len;
        for (size_t i = 0; i < n; i += 2) {
            s_buf[i] = s_mem[offset + i + 1];
            s_buf[i + 1] = s_mem[offset + i];
        }
        esp_err_t err = hulk_write(offset, s_buf, n);
        if (err != ESP_OK)
            return err;
        memcpy(s_synced + offset, s_mem + offset, n);
        offset += n;
        len -= n;
    }
    return ESP_OK;
}

esp_err_t shadow_write_range(uint32_t offset, size_t len)
{
    return write_be(offset & ~1u, (len + 1) & ~1u);
}

esp_err_t shadow_sync(void)
{
    const int token_blk = SHADOW_ROM3 / BLOCK;
    esp_err_t err = ESP_OK;
    for (int b = 0; b < BLOCKS && err == ESP_OK; b++) {
        if (b != token_blk && memcmp(s_mem + b * BLOCK, s_synced + b * BLOCK, BLOCK) != 0)
            err = write_be(b * BLOCK, BLOCK);
    }
    uint32_t t = SHADOW_ROM3;
    if (err == ESP_OK && memcmp(s_mem + t + 4, s_synced + t + 4, BLOCK - 4) != 0)
        err = write_be(t + 4, BLOCK - 4);
    if (err == ESP_OK && memcmp(s_mem + t, s_synced + t, 4) != 0)
        err = write_be(t, 4);
    return err;
}

int shadow_verify(void)
{
    static uint8_t buf[4096];
    int errors = 0;
    for (uint32_t off = 0; off < HULK_ROM_SIZE; off += sizeof(buf)) {
        if (hulk_read(off, buf, sizeof(buf)) != ESP_OK)
            return -1;
        for (size_t i = 0; i < sizeof(buf); i += 2) {
            uint16_t got = (buf[i] << 8) | buf[i + 1];
            uint16_t want = s_synced[off + i] | (s_synced[off + i + 1] << 8);
            if (got != want && errors++ < 8)
                ESP_LOGE(TAG, "verify %05lx: read %04x, expected %04x",
                         (unsigned long)(off + i), got, want);
        }
    }
    return errors;
}
