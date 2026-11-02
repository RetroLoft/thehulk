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
#define GEMDRVEMUL_RTC_DATETIME_BCD                 0x00030
#define GEMDRVEMUL_RTC_DATETIME_MSDOS               0x00038
#define GEMDRVEMUL_RTC_Y2K_PATCH                    0x00040
#define GEMDRVEMUL_DEFAULT_PATH                     0x00044     // 128 bytes
#define GEMDRVEMUL_DTA_F_FOUND                      0x000c4
#define GEMDRVEMUL_DTA_TRANSFER                     0x000c8     // DTA_SIZE_ON_ST bytes
#define GEMDRVEMUL_DTA_EXIST                        0x000f4
#define GEMDRVEMUL_DTA_RELEASE                      0x000f8
#define GEMDRVEMUL_SET_DPATH_STATUS                 0x000fc
#define GEMDRVEMUL_FOPEN_HANDLE                     0x00100
#define GEMDRVEMUL_READ_BYTES                       0x00104
#define GEMDRVEMUL_READ_BUFF                        0x00108     // 16 KB
#define GEMDRVEMUL_READ_BUFF_SIZE                   16384       // BUFFER_READ_SIZE in gemdrive.s
#define GEMDRVEMUL_WRITE_BUFF_SIZE                  2048        // BUFFER_WRITE_SIZE in gemdrive.s
#define GEMDRVEMUL_WRITE_BYTES                      0x04108
#define GEMDRVEMUL_WRITE_CHK                        0x0410c
#define GEMDRVEMUL_WRITE_CONFIRM_STATUS             0x04110
#define GEMDRVEMUL_FCLOSE_STATUS                    0x04114
#define GEMDRVEMUL_DCREATE_STATUS                   0x04118
#define GEMDRVEMUL_DDELETE_STATUS                   0x0411c
#define GEMDRVEMUL_EXEC_HEADER                      0x04120
#define GEMDRVEMUL_FCREATE_HANDLE                   0x04140
#define GEMDRVEMUL_FDELETE_STATUS                   0x04144
#define GEMDRVEMUL_FSEEK_STATUS                     0x04148
#define GEMDRVEMUL_FATTRIB_STATUS                   0x0414c
#define GEMDRVEMUL_FRENAME_STATUS                   0x04150
#define GEMDRVEMUL_FDATETIME_DATE                   0x04154
#define GEMDRVEMUL_FDATETIME_TIME                   0x04158
#define GEMDRVEMUL_FDATETIME_STATUS                 0x0415c
#define GEMDRVEMUL_DFREE_STATUS                     0x04160
#define GEMDRVEMUL_DFREE_STRUCT                     0x04164
#define GEMDRVEMUL_PEXEC_MODE                       0x04184
#define GEMDRVEMUL_PEXEC_STACK_ADDR                 0x04188
#define GEMDRVEMUL_PEXEC_FNAME                      0x0418c
#define GEMDRVEMUL_PEXEC_CMDLINE                    0x04190
#define GEMDRVEMUL_PEXEC_ENVSTR                     0x04194
#define GEMDRVEMUL_SHARED_VARIABLES                 0x04198
#define GEMDRVEMUL_EXEC_PD                          0x04298
#define GEMDRVEMUL_FLOPPY_SESSION_STATUS            0x097d0
#define GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE  0x097d8
#define GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY    0x097da
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_BPB       0x09bf0
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_RW        0x09bf4
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_MEDIACH   0x09bf8
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_XBIOS_VECTOR  0x09bfc

// The configuration program (atari/config/src/sidetnfs_probe.c)
#define GEMDRVEMUL_CONFIG_VERSION                   0x04398
#define GEMDRVEMUL_CONFIG_MAX_DRIVES                0x0439c
#define GEMDRVEMUL_CONFIG_DRIVE_COUNT               0x043a0
#define GEMDRVEMUL_CONFIG_DRIVE_LETTER              0x043a4
#define GEMDRVEMUL_CONFIG_STATUS                    0x043a8
#define GEMDRVEMUL_CONFIG_DRV_STATUS                0x043ac
#define GEMDRVEMUL_CONFIG_DRV_STATE                 0x043b0
#define GEMDRVEMUL_CONFIG_DRV_LETTER                0x043b2
#define GEMDRVEMUL_CONFIG_DRV_TYPE                  0x043b4
#define GEMDRVEMUL_CONFIG_DRV_TRANSPORT             0x043b6
#define GEMDRVEMUL_CONFIG_DRV_PORT                  0x043b8
#define GEMDRVEMUL_CONFIG_DRV_NICKNAME              0x043ba
#define GEMDRVEMUL_CONFIG_DRV_HOST                  0x043d2
#define GEMDRVEMUL_CONFIG_DRV_MOUNT                 0x04412
#define GEMDRVEMUL_CONFIG_DRV_SD_PATH               0x04432
#define GEMDRVEMUL_CONFIG_NET_STATUS                0x04474
#define GEMDRVEMUL_CONFIG_NET_AUTH                  0x04478
#define GEMDRVEMUL_CONFIG_NET_DHCP                  0x0447a
#define GEMDRVEMUL_CONFIG_NET_SSID                  0x0447c
#define GEMDRVEMUL_CONFIG_NET_PASSWORD              0x044a0
#define GEMDRVEMUL_CONFIG_NET_COUNTRY               0x044e4
#define GEMDRVEMUL_CONFIG_NET_IP                    0x044e8
#define GEMDRVEMUL_CONFIG_NET_NETMASK               0x044f8
#define GEMDRVEMUL_CONFIG_NET_GATEWAY               0x04508
#define GEMDRVEMUL_CONFIG_NET_DNS                   0x04518
#define GEMDRVEMUL_CONFIG_RTC_STATUS                0x04528
#define GEMDRVEMUL_CONFIG_RTC_ENABLED               0x0452c
#define GEMDRVEMUL_CONFIG_RTC_NTP                   0x0452e
#define GEMDRVEMUL_CONFIG_RTC_UTC                   0x0456e
#define CONFIG_PROTOCOL_VERSION  3

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
#define SHARED_VARIABLE_SET_CURRENT_DRIVE       (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 16)   // TheHulk
#define SHARED_VARIABLE_DUMP_TOS                (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 17)   // TheHulk
#define SHARED_VARIABLE_SYNC_TIMEOUT            (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 18)   // TheHulk
#define SHARED_VARIABLE_NO_INSTALL              (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 19)   // TheHulk
#define SHARED_VARIABLE_CODE_BUFFER             (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 20)   // TheHulk
#define SHARED_VARIABLE_TRACE_BUFFER            (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 21)   // TheHulk
#define SHARED_VARIABLE_HOOK_STACK              (SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 22)   // TheHulk

#define DTA_SIZE_ON_ST                          44

// GEMDOS error codes
#define GEMDOS_EOK      0
#define GEMDOS_ERROR    -1
#define GEMDOS_EFILNF   -33     // file not found
#define GEMDOS_EPTHNF   -34     // path not found
#define GEMDOS_EACCDN   -36     // access denied
#define GEMDOS_EIHNDL   -37     // invalid handle
#define GEMDOS_ENHNDL   -35     // no handles left
#define GEMDOS_EINVFN   -32     // invalid function
#define GEMDOS_ENSAME   -48     // not the same drive
#define GEMDOS_ENMFIL   -49     // no more files

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
#define GEMDRVEMUL_DGETDRV_CALL             0x0419
#define GEMDRVEMUL_FSETDTA_CALL             0x041a
#define GEMDRVEMUL_FLOPPY_SAVE_VECTORS      0x042c
#define GEMDRVEMUL_FLOPPY_SAVE_XBIOS_VECTOR 0x042d
#define GEMDRVEMUL_DFREE_CALL               0x0436
#define GEMDRVEMUL_DCREATE_CALL             0x0439
#define GEMDRVEMUL_DDELETE_CALL             0x043a
#define GEMDRVEMUL_DSETPATH_CALL            0x043b
#define GEMDRVEMUL_FCREATE_CALL             0x043c
#define GEMDRVEMUL_FOPEN_CALL               0x043d
#define GEMDRVEMUL_FCLOSE_CALL              0x043e
#define GEMDRVEMUL_FDELETE_CALL             0x0441
#define GEMDRVEMUL_FSEEK_CALL               0x0442
#define GEMDRVEMUL_FATTRIB_CALL             0x0443
#define GEMDRVEMUL_DGETPATH_CALL            0x0447
#define GEMDRVEMUL_MALLOC_CALL              0x0448
#define GEMDRVEMUL_PEXEC_CALL               0x044b
#define GEMDRVEMUL_FSFIRST_CALL             0x044e
#define GEMDRVEMUL_FSNEXT_CALL              0x044f
#define GEMDRVEMUL_FRENAME_CALL             0x0456
#define GEMDRVEMUL_FDATETIME_CALL           0x0457
#define GEMDRVEMUL_CONFIG_GET_INFO          0x040d
#define GEMDRVEMUL_CONFIG_GET_DRIVE         0x040e
#define GEMDRVEMUL_CONFIG_SET_DRIVE         0x040f
#define GEMDRVEMUL_CONFIG_DELETE_DRIVE      0x0410
#define GEMDRVEMUL_CONFIG_SET_CONFIG_DRIVE  0x0411
#define GEMDRVEMUL_CONFIG_SAVE              0x0412
#define GEMDRVEMUL_CONFIG_GET_NETWORK       0x0413
#define GEMDRVEMUL_CONFIG_SET_NETWORK       0x0414
#define GEMDRVEMUL_CONFIG_SAVE_NETWORK      0x0415
#define GEMDRVEMUL_CONFIG_GET_RTC           0x0416
#define GEMDRVEMUL_CONFIG_SET_RTC           0x0417
#define GEMDRVEMUL_CONFIG_SAVE_RTC          0x0418
#define GEMDRVEMUL_CONFIG_RESTART           0x041b
#define GEMDRVEMUL_CONFIG_CHECK_UPDATE      0x041c
#define GEMDRVEMUL_READ_BUFF_CALL           0x0481
#define GEMDRVEMUL_DEBUG                    0x0482
#define GEMDRVEMUL_SAVE_BASEPAGE            0x0483
#define GEMDRVEMUL_SAVE_EXEC_HEADER         0x0484
#define GEMDRVEMUL_SET_SHARED_VAR           0x0487
#define GEMDRVEMUL_WRITE_BUFF_CALL          0x0488
#define GEMDRVEMUL_WRITE_BUFF_CHECK         0x0489
#define GEMDRVEMUL_DTA_EXIST_CALL           0x048a
#define GEMDRVEMUL_DTA_RELEASE_CALL         0x048b

// Pico-style accessors (romemul/include/memfunc.h)
#define SWAP_LONGWORD(data) ((((uint32_t)(data) << 16) & 0xFFFF0000) | (((uint32_t)(data) >> 16) & 0xFFFF))
// A long word for the Atari, with its words swapped. A function, so that an argument with a
// side effect (a call that writes) runs once: SWAP_LONGWORD uses its argument twice.
static inline void write_and_swap_longword(uintptr_t address, uint32_t offset, uint32_t data)
{
    *((volatile uint32_t *)(address + offset)) = SWAP_LONGWORD(data);
}
#define WRITE_AND_SWAP_LONGWORD(address, offset, data) \
    write_and_swap_longword((uintptr_t)(address), (offset), (uint32_t)(data))
#define WRITE_WORD(address, offset, data) *((volatile uint16_t *)((address) + (offset))) = (data)
