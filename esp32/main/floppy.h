// The floppy emulator (docs/FLOPPY.md): the Atari sees a disk image as drive A: or B:. The
// images are in \FLOPPIES on the SD card, .ST or the compressed .MSA; the web page adds,
// renames and deletes them there. The chosen one is read completely into PSRAM (an .MSA is
// unpacked then), so the card is not used while the Atari reads, and another image can
// be put in while the Atari runs. Read-only, like a write-protected disk.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLOPPY_NAME_LEN   13            // 8.3
#define FLOPPY_MAX_SIZE   0x1C0000      // the largest image: 86 tracks of 2 x 18 sectors, and some

typedef enum {
    FLOPPY_BAD,                         // not a disk image of a size we know
    FLOPPY_ST,
    FLOPPY_MSA,
} floppy_kind_t;

typedef struct {
    char name[FLOPPY_NAME_LEN];
    uint32_t size;
    floppy_kind_t kind;
} floppy_file_t;

// An image in PSRAM: ready for the Atari
typedef struct {
    const uint8_t *data;
    uint32_t sectors;
    uint16_t sides, sectors_per_track, tracks;
    uint16_t bpb[9];                    // as TOS wants it from Getbpb
} floppy_image_t;

floppy_kind_t floppy_kind(const char *name, const uint8_t *head, size_t head_len, uint32_t size);
const char *floppy_kind_name(floppy_kind_t kind);
// An 8.3 name ending in .ST or .MSA, made upper case in place
bool floppy_name_ok(char *name);

// The images in \FLOPPIES; number found, -1 without a card
int floppy_list(floppy_file_t *out, int max);
// 0 or a negative error (FLOPPY_E_*)
int floppy_delete(const char *name);
int floppy_rename(const char *from, const char *to);
// Writing an image (upload): open, write in parts, close (keep or discard)
int floppy_create(const char *name, uint32_t size);
int floppy_write(const void *data, size_t len);
int floppy_close(bool keep);

// Chooses an image, in the background (the web server must keep answering): 0 or an error
// when it cannot start, then _busy until it is done and _result for its outcome. Leaves the
// cartridge emulator. drive: 0 = A:, 1 = B:. keep_drives: GEMDRIVE next to the floppy.
int floppy_select_start(const char *name, int drive, bool keep_drives);
bool floppy_select_busy(void);
int floppy_select_result(void);
// Out of the drive: GEMDRIVE only, from the next reset of the Atari
void floppy_off(void);

bool floppy_emulator_on(void);
const char *floppy_active_name(void);
int floppy_drive(void);
bool floppy_keep_drives(void);

// gemdrv.c, on the task that answers the Atari ---------------------------------------------
// A power-on: reads the chosen image when it is not in PSRAM yet
void floppy_boot(void);
// Up to ms milliseconds for that (the Atari is waiting for its answer)
void floppy_wait_ready(uint32_t ms);
bool floppy_change_pending(void);
// True once after a change: the new image is in use, publish it
bool floppy_take_change(void);
// The image in use, NULL when there is none
const floppy_image_t *floppy_image(void);
// A sector of it, NULL when it does not exist
const uint8_t *floppy_sector(uint32_t lba);

#define FLOPPY_E_NO_CARD   -1
#define FLOPPY_E_NAME      -2
#define FLOPPY_E_NOT_FOUND -3
#define FLOPPY_E_EXISTS    -4
#define FLOPPY_E_SIZE      -5
#define FLOPPY_E_IO        -6
#define FLOPPY_E_BUSY      -7
#define FLOPPY_E_MEMORY    -8
const char *floppy_error(int e);
