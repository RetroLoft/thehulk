// Button 1 and commands on the serial console (UART and USB): switch the cartridge on or off
#pragma once

#include <stdbool.h>

void console_start(void);

// The cartridge off or on from the next reset of the Atari (also the web page)
void console_set_cart_off(bool off);
