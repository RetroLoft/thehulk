// microSD card (push-push socket, SPI on GPIO4-7, card detect on GPIO41), FAT via VFS
#pragma once

#include <stdbool.h>

#define SD_ROOT "/sd"

// Starts a task that follows the card detect pin and mounts the card (never formats it).
// Not on the boot path.
void sd_start(void);
bool sd_ready(void);
// Held around the use of the card: it is not unmounted meanwhile
void sd_lock(void);
void sd_unlock(void);
