// Status LED: the on-board WS2812 of the DevKitC (GPIO48, standard GRB order)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "board.h"
#include "status_led.h"
#include "wifi.h"

#define POLL_MS      20
#define ACTIVITY_MS  100
#define BLINK_MS     500
#define ERROR_MS     2000
#define CARD_MS      300

typedef enum { LED_OFF, LED_BLUE, LED_GREEN, LED_RED, LED_PURPLE, LED_YELLOW } led_color_t;

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static volatile bool s_fatal;
static volatile TickType_t s_activity, s_error, s_card;
static volatile bool s_activity_seen, s_error_seen, s_card_seen, s_cart_off;

static void show(led_color_t color)
{
    static const uint8_t grb[][3] = {
        [LED_OFF]    = {  0,  0,  0 },
        [LED_BLUE]   = {  0,  0, 16 },
        [LED_GREEN]  = { 24,  0,  0 },
        [LED_RED]    = {  0, 32,  0 },
        [LED_PURPLE] = {  0, 16, 16 },
        [LED_YELLOW] = { 12, 16,  0 },
    };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_enc, grb[color], sizeof(grb[color]), &tx);
    rmt_tx_wait_all_done(s_chan, 10);
}

// Red: an error. Green: the drive at work. Purple: a card was mounted. Yellow: the
// cartridge is switched off. Blue, blinking: no Wi-Fi (yet). Otherwise off.
static void led_task(void *arg)
{
    led_color_t shown = LED_OFF;
    show(shown);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        led_color_t color = LED_OFF;
        if (s_fatal || (s_error_seen && now - s_error < pdMS_TO_TICKS(ERROR_MS)))
            color = LED_RED;
        else if (s_activity_seen && now - s_activity < pdMS_TO_TICKS(ACTIVITY_MS))
            color = LED_GREEN;
        else if (s_card_seen && now - s_card < pdMS_TO_TICKS(CARD_MS))
            color = LED_PURPLE;
        else if (s_cart_off)
            color = LED_YELLOW;
        else if (wifi_link_state() != WIFI_STATE_READY && (pdTICKS_TO_MS(now) / BLINK_MS) % 2 == 0)
            color = LED_BLUE;
        if (color != shown) {
            show(color);
            shown = color;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void status_led_init(void)
{
    rmt_tx_channel_config_t cfg = {
        .gpio_num = PIN_STATUS_LED,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,         // 0.1 us per tick
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    rmt_bytes_encoder_config_t bcfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },   // 0.3 / 0.9 us
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },   // 0.9 / 0.3 us
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&cfg, &s_chan) != ESP_OK || rmt_new_bytes_encoder(&bcfg, &s_enc) != ESP_OK) {
        ESP_LOGW("led", "status LED not available");
        s_chan = NULL;
        return;
    }
    rmt_enable(s_chan);
    xTaskCreatePinnedToCore(led_task, "led", 2048, NULL, 1, NULL, 1);
}

void status_led_fatal(bool on)
{
    s_fatal = on;
}

void status_led_activity(void)
{
    s_activity = xTaskGetTickCount();
    s_activity_seen = true;
}

void status_led_error(void)
{
    s_error = xTaskGetTickCount();
    s_error_seen = true;
}

void status_led_card(void)
{
    s_card = xTaskGetTickCount();
    s_card_seen = true;
}

void status_led_cart_off(bool off)
{
    s_cart_off = off;
}
