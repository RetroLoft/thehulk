// GEMDRIVE emulation: answers the commands of atari/gemdrive, ported from SideTNFS-Firmware
// romemul/gemdrvemul.c. Works on the shadow (shadow.h); the caller syncs it afterwards.
#pragma once

#include <stdbool.h>

#include "hulk.h"

// Sets the shared memory to its power-on state (statuses, shared variables, drive table).
void gemdrv_init(void);
// Handles one command packet and writes the answer token.
void gemdrv_command(const hulk_packet_t *pkt);
// Hands the state of the network and the clock to the shared memory; true when it changed
bool gemdrv_poll(void);
// This boot the Atari installed the floppy hooks: another image needs no reset
bool gemdrv_floppy_installed(void);
// The configuration program asked for a restart, to apply the settings
bool gemdrv_restart_wanted(void);
