// GEMDRIVE emulation, see gemdrv.h. The command handlers follow gemdrvemul.c of
// SideTNFS-Firmware: same shared memory layout, same word order (shadow.h).
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gemdrv.h"
#include "gemdrv_fs.h"
#include "gemdrv_layout.h"
#include "shadow.h"

static const char *TAG = "gemdrv";

#define ATARI_ROM4_START  0xFA0000

// Token polls before a GEMDRIVE command times out (measured on the Falcon: 730000 polls/s)
#define SYNC_TIMEOUT_68000  0x000FFFFF
#define SYNC_TIMEOUT_68030  0x00500000

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

// GEMDRIVE sends the TOS crash area $380-$3FF at every start (after the d3-d5 header):
// $380 proc_lives = $12345678 when valid, $384 d0-d7, $3A4 a0-a7, $3C4 exception
// number (first byte), $3C8 usp, $3CC the first 16 words of the supervisor stack
static void log_crash_info(const hulk_packet_t *pkt)
{
    const uint16_t *w = pkt->words + 2 + 6;     // word i = address $380 + 2 * i
    if ((pkt->size + 1) / 2 < 2 + 6 + 64)
        return;
    uint32_t magic = ((uint32_t)w[0] << 16) | w[1];
    if (magic != 0x12345678) {
        ESP_LOGI(TAG, "no crash recorded (proc_lives %08lx)", (unsigned long)magic);
        return;
    }
#define L(i) (((uint32_t)w[i] << 16) | w[(i) + 1])
    const uint16_t *stk = w + 0x26;             // $3CC
    unsigned vec = w[0x22] >> 8;                // $3C4
    ESP_LOGW(TAG, "last crash: exception %u (%u bombs), SR %04x, PC %08lx, frame format %X",
             vec, vec, stk[0], (unsigned long)(((uint32_t)stk[1] << 16) | stk[2]), stk[3] >> 12);
    if ((stk[3] >> 12) == 0xA || (stk[3] >> 12) == 0xB)
        ESP_LOGW(TAG, "bus error: SSW %04x, data fault address %08lx", stk[5],
                 (unsigned long)(((uint32_t)stk[8] << 16) | stk[9]));
    ESP_LOGW(TAG, "d0-d3 %08lx %08lx %08lx %08lx  d4-d7 %08lx %08lx %08lx %08lx",
             (unsigned long)L(2), (unsigned long)L(4), (unsigned long)L(6), (unsigned long)L(8),
             (unsigned long)L(10), (unsigned long)L(12), (unsigned long)L(14), (unsigned long)L(16));
    ESP_LOGW(TAG, "a0-a3 %08lx %08lx %08lx %08lx  a4-a7 %08lx %08lx %08lx %08lx  usp %08lx",
             (unsigned long)L(18), (unsigned long)L(20), (unsigned long)L(22), (unsigned long)L(24),
             (unsigned long)L(26), (unsigned long)L(28), (unsigned long)L(30), (unsigned long)L(32),
             (unsigned long)L(0x24));
    char line[100];
    size_t len = 0;
    for (int i = 0; i < 16; i++)
        len += snprintf(line + len, sizeof(line) - len, " %04x", stk[i]);
    ESP_LOGW(TAG, "stack:%s", line);
#undef L
}

// Diagnostics: GEMDRIVE sends the TOS ROM ($E00000-$E7FFFF) in blocks of 2 KB when
// SHARED_VARIABLE_DUMP_TOS is set. Once per ESP32 start: printed as hex lines
// "TOS <address> <32 bytes>" by a task on the second core, the Atari does not wait for it.
#ifndef TOS_DUMP
#define TOS_DUMP   0         // 1: dump the ROM (done for TOS 4.04, 2026-11-02)
#endif
#define TOS_START  0xE00000
#define TOS_SIZE   0x80000

static uint8_t *s_tos;
static bool s_tos_done;

static void tos_print_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));             // after the boot messages
    printf("TOS dump start\n");
    for (uint32_t off = 0; off < TOS_SIZE; off += 32) {
        printf("TOS %06lx ", (unsigned long)(TOS_START + off));
        for (int i = 0; i < 32; i++)
            printf("%02x", s_tos[off + i]);
        printf("\n");
        if ((off & 0x3ff) == 0x3e0)
            vTaskDelay(1);
    }
    printf("TOS dump end, crc32 %08lx\n", (unsigned long)esp_rom_crc32_le(0, s_tos, TOS_SIZE));
    vTaskDelete(NULL);
}

static void tos_block(uint32_t addr, const uint16_t *w, unsigned bytes)
{
    if (s_tos_done || addr < TOS_START || addr + bytes > TOS_START + TOS_SIZE)
        return;
    if (!s_tos)
        s_tos = heap_caps_malloc(TOS_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_tos)
        return;
    uint8_t *p = s_tos + (addr - TOS_START);
    for (unsigned i = 0; i < bytes / 2; i++) {
        p[2 * i] = w[i] >> 8;
        p[2 * i + 1] = w[i] & 0xFF;
    }
    if (addr + bytes == TOS_START + TOS_SIZE) {
        s_tos_done = true;
        set_shared_var(SHARED_VARIABLE_DUMP_TOS, 0);
        ESP_LOGI(TAG, "TOS ROM received");
        xTaskCreatePinnedToCore(tos_print_task, "tosdump", 4096, NULL, 1, NULL, 1);
    }
}

// Diagnostics: a file call reported by GEMDRIVE's hook (trace_call in gemdrive.s). The
// Fread buffer is only filled after the call, so it is sent with the next Fclose.
static void log_trace(const hulk_packet_t *pkt, unsigned opcode)
{
    uint32_t arg1 = ((uint32_t)pkt->words[5] << 16) | pkt->words[4];
    uint32_t arg2 = ((uint32_t)pkt->words[7] << 16) | pkt->words[6];
    const uint16_t *w = pkt->words + 8;
    unsigned bytes = pkt->size > 16 ? (pkt->size - 16) & ~1u : 0;
    char text[520];
    unsigned n = 0;
    for (unsigned i = 0; i < bytes && n < sizeof(text) - 1; i++) {
        uint8_t c = i & 1 ? w[i / 2] & 0xFF : w[i / 2] >> 8;
        if (!c && opcode != 0x3E)
            break;
        text[n++] = c >= 32 && c < 127 ? c : '.';
    }
    text[n] = 0;
    switch (opcode) {
    case 0x3B:
        ESP_LOGI(TAG, "trace Dsetpath \"%s\" (AES rez %lu, change %lu, mode %04lx, video %04lx)", text,
                 (unsigned long)(arg2 >> 16), (unsigned long)(arg2 & 0xFFFF),
                 (unsigned long)(arg1 >> 16), (unsigned long)(arg1 & 0xFFFF));
        break;
    case 0x3D:
        ESP_LOGI(TAG, "trace Fopen \"%s\" (AES rez %lu, change %lu, mode %04lx, video %04lx)", text,
                 (unsigned long)(arg2 >> 16), (unsigned long)(arg2 & 0xFFFF),
                 (unsigned long)(arg1 >> 16), (unsigned long)(arg1 & 0xFFFF));
        break;
    case 0x4E:
        ESP_LOGI(TAG, "trace Fsfirst \"%s\" (AES rez %lu, change %lu, mode %04lx, video %04lx)", text,
                 (unsigned long)(arg2 >> 16), (unsigned long)(arg2 & 0xFFFF),
                 (unsigned long)(arg1 >> 16), (unsigned long)(arg1 & 0xFFFF));
        break;
    case 0x3F: {
        // The data is the frame of the call: buffer address at byte 14
        uint32_t buf = ((uint32_t)w[7] << 16) | w[8];
        set_shared_var(SHARED_VARIABLE_TRACE_BUFFER, buf);
        ESP_LOGI(TAG, "trace Fread handle %lu, %lu bytes to %06lx", (unsigned long)(arg1 >> 16),
                 (unsigned long)((arg1 << 16) | (arg2 >> 16)), (unsigned long)buf);
        break;
    }
    case 0x3E:
        ESP_LOGI(TAG, "trace Fclose handle %lu, last buffer:", (unsigned long)(arg1 >> 16));
        for (unsigned i = 0; i < n; i += 100)
            ESP_LOGI(TAG, "  %.100s", text + i);
        break;
    default:
        ESP_LOGI(TAG, "trace call 0x%02x", opcode);
        break;
    }
}

static void debug_command(const hulk_packet_t *pkt)
{
    uint32_t addr = ((uint32_t)pkt->words[3] << 16) | pkt->words[2];   // d3, low word first
    unsigned bytes = pkt->size > 16 ? (pkt->size - 16) & ~1u : 0;
    if ((addr >> 16) == 0x5452) {
        log_trace(pkt, addr & 0xFF);
        return;
    }
    if (addr == 0x380)
        log_crash_info(pkt);
    else
        tos_block(addr, pkt->words + 2 + 6, bytes);
}

// Bytes in the Atari's order (byte 0 at the even address) into the shadow's CPU word order
static void write_bytes(uint32_t offset, const uint8_t *data, size_t len)
{
    uint8_t *mem = (uint8_t *)shared() + offset;
    for (size_t i = 0; i < len; i++)
        mem[i ^ 1] = data[i];
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
#ifdef HULK_GEMDRIVE_OFF
    WRITE_WORD(m, GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE, 0);
#else
    WRITE_WORD(m, GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE, 1);
#endif
    WRITE_WORD(m, GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY, 0);

    // One drive, N: (C: is usually a hard disk, on the Falcon the internal IDE drive)
    const uint32_t drive_number = 'N' - 'A';
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
    // Keep booting from the Atari's own drive (later a setting)
    set_shared_var(SHARED_VARIABLE_SET_CURRENT_DRIVE, 0);
    set_shared_var(SHARED_VARIABLE_NO_INSTALL, HULK_GEMDRIVE_NO_INSTALL);
    set_shared_var(SHARED_VARIABLE_CODE_BUFFER, 0);
    set_shared_var(SHARED_VARIABLE_HOOK_STACK, 0);
    set_shared_var(SHARED_VARIABLE_TRACE_BUFFER, 0);
    set_shared_var(SHARED_VARIABLE_DUMP_TOS, 0);              // set once the TOS version is known
    // GEMDRIVE polls the token this many times before a command times out: about
    // 5 s on an 8 MHz 68000. A 68030 polls about 4x faster, see SVERSION below.
    set_shared_var(SHARED_VARIABLE_SYNC_TIMEOUT, SYNC_TIMEOUT_68000);

    write_string(GEMDRVEMUL_DEFAULT_PATH, "", 128);
    fs_reset();

    *((volatile uint32_t *)(m + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = esp_random();
}

void gemdrv_command(const hulk_packet_t *pkt)
{
    uintptr_t m = shared();
    // The first two payload words are the token (d2), the parameters follow
    random_token = pkt->words[1] | ((uint32_t)pkt->words[0] << 16);
    const uint16_t *payloadPtr = pkt->words + 2;
#if HULK_GEMDRIVE_NO_INSTALL
    static unsigned s_seq;
    ESP_LOGI(TAG, "#%u cmd %04x, %u bytes, token %08lx", ++s_seq, pkt->cmd, pkt->size, (unsigned long)random_token);
#endif

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
            shadow_mark(xbra - ATARI_ROM4_START, 4);
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
        // First command of a boot, also after a warm reset (no gemdrv_init then): the
        // addresses of GEMDRIVE's wait code RAM and hook stack are from the previous boot
        if (index == SHARED_VARIABLE_SVERSION) {
            set_shared_var(SHARED_VARIABLE_CODE_BUFFER, 0);
            set_shared_var(SHARED_VARIABLE_HOOK_STACK, 0);
            fs_reset();
        }
        // TOS 3 and 4: a TT or Falcon with a 68030
        if (index == SHARED_VARIABLE_SVERSION && (value >> 16) >= 0x0300)
            set_shared_var(SHARED_VARIABLE_SYNC_TIMEOUT, SYNC_TIMEOUT_68030);
        // Only TOS 4 (Falcon) has its ROM at $E00000-$E7FFFF
        if (TOS_DUMP && index == SHARED_VARIABLE_SVERSION && (value >> 16) >= 0x0400 && !s_tos_done)
            set_shared_var(SHARED_VARIABLE_DUMP_TOS, 1);
        break;
    }

    case GEMDRVEMUL_FLOPPY_SAVE_VECTORS:
    case GEMDRVEMUL_FLOPPY_SAVE_XBIOS_VECTOR:
        // Only sent when the floppy emulation is installed; it is not (INSTALL_FLOPPY = 0)
        ESP_LOGW(TAG, "floppy command 0x%04x while the floppy emulation is off", pkt->cmd);
        break;

    // --- The drive: folder HD of the microSD card, read-only (gemdrv_fs.c) -----------
    case GEMDRVEMUL_FSFIRST_CALL: {
        char path[192];
        uint8_t dta_out[FS_DTA_SIZE];
        uint32_t dta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
        payload_string(pkt, path, sizeof(path));
        int16_t r = fs_fsfirst(dta, path, payloadPtr[2], dta_out);
        write_bytes(GEMDRVEMUL_DTA_TRANSFER, dta_out, FS_DTA_SIZE);
        WRITE_WORD(m, GEMDRVEMUL_DTA_F_FOUND, (uint16_t)r);
        ESP_LOGI(TAG, "Fsfirst \"%s\" (DTA %06lx): %d", path, (unsigned long)dta, r);
        break;
    }
    case GEMDRVEMUL_FSNEXT_CALL: {
        uint8_t dta_out[FS_DTA_SIZE];
        uint32_t dta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
        int16_t r = fs_fsnext(dta, dta_out);
        write_bytes(GEMDRVEMUL_DTA_TRANSFER, dta_out, FS_DTA_SIZE);
        WRITE_WORD(m, GEMDRVEMUL_DTA_F_FOUND, (uint16_t)r);
        break;
    }
    case GEMDRVEMUL_DTA_EXIST_CALL: {
        uint32_t dta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DTA_EXIST, fs_dta_exists(dta) ? dta : 0);
        break;
    }
    case GEMDRVEMUL_DTA_RELEASE_CALL: {
        uint32_t dta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DTA_RELEASE, fs_dta_release(dta) ? 1 : 0);
        break;
    }

    case GEMDRVEMUL_FOPEN_CALL: {
        char path[256];
        payload_string(pkt, path, sizeof(path));
        int32_t r = fs_fopen(path, payloadPtr[0]);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)r);
        ESP_LOGI(TAG, "Fopen \"%s\": %ld", path, (long)r);
        break;
    }
    case GEMDRVEMUL_READ_BUFF_CALL: {
        // Handle in d3, the bytes still to read in d5; one buffer per command
        static uint8_t buf[GEMDRVEMUL_READ_BUFF_SIZE];
        uint32_t pending = ((uint32_t)payloadPtr[5] << 16) | payloadPtr[4];
        int32_t n = fs_fread(payloadPtr[0], buf, pending > sizeof(buf) ? sizeof(buf) : pending);
        if (n > 0) {
            write_bytes(GEMDRVEMUL_READ_BUFF, buf, n);
            shadow_mark(SHADOW_ROM3 + GEMDRVEMUL_READ_BUFF, n);
        }
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_READ_BYTES, (uint32_t)n);
        break;
    }
    case GEMDRVEMUL_FCLOSE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_FCLOSE_STATUS, (uint16_t)fs_fclose(payloadPtr[0]));
        break;
    case GEMDRVEMUL_FSEEK_CALL: {
        int32_t offset = (int32_t)(((uint32_t)payloadPtr[3] << 16) | payloadPtr[2]);
        int32_t r = fs_fseek(payloadPtr[0], offset, payloadPtr[4]);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FSEEK_STATUS, (uint32_t)r);
        break;
    }
    case GEMDRVEMUL_FDATETIME_CALL: {
        // Flag in d3 (non-zero: set the date, not on a read-only drive), handle in d4
        uint16_t time = 0, date = 0;
        int32_t r = payloadPtr[0] ? GEMDOS_EACCDN : fs_fdatime(payloadPtr[2], &time, &date);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FDATETIME_TIME, time);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FDATETIME_DATE, date);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FDATETIME_STATUS, (uint32_t)r);
        break;
    }
    case GEMDRVEMUL_FATTRIB_CALL: {
        char path[128];
        payload_string(pkt, path, sizeof(path));
        int32_t r = fs_fattrib(path, payloadPtr[0], payloadPtr[2]);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FATTRIB_STATUS, (uint32_t)r);
        break;
    }
    case GEMDRVEMUL_SAVE_BASEPAGE:
        memcpy((void *)(m + GEMDRVEMUL_EXEC_PD), payloadPtr + 6, 256);
        break;
    case GEMDRVEMUL_SAVE_EXEC_HEADER:
        memcpy((void *)(m + GEMDRVEMUL_EXEC_HEADER), payloadPtr + 6, 28);
        break;

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
        write_string(GEMDRVEMUL_DEFAULT_PATH, fs_dgetpath(), 128);
        break;
    case GEMDRVEMUL_DSETPATH_CALL: {
        char path[256];
        payload_string(pkt, path, sizeof(path));
        int16_t r = fs_dsetpath(path);
        WRITE_WORD(m, GEMDRVEMUL_SET_DPATH_STATUS, (uint16_t)r);
        ESP_LOGI(TAG, "Dsetpath \"%s\": %d", path, r);
        break;
    }
    case GEMDRVEMUL_DFREE_CALL:
        // Free clusters, total clusters, bytes per sector, sectors per cluster. Read-only,
        // so nothing is free; counting the clusters of a big card would take too long.
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DFREE_STRUCT, 0);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DFREE_STRUCT + 4, 65535);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DFREE_STRUCT + 8, 512);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DFREE_STRUCT + 12, 2);
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_DFREE_STATUS, 0);
        break;

    // Everything that would change the card
    case GEMDRVEMUL_DCREATE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_DCREATE_STATUS, (uint16_t)GEMDOS_EACCDN);
        break;
    case GEMDRVEMUL_DDELETE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_DDELETE_STATUS, (uint16_t)GEMDOS_EACCDN);
        break;
    case GEMDRVEMUL_FCREATE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_FCREATE_HANDLE, (uint16_t)GEMDOS_EACCDN);
        break;
    case GEMDRVEMUL_FDELETE_CALL:
        WRITE_WORD(m, GEMDRVEMUL_FDELETE_STATUS, (uint16_t)GEMDOS_EACCDN);
        break;
    case GEMDRVEMUL_FRENAME_CALL:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_FRENAME_STATUS, (uint32_t)GEMDOS_EACCDN);
        break;
    case GEMDRVEMUL_WRITE_BUFF_CALL:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_WRITE_BYTES, (uint32_t)GEMDOS_EACCDN);
        break;
    case GEMDRVEMUL_WRITE_BUFF_CHECK:
        WRITE_AND_SWAP_LONGWORD(m, GEMDRVEMUL_WRITE_CONFIRM_STATUS, (uint32_t)GEMDOS_EACCDN);
        break;

    case GEMDRVEMUL_DEBUG:
        debug_command(pkt);
        break;

    case GEMDRVEMUL_CANCEL:
    case GEMDRVEMUL_SHOW_VECTOR_CALL:
    case GEMDRVEMUL_FSETDTA_CALL:
    case GEMDRVEMUL_DGETDRV_CALL:
    case GEMDRVEMUL_MALLOC_CALL:
        break;

    default:
        // Not implemented yet: answer anyway, so the Atari does not wait for the timeout
        ESP_LOGW(TAG, "command 0x%04x (%u bytes) not implemented", pkt->cmd, pkt->size);
        break;
    }

    write_random_token();
    *((volatile uint32_t *)(m + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = esp_random();
}
