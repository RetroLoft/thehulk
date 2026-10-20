// Status LED: the on-board WS2812 of the DevKitC (GPIO48)
#pragma once

#include <stdint.h>

typedef enum {
    STATUS_OFF,
    STATUS_WAIT_POWER,   // blue: waiting for the Atari to power the FPGA
    STATUS_STARTING,     // yellow: configuring the FPGA, loading the cartridge
    STATUS_ACTIVE,       // green: cartridge active
    STATUS_ERROR,        // red
} status_t;

void status_led_init(void);
void status_led_set(status_t status);
