// GEMDRIVE emulation, see gemdrv.h. The command handlers follow gemdrvemul.c of
// SideTNFS-Firmware: same shared memory layout, same word order (shadow.h).
#include <ctype.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "gemdrv.h"
#include "gemdrv_layout.h"
#include "shadow.h"

static const char *TAG = "gemdrv";

#define ATARI_ROM4_START  0xFA0000

static uint32_t random_token;

// DTAs that Fsfirst was called with: Fsnext only comes to us for these
#define MAX_DTAS 32
static uint32_t s_dtas[MAX_DTAS];

static uintptr_t shared(void)
{
    return shadow_base() + SHADOW_ROM3;
}

static void set_shared_var(uint32_t index, uint32_t value)
{
    uintptr_t m = shared();
    *((volatile uint16_t *)(m + GEMDRVEMUL_SHARED_VARIABLES + index * 4 + 2)) = value & 0xFFFF;
    *((volatile uint16_t *)(m + GEMDRVEMUL_SHARED_VARIABLES + index * 4)) = value >> 16;
}

static void write_random_token(void)
{
    *((volatile uint32_t *)(shared() + GEMDRVEMUL_RANDOM_TOKEN)) = random_token;
}

// A string from the payload of a send_write_sync command: the buffer follows the
// d3/d4/d5 registers, two characters per word, the first one in the high byte
static void payload_string(const hulk_packet_t *pkt, char *out, size_t size)
{
    const uint16_t *w = pkt->words + 2 + 6;
    size_t words = (pkt->size + 1) / 2;
    size_t n = 0;
    for (size_t i = 0; 2 + 6 + i < words && n + 1 < size; i++) {
        char c[2] = { (char)(w[i] >> 8), (char)(w[i] & 0xff) };
        for (int k = 0; k < 2 && n + 1 < size; k++) {
            if (!c[k]) {
                out[n] = 0;
                return;
            }
            out[n++] = c[k];
        }
    }
    out[n] = 0;
}

// Writes a string the Atari reads byte by byte (bytes swapped within each word)
static void write_string(uint32_t offset, const char *s, size_t size)
{
    uint8_t *p = (uint8_t *)shared();
    for (size_t i = 0; i < size; i++) {
        p[(offset + i) ^ 1] = *s;
        if (*s)
            s++;
    }
}

static bool dta_known(uint32_t dta, bool remove)
{
    for (int i = 0; i < MAX_DTAS; i++) {
        if (s_dtas[i] == dta && dta) {
            if (remove)
                s_dtas[i] = 0;
            return true;
        }
    }
    return false;
}

static void dta_add(uint32_t dta)
{
    if (dta_known(dta, false))
        return;
    for (int i = 0; i < MAX_DTAS; i++) {
        if (!s_dtas[i]) {
            s_dtas[i] = dta;
            return;
        }
    }
    memmove(s_dtas, s_dtas + 1, sizeof(s_dtas) - sizeof(s_dtas[0]));   // full: drop the oldest
    s_dtas[MAX_DTAS - 1] = dta;
}

// The root of the drive: "\", "C:", "C:\" or empty
static bool is_root(const char *path)
{
    if (isalpha((unsigned char)path[0]) && path[1] == ':')
        path += 2;
    return !path[0] || (path[0] == '\\' && !path[1]);
}

void gemdrv_init(void)
{
    uintptr_t m = shared();

    // No network and no clock yet: report them as failed, so the Atari does not
    // wait for them
    *((volatile uint32_t *)(m + GEMDRVEMUL_RTC_STATUS)) = 0;
    *((volatile uint32_t *)(m + GEMDRVEMUL_NETWORK_STATUS)) = GEMDRV_STATUS_FAILED;
    *((volatile uint32_t *)(m + GEMDRVEMUL_RTC_ENABLED)) = 0;
    *((volatile uint16_t *)(m + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0;
    *((volatile uint32_t *)(m + GEMDRVEMUL_RTC_Y2K_PATCH)) = 0;
    *((volatile uint16_t *)(m + GEMDRVEMUL_PING_STATUS)) = 0;
    WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_TIMEOUT_SEC, 45);

    // GEMDRIVE on, floppy emulation off
    WRITE_WORD(m, GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE, 1);
    WRITE_WORD(m, GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY, 0);

    // One drive, C:
    const uint32_t drive_number = 2;
    set_shared_var(SHARED_VARIABLE_FIRST_FILE_DESCRIPTOR, FIRST_FILE_DESCRIPTOR);
    set_shared_var(SHARED_VARIABLE_DRIVE_LETTER, 'A' + drive_number);
    set_shared_var(SHARED_VARIABLE_DRIVE_NUMBER, drive_number);
    set_shared_var(SHARED_VARIABLE_BUFFER_TYPE, 0);            // 0: _dskbufp, 1: stack
    set_shared_var(SHARED_VARIABLE_FAKE_FLOPPY, 0);
    set_shared_var(SHARED_VARIABLE_PROTOCOL_VERSION, GEMDOS_SLOT_PROTOCOL_VERSION);
    set_shared_var(SHARED_VARIABLE_DRIVE_COUNT, 1);
    set_shared_var(SHARED_VARIABLE_DRIVE_NUMBER_TABLE, drive_number);
    for (int i = 1; i < GEMDRIVE_MAX_RUNTIME_DRIVES; i++)
        set_shared_var(SHARED_VARIABLE_DRIVE_NUMBER_TABLE + i, 0xFFFFFFFF);

    write_string(GEMDRVEMUL_DEFAULT_PATH, "", 128);
    memset(s_dtas, 0, sizeof(s_dtas));

    *((volatile uint32_t *)(m + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = esp_random();
}

void gemdrv_command(const hulk_packet_t *pkt)
{
    uintptr_t m = shared();
    // The first two payload words are the token (d2), the parameters follow
    random_token = pkt->words[1] | ((uint32_t)pkt->words[0] << 16);
    const uint16_t *payloadPtr = pkt->words + 2;

    switch (pkt->cmd) {
    case GEMDRVEMUL_PING:
        *((volatile uint16_t *)(m + GEMDRVEMUL_PING_STATUS)) = 0x1;
        ESP_LOGI(TAG, "PING");
        break;

    case GEMDRVEMUL_SAVE_VECTORS: {
        uint32_t trap_old = ((uint32_t)payloadPtr[0] << 16) | payloadPtr[1];
        uint32_t xbra = ((uint32_t)payloadPtr[3] << 16) | payloadPtr[2];
        // Self-modifying cartridge code: the XBRA structure in the ROM4 image
        if (xbra >= ATARI_ROM4_START && xbra + 4 <= ATARI_ROM4_START + SHADOW_ROM3) {
            uintptr_t code = shadow_base() + xbra - ATARI_ROM4_START;
            *((volatile uint16_t *)code) = trap_old & 0xFFFF;
            *((volatile uint16_t *)(code + 2)) = trap_old >> 16;
        }
        ESP_LOGI(TAG, "SAVE_VECTORS: GEMDOS trap %08lx, XBRA at %06lx",
                 (unsigned long)trap_old, (unsigned long)xbra);
        break;
    }

    case GEMDRVEMUL_SAVE_XBIOS_VECTOR: {
        uint32_t old = ((uint32_t)payloadPtr[0] << 16) | payloadPtr[1];
        *((volatile uint16_t *)(m + GEMDRVEMUL_OLD_XBIOS_TRAP)) = old & 0xFFFF;
        *((volatile uint16_t *)(m + GEMDRVEMUL_OLD_XBIOS_TRAP + 2)) = old >> 16;
        ESP_LOGI(TAG, "SAVE_XBIOS_VECTOR: %08lx", (unsigned long)old);
        break;
    }

    case GEMDRVEMUL_REENTRY_LOCK:
        *((volatile uint16_t *)(m + GEMDRVEMUL_REENTRY_TRAP)) = 0xFFFF;
        break;
    case GEMDRVEMUL_REENTRY_UNLOCK:
        *((volatile uint16_t *)(m + GEMDRVEMUL_REENTRY_TRAP)) = 0;
        break;
    case GEMDRVEMUL_REENTRY_XBIOS_LOCK:
        *((volatile uint16_t *)(m + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0xFFFF;
        break;
    case GEMDRVEMUL_REENTRY_XBIOS_UNLOCK:
        *((volatile uint16_t *)(m + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0;
        break;

    case GEMDRVEMUL_SET_SHARED_VAR: {
        uint32_t index = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];   // d3
        uint32_t value = ((uint32_t)payloadPtr[3] << 16) | payloadPtr[2];   // d4
        set_shared_var(index, value);
        ESP_LOGI(TAG, "SET_SHARED_VAR %lu = %08lx", (unsigned long)index, (unsigned long)value);
        break;
    }

    case GEMDRVEMUL_FLOPPY_SAVE_VECTORS:
    case GEMDRVEMUL_FLOPPY_SAVE_XBIOS_VECTOR:
        // Only sent when the floppy emulation is installed; it is not (INSTALL_FLOPPY = 0)
        ESP_LOGW(TAG, "floppy command 0x%04x while the floppy emulation is off", pkt->cmd);
        break;

    // --- Drive C: is empty and read-only for now -----------------------------------
    case GEMDRVEMUL_FSFIRST_CALL: {
        char path[192];
        uint32_t dta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
        payload_string(pkt, path, sizeof(path));
        dta_add(dta);
        memset((void *)(m + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
        WRITE_WORD(m, GEMDRVEMUL_DTA_F_FOUND, (uint16_t)GEMDOS_EFILNF);
        ESP_LOGI(TAG, "Fsfirst \"%s\" (DTA %06lx): no files", path, (unsigned long)dta);
        break;
    }
    case GEMDRVEMUL_FSNEXT_CALL:
        memset((void *)(m + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
        WRITE_WORD(m, GEMDRVEMUL_DTA_F_FOUND, (uint16_t)GEMDOS_ENMFIL);
        break;
    case GEMDRVEMUL_DTA_EXIST_CALL: {
        uint32_t dta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DTA_EXIST, dta_known(dta, false) ? dta : 0);
        break;
    }
    case GEMDRVEMUL_DTA_RELEASE_CALL: {
        uint32_t dta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DTA_RELEASE, dta_known(dta, true) ? 1 : 0);
        break;
    }

    case GEMDRVEMUL_FOPEN_CALL: {
        char path[256];
        payload_string(pkt, path, sizeof(path));
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)GEMDOS_EFILNF);
        ESP_LOGI(TAG, "Fopen \"%s\": not found", path);
        break;
    }
    case GEMDRVEMUL_PEXEC_CALL: {
        uint16_t mode = payloadPtr[0];
        uint32_t stack = ((uint32_t)payloadPtr[3] << 16) | payloadPtr[2];
        uint32_t fname = ((uint32_t)payloadPtr[5] << 16) | payloadPtr[4];
        uint32_t cmdline = ((uint32_t)payloadPtr[7] << 16) | payloadPtr[6];
        uint32_t envstr = ((uint32_t)payloadPtr[9] << 16) | payloadPtr[8];
        *((volatile uint16_t *)(m + GEMDRVEMUL_PEXEC_MODE)) = mode;
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_PEXEC_STACK_ADDR, stack);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_PEXEC_FNAME, fname);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_PEXEC_CMDLINE, cmdline);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_PEXEC_ENVSTR, envstr);
        ESP_LOGI(TAG, "Pexec mode %u", mode);
        break;
    }

    case GEMDRVEMUL_DGETPATH_CALL:
        write_string(GEMDRVEMUL_DEFAULT_PATH, "", 128);
        break;
    case GEMDRVEMUL_DSETPATH_CALL: {
        char path[256];
        payload_string(pkt, path, sizeof(path));
        bool ok = is_root(path);
        WRITE_WORD(m, GEMDRVEMUL_SET_DPATH_STATUS, (uint16_t)(ok ? GEMDOS_EOK : GEMDOS_EPTHNF));
        ESP_LOGI(TAG, "Dsetpath \"%s\": %s", path, ok ? "ok" : "path not found");
        break;
    }
    case GEMDRVEMUL_DFREE_CALL:
        // GEMDRIVE copies the result from the address held in DFREE_STRUCT (a
        // move.l where a lea was meant), so report an error until that is sorted out
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DFREE_STATUS, (uint32_t)GEMDOS_ERROR);
        break;

    case GEMDRVEMUL_DCREATE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_DCREATE_STATUS, (uint16_t)GEMDOS_EACCDN);
        break;
    case GEMDRVEMUL_DDELETE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_DDELETE_STATUS, (uint16_t)GEMDOS_EPTHNF);
        break;
    case GEMDRVEMUL_FCREATE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_FCREATE_HANDLE, (uint16_t)GEMDOS_EACCDN);
        break;
    case GEMDRVEMUL_FDELETE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_FDELETE_STATUS, (uint16_t)GEMDOS_EFILNF);
        break;
    case GEMDRVEMUL_FRENAME_CALL:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FRENAME_STATUS, (uint32_t)GEMDOS_EFILNF);
        break;
    case GEMDRVEMUL_FATTRIB_CALL:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FATTRIB_STATUS, (uint32_t)GEMDOS_EFILNF);
        break;

    // No file is ever open: every handle is invalid
    case GEMDRVEMUL_FCLOSE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_FCLOSE_STATUS, (uint16_t)GEMDOS_EIHNDL);
        break;
    case GEMDRVEMUL_FSEEK_CALL:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FSEEK_STATUS, (uint32_t)GEMDOS_EIHNDL);
        break;
    case GEMDRVEMUL_FDATETIME_CALL:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FDATETIME_STATUS, (uint32_t)GEMDOS_EIHNDL);
        break;
    case GEMDRVEMUL_READ_BUFF_CALL:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_READ_BYTES, (uint32_t)GEMDOS_EIHNDL);
        break;
    case GEMDRVEMUL_WRITE_BUFF_CALL:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_WRITE_BYTES, (uint32_t)GEMDOS_EIHNDL);
        break;
    case GEMDRVEMUL_WRITE_BUFF_CHECK:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_WRITE_CONFIRM_STATUS, (uint32_t)GEMDOS_EIHNDL);
        break;

    case GEMDRVEMUL_CANCEL:
    case GEMDRVEMUL_SHOW_VECTOR_CALL:
    case GEMDRVEMUL_DEBUG:
    case GEMDRVEMUL_FSETDTA_CALL:
    case GEMDRVEMUL_DGETDRV_CALL:
    case GEMDRVEMUL_MALLOC_CALL:
    case GEMDRVEMUL_SAVE_BASEPAGE:
    case GEMDRVEMUL_SAVE_EXEC_HEADER:
        break;

    default:
        // Not implemented yet: answer anyway, so the Atari does not wait for the timeout
        ESP_LOGW(TAG, "command 0x%04x (%u bytes) not implemented", pkt->cmd, pkt->size);
        break;
    }

    write_random_token();
    *((volatile uint32_t *)(m + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = esp_random();
}
