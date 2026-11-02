// A TNFS client over UDP: read-only, one session per drive slot (protocol as in acsi2tnfs).
// Results are 0 or a value, or a negative GEMDOS error code.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TNFS_MAX_SLOTS 9

typedef struct {
    char name[64];
    bool is_dir;
    uint32_t size;
    uint32_t mtime;     // Unix time
} tnfs_entry_t;

void tnfs_set_slot(int slot, const char *host, uint16_t port, const char *mount);

int tnfs_stat(int slot, const char *path, uint32_t *size, uint32_t *mtime, bool *is_dir);

int tnfs_dir_open(int slot, const char *path, uint8_t *handle);
// 1: *e filled, 0: end of the directory, negative: error
int tnfs_dir_next(int slot, uint8_t handle, tnfs_entry_t *e);
void tnfs_dir_close(int slot, uint8_t handle);

int tnfs_file_open(int slot, const char *path, uint8_t *handle);
int tnfs_file_read(int slot, uint8_t handle, uint8_t *buf, uint32_t len);   // bytes read
int tnfs_file_seek(int slot, uint8_t handle, uint32_t pos);
void tnfs_file_close(int slot, uint8_t handle);
