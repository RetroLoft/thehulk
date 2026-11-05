// The network for the Atari, see netbridge.h.
//
// Threads: the filter runs in the Wi-Fi task and only fills the receive ring. The ring is
// emptied by one consumer (the command handler, or the test responder): one producer and
// one consumer, so the head and tail need no lock, only barriers.
#include <string.h>
#include "esp_log.h"
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#include "netbridge.h"

static const char *TAG = "net";

#define RX_SLOTS    32          // a power of two

static esp_netif_t *s_netif;
static uint8_t s_mac[6];
static volatile uint32_t s_my_ip, s_my_mask, s_my_gw, s_atari_ip, s_atari_mask;

static uint8_t s_rx_buf[RX_SLOTS][NET_FRAME_MAX];
static volatile uint16_t s_rx_len[RX_SLOTS];
static volatile uint32_t s_rx_head, s_rx_tail;

static volatile uint32_t s_rx, s_rx_drop, s_tx, s_tx_bad, s_tx_err;
static uint32_t s_test_ip;

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return get16(p) << 16 | get16(p + 2); }
static void put16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }

// Its IP address as an IPv4 destination or as the target of ARP
static bool for_atari(const uint8_t *f, uint32_t len)
{
    uint32_t type = get16(f + 12);
    if (type == 0x0800 && len >= 34)
        return get32(f + 30) == s_atari_ip;
    if (type == 0x0806 && len >= 42)
        return get32(f + 38) == s_atari_ip;
    return false;
}

static esp_err_t filter(void *buffer, uint16_t len, void *eb)
{
    if (!s_atari_ip || len < 14 || !for_atari(buffer, len))
        return esp_netif_receive(s_netif, buffer, len, eb);
    if (len > NET_FRAME_MAX || s_rx_head - s_rx_tail >= RX_SLOTS) {
        s_rx_drop++;                    // full: the newest goes
    } else {
        uint32_t s = s_rx_head % RX_SLOTS;
        memcpy(s_rx_buf[s], buffer, len);
        s_rx_len[s] = len;
        __sync_synchronize();
        s_rx_head++;
        s_rx++;
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

void net_bridge_install(esp_netif_t *netif, const esp_netif_ip_info_t *ip)
{
    s_netif = netif;
    s_my_ip = __builtin_bswap32(ip->ip.addr);
    s_my_mask = __builtin_bswap32(ip->netmask.addr);
    s_my_gw = __builtin_bswap32(ip->gw.addr);
    esp_wifi_get_mac(WIFI_IF_STA, s_mac);
    // esp_netif registered its own receive function when the station started: ours
    // passes everything that is not for the Atari on to it
    if (esp_wifi_internal_reg_rxcb(WIFI_IF_STA, filter) != ESP_OK)
        ESP_LOGE(TAG, "cannot install the filter");
}

uint32_t net_bridge_set(uint32_t ip, uint32_t mask)
{
    if (!ip) {
        s_atari_ip = 0;
        s_rx_tail = s_rx_head;
        return 0;
    }
    if (ip == 0xFFFFFFFF || !mask || (~mask & (~mask + 1)))
        return 3;
    if (ip == s_my_ip)
        return 4;
    if (ip != s_atari_ip)
        s_rx_tail = s_rx_head;          // frames for an old address
    s_atari_mask = mask;
    s_atari_ip = ip;
    return 0;
}

uint32_t net_bridge_rx(uint8_t *buf, uint32_t size, uint32_t *pending)
{
    if (s_rx_tail == s_rx_head) {
        *pending = 0;
        return 0;
    }
    uint32_t s = s_rx_tail % RX_SLOTS, len = s_rx_len[s];
    if (len > size)
        len = size;
    memcpy(buf, s_rx_buf[s], len);
    __sync_synchronize();
    s_rx_tail++;
    *pending = s_rx_head - s_rx_tail;
    return len;
}

uint32_t net_bridge_tx(const uint8_t *f, uint32_t len)
{
    // Only from our shared MAC address, and nothing for the ESP32 itself: that would
    // not come back over the air
    if (!s_atari_ip || len < 14 || len > NET_FRAME_MAX || memcmp(f + 6, s_mac, 6)) {
        s_tx_bad++;
        return 1;
    }
    uint32_t type = get16(f + 12);
    if ((type == 0x0800 && len >= 34 && get32(f + 30) == s_my_ip) ||
        (type == 0x0806 && len >= 42 && get32(f + 38) == s_my_ip)) {
        s_tx_bad++;
        return 0;
    }
    if (esp_wifi_internal_tx(WIFI_IF_STA, (void *)f, len) != ESP_OK) {
        s_tx_err++;
        return 0;
    }
    s_tx++;
    return 0;
}

uint32_t net_bridge_pending(void)
{
    return s_rx_head - s_rx_tail;
}

void net_bridge_info(uint8_t *b, bool link)
{
    memset(b, 0, 128);
    memcpy(b, "ATN", 3);
    put16(b + 4, 1);                            // protocol version
    put16(b + 6, 0x0001 | (link ? 0x0002 : 0)); // NET_CTRL works, frames can pass
    b[8] = (link ? 0x01 : 0) | (s_atari_ip ? 0x02 : 0);
    memcpy(b + 10, s_mac, 6);
    put32(b + 16, s_my_ip);
    put32(b + 20, s_my_mask);
    put32(b + 24, s_my_gw);
    put32(b + 28, s_atari_ip);
    put32(b + 32, s_atari_mask);
    put16(b + 36, 1500);                        // MTU
    b[40] = RX_SLOTS;
    put32(b + 44, s_rx);
    put32(b + 48, s_tx);
    put32(b + 52, s_rx_drop);
    put32(b + 56, s_tx_bad + s_tx_err);
    memcpy(b + 64, "TheHulk", 8);
}

void net_bridge_status(void)
{
    uint32_t a = s_atari_ip, m = s_atari_mask;
    ESP_LOGI(TAG, "bridge %s, Atari %lu.%lu.%lu.%lu/%lu.%lu.%lu.%lu, rx %lu (dropped %lu), "
             "tx %lu (refused %lu, errors %lu)", a ? "on" : "off",
             a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255, m >> 24, m >> 16 & 255, m >> 8 & 255,
             m & 255, (unsigned long)s_rx, (unsigned long)s_rx_drop, (unsigned long)s_tx,
             (unsigned long)s_tx_bad, (unsigned long)s_tx_err);
}

// ---- Diagnostics: the ESP32 plays the Atari --------------------------------------

void net_bridge_test(uint32_t ip)
{
    s_test_ip = ip;
    uint32_t err = net_bridge_set(ip, ip ? 0xFFFFFF00 : 0);
    ESP_LOGI(TAG, "test %s (%lu)", ip ? "on: the ESP32 answers ARP and ping for the Atari" : "off",
             (unsigned long)err);
}

static uint16_t checksum(const uint8_t *p, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += get16(p + i);
    if (len & 1)
        sum += p[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

void net_bridge_test_poll(void)
{
    static uint8_t f[NET_FRAME_MAX];
    uint32_t pending, len;
    while (s_test_ip && (len = net_bridge_rx(f, sizeof(f), &pending)) > 0) {
        uint32_t type = get16(f + 12);
        if (type == 0x0806 && len >= 42 && get16(f + 20) == 1) {         // ARP request
            memcpy(f, f + 22, 6);                                           // to the asker
            memcpy(f + 6, s_mac, 6);
            put16(f + 20, 2);                                               // reply
            uint8_t ip[4];
            memcpy(ip, f + 38, 4);
            memcpy(f + 32, f + 22, 10);                                     // target = asker
            memcpy(f + 22, s_mac, 6);
            memcpy(f + 28, ip, 4);
            net_bridge_tx(f, 42);
        } else if (type == 0x0800 && len >= 42 && f[23] == 1) {            // ICMP
            uint32_t ihl = (f[14] & 15) * 4;
            uint8_t *icmp = f + 14 + ihl;
            uint32_t total = get16(f + 16);
            if (icmp[0] != 8 || 14 + total > len)
                continue;
            uint8_t ip[4];
            memcpy(f, f + 6, 6);
            memcpy(f + 6, s_mac, 6);
            memcpy(ip, f + 26, 4);
            memcpy(f + 26, f + 30, 4);
            memcpy(f + 30, ip, 4);
            icmp[0] = 0;                                                    // echo reply
            icmp[2] = icmp[3] = 0;
            put16(icmp + 2, checksum(icmp, total - ihl));
            net_bridge_tx(f, 14 + total);
        }
    }
}
