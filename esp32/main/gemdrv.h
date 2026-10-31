// GEMDRIVE emulation: answers the commands of atari/gemdrive, ported from SideTNFS-Firmware
// romemul/gemdrvemul.c. Works on the shadow (shadow.h); the caller syncs it afterwards.
#pragma once

#include "hulk.h"

// Sets the shared memory to its power-on state (statuses, shared variables, drive table).
void gemdrv_init(void);
// Handles one command packet and writes the answer token.
void gemdrv_command(const hulk_packet_t *pkt);
