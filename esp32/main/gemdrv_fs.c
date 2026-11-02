// GEMDRIVE storage, see gemdrv_fs.h. The SD drive uses FatFs directly (not the VFS): it
// gives the DOS date/time and the FAT attributes, which are the GEMDOS ones. The flash
// drive is a fixed list of files in the firmware.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ff.h"
#include "gemdrv_fs.h"
#include "gemdrv_layout.h"
#include "sd.h"

#define MAX_PATH    256
#define MAX_FILES   16
#define MAX_SEARCH  16

#define GEMDOS_ERANGE  -64

#define FA_ST_HIDDEN  0x02
#define FA_ST_SYSTEM  0x04
#define FA_ST_VOLUME  0x08
#define FA_ST_DIR     0x10

// Per drive slot: the folder on the card (FatFs drive 0 = the card mounted by sd.c) and the
// current folder, Atari form: "" or "\DIR\SUB"
static char s_root[FS_MAX_SLOTS][MAX_PATH];
static char s_cwds[FS_MAX_SLOTS][MAX_PATH];
static int s_flash_slot = -1;

// The flash drive: one folder, read-only
extern const uint8_t thehulk_prg_start[] asm("_binary_THEHULK_PRG_start");
extern const uint8_t thehulk_prg_end[]   asm("_binary_THEHULK_PRG_end");
#define FLASH_TIME  (12 << 11)
#define FLASH_DATE  (((2026 - 1980) << 9) | (11 << 5) | 2)

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
} flash_file_t;
static flash_file_t s_flash[1];

static void flash_files_init(void)
{
    s_flash[0] = (flash_file_t){ "THEHULK.PRG", thehulk_prg_start,
                                 (uint32_t)(thehulk_prg_end - thehulk_prg_start) };
}

typedef struct {
    bool used;
    FIL fil;
    const flash_file_t *flash;      // on the flash drive: no FIL
    uint32_t pos;
    uint16_t time, date;
} file_t;
static file_t *s_files;             // handle = FIRST_FILE_DESCRIPTOR + index

typedef struct {
    uint32_t dta;                   // 0: free
    bool open;                      // directory still open (not at its end)
    bool flash;                     // on the flash drive: no FF_DIR
    int flash_next;
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
    size_t n = snprintf(s_root[slot], MAX_PATH, "0:%s%s", sd_path[0] == '/' || sd_path[0] == '\\' ? "" : "/", sd_path);
    for (size_t i = 0; i < n && i < MAX_PATH; i++)
        if (s_root[slot][i] == '\\')
            s_root[slot][i] = '/';
    while (n > 2 && s_root[slot][n - 1] == '/')
        s_root[slot][--n] = 0;
}

void fs_set_flash_slot(int slot)
{
    s_flash_slot = slot;
}

void fs_reset(void)
{
    memset(s_cwds, 0, sizeof(s_cwds));
    flash_files_init();
    if (!s_files)
        s_files = calloc(MAX_FILES, sizeof(file_t));
    for (int i = 0; s_files && i < MAX_FILES; i++) {
        if (s_files[i].used && !s_files[i].flash)
            f_close(&s_files[i].fil);
        s_files[i].used = false;
    }
    for (int i = 0; i < MAX_SEARCH; i++) {
        if (s_search[i].open && !s_search[i].flash)
            f_closedir(&s_search[i].dir);
        s_search[i].dta = 0;
        s_search[i].open = false;
    }
}

// --- Paths --------------------------------------------------------------------------

// Atari path -> FatFs path below the folder of the slot. Handles the drive letter, the current folder,
// "." and "..". Returns false when the path would leave the root.
static bool to_fat(int slot, const char *apath, char *out, size_t size)
{
    if (slot < 0 || slot >= FS_MAX_SLOTS || !s_root[slot][0])
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
    return len < size;
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
    default:                return GEMDOS_ERROR;
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
    if (s->open && !s->flash)
        f_closedir(&s->dir);
    memset(s, 0, sizeof(*s));
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

static int16_t next_match(search_t *s, uint8_t *dta_out)
{
    FILINFO fno;
    while (s->open && s->flash) {
        if (s->flash_next >= (int)(sizeof(s_flash) / sizeof(s_flash[0]))) {
            s->open = false;
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
    while (s->open && !s->flash) {
        if (f_readdir(&s->dir, &fno) != FR_OK || !fno.fname[0]) {
            f_closedir(&s->dir);
            s->open = false;
            break;
        }
        if (match(s, fno.fname) && attribs_ok(s->attribs, fno.fattrib)) {
            fill_dta(dta_out, &fno);
            return GEMDOS_EOK;
        }
    }
    memset(dta_out, 0, FS_DTA_SIZE);
    return GEMDOS_ENMFIL;
}

// The flash drive has one folder: true for "", "\\", "S:" and "S:\\"
static bool flash_is_root(const char *apath)
{
    if (isalpha((unsigned char)apath[0]) && apath[1] == ':')
        apath += 2;
    return !apath[0] || (apath[0] == '\\' && !apath[1]) || !strcmp(apath, ".");
}

// The file name in a flash drive path, NULL when it is not in the root
static const flash_file_t *flash_find(const char *apath)
{
    if (isalpha((unsigned char)apath[0]) && apath[1] == ':')
        apath += 2;
    if (*apath == '\\')
        apath++;
    for (size_t i = 0; i < sizeof(s_flash) / sizeof(s_flash[0]); i++)
        if (!strcasecmp(apath, s_flash[i].name))
            return &s_flash[i];
    return NULL;
}

int16_t fs_fsfirst(int slot, uint32_t dta, const char *path, uint16_t attribs, uint8_t *dta_out)
{
    char adir[MAX_PATH], fat[MAX_PATH];
    const char *pattern;
    memset(dta_out, 0, FS_DTA_SIZE);
    split_pattern(path, adir, sizeof(adir), &pattern);
    if (slot == s_flash_slot) {
        if (!flash_is_root(adir))
            return GEMDOS_EPTHNF;
        search_t *s = new_search(dta);
        s->attribs = attribs;
        s->flash = true;
        s->open = true;
        make_pattern(pattern, s->name, s->ext);
        int16_t r = next_match(s, dta_out);
        return r == GEMDOS_ENMFIL ? GEMDOS_EFILNF : r;
    }
    if (!sd_ready() || !to_fat(slot, adir[0] ? adir : ".", fat, sizeof(fat)))
        return GEMDOS_EPTHNF;

    search_t *s = new_search(dta);
    s->attribs = attribs;
    make_pattern(pattern, s->name, s->ext);
    FRESULT fr = f_opendir(&s->dir, fat);
    if (fr != FR_OK)
        return fr == FR_NO_PATH || fr == FR_NO_FILE ? GEMDOS_EPTHNF : fresult_to_gemdos(fr);
    s->open = true;
    int16_t r = next_match(s, dta_out);
    return r == GEMDOS_ENMFIL ? GEMDOS_EFILNF : r;
}

int16_t fs_fsnext(uint32_t dta, uint8_t *dta_out)
{
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
    if (s->open && !s->flash)
        f_closedir(&s->dir);
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

int32_t fs_fopen(int slot, const char *path, uint16_t mode)
{
    char fat[MAX_PATH];
    if ((mode & 3) != 0)
        return GEMDOS_EACCDN;           // read-only for now
    if (slot == s_flash_slot) {
        const flash_file_t *ff = flash_find(path);
        if (!ff || !s_files)
            return GEMDOS_EFILNF;
        for (int i = 0; i < MAX_FILES; i++) {
            if (s_files[i].used)
                continue;
            s_files[i].used = true;
            s_files[i].flash = ff;
            s_files[i].pos = 0;
            s_files[i].time = FLASH_TIME;
            s_files[i].date = FLASH_DATE;
            return FIRST_FILE_DESCRIPTOR + i;
        }
        return GEMDOS_ENHNDL;
    }
    if (!sd_ready() || !s_files || !to_fat(slot, path, fat, sizeof(fat)))
        return GEMDOS_EFILNF;
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i].used)
            continue;
        FILINFO fno;
        FRESULT fr = f_stat(fat, &fno);
        if (fr == FR_OK && (fno.fattrib & AM_DIR))
            return GEMDOS_EFILNF;
        if (fr == FR_OK)
            fr = f_open(&s_files[i].fil, fat, FA_READ);
        if (fr != FR_OK)
            return fresult_to_gemdos(fr);
        s_files[i].used = true;
        s_files[i].flash = NULL;
        s_files[i].time = fno.ftime;
        s_files[i].date = fno.fdate;
        return FIRST_FILE_DESCRIPTOR + i;
    }
    return GEMDOS_ENHNDL;
}

int32_t fs_fread(uint16_t handle, uint8_t *buf, uint32_t len)
{
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    if (f->flash) {
        uint32_t left = f->flash->size - f->pos;
        if (len > left)
            len = left;
        memcpy(buf, f->flash->data + f->pos, len);
        f->pos += len;
        return len;
    }
    UINT n = 0;
    FRESULT fr = f_read(&f->fil, buf, len, &n);
    return fr == FR_OK ? (int32_t)n : fresult_to_gemdos(fr);
}

int16_t fs_fclose(uint16_t handle)
{
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    if (!f->flash)
        f_close(&f->fil);
    f->used = false;
    return GEMDOS_EOK;
}

int32_t fs_fseek(uint16_t handle, int32_t offset, uint16_t mode)
{
    file_t *f = get_file(handle);
    if (!f)
        return GEMDOS_EIHNDL;
    int64_t pos = offset;
    int64_t size = f->flash ? f->flash->size : f_size(&f->fil);
    if (mode == 1)
        pos += f->flash ? f->pos : f_tell(&f->fil);
    else if (mode == 2)
        pos += size;
    else if (mode != 0)
        return GEMDOS_EINVFN;
    if (pos < 0 || pos > size)
        return GEMDOS_ERANGE;
    if (f->flash) {
        f->pos = pos;
        return pos;
    }
    FRESULT fr = f_lseek(&f->fil, pos);
    return fr == FR_OK ? (int32_t)pos : fresult_to_gemdos(fr);
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

int32_t fs_fattrib(int slot, const char *path, uint16_t flag, uint16_t attr)
{
    char fat[MAX_PATH];
    if (flag)
        return GEMDOS_EACCDN;           // setting attributes: read-only for now
    if (slot == s_flash_slot) {
        if (flash_is_root(path))
            return FA_ST_DIR;
        return flash_find(path) ? AM_RDO : GEMDOS_EFILNF;
    }
    if (!sd_ready() || !to_fat(slot, path, fat, sizeof(fat)))
        return GEMDOS_EFILNF;
    if (!strcmp(fat, s_root[slot]))
        return FA_ST_DIR;
    FILINFO fno;
    FRESULT fr = f_stat(fat, &fno);
    return fr == FR_OK ? fno.fattrib : fresult_to_gemdos(fr);
}

// --- Current folder -----------------------------------------------------------------

int16_t fs_dsetpath(int slot, const char *path)
{
    char fat[MAX_PATH];
    if (slot == s_flash_slot)
        return flash_is_root(path) ? GEMDOS_EOK : GEMDOS_EPTHNF;
    if (!sd_ready() || !to_fat(slot, path, fat, sizeof(fat)))
        return GEMDOS_EPTHNF;
    if (strcmp(fat, s_root[slot])) {
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
    return slot == s_flash_slot || slot < 0 || slot >= FS_MAX_SLOTS ? "" : s_cwds[slot];
}
