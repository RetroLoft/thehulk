// GEMDRIVE shared memory layout and command codes, from SideTNFS-Firmware
// (romemul/include/gemdrvemul.h and commands.h, commit 54eb402), which matches
// atari/gemdrive. Offsets are relative to 0xFB0000 (ROM3); the values were computed
// from the SideTNFS headers and checked against the addresses in GEMDRIVE.BIN.
#pragma once

#include <stdint.h>

// Shared memory (offsets from ROM3)
#define GEMDRVEMUL_RANDOM_TOKEN                     0x00000
#define GEMDRVEMUL_RANDOM_TOKEN_SEED                0x00004
#define GEMDRVEMUL_TIMEOUT_SEC                      0x00008
#define GEMDRVEMUL_PING_STATUS                      0x0000c
#define GEMDRVEMUL_RTC_STATUS                       0x00010
#define GEMDRVEMUL_NETWORK_STATUS                   0x00018
#define GEMDRVEMUL_RTC_ENABLED                      0x0001c
#define GEMDRVEMUL_REENTRY_TRAP                     0x00024
#define GEMDRVEMUL_OLD_XBIOS_TRAP                   0x00028
#define GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP           0x0002c
#define GEMDRVEMUL_RTC_Y2K_PATCH                    0x00040
#define GEMDRVEMUL_SHARED_VARIABLES                 0x04198
#define GEMDRVEMUL_FLOPPY_SESSION_STATUS            0x097d0
#define GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE  0x097d8
#define GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY    0x097da
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_BPB       0x09bf0
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_RW        0x09bf4
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_MEDIACH   0x09bf8
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_XBIOS_VECTOR  0x09bfc

// Status long words (GEMDRV_STATUS_* in gemdrive.s)
#define GEMDRV_STATUS_BUSY      0x00000000
#define GEMDRV_STATUS_FAILED    0x00000001
#define GEMDRV_STATUS_READY     0xffffffff

// Shared variables (index * 4 from GEMDRVEMUL_SHARED_VARIABLES)
#define SHARED_VARIABLE_HARDWARE_TYPE           0
#define SHARED_VARIABLE_SVERSION                1
#define SHARED_VARIABLE_BUFFER_TYPE             2
#define SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE   16
#define SHARED_VARIABLE_FIRST_FILE_DESCRIPTOR   (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 0)
#define SHARED_VARIABLE_DRIVE_LETTER            (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 1)
#define SHARED_VARIABLE_DRIVE_NUMBER            (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 2)
#define SHARED_VARIABLE_PEXEC_RESTORE           (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 3)
#define SHARED_VARIABLE_FAKE_FLOPPY             (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 4)
#define SHARED_VARIABLE_PROTOCOL_VERSION        (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 5)
#define SHARED_VARIABLE_DRIVE_COUNT             (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 6)
#define SHARED_VARIABLE_DRIVE_NUMBER_TABLE      (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 7)

#define FIRST_FILE_DESCRIPTOR                   16384
#define GEMDOS_SLOT_PROTOCOL_VERSION            1
#define GEMDRIVE_MAX_RUNTIME_DRIVES             9

// Commands (APP_GEMDRVEMUL << 8 | n)
#define GEMDRVEMUL_PING                     0x0400
#define GEMDRVEMUL_SAVE_VECTORS             0x0401
#define GEMDRVEMUL_SHOW_VECTOR_CALL         0x0402
#define GEMDRVEMUL_REENTRY_LOCK             0x0403
#define GEMDRVEMUL_REENTRY_UNLOCK           0x0404
#define GEMDRVEMUL_CANCEL                   0x0405
#define GEMDRVEMUL_SAVE_XBIOS_VECTOR        0x040a
#define GEMDRVEMUL_REENTRY_XBIOS_LOCK       0x040b
#define GEMDRVEMUL_REENTRY_XBIOS_UNLOCK     0x040c
#define GEMDRVEMUL_FLOPPY_SAVE_VECTORS      0x042c
#define GEMDRVEMUL_FLOPPY_SAVE_XBIOS_VECTOR 0x042d
#define GEMDRVEMUL_DEBUG                    0x0482
#define GEMDRVEMUL_SET_SHARED_VAR           0x0487

// Pico-style accessors (romemul/include/memfunc.h)
#define SWAP_LONGWORD(data) ((((uint32_t)(data) << 16) & 0xFFFF0000) | (((uint32_t)(data) >> 16) & 0xFFFF))
#define WRITE_AND_SWAP_LONGWORD(address, offset, data) \
    *((volatile uint32_t *)((address) + (offset))) = SWAP_LONGWORD(data)
#define WRITE_WORD(address, offset, data) *((volatile uint16_t *)((address) + (offset))) = (data)
