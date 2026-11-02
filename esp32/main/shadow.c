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

// What a command can change without shadow_mark: the first block of ROM3 (token, status
// words, DTA) and the status block behind the read buffer (with the shared variables)
#define SYNC_STATUS_START  0x4000
#define SYNC_STATUS_LEN    0x400

static uint32_t s_mark_lo = HULK_ROM_SIZE, s_mark_hi;

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

void shadow_mark(uint32_t offset, size_t len)
{
    if (offset < s_mark_lo)
        s_mark_lo = offset;
    if (offset + len > s_mark_hi)
        s_mark_hi = offset + len;
}

// Writes the words of a range that changed, as one piece from the first to the last
static esp_err_t sync_range(uint32_t offset, size_t len)
{
    if (memcmp(s_mem + offset, s_synced + offset, len) == 0)
        return ESP_OK;
    size_t first = 0, last = len;
    while (s_mem[offset + first] == s_synced[offset + first] &&
           s_mem[offset + first + 1] == s_synced[offset + first + 1])
        first += 2;
    while (s_mem[offset + last - 1] == s_synced[offset + last - 1] &&
           s_mem[offset + last - 2] == s_synced[offset + last - 2])
        last -= 2;
    return write_be(offset + first, last - first);
}

// Comparing the whole 128 KB in PSRAM takes ~10 ms, too long for each command
esp_err_t shadow_sync(void)
{
    esp_err_t err = ESP_OK;
    uint32_t lo = s_mark_lo & ~(BLOCK - 1), hi = s_mark_hi;
    for (uint32_t off = lo; off < hi && err == ESP_OK; off += BLOCK) {
        if (off != SHADOW_ROM3)         // the token block goes last
            err = sync_range(off, BLOCK);
    }
    s_mark_lo = HULK_ROM_SIZE;
    s_mark_hi = 0;
    for (uint32_t off = 0; off < SYNC_STATUS_LEN && err == ESP_OK; off += BLOCK)
        err = sync_range(SHADOW_ROM3 + SYNC_STATUS_START + off, BLOCK);
    uint32_t t = SHADOW_ROM3;
    if (err == ESP_OK)
        err = sync_range(t + 4, BLOCK - 4);
    if (err == ESP_OK)
        err = sync_range(t, 4);         // the token itself at the very end
    return err;
}

bool shadow_is_synced(void)
{
    return memcmp(s_mem, s_synced, HULK_ROM_SIZE) == 0;
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
