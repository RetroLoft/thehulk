// The floppy emulator, see floppy.h
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "floppy.h"
#include "floppy_image.h"
#include "cart.h"
#include "ff.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sd.h"

static const char *TAG = "floppy";

#define FLOPPY_DIR   "0:/FLOPPIES"
#define UPLOAD_PATH  FLOPPY_DIR "/~UPLOAD.TMP"
#define CHUNK        4096
#define HEAD_SIZE    32                 // enough for the BPB and the MSA header
#define CARD_WAIT_MS 3000               // at power-on the card is not mounted yet

#define NVS_NS         "settings"
#define NVS_KEY_ON     "flop_on"
#define NVS_KEY_NAME   "flop_name"
#define NVS_KEY_DRIVE  "flop_drive"
#define NVS_KEY_KEEP   "flop_keep"

const char *floppy_kind_name(floppy_kind_t kind)
{
    return kind == FLOPPY_ST ? "st" : kind == FLOPPY_MSA ? "msa" : "bad";
}

bool floppy_name_ok(char *name)
{
    char *dot = strchr(name, '.');
    if (!dot || dot == name || dot - name > 8 || strchr(dot + 1, '.') || strlen(dot + 1) > 3)
        return false;
    for (char *c = name; *c; c++) {
        *c = toupper((unsigned char)*c);
        if (c != dot && !isalnum((unsigned char)*c) && *c != '_' && *c != '-')
            return false;
    }
    return !strcmp(dot + 1, "ST") || !strcmp(dot + 1, "MSA");
}

const char *floppy_error(int e)
{
    switch (e) {
    case FLOPPY_E_NO_CARD:   return "There is no SD card in TheHulk.";
    case FLOPPY_E_NAME:      return "Use an 8.3 name ending in .ST or .MSA.";
    case FLOPPY_E_NOT_FOUND: return "The image is not on the SD card.";
    case FLOPPY_E_EXISTS:    return "An image with that name is already there.";
    case FLOPPY_E_SIZE:      return "Not a disk image of a size TheHulk knows (.ST or .MSA, 360 KB to 1.44 MB).";
    case FLOPPY_E_BUSY:      return "TheHulk is still busy with an image. Try again in a moment.";
    case FLOPPY_E_MEMORY:    return "TheHulk has no memory left for this image.";
    default:                 return "The SD card could not be read or written.";
    }
}

floppy_kind_t floppy_kind(const char *name, const uint8_t *head, size_t head_len, uint32_t size)
{
    const char *dot = strrchr(name, '.');
    floppy_geometry_t g;
    if (dot && !strcmp(dot, ".MSA"))
        return floppy_msa_header(head, head_len, size, &g) ? FLOPPY_MSA : FLOPPY_BAD;
    return floppy_st_geometry(head, head_len, size, &g) ? FLOPPY_ST : FLOPPY_BAD;
}

static void path_of(char *out, size_t size, const char *name)
{
    snprintf(out, size, FLOPPY_DIR "/%s", name);
}

static int check_name(const char *in, char *out)
{
    strlcpy(out, in, FLOPPY_NAME_LEN);
    return strlen(in) < FLOPPY_NAME_LEN && floppy_name_ok(out) ? 0 : FLOPPY_E_NAME;
}

int floppy_list(floppy_file_t *out, int max)
{
    sd_lock();
    if (!sd_ready()) {
        sd_unlock();
        return -1;
    }
    int n = 0;
    FF_DIR dir;
    FILINFO fno;
    FIL *fil = malloc(sizeof(FIL));     // over 4 KB: never on a stack
    if (f_opendir(&dir, FLOPPY_DIR) == FR_OK) {
        while (n < max && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
            char name[FLOPPY_NAME_LEN], path[36];
            strlcpy(name, fno.fname, sizeof(name));
            if ((fno.fattrib & AM_DIR) || !floppy_name_ok(name))
                continue;
            floppy_file_t *f = &out[n++];
            strlcpy(f->name, name, sizeof(f->name));
            f->size = fno.fsize;
            f->kind = FLOPPY_BAD;
            path_of(path, sizeof(path), name);
            uint8_t head[HEAD_SIZE];
            UINT got = 0;
            if (fil && f_open(fil, path, FA_READ) == FR_OK) {
                f_read(fil, head, sizeof(head), &got);
                f_close(fil);
                f->kind = floppy_kind(name, head, got, f->size);
            }
        }
        f_closedir(&dir);
    }
    free(fil);
    sd_unlock();
    return n;
}

int floppy_delete(const char *name)
{
    char n[FLOPPY_NAME_LEN], path[36];
    int r = check_name(name, n);
    if (r)
        return r;
    path_of(path, sizeof(path), n);
    sd_lock();
    FRESULT fr = sd_ready() ? f_unlink(path) : FR_NOT_READY;
    sd_unlock();
    return fr == FR_OK ? 0 : fr == FR_NOT_READY ? FLOPPY_E_NO_CARD :
           fr == FR_NO_FILE ? FLOPPY_E_NOT_FOUND : FLOPPY_E_IO;
}

// --- What is chosen ------------------------------------------------------------------

static char s_active[FLOPPY_NAME_LEN];
static int s_on = -1;                   // -1: not read from NVS yet
static uint8_t s_drive, s_keep;

static void read_nvs(void)
{
    if (s_on >= 0)
        return;
    nvs_handle_t h;
    uint8_t on = 0;
    size_t len = sizeof(s_active);
    s_active[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_ON, &on);
        nvs_get_u8(h, NVS_KEY_DRIVE, &s_drive);
        nvs_get_u8(h, NVS_KEY_KEEP, &s_keep);
        if (nvs_get_str(h, NVS_KEY_NAME, s_active, &len) != ESP_OK)
            s_active[0] = 0;
        nvs_close(h);
    }
    s_on = on && s_active[0];
}

bool floppy_emulator_on(void)
{
    read_nvs();
    return s_on;
}

const char *floppy_active_name(void)
{
    read_nvs();
    return s_active;
}

int floppy_drive(void)
{
    read_nvs();
    return s_drive != 0;
}

bool floppy_keep_drives(void)
{
    read_nvs();
    return s_keep != 0;
}

static bool save_nvs(bool on, const char *name, int drive, bool keep)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_u8(h, NVS_KEY_ON, on) == ESP_OK &&
              (!name || (nvs_set_str(h, NVS_KEY_NAME, name) == ESP_OK &&
                         nvs_set_u8(h, NVS_KEY_DRIVE, drive) == ESP_OK &&
                         nvs_set_u8(h, NVS_KEY_KEEP, keep) == ESP_OK)) &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        read_nvs();
        s_on = on;
        if (name) {
            strlcpy(s_active, name, sizeof(s_active));
            s_drive = drive;
            s_keep = keep;
        }
    }
    return ok;
}

int floppy_rename(const char *from, const char *to)
{
    char a[FLOPPY_NAME_LEN], b[FLOPPY_NAME_LEN], pa[36], pb[36];
    if (check_name(from, a) || check_name(to, b))
        return FLOPPY_E_NAME;
    path_of(pa, sizeof(pa), a);
    path_of(pb, sizeof(pb), b);
    sd_lock();
    FRESULT fr = sd_ready() ? f_rename(pa, pb) : FR_NOT_READY;
    sd_unlock();
    if (fr == FR_OK && !strcmp(floppy_active_name(), a))
        save_nvs(s_on, b, s_drive, s_keep);     // the chosen one keeps its name on the page
    return fr == FR_OK ? 0 : fr == FR_NOT_READY ? FLOPPY_E_NO_CARD : fr == FR_NO_FILE ? FLOPPY_E_NOT_FOUND :
           fr == FR_EXIST ? FLOPPY_E_EXISTS : FLOPPY_E_IO;
}

// --- Writing an image ---------------------------------------------------------------

static FIL s_up;
static bool s_up_open;
static char s_up_name[FLOPPY_NAME_LEN];
static uint32_t s_up_size, s_up_done;
static uint8_t s_up_head[HEAD_SIZE];

int floppy_create(const char *name, uint32_t size)
{
    if (s_up_open)
        return FLOPPY_E_BUSY;
    int r = check_name(name, s_up_name);
    if (r)
        return r;
    if (size == 0 || size > FLOPPY_MAX_SIZE)
        return FLOPPY_E_SIZE;
    char path[36];
    path_of(path, sizeof(path), s_up_name);
    sd_lock();
    FILINFO fno;
    FRESULT fr;
    if (!sd_ready()) {
        fr = FR_NOT_READY;
    } else {
        if (f_stat(FLOPPY_DIR, &fno) != FR_OK)
            f_mkdir(FLOPPY_DIR);        // only here: looking never writes to the card
        fr = f_stat(path, &fno) == FR_OK ? FR_EXIST : f_open(&s_up, UPLOAD_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    }
    sd_unlock();
    if (fr != FR_OK)
        return fr == FR_NOT_READY ? FLOPPY_E_NO_CARD : fr == FR_EXIST ? FLOPPY_E_EXISTS : FLOPPY_E_IO;
    s_up_open = true;
    s_up_size = size;
    s_up_done = 0;
    return 0;
}

int floppy_write(const void *data, size_t len)
{
    if (!s_up_open)
        return FLOPPY_E_IO;
    if (s_up_done + len > s_up_size)
        return FLOPPY_E_SIZE;
    if (s_up_done < sizeof(s_up_head)) {
        size_t n = sizeof(s_up_head) - s_up_done < len ? sizeof(s_up_head) - s_up_done : len;
        memcpy(s_up_head + s_up_done, data, n);
    }
    UINT put = 0;
    sd_lock();
    FRESULT fr = f_write(&s_up, data, len, &put);
    sd_unlock();
    s_up_done += put;
    return fr == FR_OK && put == len ? 0 : FLOPPY_E_IO;
}

int floppy_close(bool keep)
{
    if (!s_up_open)
        return FLOPPY_E_IO;
    s_up_open = false;
    char path[36];
    path_of(path, sizeof(path), s_up_name);
    keep = keep && s_up_done == s_up_size;
    size_t head = s_up_done < sizeof(s_up_head) ? s_up_done : sizeof(s_up_head);
    bool image = floppy_kind(s_up_name, s_up_head, head, s_up_done) != FLOPPY_BAD;
    sd_lock();
    FRESULT fr = f_close(&s_up);
    if (fr == FR_OK && keep && image)
        fr = f_rename(UPLOAD_PATH, path);
    else
        f_unlink(UPLOAD_PATH);
    sd_unlock();
    if (!keep)
        return FLOPPY_E_IO;
    if (!image)
        return FLOPPY_E_SIZE;
    if (fr == FR_OK)
        ESP_LOGI(TAG, "%s added (%lu bytes)", s_up_name, (unsigned long)s_up_done);
    return fr == FR_OK ? 0 : fr == FR_EXIST ? FLOPPY_E_EXISTS : FLOPPY_E_IO;
}

// --- Into PSRAM -----------------------------------------------------------------------

typedef struct {
    uint8_t *data;
    floppy_image_t image;
    char name[FLOPPY_NAME_LEN];
} loaded_t;

// s_cur belongs to the task that answers the Atari, s_next is handed over by s_changed
static loaded_t s_cur, s_next;
static volatile bool s_changed;
static volatile bool s_job_busy;
static volatile int s_job_result;
static char s_job_name[FLOPPY_NAME_LEN];
static uint8_t s_job_drive, s_job_keep;
static bool s_job_select;               // a choice (kept in NVS), not the image at power-on

static void *psram(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

// The whole file into PSRAM. The card is only locked per read, the Atari may be using it.
static int read_file(const char *name, uint8_t **out, uint32_t *out_size)
{
    char path[36];
    path_of(path, sizeof(path), name);
    uint8_t *buf = malloc(CHUNK);
    FIL *fil = malloc(sizeof(FIL));
    uint8_t *data = NULL;
    int r = 0;
    uint32_t size = 0;
    if (!buf || !fil) {
        r = FLOPPY_E_MEMORY;
        goto done;
    }
    sd_lock();
    FRESULT fr = sd_ready() ? f_open(fil, path, FA_READ) : FR_NOT_READY;
    sd_unlock();
    if (fr != FR_OK) {
        r = fr == FR_NOT_READY ? FLOPPY_E_NO_CARD : FLOPPY_E_NOT_FOUND;
        goto done;
    }
    size = f_size(fil);
    if (size == 0 || size > FLOPPY_MAX_SIZE)
        r = FLOPPY_E_SIZE;
    else if (!(data = psram(size)))
        r = FLOPPY_E_MEMORY;
    for (uint32_t done = 0; !r && done < size; ) {
        UINT n = 0;
        uint32_t want = size - done > CHUNK ? CHUNK : size - done;
        sd_lock();
        fr = sd_ready() ? f_read(fil, buf, want, &n) : FR_NOT_READY;
        sd_unlock();
        if (fr != FR_OK || n != want)
            r = FLOPPY_E_IO;
        else
            memcpy(data + done, buf, n);
        done += n;
    }
    sd_lock();
    if (sd_ready())
        f_close(fil);
    sd_unlock();
done:
    free(buf);
    free(fil);
    if (r) {
        heap_caps_free(data);
        return r;
    }
    *out = data;
    *out_size = size;
    return 0;
}

static int load_image(const char *name, loaded_t *to)
{
    int64_t t0 = esp_timer_get_time();
    uint8_t *file = NULL;
    uint32_t size = 0;
    int r = read_file(name, &file, &size);
    if (r)
        return r;
    uint8_t *data = NULL;
    uint32_t data_size = 0;
    floppy_geometry_t g;
    const char *dot = strrchr(name, '.');
    if (dot && !strcmp(dot, ".MSA")) {
        if (!floppy_msa_header(file, size, size, &g)) {
            r = FLOPPY_E_SIZE;
        } else if (!(data = psram(g.sectors * 512))) {
            r = FLOPPY_E_MEMORY;
        } else if (!floppy_msa_unpack(file, size, &g, data)) {
            r = FLOPPY_E_SIZE;
            heap_caps_free(data);
            data = NULL;
        } else {
            data_size = g.sectors * 512;
        }
        heap_caps_free(file);
    } else if (floppy_st_geometry(file, size, size, &g)) {
        data = file;
        data_size = size;
    } else {
        r = FLOPPY_E_SIZE;
        heap_caps_free(file);
    }
    if (r)
        return r;
    memset(to, 0, sizeof(*to));
    to->data = data;
    to->image.data = data;
    to->image.sectors = data_size / 512;
    to->image.sides = g.sides;
    to->image.sectors_per_track = g.sectors_per_track;
    to->image.tracks = g.tracks;
    floppy_make_bpb(data, g.sectors, to->image.bpb);
    strlcpy(to->name, name, sizeof(to->name));
    ESP_LOGI(TAG, "%s in memory: %u tracks, %u sides, %u sectors per track, %lu ms", name, g.tracks, g.sides,
             g.sectors_per_track, (unsigned long)((esp_timer_get_time() - t0) / 1000));
    return 0;
}

static void job_task(void *arg)
{
    if (!s_job_select) {                // at power-on the card is not mounted yet
        for (int ms = 0; ms < CARD_WAIT_MS && !sd_ready(); ms += 20)
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    int r = load_image(s_job_name, &s_next);
    if (r) {
        ESP_LOGE(TAG, "%s: %s", s_job_name, floppy_error(r));
    } else if (s_job_select) {
        if (!save_nvs(true, s_job_name, s_job_drive, s_job_keep)) {
            r = FLOPPY_E_IO;
            heap_caps_free(s_next.data);
            s_next.data = NULL;
        } else if (cart_emulator_on()) {
            cart_select(NULL);          // GEMDRIVE has to be in the cartridge for the hooks
            cart_apply_now();
        }
    }
    s_job_result = r;
    s_changed = true;                   // also a failure at power-on: the state is published
    s_job_busy = false;
    vTaskDelete(NULL);
}

static int job_start(const char *name, bool select, int drive, bool keep)
{
    if (s_job_busy || s_changed)
        return FLOPPY_E_BUSY;
    int r = check_name(name, s_job_name);
    if (r)
        return r;
    s_job_select = select;
    s_job_drive = drive != 0;
    s_job_keep = keep;
    s_job_result = 0;
    s_job_busy = true;
    if (xTaskCreatePinnedToCore(job_task, "floppy", 6144, NULL, 1, NULL, 1) != pdPASS) {
        s_job_busy = false;
        return FLOPPY_E_MEMORY;
    }
    return 0;
}

int floppy_select_start(const char *name, int drive, bool keep_drives)
{
    return job_start(name, true, drive, keep_drives);
}

bool floppy_select_busy(void)
{
    return s_job_busy;
}

int floppy_select_result(void)
{
    return s_job_result;
}

void floppy_off(void)
{
    if (save_nvs(false, NULL, 0, false)) {
        ESP_LOGI(TAG, "floppy emulator off");
        s_job_result = 0;
        s_changed = true;
    }
}

void floppy_boot(void)
{
    if (!floppy_emulator_on() || s_job_busy || s_changed)
        return;
    if (s_cur.data && !strcmp(s_cur.name, s_active))
        return;
    job_start(s_active, false, s_drive, s_keep);
}

void floppy_wait_ready(uint32_t ms)
{
    for (uint32_t t = 0; s_job_busy && t < ms; t += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
}

bool floppy_change_pending(void)
{
    return s_changed;
}

bool floppy_take_change(void)
{
    if (!s_changed || s_job_busy)
        return false;
    if (s_next.data) {                  // the new one is complete: only now the old one goes
        heap_caps_free(s_cur.data);
        s_cur = s_next;
        memset(&s_next, 0, sizeof(s_next));
    } else if (!floppy_emulator_on()) {
        heap_caps_free(s_cur.data);
        memset(&s_cur, 0, sizeof(s_cur));
    }
    s_changed = false;
    return true;
}

const floppy_image_t *floppy_image(void)
{
    return floppy_emulator_on() && s_cur.data ? &s_cur.image : NULL;
}

const uint8_t *floppy_sector(uint32_t lba)
{
    return floppy_emulator_on() && s_cur.data && lba < s_cur.image.sectors ? s_cur.data + lba * 512 : NULL;
}
