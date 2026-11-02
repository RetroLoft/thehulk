// GEMDRIVE storage: the GEMDOS calls for the cartridge drives, by drive slot. A slot is a
// folder on the microSD card, a TNFS share, or the flash drive with the configuration program.
// The card and the flash drive are read-only: everything that would change them returns
// EACCDN. A TNFS drive can be written.
// Paths are Atari paths ("C:\DIR\FILE.PRG", or relative to the current folder);
// results are GEMDOS error codes (negative) or values.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FS_DTA_SIZE  44
#define FS_MAX_SLOTS 9

void fs_set_sd_slot(int slot, const char *sd_path);
void fs_set_tnfs_slot(int slot, const char *host, uint16_t port, const char *mount);
void fs_set_flash_slot(int slot);
void fs_reset(void);

// The DTA is filled in the Atari's byte order (dta[0] is the byte at the DTA address)
int16_t fs_fsfirst(int slot, uint32_t dta, const char *path, uint16_t attribs, uint8_t *dta_out);
int16_t fs_fsnext(uint32_t dta, uint8_t *dta_out);
bool fs_dta_exists(uint32_t dta);
bool fs_dta_release(uint32_t dta);

int32_t fs_fopen(int slot, const char *path, uint16_t mode);
int32_t fs_fread(uint16_t handle, uint8_t *buf, uint32_t len);
int32_t fs_fcreate(int slot, const char *path, uint16_t attr);
// Writes at the file position; fs_fwrite_commit moves it on, after the checksum is confirmed
int32_t fs_fwrite(uint16_t handle, const uint8_t *buf, uint32_t len);
int16_t fs_fwrite_commit(uint16_t handle, uint32_t len);
bool fs_writable(uint16_t handle);
int16_t fs_fdelete(int slot, const char *path);
int16_t fs_dcreate(int slot, const char *path);
int16_t fs_ddelete(int slot, const char *path);
int32_t fs_frename(int slot, int slot_to, const char *from, const char *to);
uint32_t fs_free_clusters(int slot);
int16_t fs_fclose(uint16_t handle);
int32_t fs_fseek(uint16_t handle, int32_t offset, uint16_t mode);
int32_t fs_fdatime(uint16_t handle, uint16_t *time, uint16_t *date);
int32_t fs_fattrib(int slot, const char *path, uint16_t flag, uint16_t attr);

int16_t fs_dsetpath(int slot, const char *path);
const char *fs_dgetpath(int slot);
