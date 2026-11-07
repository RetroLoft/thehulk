// Status LED: the on-board WS2812 of the DevKitC (GPIO48). Blue and blinking without Wi-Fi,
// off with Wi-Fi, green while a drive is at work, red on an error, a purple flash when a
// card is mounted, yellow while the cartridge is switched off.
#pragma once

#include <stdbool.h>

void status_led_init(void);
void status_led_fatal(bool on);     // red until switched off
void status_led_activity(void);     // green for a moment
void status_led_error(void);        // red for a moment
void status_led_card(void);         // purple for a moment
void status_led_cart_off(bool off); // yellow instead of the Wi-Fi blink while switched off

// Setup mode (wifi.c): white blinking; with other states (no Wi-Fi, cartridge off) in turn
void status_led_setup(bool on);
