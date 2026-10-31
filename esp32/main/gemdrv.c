// GEMDRIVE emulation, see gemdrv.h. The command handlers follow gemdrvemul.c of
// SideTNFS-Firmware: same shared memory layout, same word order (shadow.h).
#include "esp_log.h"
#include "esp_random.h"
#include "gemdrv.h"
#include "gemdrv_layout.h"
#include "shadow.h"

static const char *TAG = "gemdrv";

#define ATARI_ROM4_START  0xFA0000

static uint32_t random_token;

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

    case GEMDRVEMUL_CANCEL:
    case GEMDRVEMUL_SHOW_VECTOR_CALL:
    case GEMDRVEMUL_DEBUG:
        break;

    default:
        // Not implemented yet: answer anyway, so the Atari does not wait for the timeout
        ESP_LOGW(TAG, "command 0x%04x (%u bytes) not implemented", pkt->cmd, pkt->size);
        break;
    }

    write_random_token();
    *((volatile uint32_t *)(m + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = esp_random();
}
