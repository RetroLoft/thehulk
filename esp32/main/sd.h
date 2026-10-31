// microSD card (push-push socket, SPI on GPIO4-7, card detect on GPIO41), FAT via VFS
#pragma once

#include <stdbool.h>

#define SD_ROOT "/sd"

// Starts a task that mounts the card (never formats it). Not on the boot path.
void sd_start(void);
bool sd_ready(void);
