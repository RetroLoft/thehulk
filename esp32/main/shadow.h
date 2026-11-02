// Shadow of the 128 KB cartridge space (SPRAM), in the layout of the SideTNFS Pico's
// ROM window: 16-bit words in CPU (little-endian) order, so the GEMDRIVE code ported
// from SideTNFS-Firmware gemdrvemul.c can access it with the same pointer macros.
// Offset 0x00000 = 0xFA0000 (/ROM4), 0x10000 = 0xFB0000 (/ROM3).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SHADOW_ROM3      0x10000

esp_err_t shadow_init(void);
// Base address for the Pico-style macros: shadow_base() + offset
uintptr_t shadow_base(void);
// Copies the cartridge image (big-endian, as the Atari reads it) to the start.
void shadow_load_image(const uint8_t *img, size_t len);
// Writes [offset, offset + len) to the SPRAM without comparing (boot path).
esp_err_t shadow_write_range(uint32_t offset, size_t len);
// Writes what changed to the SPRAM: the first block of ROM3 and the status area, and the
// ranges given to shadow_mark. The token (0xFB0000) goes last. Everything a command writes
// outside those two areas must be marked.
void shadow_mark(uint32_t offset, size_t len);
esp_err_t shadow_sync(void);
// Diagnostics: true when the SPRAM holds the whole shadow
bool shadow_is_synced(void);
// Compares the SPRAM with the shadow, returns the number of differing bytes.
int shadow_verify(void);
