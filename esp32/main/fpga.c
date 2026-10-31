// iCE40UP5K configuration over slave SPI (Lattice TN1248 / FPGA-TN-02001), and the
// SPI link to the gateware on the same pins (SCK, MOSI, MISO, CS on GPIO10).
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "board.h"
#include "fpga.h"

static const char *TAG = "fpga";

#define CHUNK            4096
#define CONFIG_CLOCK_HZ  (20 * 1000 * 1000)   // 50/50 OK at 10, 20 and 26.7 MHz (v1)
#define USER_CLOCK_HZ    (4 * 1000 * 1000)    // the FPGA oversamples SCK, max ~5 MHz

static spi_device_handle_t s_cfg_dev;    // mode 3, configuration
static spi_device_handle_t s_user_dev;   // mode 0, gateware commands
static uint8_t *s_dma;                   // DMA bounce buffer (tx)
static uint8_t *s_dma_rx;                // DMA bounce buffer (rx)
static bool s_bus_on;

// The SPI bus is only attached while the FPGA side is powered. Otherwise SCK (idle high
// in mode 3) and MOSI would feed the unpowered FPGA through its I/O protection diodes.
static void bus_on(void)
{
    if (s_bus_on)
        return;
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_FPGA_MOSI,
        .miso_io_num = PIN_FPGA_MISO,
        .sclk_io_num = PIN_FPGA_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(FPGA_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    // Chip select is driven by hand: SPI_SS must stay low while loading the bitstream
    spi_device_interface_config_t cfg = {
        .mode = 3,
        .clock_speed_hz = CONFIG_CLOCK_HZ,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(FPGA_SPI_HOST, &cfg, &s_cfg_dev));

    spi_device_interface_config_t user = {
        .mode = 0,
        .clock_speed_hz = USER_CLOCK_HZ,
        .spics_io_num = -1,
        .queue_size = 1,
        .command_bits = 8,
        .flags = SPI_DEVICE_HALFDUPLEX,    // command, address, then write or read
    };
    ESP_ERROR_CHECK(spi_bus_add_device(FPGA_SPI_HOST, &user, &s_user_dev));
    s_bus_on = true;
}

static void bus_off(void)
{
    if (s_bus_on) {
        spi_bus_remove_device(s_cfg_dev);
        spi_bus_remove_device(s_user_dev);
        spi_bus_free(FPGA_SPI_HOST);
        s_bus_on = false;
    }
    gpio_config_t low = {
        .pin_bit_mask = BIT64(PIN_FPGA_SCK) | BIT64(PIN_FPGA_MOSI),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(PIN_FPGA_SCK, 0);
    gpio_set_level(PIN_FPGA_MOSI, 0);
    gpio_config(&low);
    gpio_config_t in = {
        .pin_bit_mask = BIT64(PIN_FPGA_MISO),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&in);
}

esp_err_t fpga_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_FPGA_CRESET) | BIT64(PIN_FPGA_CS),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_set_level(PIN_FPGA_CS, 0);
    gpio_set_level(PIN_FPGA_CRESET, 0);
    ESP_ERROR_CHECK(gpio_config(&out));

    // CDONE and /IRQ have pull-ups to +3V3_LOGIC on the board (R1, R9): with a weak
    // pull-down they read low when the FPGA side is unpowered.
    gpio_config_t in = {
        .pin_bit_mask = BIT64(PIN_FPGA_CDONE) | BIT64(PIN_FPGA_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    // PWR_ON is +3V3_LOGIC through R10 (10k): no pull-down, it would form a divider
    gpio_config_t pwr = {
        .pin_bit_mask = BIT64(PIN_PWR_ON),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&pwr));
    bus_off();

    s_dma = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA);
    s_dma_rx = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA);
    return (s_dma && s_dma_rx) ? ESP_OK : ESP_ERR_NO_MEM;
}

bool fpga_power_present(void)
{
    return gpio_get_level(PIN_PWR_ON);
}

void fpga_safe(void)
{
    gpio_set_level(PIN_FPGA_CS, 0);
    gpio_set_level(PIN_FPGA_CRESET, 0);
    bus_off();
}

bool fpga_done(void)
{
    return gpio_get_level(PIN_FPGA_CDONE);
}

static esp_err_t send(spi_device_handle_t dev, const uint8_t *data, size_t len)
{
    while (len) {
        size_t n = len > CHUNK ? CHUNK : len;
        memcpy(s_dma, data, n);
        spi_transaction_t t = { .length = n * 8, .tx_buffer = s_dma };
        esp_err_t err = spi_device_polling_transmit(dev, &t);
        if (err != ESP_OK)
            return err;
        data += n;
        len -= n;
    }
    return ESP_OK;
}

static esp_err_t dummy_clocks(int bytes)
{
    static const uint8_t ff[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return send(s_cfg_dev, ff, bytes);
}

esp_err_t fpga_configure(const uint8_t *bitstream, size_t len)
{
    int64_t t0 = esp_timer_get_time();

    bus_on();
    spi_device_acquire_bus(s_cfg_dev, portMAX_DELAY);

    // SPI_SS stays low during the whole sequence (tested on v1: the 8 clocks with SS
    // high from TN1248 are not needed). Park SCK high (mode 3 idle) first.
    gpio_set_level(PIN_FPGA_CS, 0);
    dummy_clocks(1);

    // CRESET_B low with SPI_SS low selects slave SPI mode when CRESET_B rises
    gpio_set_level(PIN_FPGA_CRESET, 0);
    esp_rom_delay_us(2);                       // >= 200 ns
    gpio_set_level(PIN_FPGA_CRESET, 1);
    esp_rom_delay_us(1300);                    // >= 1200 us housekeeping (UP5K)

    esp_err_t err = send(s_cfg_dev, bitstream, len);

    // >= 100 clocks for CDONE, then >= 49 more to start the user I/O
    if (err == ESP_OK)
        err = dummy_clocks(13);
    bool done = fpga_done();
    dummy_clocks(7);
    done |= fpga_done();
    gpio_set_level(PIN_FPGA_CS, 1);            // CS idle for the gateware

    spi_device_release_bus(s_cfg_dev);

    int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI error during configuration: %s", esp_err_to_name(err));
        return err;
    }
    if (!done) {
        ESP_LOGE(TAG, "CDONE did not go high after %u bytes", (unsigned)len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "configured: %u bytes in %lld ms", (unsigned)len, (t1 - t0) / 1000);
    return ESP_OK;
}

esp_err_t fpga_command(uint8_t cmd, uint32_t addr, int addr_bits,
                       const void *tx, size_t tx_len, void *rx, size_t rx_len)
{
    if (tx_len > CHUNK || rx_len > CHUNK || (tx_len && rx_len))
        return ESP_ERR_INVALID_ARG;
    if (tx_len)
        memcpy(s_dma, tx, tx_len);
    spi_transaction_ext_t t = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_ADDR,
            .cmd = cmd,
            .addr = addr,
            .length = tx_len * 8,
            .tx_buffer = tx_len ? s_dma : NULL,
            .rxlength = rx_len * 8,
            .rx_buffer = rx_len ? s_dma_rx : NULL,
        },
        .address_bits = addr_bits,
    };
    spi_device_acquire_bus(s_user_dev, portMAX_DELAY);
    gpio_set_level(PIN_FPGA_CS, 0);
    esp_err_t err = spi_device_polling_transmit(s_user_dev, &t.base);
    gpio_set_level(PIN_FPGA_CS, 1);
    spi_device_release_bus(s_user_dev);
    if (err == ESP_OK && rx_len)
        memcpy(rx, s_dma_rx, rx_len);
    return err;
}
