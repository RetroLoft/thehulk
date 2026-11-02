// A TNFS client over UDP, one session per drive slot (protocol as in acsi2tnfs).
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

#define TNFS_O_RDONLY  0x0001
#define TNFS_O_WRONLY  0x0002
#define TNFS_O_RDWR    0x0003
#define TNFS_O_CREAT   0x0100
#define TNFS_O_TRUNC   0x0200

int tnfs_file_open(int slot, const char *path, uint16_t flags, uint8_t *handle);
int tnfs_file_write(int slot, uint8_t handle, const uint8_t *buf, uint32_t len);    // bytes written
int tnfs_file_read(int slot, uint8_t handle, uint8_t *buf, uint32_t len);   // bytes read
int tnfs_file_seek(int slot, uint8_t handle, uint32_t pos);
void tnfs_file_close(int slot, uint8_t handle);

int tnfs_unlink(int slot, const char *path);
int tnfs_mkdir(int slot, const char *path);
int tnfs_rmdir(int slot, const char *path);
int tnfs_rename(int slot, const char *from, const char *to);
// Free space in KB, or a negative GEMDOS error
int64_t tnfs_free_kb(int slot);
