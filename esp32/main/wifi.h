// Wi-Fi and the time from NTP, in a task on the second core, using the settings. The state
// is read with these functions; gemdrv.c hands it to GEMDRIVE and THEHULK.PRG, web.c to the
// web page. Setup mode (AP mode) opens an own, open network "<device name>-XXXX" at
// 192.168.4.1 next to the normal connection, without a restart (the Atari may be on).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    WIFI_STATE_BUSY,
    WIFI_STATE_READY,
    WIFI_STATE_FAILED,
} wifi_state_t;

typedef struct {
    char ssid[33];              // the network of the settings
    char ip[16];                // "" when not connected
    int rssi;                   // 0 when not connected
    char hostname[33];
    bool setup;                 // setup mode on
    char ap_ssid[33];           // its network
    int ap_clients;
} wifi_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    bool open;
} wifi_scan_entry_t;

void wifi_start(void);          // after settings_init
wifi_state_t wifi_link_state(void);
wifi_state_t wifi_time_state(void);
// Local time from NTP with the UTC offset of the settings; false until it is known
bool wifi_local_time(struct tm *out);

void wifi_status(wifi_status_t *out);
// Connect again with the network settings as they are now (after they were saved)
void wifi_apply(void);
// The clock settings changed: new UTC offset
void wifi_clock_changed(void);
// The device name changed: new host name (mDNS at once, DHCP from the next connection)
void wifi_name_changed(void);

// Setup mode: button 1 held 10 s, the console, or no network set at the start
void wifi_setup_start(void);
void wifi_setup_stop(void);
bool wifi_setup_active(void);
// Networks nearby, strongest first; number found (blocks a few seconds)
int wifi_scan(wifi_scan_entry_t *out, int max);
