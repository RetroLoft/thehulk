// GEMDRIVE storage, see gemdrv_fs.h. The SD drive uses FatFs directly (not the VFS): it
// gives the DOS date/time and the FAT attributes, which are the GEMDOS ones. The flash
// drive is a fixed list of files in the firmware. The card and a TNFS drive show names that
// GEMDOS can use: 8.3, shown in capitals.
// The flash drive is a FAT volume too (flashfs.c, FatFs drive "1:"). Its root also shows the
// files built into the firmware, read-only: they cannot be changed or removed there.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "atari_charset.h"
#include "ff.h"
#include "flashfs.h"
#include "gemdrv_fs.h"
#include "gemdrv_layout.h"
#include "sd.h"
#include "settings.h"
#include "status_led.h"
#include "tnfs.h"

#define MAX_PATH    256
#define MAX_FILES   16
#define MAX_SEARCH  16

#define GEMDOS_ERANGE  -64
#define MAX_FREE_KB    0x1FFFFF         // Dfree: the free bytes have to fit in a positive long

#define FA_ST_HIDDEN  0x02
#define FA_ST_SYSTEM  0x04
#define FA_ST_VOLUME  0x08
#define FA_ST_DIR     0x10
#define SD_ATTR_MASK  (AM_RDO | AM_HID | AM_SYS | AM_ARC)   // what GEMDOS can set

// Per drive slot: the folder on the card (FatFs drive 0 = the card mounted by sd.c) and the
// current folder, Atari form: "" or "\DIR\SUB"
static char s_root[FS_MAX_SLOTS][MAX_PATH];
static char s_cwds[FS_MAX_SLOTS][MAX_PATH];
static int s_flash_slot = -1;
static bool s_is_tnfs[FS_MAX_SLOTS];

// The files built into the firmware, in the root of the flash drive
extern const uint8_t thehulk_prg_start[] asm("_binary_THEHULK_PRG_start");
extern const uint8_t thehulk_prg_end[]   asm("_binary_THEHULK_PRG_end");
extern const uint8_t stingins_tos_start[] asm("_binary_STINGINS_TOS_start");
extern const uint8_t stingins_tos_end[]   asm("_binary_STINGINS_TOS_end");
#define FLASH_TIME  (12 << 11)
#define FLASH_DATE  (((2026 - 1980) << 9) | (11 << 5) | 2)

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
} flash_file_t;
static flash_file_t s_flash[2];

static void flash_files_init(void)
{
    s_flash[0] = (flash_file_t){ "THEHULK.PRG", thehulk_prg_start,
                                 (uint32_t)(thehulk_prg_end - thehulk_prg_start) };
    s_flash[1] = (flash_file_t){ "STINGINS.TOS", stingins_tos_start,      // STinG installer
                                 (uint32_t)(stingins_tos_end - stingins_tos_start) };
}

typedef struct {
    bool used;
    FIL fil;
    const flash_file_t *flash;      // a built-in file: no FIL
    int tnfs_slot;                  // on a TNFS drive: no FIL (-1: not)
    uint8_t tnfs_handle;
    uint32_t size;
    uint32_t pos;
    uint32_t wire_pos;              // where the server stands in the file
    bool writable;
    uint16_t time, date;
    bool date_set;                  // on the card: to apply after closing the file
    char path[MAX_PATH];            // on the card
} file_t;
static file_t *s_files;             // handle = FIRST_FILE_DESCRIPTOR + index

typedef struct {
    uint32_t dta;                   // 0: free
    bool open;                      // directory still open (not at its end)
    bool flash;                     // still listing the built-in files
    bool flash_root;                // the root of the flash drive: built-in names win
    int flash_next;
    bool dir_open;                  // FF_DIR open
    bool on_card;
    int tnfs_slot;                  // on a TNFS drive: no FF_DIR (-1: not)
    uint8_t tnfs_handle;
    FF_DIR dir;
    char name[8], ext[3];           // pattern, '?' for any character
    uint8_t attribs;
    uint32_t age;
} search_t;
static search_t s_search[MAX_SEARCH];
static uint32_t s_age;

void fs_set_sd_slot(int slot, const char *sd_path)
{
    if (slot < 0 || slot >= FS_MAX_SLOTS)
        return;
    s_is_tnfs[slot] = false;
    size_t n = snprintf(s_root[slot], MAX_PATH, "0:%s%s", sd_path[0] == '/' || sd_path[0] == '\\' ? "" : "/", sd_path);
    for (size_t i = 0; i < n && i < MAX_PATH; i++)
        if (s_root[slot][i] == '\\')
            s_root[slot][i] = '/';
    while (n > 2 && s_root[slot][n - 1] == '/')
        s_root[slot][--n] = 0;
}

static bool slot_is_tnfs(int slot)
{
    return slot >= 0 && slot < FS_MAX_SLOTS && s_is_tnfs[slot];
}

// The Atari asks for the card: without a card the LED shows it
static bool card_there(void)
{
    if (sd_ready())
        return true;
    status_led_error();
    return false;
}

// A folder on the card
static bool slot_is_sd(int slot)
{
    return slot >= 0 && slot < FS_MAX_SLOTS && !s_is_tnfs[slot] && slot != s_flash_slot && s_root[slot][0];
}

// FatFs: the card or the flash drive
static bool slot_is_fat(int slot)
{
    return slot >= 0 && slot < FS_MAX_SLOTS && !s_is_tnfs[slot] && s_root[slot][0];
}

// The volume of a FatFs path ("0:/..." the card, "1:/..." the flash drive) can be used
static bool fat_ready(const char *fat)
{
    return fat[0] == FLASHFS_DRIVE[0] ? flashfs_ready() : card_there();
}

void fs_set_tnfs_slot(int slot, const char *host, uint16_t port, const char *mount)
{
    if (slot < 0 || slot >= FS_MAX_SLOTS)
        return;
    s_is_tnfs[slot] = true;
    s_root[slot][0] = 0;
    tnfs_set_slot(slot, host, port, mount);
}

void fs_set_flash_slot(int slot)
{
    if (slot < 0 || slot >= FS_MAX_SLOTS)
        return;
    s_flash_slot = slot;
    s_is_tnfs[slot] = false;
    strcpy(s_root[slot], FLASHFS_DRIVE);
}

static void search_close(search_t *s)
{
    if (!s->open)
        return;
    if (s->tnfs_slot >= 0)
        tnfs_dir_close(s->tnfs_slot, s->tnfs_handle);
    else if (s->dir_open)
        f_closedir(&s->dir);
    s->dir_open = false;
    s->open = false;
}

static FRESULT sd_close(file_t *f)
{
    FRESULT fr = f_close(&f->fil);
    if (fr == FR_OK && f->date_set) {
        FILINFO fno = { .fdate = f->date, .ftime = f->time };
        fr = f_utime(f->path, &fno);
    }
    return fr;
}

void fs_reset(void)
{
    memset(s_cwds, 0, sizeof(s_cwds));
    flash_files_init();
    if (!s_files)
        s_files = calloc(MAX_FILES, sizeof(file_t));
    for (int i = 0; s_files && i < MAX_FILES; i++) {
        if (s_files[i].used && s_files[i].tnfs_slot >= 0)
            tnfs_file_close(s_files[i].tnfs_slot, s_files[i].tnfs_handle);
        else if (s_files[i].used && !s_files[i].flash)
            sd_close(&s_files[i]);
        s_files[i].used = false;
    }
    for (int i = 0; i < MAX_SEARCH; i++) {
        search_close(&s_search[i]);
        s_search[i].dta = 0;
        s_search[i].open = false;
    }
}

void fs_sd_removed(void)
{
    for (int i = 0; s_files && i < MAX_FILES; i++)
        if (s_files[i].used && s_files[i].tnfs_slot < 0 && !s_files[i].flash && s_files[i].path[0] != FLASHFS_DRIVE[0])
            s_files[i].used = false;
    for (int i = 0; i < MAX_SEARCH; i++)
        if (s_search[i].open && s_search[i].on_card)
            s_search[i].open = s_search[i].dir_open = false;
    for (int i = 0; i < FS_MAX_SLOTS; i++)
        if (slot_is_sd(i))
            s_cwds[i][0] = 0;
}

// --- Paths --------------------------------------------------------------------------

// Atari path -> FatFs path below the folder of the slot. Handles the drive letter, the current folder,
// "." and "..". Returns false when the path would leave the root.
static bool to_fat(int slot, const char *apath, char *out, size_t size)
{
    if (slot < 0 || slot >= FS_MAX_SLOTS || (!s_root[slot][0] && !s_is_tnfs[slot]))
        return false;
    char comp[16][13];
    int n = 0;
    if (isalpha((unsigned char)apath[0]) && apath[1] == ':')
        apath += 2;
    const char *p = apath;
    if (*p != '\\') {
        // Relative: start from the current folder
        const char *c = s_cwds[slot];
        while (*c) {
            while (*c == '\\')
                c++;
            int len = 0;
            while (*c && *c != '\\') {
                if (len < 12 && n < 16)
                    comp[n][len++] = *c;
                c++;
            }
            if (len && n < 16)
                comp[n++][len] = 0;
        }
    }
    while (*p) {
        while (*p == '\\')
            p++;
        if (!*p)
            break;
        char part[13];
        int len = 0;
        while (*p && *p != '\\') {
            if (len < 12)
                part[len++] = toupper((unsigned char)*p);
            p++;
        }
        part[len] = 0;
        if (!strcmp(part, "."))
            continue;
        if (!strcmp(part, "..")) {
            if (n == 0)
                return false;
            n--;
            continue;
        }
        if (n == 16)
            return false;
        strcpy(comp[n++], part);
    }
    size_t len = snprintf(out, size, "%s", s_root[slot]);
    for (int i = 0; i < n && len < size; i++)
        len += snprintf(out + len, size - len, "/%s", comp[i]);
    if (!len && len + 1 < size) {       // the root of a TNFS drive
        out[0] = '/';
        out[1] = 0;
        len = 1;
    }
    return len < size;
}

// The server tells capitals from small letters, the Atari only has capitals: finds the
// spelling on the server of a path from to_fat, the part that is not there stays as it is.
static void tnfs_real_path(int slot, const char *up, char *out, size_t size)
{
    uint32_t file_size, mtime;
    bool is_dir;
    strlcpy(out, up, size);
    if (!strcmp(up, "/") || tnfs_stat(slot, up, &file_size, &mtime, &is_dir) != GEMDOS_EFILNF)
        return;
    size_t len = 0;
    out[0] = 0;
    for (const char *p = up; *p == '/' && p[1] && len < size;) {
        const char *end = strchr(++p, '/');
        char comp[40], real[sizeof(((tnfs_entry_t *)0)->name)];    // UTF-8: up to 3 bytes a character
        size_t n = end ? (size_t)(end - p) : strlen(p);
        snprintf(comp, sizeof(comp), "%.*s", (int)n, p);
        strlcpy(real, comp, sizeof(real));
        uint8_t h;
        if (tnfs_dir_open(slot, len ? out : "/", &h) == 0) {
            tnfs_entry_t e;
            bool found = false;
            while (tnfs_dir_next(slot, h, &e) > 0) {
                if (strcasecmp(e.name, comp))
                    continue;
                if (!found || !strcmp(e.name, comp))    // the same spelling goes first
                    strlcpy(real, e.name, sizeof(real));
                found = true;
                if (!strcmp(e.name, comp))
                    break;
            }
            tnfs_dir_close(slot, h);
        }
        len += snprintf(out + len, size - len, "/%s", real);
        p += n;
    }
}

// The server keeps names in UTF-8, GEMDOS in the Atari character set
static bool to_tnfs(int slot, const char *apath, char *out, size_t size)
{
    char up[MAX_PATH], utf8[MAX_PATH];
    if (!to_fat(slot, apath, up, sizeof(up)))
        return false;
    atari_to_utf8(up, utf8, sizeof(utf8));
    tnfs_real_path(slot, utf8, out, size);
    return true;
}

// Splits "C:\DIR\*.PRG" into the folder ("C:\DIR") and the pattern ("*.PRG")
static void split_pattern(const char *apath, char *dir, size_t size, const char **pattern)
{
    const char *slash = strrchr(apath, '\\');
    const char *colon = strchr(apath, ':');
    const char *start = slash ? slash + 1 : colon ? colon + 1 : apath;
    size_t len = start - apath;
    if (len >= size)
        len = size - 1;
    memcpy(dir, apath, len);
    dir[len] = 0;
    *pattern = start;
}

// "*.PRG" -> name "????????", ext "PRG"; "*" or "*.*" match everything
static void make_pattern(const char *pat, char name[8], char ext[3])
{
    memset(name, ' ', 8);
    memset(ext, ' ', 3);
    if (!*pat || !strcmp(pat, "*") || !strcmp(pat, "*.*")) {
        memset(name, '?', 8);
        memset(ext, '?', 3);
        return;
    }
    char *field = name;
    int max = 8, i = 0;
    for (; *pat; pat++) {
        if (*pat == '.') {
            if (field == ext)
                break;
            field = ext;
            max = 3;
            i = 0;
        } else if (*pat == '*') {
            while (i < max)
                field[i++] = '?';
        } else if (i < max) {
            field[i++] = toupper((unsigned char)*pat);
        }
    }
}

static bool match(const search_t *s, const char *fname)
{
    char name[8], ext[3];
    memset(name, ' ', 8);
    memset(ext, ' ', 3);
    const char *dot = strchr(fname, '.');
    size_t nlen = dot ? (size_t)(dot - fname) : strlen(fname);
    memcpy(name, fname, nlen > 8 ? 8 : nlen);
    if (dot) {
        size_t elen = strlen(dot + 1);
        memcpy(ext, dot + 1, elen > 3 ? 3 : elen);
    }
    for (int i = 0; i < 8; i++)
        if (s->name[i] != '?' && toupper((unsigned char)name[i]) != s->name[i])
            return false;
    for (int i = 0; i < 3; i++)
        if (s->ext[i] != '?' && toupper((unsigned char)ext[i]) != s->ext[i])
            return false;
    return true;
}

// GEMDOS Fsfirst attribute rules: plain files always, folders, hidden and system
// entries only when asked for, the volume label only when asked for on its own
static bool attribs_ok(uint8_t want, uint8_t have)
{
    if (want == FA_ST_VOLUME)
        return have & FA_ST_VOLUME;
    if (have & FA_ST_VOLUME)
        return false;
    uint8_t special = have & (FA_ST_HIDDEN | FA_ST_SYSTEM | FA_ST_DIR);
    return !special || (special & want);
}

static int16_t fresult_to_gemdos(FRESULT fr)
{
    switch (fr) {
    case FR_OK:             return GEMDOS_EOK;
    case FR_NO_FILE:        return GEMDOS_EFILNF;
    case FR_NO_PATH:
    case FR_INVALID_NAME:   return GEMDOS_EPTHNF;
    case FR_DENIED:
    case FR_EXIST:
    case FR_WRITE_PROTECTED: return GEMDOS_EACCDN;
    default:
        status_led_error();
        return GEMDOS_ERROR;
    }
}

// --- Directory search ---------------------------------------------------------------

static search_t *find_search(uint32_t dta)
{
    for (int i = 0; i < MAX_SEARCH; i++)
        if (s_search[i].dta == dta && dta)
            return &s_search[i];
    return NULL;
}

static search_t *new_search(uint32_t dta)
{
    search_t *s = find_search(dta);
    if (!s) {
        s = &s_search[0];
        for (int i = 0; i < MAX_SEARCH; i++) {
            if (!s_search[i].dta) {
                s = &s_search[i];
                break;
            }
            if (s_search[i].age < s->age)
                s = &s_search[i];       // all in use: reuse the oldest
        }
    }
    search_close(s);
    memset(s, 0, sizeof(*s));
    s->tnfs_slot = -1;
    s->dta = dta;
    s->age = ++s_age;
    return s;
}

static void fill_dta(uint8_t *d, const FILINFO *fno)
{
    memset(d, 0, FS_DTA_SIZE);
    d[20] = fno->fattrib;
    d[21] = fno->fattrib;
    d[22] = fno->ftime >> 8;
    d[23] = fno->ftime;
    d[24] = fno->fdate >> 8;
    d[25] = fno->fdate;
    uint32_t size = fno->fsize;
    d[26] = size >> 24;
    d[27] = size >> 16;
    d[28] = size >> 8;
    d[29] = size;
    strncpy((char *)d + 30, fno->fname, 13);
}

// A name GEMDOS can use: 8.3. The Atari sees it in capitals, see tnfs_real_path.
static bool fits_gemdos(const char *name)
{
    const char *dot = strchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    return base && base <= 8 && !(dot && (strlen(dot + 1) > 3 || strchr(dot + 1, '.')));
}

// Unix time -> DOS date and time, in the time zone of the settings
static void unix_to_dos(uint32_t t, uint16_t *date, uint16_t *tod)
{
    time_t local = (time_t)t + atoi(settings_rtc()->utc_offset) * 3600;
    struct tm tm;
    gmtime_r(&local, &tm);
    if (tm.tm_year < 80) {
        *date = (1 << 5) | 1;           // 1980-01-01
        *tod = 0;
        return;
    }
    *date = ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
    *tod = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
}

static bool is_builtin_name(const char *name)
{
    for (size_t i = 0; i < sizeof(s_flash) / sizeof(s_flash[0]); i++)
        if (!strcasecmp(name, s_flash[i].name))
            return true;
    return false;
}

// The built-in file a path names (in the root of the flash drive), or NULL
static const flash_file_t *builtin(int slot, const char *apath)
{
    char fat[MAX_PATH];
    if (slot != s_flash_slot || !to_fat(slot, apath, fat, sizeof(fat)))
        return NULL;
    size_t n = strlen(s_root[slot]);
    if (strncmp(fat, s_root[slot], n) || fat[n] != '/' || strchr(fat + n + 1, '/'))
        return NULL;
    for (size_t i = 0; i < sizeof(s_flash) / sizeof(s_flash[0]); i++)
        if (!strcasecmp(fat + n + 1, s_flash[i].name))
            return &s_flash[i];
    return NULL;
}

static int16_t next_match(search_t *s, uint8_t *dta_out)
{
    FILINFO fno;
    while (s->open && s->tnfs_slot >= 0) {
        tnfs_entry_t e;
        if (tnfs_dir_next(s->tnfs_slot, s->tnfs_handle, &e) <= 0) {
            search_close(s);
            break;
        }
        uint8_t attr = e.is_dir ? AM_DIR : 0;
        char name[sizeof(e.name)];
        if (!utf8_to_atari(e.name, name, sizeof(name)))
            continue;                   // a character the Atari does not have: hidden
        strcpy(e.name, name);
        for (char *c = e.name; *c; c++)
            if ((unsigned char)*c < 0x80)
                *c = toupper((unsigned char)*c);
        if (fits_gemdos(e.name) && match(s, e.name) && attribs_ok(s->attribs, attr)) {
            memset(&fno, 0, sizeof(fno));
            fno.fsize = e.size;
            fno.fattrib = attr;
            unix_to_dos(e.mtime, &fno.fdate, &fno.ftime);
            strncpy(fno.fname, e.name, sizeof(fno.fname) - 1);
            fill_dta(dta_out, &fno);
            return GEMDOS_EOK;
        }
    }
    while (s->open && s->flash) {
        if (s->flash_next >= (int)(sizeof(s_flash) / sizeof(s_flash[0]))) {
            s->flash = false;
            s->open = s->dir_open;
            break;
        }
        const flash_file_t *f = &s_flash[s->flash_next++];
        if (match(s, f->name) && attribs_ok(s->attribs, AM_RDO)) {
            memset(&fno, 0, sizeof(fno));
            fno.fsize = f->size;
            fno.ftime = FLASH_TIME;
            fno.fdate = FLASH_DATE;
            fno.fattrib = AM_RDO;
            strncpy(fno.fname, f->name, sizeof(fno.fname) - 1);
            fill_dta(dta_out, &fno);
            return GEMDOS_EOK;
        }
    }
    while (s->open && s->dir_open) {
        if (f_readdir(&s->dir, &fno) != FR_OK || !fno.fname[0]) {
            f_closedir(&s->dir);
            s->open = s->dir_open = false;
            break;
        }
        // FatFs reads the short names, in capitals. A long name shows as NAME~1.EXT: hidden.
        const char *tilde = strchr(fno.fname, '~');
        if (tilde && isdigit((unsigned char)tilde[1]))
            continue;
        if (s->flash_root && is_builtin_name(fno.fname))
            continue;                   // the built-in file goes first
        if (match(s, fno.fname) && attribs_ok(s->attribs, fno.fattrib)) {
            fill_dta(dta_out, &fno);
            return GEMDOS_EOK;
        }
    }
    memset(dta_out, 0, FS_DTA_SIZE);
    return GEMDOS_ENMFIL;
}

int16_t fs_fsfirst(int slot, uint32_t dta, const char *path, uint16_t attribs, uint8_t *dta_out)
{
    status_led_activity();
    char adir[MAX_PATH], fat[MAX_PATH];
    const char *pattern;
    memset(dta_out, 0, FS_DTA_SIZE);
    split_pattern(path, adir, sizeof(adir), &pattern);
    if (slot_is_tnfs(slot)) {
        uint8_t h;
        if (!to_tnfs(slot, adir[0] ? adir : ".", fat, sizeof(fat)))
            return GEMDOS_EPTHNF;
        int r = tnfs_dir_open(slot, fat, &h);
        if (r < 0)
            return r;
        search_t *s = new_search(dta);
        s->attribs = attribs;
        s->tnfs_slot = slot;
        s->tnfs_handle = h;
        s->open = true;
        make_pattern(pattern, s->name, s->ext);
        int16_t found = next_match(s, dta_out);
        return found == GEMDOS_ENMFIL ? GEMDOS_EFILNF : found;
    }
    if (!to_fat(slot, adir[0] ? adir : ".", fat, sizeof(fat)))
        return GEMDOS_EPTHNF;
    bool flash_root = slot == s_flash_slot && !strcmp(fat, s_root[slot]);
    bool ready = fat_ready(fat);
    if (!ready && !flash_root)
        return GEMDOS_EPTHNF;

    search_t *s = new_search(dta);
    s->attribs = attribs;
    s->flash = s->flash_root = flash_root;
    s->on_card = slot != s_flash_slot;
    make_pattern(pattern, s->name, s->ext);
    if (ready) {
        FRESULT fr = f_opendir(&s->dir, fat);
        if (fr != FR_OK && !flash_root)
            return fr == FR_NO_PATH || fr == FR_NO_FILE ? GEMDOS_EPTHNF : fresult_to_gemdos(fr);
        s->dir_open = fr == FR_OK;
    }
    s->open = true;
    int16_t r = next_match(s, dta_out);
    return r == GEMDOS_ENMFIL ? GEMDOS_EFILNF : r;
}

int16_t fs_fsnext(uint32_t dta, uint8_t *dta_out)
{
    status_led_activity();
    search_t *s = find_search(dta);
    if (!s) {
        memset(dta_out, 0, FS_DTA_SIZE);
        return GEMDOS_ENMFIL;
    }
    return next_match(s, dta_out);
}

bool fs_dta_exists(uint32_t dta)
{
    return find_search(dta) != NULL;
}

bool fs_dta_release(uint32_t dta)
{
    search_t *s = find_search(dta);
    if (!s)
        return false;
    search_close(s);
    memset(s, 0, sizeof(*s));
    return true;
}

// --- Files --------------------------------------------------------------------------

static file_t *get_file(uint16_t handle)
{
    int i = handle - FIRST_FILE_DESCRIPTOR;
    if (!s_files || i < 0 || i >= MAX_FILES || !s_files[i].used)
        return NULL;
    return &s_files[i];
}

// A TNFS file: flags as in tnfs.h, create: no need to be there already
static int32_t tnfs_open_file(int slot, const char *fat, uint16_t flags, bool writable)
{
    if (!s_files)
        return GEMDOS_EFILNF;
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i].used)
            continue;
        uint32_t size = 0, mtime = 0;
        bool is_dir = false;
        int r = tnfs_stat(slot, fat, &size, &mtime, &is_dir);
        if (r == 0 && is_dir)
            return GEMDOS_EFILNF;
        if (r < 0 && !(flags & TNFS_O_CREAT))
            return r == GEMDOS_EPTHNF ? GEMDOS_EFILNF : r;
        uint8_t h;
        r = tnfs_file_open(slot, fat, flags, &h);
        if (r < 0)
            return r;
        file_t *f = &s_files[i];
        f->used = true;
        f->flash = NULL;
        f->tnfs_slot = slot;
        f->tnfs_handle = h;
        f->size = flags & TNFS_O_TRUNC ? 0 : size;
        f->pos = f->wire_pos = 0;
        f->writable = writable;
        unix_to_dos(flags & TNFS_O_TRUNC ? (uint32_t)time(NULL) : mtime, &f->date, &f->time);
        return FIRST_FILE_DESCRIPTOR + i;
    }
    return GEMDOS_ENHNDL;
}

// A file that gets changed or removed cannot stay open: FatFs would write over it with its
// old view. A program that ended without closing its file also leaves a handle behind.
static void sd_close_path(const char *fat)
{
    for (int i = 0; s_files && i < MAX_FILES; i++) {
        file_t *f = &s_files[i];
        if (f->used && f->tnfs_slot < 0 && !f->flash && !strcasecmp(f->path, fat)) {
            sd_close(f);
            f->used = false;
        }
    }
}

// A file on the card: FatFs mode, FA_READ alone leaves other handles on the file alone
static int32_t sd_open_file(const char *fat, BYTE fa_mode)
{
    if (!fat_ready(fat) || !s_files)
        return GEMDOS_EFILNF;
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i].used)
            continue;
        file_t *f = &s_files[i];
        FILINFO fno;
        bool create = fa_mode & FA_CREATE_ALWAYS;
        if (!create) {
            FRESULT fr = f_stat(fat, &fno);
            if (fr != FR_OK)
                return fresult_to_gemdos(fr);
            if (fno.fattrib & AM_DIR)
                return GEMDOS_EFILNF;
        }
        if (fa_mode != FA_READ)
            sd_close_path(fat);
        FRESULT fr = f_open(&f->fil, fat, fa_mode);
        if (fr != FR_OK)
            return fresult_to_gemdos(fr);
        f->used = true;
        f->flash = NULL;
        f->tnfs_slot = -1;
        f->writable = fa_mode != FA_READ;
        f->pos = 0;
        f->date_set = false;
        strlcpy(f->path, fat, sizeof(f->path));
        if (create) {
            unix_to_dos((uint32_t)time(NULL), &f->date, &f->time);
        } else {
            f->time = fno.ftime;
            f->date = fno.fdate;
        }
        return FIRST_FILE_DESCRIPTOR + i;
    }
    return GEMDOS_ENHNDL;
}

int32_t fs_fopen(int slot, const char *path, uint16_t mode)
{
    status_led_activity();
    char fat[MAX_PATH];
    const flash_file_t *ff = builtin(slot, path);
    if ((mode & 3) != 0 && (ff || (!slot_is_tnfs(slot) && !slot_is_fat(slot))))
        return GEMDOS_EACCDN;           // the built-in files are read-only
    if (ff) {
        if (!s_files)
            return GEMDOS_EFILNF;
        for (int i = 0; i < MAX_FILES; i++) {
            if (s_files[i].used)
                continue;
            s_files[i].used = true;
            s_files[i].tnfs_slot = -1;
            s_files[i].flash = ff;
            s_files[i].writable = false;
            s_files[i].pos = 0;
            s_files[i].time = FLASH_TIME;
            s_files[i].date = FLASH_DATE;
            return FIRST_FILE_DESCRIPTOR + i;
        }
        return GEMDOS_ENHNDL;
    }
    if (slot_is_tnfs(slot)) {
        static const uint16_t flags[3] = { TNFS_O_RDONLY, TNFS_O_WRONLY, TNFS_O_RDWR };
        if ((mode & 3) > 2 || !to_tnfs(slot, path, fat, sizeof(fat)))
            return GEMDOS_EFILNF;
        return tnfs_open_file(slot, fat, flags[mode & 3], (mode & 3) != 0);
    }
    static const BYTE fa_modes[3] = { FA_READ, FA_WRITE, FA_READ | FA_WRITE };
    if ((mode & 3) > 2 || !to_fat(slot, path, fat, sizeof(fat)))
        return GEMDOS_EFILNF;
    return sd_open_file(fat, fa_modes[mode & 3]);
}

int32_t fs_fcreate(int slot, const char *path, uint16_t attr)
{
    status_led_activity();
    char fat[MAX_PATH];
    if ((!slot_is_tnfs(slot) && !slot_is_fat(slot)) || builtin(slot, path))
        return GEMDOS_EACCDN;
    if (slot_is_tnfs(slot))
        return to_tnfs(slot, path, fat, sizeof(fat))
                   ? tnfs_open_file(slot, fat, TNFS_O_RDWR | TNFS_O_CREAT | TNFS_O_TRUNC, true)
                   : GEMDOS_EPTHNF;
    if (!to_fat(slot, path, fat, sizeof(fat)))
        return GEMDOS_EPTHNF;
    if (attr & (FA_ST_VOLUME | FA_ST_DIR))
        return GEMDOS_EACCDN;
    int32_t h = sd_open_file(fat, FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
    if (h >= 0 && (attr & SD_ATTR_MASK))
        f_chmod(fat, attr & SD_ATTR_MASK, SD_ATTR_MASK);
    return h;
}

// The server stands where the file position says
static int tnfs_sync_pos(file_t *f)
{
    if (f->wire_pos == f->pos)
        return 0;
    int r = tnfs_file_seek(f->tnfs_slot, f->tnfs_handle, f->pos);
    if (r == 0)
        f->wire_pos = f->pos;
    return r;
}

int32_t fs_fwrite(uint16_t handle, const uint8_t *buf, uint32_t len)
{
    status_led_activity();
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    if (!f->writable)
        return GEMDOS_EACCDN;
    if (f->tnfs_slot < 0) {
        UINT n = 0;
        FRESULT fr = FR_OK;
        if (f_tell(&f->fil) != f->pos)
            fr = f_lseek(&f->fil, f->pos);
        if (fr == FR_OK)
            fr = f_write(&f->fil, buf, len, &n);
        return fr == FR_OK ? (int32_t)n : fresult_to_gemdos(fr);
    }
    int r = tnfs_sync_pos(f);
    if (r < 0)
        return r;
    int n = tnfs_file_write(f->tnfs_slot, f->tnfs_handle, buf, len);
    if (n > 0)
        f->wire_pos += n;
    return n;
}

int16_t fs_fwrite_commit(uint16_t handle, uint32_t len)
{
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    if ((int32_t)len < 0)               // the write failed
        return GEMDOS_EOK;
    f->pos += len;
    if (f->pos > f->size)
        f->size = f->pos;
    return GEMDOS_EOK;
}

int16_t fs_fdelete(int slot, const char *path)
{
    status_led_activity();
    char fat[MAX_PATH];
    if ((!slot_is_tnfs(slot) && !slot_is_fat(slot)) || builtin(slot, path))
        return GEMDOS_EACCDN;
    if (slot_is_tnfs(slot))
        return to_tnfs(slot, path, fat, sizeof(fat)) ? tnfs_unlink(slot, fat) : GEMDOS_EFILNF;
    if (!to_fat(slot, path, fat, sizeof(fat)))
        return GEMDOS_EFILNF;
    if (!fat_ready(fat))
        return GEMDOS_EFILNF;
    FILINFO fno;
    FRESULT fr = f_stat(fat, &fno);
    if (fr == FR_OK && (fno.fattrib & AM_DIR))
        return GEMDOS_EFILNF;
    if (fr == FR_OK) {
        sd_close_path(fat);
        fr = f_unlink(fat);
    }
    return fresult_to_gemdos(fr);
}

int16_t fs_dcreate(int slot, const char *path)
{
    status_led_activity();
    char fat[MAX_PATH];
    if ((!slot_is_tnfs(slot) && !slot_is_fat(slot)) || builtin(slot, path))
        return GEMDOS_EACCDN;
    if (slot_is_tnfs(slot))
        return to_tnfs(slot, path, fat, sizeof(fat)) ? tnfs_mkdir(slot, fat) : GEMDOS_EPTHNF;
    if (!to_fat(slot, path, fat, sizeof(fat)))
        return GEMDOS_EPTHNF;
    return fat_ready(fat) ? fresult_to_gemdos(f_mkdir(fat)) : GEMDOS_EPTHNF;
}

int16_t fs_ddelete(int slot, const char *path)
{
    status_led_activity();
    char fat[MAX_PATH];
    if (!slot_is_tnfs(slot) && !slot_is_fat(slot))
        return GEMDOS_EACCDN;
    if (slot_is_tnfs(slot)) {
        if (!to_tnfs(slot, path, fat, sizeof(fat)))
            return GEMDOS_EPTHNF;
        int r = tnfs_rmdir(slot, fat);
        return r == GEMDOS_EFILNF ? GEMDOS_EPTHNF : r;
    }
    if (!to_fat(slot, path, fat, sizeof(fat)))
        return GEMDOS_EPTHNF;
    if (!fat_ready(fat))
        return GEMDOS_EPTHNF;
    if (!strcmp(fat, s_root[slot]))
        return GEMDOS_EACCDN;
    FILINFO fno;
    FRESULT fr = f_stat(fat, &fno);
    if (fr == FR_NO_FILE || (fr == FR_OK && !(fno.fattrib & AM_DIR)))
        return GEMDOS_EPTHNF;
    if (fr == FR_OK)
        fr = f_unlink(fat);             // not empty: FR_DENIED
    return fresult_to_gemdos(fr);
}

int32_t fs_frename(int slot, int slot_to, const char *from, const char *to)
{
    status_led_activity();
    char a[MAX_PATH], b[MAX_PATH];
    if ((!slot_is_tnfs(slot) && !slot_is_fat(slot)) || builtin(slot, from) || builtin(slot_to, to))
        return GEMDOS_EACCDN;
    if (slot != slot_to)
        return GEMDOS_ENSAME;
    if (slot_is_tnfs(slot))
        return to_tnfs(slot, from, a, sizeof(a)) && to_tnfs(slot, to, b, sizeof(b))
                   ? tnfs_rename(slot, a, b) : GEMDOS_EFILNF;
    if (!to_fat(slot, from, a, sizeof(a)) || !to_fat(slot, to, b, sizeof(b)))
        return GEMDOS_EFILNF;
    if (!fat_ready(a))
        return GEMDOS_EFILNF;
    size_t len = strlen(a);
    if (!strcmp(a, s_root[slot]) || (!strncasecmp(a, b, len) && b[len] == '/'))
        return GEMDOS_EACCDN;           // the root, or a folder into itself
    sd_close_path(a);
    return fresult_to_gemdos(f_rename(a, b));
}

uint32_t fs_free_clusters(int slot)
{
    int64_t kb = 0;                     // a cluster is 1 KB, see the Dfree command
    if (slot_is_tnfs(slot)) {
        kb = tnfs_free_kb(slot);
    } else if (slot_is_fat(slot) && fat_ready(s_root[slot])) {
        DWORD clusters;
        FATFS *fs;
        char drv[3] = { s_root[slot][0], ':', 0 };
        if (f_getfree(drv, &clusters, &fs) == FR_OK)
            kb = (int64_t)clusters * fs->csize * fs->ssize / 1024;
    }
    return kb < 0 ? 0 : kb > MAX_FREE_KB ? MAX_FREE_KB : kb;
}

int32_t fs_fread(uint16_t handle, uint8_t *buf, uint32_t len)
{
    status_led_activity();
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    if (f->tnfs_slot >= 0) {
        int r = tnfs_sync_pos(f);
        if (r < 0)
            return r;
        int n = tnfs_file_read(f->tnfs_slot, f->tnfs_handle, buf, len);
        if (n > 0) {
            f->pos += n;
            f->wire_pos += n;
        }
        return n;
    }
    if (f->flash) {
        uint32_t left = f->flash->size - f->pos;
        if (len > left)
            len = left;
        memcpy(buf, f->flash->data + f->pos, len);
        f->pos += len;
        return len;
    }
    UINT n = 0;
    FRESULT fr = FR_OK;
    if (f_tell(&f->fil) != f->pos)
        fr = f_lseek(&f->fil, f->pos);
    if (fr == FR_OK)
        fr = f_read(&f->fil, buf, len, &n);
    if (fr != FR_OK)
        return fresult_to_gemdos(fr);
    f->pos += n;
    return n;
}

int16_t fs_fclose(uint16_t handle)
{
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    FRESULT fr = FR_OK;
    if (f->tnfs_slot >= 0)
        tnfs_file_close(f->tnfs_slot, f->tnfs_handle);
    else if (!f->flash)
        fr = sd_close(f);
    f->used = false;
    return fresult_to_gemdos(fr);
}

int32_t fs_fseek(uint16_t handle, int32_t offset, uint16_t mode)
{
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    int64_t pos = offset;
    int64_t size = f->flash ? f->flash->size : f->tnfs_slot >= 0 ? f->size : f_size(&f->fil);
    if (mode == 1)
        pos += f->pos;
    else if (mode == 2)
        pos += size;
    else if (mode != 0)
        return GEMDOS_EINVFN;
    if (pos < 0 || pos > size)
        return GEMDOS_ERANGE;
    f->pos = pos;
    return pos;
}

int32_t fs_fdatime(uint16_t handle, uint16_t *time, uint16_t *date)
{
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    *time = f->time;
    *date = f->date;
    return GEMDOS_EOK;
}

int32_t fs_fdatime_set(uint16_t handle, uint16_t time, uint16_t date)
{
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    if (!f->writable)
        return GEMDOS_EACCDN;
    f->time = time;                     // TNFS cannot set a date: only kept for Fdatime
    f->date = date;
    f->date_set = f->tnfs_slot < 0;
    return GEMDOS_EOK;
}

int32_t fs_fattrib(int slot, const char *path, uint16_t flag, uint16_t attr)
{
    char fat[MAX_PATH];
    if (flag && slot_is_tnfs(slot))
        return attr;                    // TNFS has no attributes: accepted
    if (flag && (!slot_is_fat(slot) || builtin(slot, path)))
        return GEMDOS_EACCDN;
    if (flag) {
        if (!to_fat(slot, path, fat, sizeof(fat)) || !fat_ready(fat))
            return GEMDOS_EFILNF;
        if (!strcmp(fat, s_root[slot]))
            return GEMDOS_EACCDN;
        FRESULT fr = f_chmod(fat, attr & SD_ATTR_MASK, SD_ATTR_MASK);
        return fr == FR_OK ? attr : fresult_to_gemdos(fr);
    }
    if (builtin(slot, path))
        return AM_RDO;
    if (slot_is_tnfs(slot)) {
        uint32_t size, mtime;
        bool is_dir;
        if (!to_tnfs(slot, path, fat, sizeof(fat)))
            return GEMDOS_EFILNF;
        int r = tnfs_stat(slot, fat, &size, &mtime, &is_dir);
        if (r < 0)
            return r == GEMDOS_EPTHNF ? GEMDOS_EFILNF : r;
        return is_dir ? FA_ST_DIR : 0;
    }
    if (!to_fat(slot, path, fat, sizeof(fat)))
        return GEMDOS_EFILNF;
    if (!strcmp(fat, s_root[slot]))
        return FA_ST_DIR;
    if (!fat_ready(fat))
        return GEMDOS_EFILNF;
    FILINFO fno;
    FRESULT fr = f_stat(fat, &fno);
    return fr == FR_OK ? fno.fattrib : fresult_to_gemdos(fr);
}

// --- Current folder -----------------------------------------------------------------

int16_t fs_dsetpath(int slot, const char *path)
{
    char fat[MAX_PATH];
    if (slot_is_tnfs(slot)) {
        uint32_t size, mtime;
        bool is_dir = false;
        if (!to_fat(slot, path, fat, sizeof(fat)))
            return GEMDOS_EPTHNF;
        // The current folder stays in capitals, like the Atari asks for it
        char utf8[MAX_PATH], real[MAX_PATH];
        atari_to_utf8(fat, utf8, sizeof(utf8));
        tnfs_real_path(slot, utf8, real, sizeof(real));
        if (strcmp(fat, "/") && (tnfs_stat(slot, real, &size, &mtime, &is_dir) < 0 || !is_dir))
            return GEMDOS_EPTHNF;
        if (!strcmp(fat, "/"))
            fat[0] = 0;
    } else if (!to_fat(slot, path, fat, sizeof(fat))) {
        return GEMDOS_EPTHNF;
    } else if (strcmp(fat, s_root[slot])) {
        if (!fat_ready(fat))
            return GEMDOS_EPTHNF;
        FILINFO fno;
        if (f_stat(fat, &fno) != FR_OK || !(fno.fattrib & AM_DIR))
            return GEMDOS_EPTHNF;
    }
    // Back to Atari form: "0:/HD/A/B" -> "\A\B"
    const char *rel = fat + strlen(s_root[slot]);
    size_t i = 0;
    for (; rel[i] && i + 1 < MAX_PATH; i++)
        s_cwds[slot][i] = rel[i] == '/' ? '\\' : rel[i];
    s_cwds[slot][i] = 0;
    return GEMDOS_EOK;
}

const char *fs_dgetpath(int slot)
{
    return slot < 0 || slot >= FS_MAX_SLOTS ? "" : s_cwds[slot];
}
