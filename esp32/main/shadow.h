// Shadow of the 128 KB cartridge space (SPRAM), in the layout of the SideTNFS Pico's
// ROM window: 16-bit words in CPU (little-endian) order, so the GEMDRIVE code ported
// from SideTNFS-Firmware gemdrvemul.c can access it with the same pointer macros.
// Offset 0x00000 = 0xFA0000 (/ROM4), 0x10000 = 0xFB0000 (/ROM3).
#pragma once

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
// Writes every 512-byte block that changed since the last sync. The block holding
// the token (0xFB0000) goes last, its first 4 bytes (the token itself) at the very end.
esp_err_t shadow_sync(void);
// Compares the SPRAM with the shadow, returns the number of differing bytes.
int shadow_verify(void);
