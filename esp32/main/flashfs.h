// The flash drive: a FAT volume with wear levelling in the "hulkfs" partition, FatFs
// drive "1:" (the card is "0:"). Mounted when first needed, formatted when empty.
#pragma once

#include <stdbool.h>

#define FLASHFS_DRIVE "1:"

// Mounts the volume (once): false when there is no partition or it cannot be used
bool flashfs_ready(void);
