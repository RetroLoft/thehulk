// GEMDRIVE storage: the GEMDOS calls for the cartridge drives. Slot 0 is the folder HD of
// the microSD card, slot 1 the flash drive with the configuration program.
// Read-only for now: everything that would change the card returns EACCDN.
// Paths are Atari paths ("C:\DIR\FILE.PRG", or relative to the current folder);
// results are GEMDOS error codes (negative) or values.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FS_DTA_SIZE  44
#define FS_SLOT_SD     0
#define FS_SLOT_FLASH  1

void fs_reset(void);

// The DTA is filled in the Atari's byte order (dta[0] is the byte at the DTA address)
int16_t fs_fsfirst(int slot, uint32_t dta, const char *path, uint16_t attribs, uint8_t *dta_out);
int16_t fs_fsnext(uint32_t dta, uint8_t *dta_out);
bool fs_dta_exists(uint32_t dta);
bool fs_dta_release(uint32_t dta);

int32_t fs_fopen(int slot, const char *path, uint16_t mode);
int32_t fs_fread(uint16_t handle, uint8_t *buf, uint32_t len);
int16_t fs_fclose(uint16_t handle);
int32_t fs_fseek(uint16_t handle, int32_t offset, uint16_t mode);
int32_t fs_fdatime(uint16_t handle, uint16_t *time, uint16_t *date);
int32_t fs_fattrib(int slot, const char *path, uint16_t flag, uint16_t attr);

int16_t fs_dsetpath(int slot, const char *path);
const char *fs_dgetpath(int slot);
