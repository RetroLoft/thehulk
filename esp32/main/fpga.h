// iCE40UP5K configuration over slave SPI, and the SPI link to the gateware
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Initialise the SPI bus and the GPIOs. All FPGA lines start low.
esp_err_t fpga_init(void);

// The FPGA is powered from the Atari only (+5V_CART -> +5V_SYS -> regulators), while
// the ESP32 may run from USB. fpga_power_present() reads PWR_ON (+3V3_LOGIC).
// fpga_safe() drives all FPGA lines low and holds the FPGA in reset, so nothing
// back-powers an unpowered FPGA through its pins.
bool fpga_power_present(void);
void fpga_safe(void);

// Load a bitstream into the FPGA (CRAM, volatile). ESP_OK when CDONE goes high.
esp_err_t fpga_configure(const uint8_t *bitstream, size_t len);

bool fpga_done(void);

// One command to the gateware (one CS-low frame): command byte, addr_bits of address
// (sent MSB first, may include dummy bits), then either tx_len bytes written or
// rx_len bytes read (at most 4096).
esp_err_t fpga_command(uint8_t cmd, uint32_t addr, int addr_bits,
                       const void *tx, size_t tx_len, void *rx, size_t rx_len);
