// Register protocol of the TheHulk gateware (fpga/rtl/spi_slave.v)
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define HULK_ROM_SIZE      0x20000   // 128 KB: 0xFA0000-0xFBFFFF
#define HULK_CTRL_ROM_EN   0x01

typedef struct {
    uint8_t  ctrl;
    uint32_t rom4_count;   // number of /ROM4 cycles (0xFA0000-0xFAFFFF)
    uint32_t rom3_count;   // number of /ROM3 cycles (0xFB0000-0xFBFFFF)
    uint32_t last_addr;    // Atari address of the last ROM cycle
    uint32_t addr_changes; // address bus changes: increases while the Atari runs
    uint16_t early_count;  // ROM cycles while the cartridge was not enabled yet
} hulk_status_t;

// Reads the ID, returns the gateware version or -1 when the FPGA does not answer.
int hulk_probe(void);
esp_err_t hulk_write(uint32_t offset, const uint8_t *data, size_t len);
esp_err_t hulk_read(uint32_t offset, uint8_t *data, size_t len);
esp_err_t hulk_set_ctrl(uint8_t ctrl);
esp_err_t hulk_get_status(hulk_status_t *st);
