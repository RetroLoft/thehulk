// Status LED: the on-board WS2812 of the DevKitC (GPIO48, standard GRB order)
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "board.h"
#include "status_led.h"

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;

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
}

void status_led_set(status_t status)
{
    static const uint8_t rgb[][3] = {
        [STATUS_OFF]        = {  0,  0,  0 },
        [STATUS_WAIT_POWER] = {  0,  0, 16 },
        [STATUS_STARTING]   = { 24, 16,  0 },
        [STATUS_ACTIVE]     = {  0, 24,  0 },
        [STATUS_ERROR]      = { 32,  0,  0 },
    };
    if (!s_chan)
        return;
    uint8_t grb[3] = { rgb[status][1], rgb[status][0], rgb[status][2] };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &tx);
    rmt_tx_wait_all_done(s_chan, 10);
}
