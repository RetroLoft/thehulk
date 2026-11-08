// The cartridge emulator, see cart.h
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ff.h"
#include "nvs.h"
#include "cart.h"
#include "floppy.h"
#include "flashfs.h"
#include "sd.h"
#include "shadow.h"

static const char *TAG = "cart";

#define CART_DIR     "0:/CARTS"
#define CACHE_PATH   FLASHFS_DRIVE "/" CART_CACHE_NAME
#define UPLOAD_PATH  CART_DIR "/~UPLOAD.TMP"
#define TEMP_PATH    FLASHFS_DRIVE "/" CART_TEMP_NAME
#define CHUNK        4096

#define NVS_NS        "settings"
#define NVS_KEY_EMU   "cart_emu"
#define NVS_KEY_NAME  "cart_name"

// --- Images -------------------------------------------------------------------------

cart_kind_t cart_kind(const uint8_t *head, size_t head_len, uint32_t size)
{
    const uint8_t *p = head;
    if (size == CART_STC_SIZE) {
        p += 4;                         // the .STC header
        head_len = head_len >= 4 ? head_len - 4 : 0;
    } else if (size == 0 || size > CART_SIZE || (size & 1)) {
        return CART_BAD;
    }
    if (head_len < 4)
        return CART_UNKNOWN;
    uint32_t magic = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    return magic == 0xABCDEF42 ? CART_NORMAL : magic == 0xFA52235F ? CART_DIAG : CART_UNKNOWN;
}

const char *cart_kind_name(cart_kind_t kind)
{
    switch (kind) {
    case CART_NORMAL:  return "normal";
    case CART_DIAG:    return "diagnostic";
    case CART_UNKNOWN: return "unknown";
    default:           return "bad";
    }
}

bool cart_name_ok(char *name)
{
    static const char *const ext[] = { "STC", "IMG", "BIN", "ROM", "CAR" };
    char *dot = strchr(name, '.');
    if (!dot || dot == name || dot - name > 8 || strchr(dot + 1, '.') || strlen(dot + 1) > 3)
        return false;
    for (char *c = name; *c; c++) {
        *c = toupper((unsigned char)*c);
        if (c != dot && !isalnum((unsigned char)*c) && *c != '_' && *c != '-')
            return false;
    }
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (!strcmp(dot + 1, ext[i]))
            return true;
    return false;
}

const char *cart_error(int e)
{
    switch (e) {
    case CART_E_NO_CARD:   return "There is no SD card in TheHulk.";
    case CART_E_NAME:      return "Use an 8.3 name ending in .STC, .IMG, .BIN or .ROM.";
    case CART_E_NOT_FOUND: return "The image is not on the SD card.";
    case CART_E_EXISTS:    return "An image with that name is already there.";
    case CART_E_SIZE:      return "Not a cartridge image: it must be 128 KB at most (an .STC 128 KB + 4).";
    case CART_E_BUSY:      return "TheHulk is still writing an image. Try again in a moment.";
    default:               return "The SD card or the flash could not be written.";
    }
}

static void path_of(char *out, size_t size, const char *name)
{
    snprintf(out, size, CART_DIR "/%s", name);
}

// The card is there; 'make': create \CARTS when it is not (only to write an image: looking
// never writes to the card)
static bool card_dir(bool make)
{
    if (!sd_ready())
        return false;
    FILINFO fno;
    if (make && f_stat(CART_DIR, &fno) != FR_OK)
        f_mkdir(CART_DIR);
    return true;
}
#define card() card_dir(false)

int cart_list(cart_file_t *out, int max)
{
    sd_lock();
    if (!card()) {
        sd_unlock();
        return -1;
    }
    int n = 0;
    FF_DIR dir;
    FILINFO fno;
    FIL *fil = malloc(sizeof(FIL));     // over 4 KB: never on a stack
    if (f_opendir(&dir, CART_DIR) == FR_OK) {
        while (n < max && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
            char name[CART_NAME_LEN];
            strlcpy(name, fno.fname, sizeof(name));
            if ((fno.fattrib & AM_DIR) || !cart_name_ok(name))
                continue;
            cart_file_t *f = &out[n++];
            strlcpy(f->name, name, sizeof(f->name));
            f->size = fno.fsize;
            f->kind = CART_BAD;
            char path[32];
            path_of(path, sizeof(path), name);
            uint8_t head[8];
            UINT got = 0;
            if (fil && f_open(fil, path, FA_READ) == FR_OK) {
                f_read(fil, head, sizeof(head), &got);
                f_close(fil);
                f->kind = cart_kind(head, got, f->size);
            }
        }
        f_closedir(&dir);
    }
    free(fil);
    sd_unlock();
    return n;
}

static int check_name(const char *in, char *out)
{
    strlcpy(out, in, CART_NAME_LEN);
    return strlen(in) < CART_NAME_LEN && cart_name_ok(out) ? 0 : CART_E_NAME;
}

int cart_delete(const char *name)
{
    char n[CART_NAME_LEN], path[32];
    int r = check_name(name, n);
    if (r)
        return r;
    path_of(path, sizeof(path), n);
    sd_lock();
    FRESULT fr = card() ? f_unlink(path) : FR_NOT_READY;
    sd_unlock();
    return fr == FR_OK ? 0 : fr == FR_NOT_READY ? CART_E_NO_CARD : fr == FR_NO_FILE ? CART_E_NOT_FOUND : CART_E_IO;
}

int cart_rename(const char *from, const char *to)
{
    char a[CART_NAME_LEN], b[CART_NAME_LEN], pa[32], pb[32];
    if (check_name(from, a) || check_name(to, b))
        return CART_E_NAME;
    path_of(pa, sizeof(pa), a);
    path_of(pb, sizeof(pb), b);
    sd_lock();
    FRESULT fr = card() ? f_rename(pa, pb) : FR_NOT_READY;
    sd_unlock();
    if (fr == FR_OK && !strcmp(cart_active_name(), a)) {
        nvs_handle_t h;                 // the chosen one keeps its name on the page
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, NVS_KEY_NAME, b);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    return fr == FR_OK ? 0 : fr == FR_NOT_READY ? CART_E_NO_CARD : fr == FR_NO_FILE ? CART_E_NOT_FOUND :
           fr == FR_EXIST ? CART_E_EXISTS : CART_E_IO;
}

// --- Writing an image ---------------------------------------------------------------

static FIL s_up;
static bool s_up_open;
static char s_up_name[CART_NAME_LEN];
static uint32_t s_up_size, s_up_done;
static uint8_t s_up_head[8];

int cart_create(const char *name, uint32_t size)
{
    if (s_up_open)
        return CART_E_BUSY;
    int r = check_name(name, s_up_name);
    if (r)
        return r;
    if (size == 0 || (size > CART_SIZE && size != CART_STC_SIZE))
        return CART_E_SIZE;
    char path[32];
    path_of(path, sizeof(path), s_up_name);
    sd_lock();
    FILINFO fno;
    FRESULT fr = !card_dir(true) ? FR_NOT_READY : f_stat(path, &fno) == FR_OK ? FR_EXIST :
                 f_open(&s_up, UPLOAD_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    sd_unlock();
    if (fr != FR_OK)
        return fr == FR_NOT_READY ? CART_E_NO_CARD : fr == FR_EXIST ? CART_E_EXISTS : CART_E_IO;
    s_up_open = true;
    s_up_size = size;
    s_up_done = 0;
    return 0;
}

int cart_write(const void *data, size_t len)
{
    if (!s_up_open)
        return CART_E_IO;
    if (s_up_done + len > s_up_size)
        return CART_E_SIZE;
    if (s_up_done < sizeof(s_up_head)) {
        size_t n = sizeof(s_up_head) - s_up_done < len ? sizeof(s_up_head) - s_up_done : len;
        memcpy(s_up_head + s_up_done, data, n);
    }
    UINT put = 0;
    sd_lock();
    FRESULT fr = f_write(&s_up, data, len, &put);
    sd_unlock();
    s_up_done += put;
    return fr == FR_OK && put == len ? 0 : CART_E_IO;
}

int cart_close(bool keep)
{
    if (!s_up_open)
        return CART_E_IO;
    s_up_open = false;
    char path[32];
    path_of(path, sizeof(path), s_up_name);
    keep = keep && s_up_done == s_up_size;
    bool image = cart_kind(s_up_head, s_up_done < 8 ? s_up_done : 8, s_up_done) != CART_BAD;
    sd_lock();
    FRESULT fr = f_close(&s_up);
    if (fr == FR_OK && keep && image)
        fr = f_rename(UPLOAD_PATH, path);
    else
        f_unlink(UPLOAD_PATH);
    sd_unlock();
    if (!keep)
        return CART_E_IO;
    if (!image)
        return CART_E_SIZE;
    if (fr == FR_OK)
        ESP_LOGI(TAG, "%s added (%lu bytes)", s_up_name, (unsigned long)s_up_done);
    return fr == FR_OK ? 0 : fr == FR_EXIST ? CART_E_EXISTS : CART_E_IO;
}

// --- Choosing -----------------------------------------------------------------------

static char s_active[CART_NAME_LEN];
static int s_emu = -1;                  // -1: not read from NVS yet
static volatile bool s_changed;
static volatile bool s_reload;          // chosen after the contents were last loaded
static volatile bool s_load_failed;     // the chosen image could not be loaded
static volatile int s_job_result;       // of the last choice in the background

bool cart_take_change(void)
{
    bool c = s_changed;
    s_changed = false;
    return c;
}

bool cart_reload_pending(void)
{
    return s_reload;
}

bool cart_change_pending(void)
{
    return s_changed;
}

void cart_apply_now(void)
{
    s_changed = true;
}

static void read_nvs(void)
{
    if (s_emu >= 0)
        return;
    nvs_handle_t h;
    uint8_t emu = 0;
    size_t len = sizeof(s_active);
    s_active[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_EMU, &emu);
        if (nvs_get_str(h, NVS_KEY_NAME, s_active, &len) != ESP_OK)
            s_active[0] = 0;
        nvs_close(h);
    }
    s_emu = emu != 0;
}

bool cart_emulator_on(void)
{
    read_nvs();
    return s_emu;
}

const char *cart_active_name(void)
{
    read_nvs();
    return s_active;
}

static bool save_nvs(bool emu, const char *name)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_u8(h, NVS_KEY_EMU, emu) == ESP_OK &&
              (!name || nvs_set_str(h, NVS_KEY_NAME, name) == ESP_OK) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        s_emu = emu;
        if (name)
            strlcpy(s_active, name, sizeof(s_active));
    }
    return ok;
}

// The image from the card to the flash, normalised to 128 KB. Into a new file first: when
// it fails, or TheHulk restarts halfway, the image that was there stays good. The card is
// only locked per read, the Atari may be using it.
static int copy_to_flash(const char *name)
{
    char path[32];
    path_of(path, sizeof(path), name);
    uint8_t *buf = malloc(CHUNK);
    FIL *files = malloc(2 * sizeof(FIL));       // over 4 KB each: never on a stack
    if (!buf || !files) {
        free(buf);
        free(files);
        return CART_E_IO;
    }
    FIL *in = &files[0], *out = &files[1];
    int64_t t0 = esp_timer_get_time();
    int r = 0;
    UINT got = 0;
    uint32_t size = 0;
    sd_lock();
    if (!card()) {
        r = CART_E_NO_CARD;
    } else if (f_open(in, path, FA_READ) != FR_OK) {
        r = CART_E_NOT_FOUND;
    } else {
        size = f_size(in);
        f_read(in, buf, 8, &got);
        if (cart_kind(buf, got, size) == CART_BAD)
            r = CART_E_SIZE;
        else
            f_lseek(in, size == CART_STC_SIZE ? 4 : 0);
        if (r)
            f_close(in);
    }
    sd_unlock();
    if (r) {
        free(buf);
        free(files);
        return r;
    }
    if (!flashfs_ready() || f_open(out, TEMP_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        r = CART_E_IO;
    } else {
        for (uint32_t done = 0; !r && done < CART_SIZE; done += CHUNK) {
            UINT n = 0, put = 0;
            sd_lock();
            FRESULT fr = f_read(in, buf, CHUNK, &n);
            sd_unlock();
            if (fr != FR_OK)
                r = CART_E_IO;
            if (n < CHUNK)
                memset(buf + n, 0xFF, CHUNK - n);   // past the end: as an empty EPROM
            if (!r && (f_write(out, buf, CHUNK, &put) != FR_OK || put != CHUNK))
                r = CART_E_IO;
        }
        if (f_close(out) != FR_OK)
            r = CART_E_IO;
        if (!r) {
            f_unlink(CACHE_PATH);
            if (f_rename(TEMP_PATH, CACHE_PATH) != FR_OK)
                r = CART_E_IO;
        }
        if (r)
            f_unlink(TEMP_PATH);
    }
    sd_lock();
    f_close(in);
    sd_unlock();
    free(buf);
    free(files);
    ESP_LOGI(TAG, "%s to the flash: %s, %lu ms", name, r ? cart_error(r) : "ok",
             (unsigned long)((esp_timer_get_time() - t0) / 1000));
    return r;
}

int cart_select(const char *name)
{
    if (!name) {
        if (!save_nvs(false, NULL))
            return CART_E_IO;
        ESP_LOGI(TAG, "back to GEMDRIVE");
        s_reload = true;
        s_load_failed = false;
        s_job_result = 0;
        return 0;
    }
    char n[CART_NAME_LEN];
    int r = check_name(name, n);
    if (!r)
        r = copy_to_flash(n);
    if (!r && !save_nvs(true, n))
        r = CART_E_IO;
    if (!r) {
        ESP_LOGI(TAG, "cartridge emulator with %s", n);
        if (floppy_emulator_on())
            floppy_off();               // the cartridge cannot be both
        s_reload = true;
        s_load_failed = false;
    }
    return r;
}

// What is in the cartridge now, for the web page
static char s_running[CART_NAME_LEN];

const char *cart_running_name(void)
{
    return s_running;
}

bool cart_load_failed(void)
{
    return s_load_failed;
}

bool cart_load_shadow(void)
{
    s_reload = false;
    s_running[0] = 0;
    s_load_failed = false;
    if (!cart_emulator_on())
        return false;
    s_load_failed = true;               // until it has worked
    if (!flashfs_ready())
        return false;
    FIL *f = malloc(sizeof(FIL));
    if (!f || f_open(f, CACHE_PATH, FA_READ) != FR_OK) {
        free(f);
        ESP_LOGE(TAG, "the chosen image %s is not in the flash: GEMDRIVE", s_active);
        return false;
    }
    uint32_t size = f_size(f);
    bool ok = size == CART_SIZE;
    uint8_t *buf = malloc(CHUNK);
    for (uint32_t off = 0; ok && buf && off < CART_SIZE; off += CHUNK) {
        UINT n = 0;
        ok = f_read(f, buf, CHUNK, &n) == FR_OK && n == CHUNK;
        if (ok)
            shadow_load_part(off, buf, CHUNK);
    }
    ok = ok && buf;
    free(buf);
    f_close(f);
    free(f);
    if (!ok) {
        ESP_LOGE(TAG, "the chosen image %s cannot be read from the flash (%lu bytes): GEMDRIVE",
                 s_active, (unsigned long)size);
        return false;
    }
    strlcpy(s_running, s_active, sizeof(s_running));
    s_load_failed = false;
    return true;
}

// --- Choosing in the background -----------------------------------------------------

// Writing 128 KB to the flash takes a while; the web server has one task and must keep
// answering meanwhile.
static volatile bool s_job_busy;
static char s_job_name[CART_NAME_LEN];

static void job_task(void *arg)
{
    s_job_result = cart_select(s_job_name);
    if (!s_job_result)
        cart_apply_now();               // before busy ends: the page waits for it
    s_job_busy = false;
    vTaskDelete(NULL);
}

int cart_select_start(const char *name)
{
    if (s_job_busy)
        return CART_E_BUSY;
    int r = check_name(name, s_job_name);
    if (r)
        return r;
    s_job_result = 0;
    s_job_busy = true;
    if (xTaskCreatePinnedToCore(job_task, "cart", 6144, NULL, 1, NULL, 1) != pdPASS) {
        s_job_busy = false;
        return CART_E_IO;
    }
    return 0;
}

bool cart_select_busy(void)
{
    return s_job_busy;
}

int cart_select_result(void)
{
    return s_job_result;
}
