// The cartridge emulator (docs/CARTRIDGE.md): instead of GEMDRIVE the Atari sees a cartridge
// image, a game or a tool. The images are in \CARTS on the SD card; the web page adds,
// renames and deletes them there. The chosen one is copied to TheHulk's own flash, so it is
// there in time at power-on (the card is too slow for that, TNFS needs Wi-Fi), normalised
// to 128 KB: an .STC loses its 4-byte header, a smaller image is filled with $FF.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CART_SIZE      0x20000          // what the Atari sees: $FA0000-$FBFFFF
#define CART_STC_SIZE  (CART_SIZE + 4)  // Steem's .STC: 4 bytes before the image
#define CART_NAME_LEN  13               // 8.3

typedef enum {
    CART_BAD,           // not a cartridge image (size)
    CART_NORMAL,        // $ABCDEF42: TOS starts it late in the boot
    CART_DIAG,          // $FA52235F: TOS jumps to it at once after a reset
    CART_UNKNOWN,       // a size that fits, no known header
} cart_kind_t;

typedef struct {
    char name[CART_NAME_LEN];
    uint32_t size;
    cart_kind_t kind;
} cart_file_t;

// What an image of 'size' bytes starting with 'head' (8 bytes or more) is
cart_kind_t cart_kind(const uint8_t *head, size_t head_len, uint32_t size);
const char *cart_kind_name(cart_kind_t kind);

// An 8.3 name with a known extension (.STC .IMG .BIN .ROM .CAR), made upper case in place
bool cart_name_ok(char *name);

// The images in \CARTS; number found, -1 without a card
int cart_list(cart_file_t *out, int max);
// 0 or a negative error (CART_E_*)
int cart_delete(const char *name);
int cart_rename(const char *from, const char *to);
// Writing an image (upload, download): open, write in parts, close (keep or discard)
int cart_create(const char *name, uint32_t size);
int cart_write(const void *data, size_t len);
int cart_close(bool keep);

// Chooses an image: copies it to the flash and switches the emulator on (NVS). NULL: back to
// GEMDRIVE. cart_apply_now puts it in the cartridge.
int cart_select(const char *name);
// The chosen contents into the cartridge at once (main.c), with the Atari on: its next reset
// starts them. With the Atari off they are loaded before it starts.
void cart_apply_now(void);
bool cart_change_pending(void);
// Chosen after the contents were last loaded: main.c loads them again before the Atari starts
bool cart_reload_pending(void);
// main.c: true once after cart_apply_now
bool cart_take_change(void);
// cart_select + cart_apply_now in the background (the web server must keep answering):
// 0 or an error when it cannot start; then _busy until done and _result for its outcome
int cart_select_start(const char *name);
bool cart_select_busy(void);
int cart_select_result(void);
// In the cartridge now: the image, "" for GEMDRIVE
const char *cart_running_name(void);
// The chosen image could not be loaded: GEMDRIVE runs instead
bool cart_load_failed(void);
// The emulator from the next power-on, and the image (the name it had)
bool cart_emulator_on(void);
const char *cart_active_name(void);

// Power-on: the chosen image into the shadow; false when there is none (then GEMDRIVE)
bool cart_load_shadow(void);

// The copy on the flash drive: not shown there
#define CART_CACHE_NAME "HULKCART.SYS"
#define CART_TEMP_NAME  "HULKCART.TMP"

#define CART_E_NO_CARD   -1
#define CART_E_NAME      -2
#define CART_E_NOT_FOUND -3
#define CART_E_EXISTS    -4
#define CART_E_SIZE      -5
#define CART_E_IO        -6
#define CART_E_BUSY      -7
const char *cart_error(int e);
