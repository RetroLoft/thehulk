// The network for the Atari (STinG): the Atari shares the MAC address of the ESP32 and
// has an IP address of its own. Frames for that address go to a ring for the Atari
// instead of to lwIP; frames from the Atari go out as they are. As in ACSI2TNFS
// (netbridge.c there), but on the Wi-Fi driver of the ESP32.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

#define NET_FRAME_MAX   1514    // an Ethernet frame without the FCS

// From the Wi-Fi task, on every new address: (re)install the filter
void net_bridge_install(esp_netif_t *netif, const esp_netif_ip_info_t *ip);

// The Atari's address and mask, ip 0: the bridge is off. Returns 0, or an error:
// 3 not a valid address or mask, 4 the address of the ESP32 itself.
uint32_t net_bridge_set(uint32_t ip, uint32_t mask);

// The next frame for the Atari: copied to buf, its length returned, 0 for none
uint32_t net_bridge_rx(uint8_t *buf, uint32_t size, uint32_t *pending);
// A frame from the Atari: sends it. Returns 0, or 1 when it was not accepted
uint32_t net_bridge_tx(const uint8_t *frame, uint32_t len);

uint32_t net_bridge_pending(void);      // frames waiting for the Atari
// NET_INFO for the Atari, 128 bytes as in ACSI2TNFS: "ATN", version, capabilities, state,
// MAC, the ESP32's IP, mask and gateway, the Atari's IP and mask, MTU, counters
void net_bridge_info(uint8_t *b, bool link);

void net_bridge_status(void);           // to the log: addresses and counters
// Diagnostics: the ESP32 itself answers ARP and ping for 'ip', as the Atari would
void net_bridge_test(uint32_t ip);
void net_bridge_test_poll(void);
