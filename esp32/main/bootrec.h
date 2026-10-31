// Boot records: timing of the last starts, kept in NVS so that a cold start without
// USB can be read back later over the serial port.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  reset_reason;   // esp_reset_reason_t
    uint8_t  brownouts;      // brown-out resets just before this start
    uint8_t  waited;         // 1: the ESP32 ran before the Atari (USB), 0: cold start together
    uint8_t  verify_errors;  // capped at 255
    uint32_t t_power;        // ms since the app started: PWR_ON seen
    uint32_t t_configured;   // FPGA configured
    uint32_t t_enabled;      // cartridge enabled
    uint32_t t_rom4;         // first /ROM4 read, 0 = none in the first 3 s
    uint32_t t_rom3;         // first /ROM3 read, 0 = none in the first 3 s
    uint32_t addr_changes;   // address bus activity when the cartridge was enabled
    uint16_t early;          // ROM reads before the cartridge was enabled
} bootrec_t;

// Call first in app_main: counts brown-out resets in RTC memory.
void bootrec_start(void);
uint8_t bootrec_brownouts(void);
// Stores a record (ring of BOOTREC_COUNT) and prints the stored ones.
void bootrec_save(const bootrec_t *rec);
void bootrec_print(void);

#define BOOTREC_COUNT 8
