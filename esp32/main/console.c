// Button 1 and commands on the serial console, see console.h. Both serial ports are read:
// the UART of the CH343 and the native USB port.
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "board.h"
#include "cart.h"
#include "floppy.h"
#include "console.h"
#include "netbridge.h"
#include "settings.h"
#include "status_led.h"
#include "wifi.h"

static const char *TAG = "console";

#define POLL_MS     20
#define DEBOUNCE    3           // polls the button must agree
#define SHORT_MS    1500        // shorter: cartridge on/off
#define LONG_MS     10000       // held this long: setup mode on (or off)

void console_set_cart_off(bool off)
{
    if (!settings_set_cart_off(off)) {
        ESP_LOGE(TAG, "could not save the setting");
        return;
    }
    status_led_cart_off(off);
    ESP_LOGW(TAG, "cartridge %s from the next reset or power-on of the Atari", off ? "OFF" : "ON");
}

static bool s_test;

static void command(char c)
{
    switch (c) {
    case 'd':
        console_set_cart_off(true);
        break;
    case 'e':
        console_set_cart_off(false);
        break;
    case 's':
        ESP_LOGI(TAG, "cartridge %s", settings_cart_off() ? "off" : "on");
        break;
    case 'w':
        net_bridge_status();
        break;
    case 'a':
        if (wifi_setup_active())
            wifi_setup_stop();
        else
            wifi_setup_start();
        break;
    case 't':                           // diagnostics: the ESP32 plays an Atari on .211
        s_test = !s_test;
        net_bridge_test(s_test ? 0xC0A8B2D3 : 0);
        break;
    case '\r':
    case '\n':
        break;
    default:
        ESP_LOGI(TAG, "keys: d = cartridge off, e = cartridge on, s = status, w = network, "
                 "a = setup mode on/off. Button 1: short = cartridge on/off, 10 s = setup mode.");
        break;
    }
}

static void console_task(void *arg)
{
    bool uart = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0) == ESP_OK;
    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    bool usb = usb_serial_jtag_driver_install(&usb_cfg) == ESP_OK;
    gpio_config_t btn = {
        .pin_bit_mask = BIT64(PIN_BTN1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    status_led_cart_off(settings_cart_off());

    bool pressed = false, long_done = false;
    int same = 0;
    TickType_t down = 0;
    for (;;) {
        char c;
        if (uart && uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, 0) == 1)
            command(c);
        if (usb && usb_serial_jtag_read_bytes(&c, 1, 0) == 1)
            command(c);
        bool now = !gpio_get_level(PIN_BTN1);
        same = now != pressed ? same + 1 : 0;
        if (same >= DEBOUNCE) {
            same = 0;
            pressed = now;
            if (pressed) {
                down = xTaskGetTickCount();
                long_done = false;
            } else if (!long_done && xTaskGetTickCount() - down < pdMS_TO_TICKS(SHORT_MS)) {
                // A short press, on release: leaves the cartridge or floppy emulator,
                // otherwise switches the cartridge off or on
                if (cart_emulator_on())
                {
                    cart_select(NULL);
                    cart_apply_now();
                }
                else if (floppy_emulator_on())
                    floppy_off();
                else
                    console_set_cart_off(!settings_cart_off());
            }
        }
        if (pressed && !long_done && xTaskGetTickCount() - down >= pdMS_TO_TICKS(LONG_MS)) {
            long_done = true;           // held 10 s: setup mode, at once (the LED shows it)
            if (wifi_setup_active())
                wifi_setup_stop();
            else
                wifi_setup_start();
        }
        net_bridge_test_poll();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void console_start(void)
{
    xTaskCreatePinnedToCore(console_task, "console", 3072, NULL, 1, NULL, 1);
}
