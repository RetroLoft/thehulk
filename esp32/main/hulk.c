// Register protocol of the TheHulk gateware (fpga/rtl/spi_slave.v)
#include <string.h>
#include "esp_log.h"
#include "fpga.h"
#include "hulk.h"

#define CMD_WRITE   0x01
#define CMD_READ    0x02
#define CMD_ID      0x03
#define CMD_CTRL    0x04
#define CMD_STATUS  0x05
#define CMD_ACK     0x06

#define PACKET_ADDR 0x800000     // address bit 23: the command packet buffer

#define CHUNK       4096

int hulk_probe(void)
{
    uint8_t id[5];
    if (fpga_command(CMD_ID, 0, 8, NULL, 0, id, sizeof(id)) != ESP_OK)   // 8 bits: dummy
        return -1;
    if (memcmp(id, "HULK", 4) != 0) {
        ESP_LOGW("hulk", "ID answer: %02x %02x %02x %02x %02x", id[0], id[1], id[2], id[3], id[4]);
        return -1;
    }
    return id[4];
}

esp_err_t hulk_write(uint32_t offset, const uint8_t *data, size_t len)
{
    while (len) {
        size_t n = len > CHUNK ? CHUNK : len;
        esp_err_t err = fpga_command(CMD_WRITE, offset, 24, data, n, NULL, 0);
        if (err != ESP_OK)
            return err;
        offset += n;
        data += n;
        len -= n;
    }
    return ESP_OK;
}

esp_err_t hulk_read(uint32_t offset, uint8_t *data, size_t len)
{
    while (len) {
        size_t n = len > CHUNK ? CHUNK : len;
        // 24 address bits followed by 8 dummy bits
        esp_err_t err = fpga_command(CMD_READ, offset << 8, 32, NULL, 0, data, n);
        if (err != ESP_OK)
            return err;
        offset += n;
        data += n;
        len -= n;
    }
    return ESP_OK;
}

esp_err_t hulk_set_ctrl(uint8_t ctrl)
{
    return fpga_command(CMD_CTRL, ctrl, 8, NULL, 0, NULL, 0);
}

esp_err_t hulk_get_status(hulk_status_t *st)
{
    uint8_t b[22];
    esp_err_t err = fpga_command(CMD_STATUS, 0, 8, NULL, 0, b, sizeof(b));   // 8 bits: dummy
    if (err != ESP_OK)
        return err;
    st->ctrl = b[0];
    st->rom4_count = (b[1] << 24) | (b[2] << 16) | (b[3] << 8) | b[4];
    st->rom3_count = (b[5] << 24) | (b[6] << 16) | (b[7] << 8) | b[8];
    uint16_t w = (b[9] << 8) | b[10];   // {bank, A15..A1}
    st->last_addr = 0xFA0000 + ((uint32_t)w << 1);
    st->addr_changes = (b[11] << 24) | (b[12] << 16) | (b[13] << 8) | b[14];
    st->early_count = (b[15] << 8) | b[16];
    st->pkt_ready = b[17] & 1;
    st->pkt_count = (b[18] << 8) | b[19];
    st->pkt_drops = (b[20] << 8) | b[21];
    return ESP_OK;
}

esp_err_t hulk_get_packet(hulk_packet_t *p)
{
    static uint8_t b[4 + HULK_PACKET_MAX * 2];
    esp_err_t err = hulk_read(PACKET_ADDR, b, 4);
    if (err != ESP_OK)
        return err;
    p->cmd = (b[0] << 8) | b[1];
    p->size = (b[2] << 8) | b[3];
    size_t words = (p->size + 1) / 2;
    if (words > HULK_PACKET_MAX)
        words = HULK_PACKET_MAX;
    err = hulk_read(PACKET_ADDR + 4, b, words * 2);
    if (err != ESP_OK)
        return err;
    for (size_t i = 0; i < words; i++)
        p->words[i] = (b[2 * i] << 8) | b[2 * i + 1];
    return ESP_OK;
}

esp_err_t hulk_ack_packet(void)
{
    return fpga_command(CMD_ACK, 0, 0, NULL, 0, NULL, 0);
}
