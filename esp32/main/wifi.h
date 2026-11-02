// Wi-Fi and the time from NTP, in a task on the second core, using the settings. The state
// is read with these functions; gemdrv.c hands it to GEMDRIVE and THEHULK.PRG.
#pragma once

#include <stdbool.h>
#include <time.h>

typedef enum {
    WIFI_STATE_BUSY,
    WIFI_STATE_READY,
    WIFI_STATE_FAILED,
} wifi_state_t;

void wifi_start(void);          // after settings_init
wifi_state_t wifi_link_state(void);
wifi_state_t wifi_time_state(void);
// Local time from NTP with the UTC offset of the settings; false until it is known
bool wifi_local_time(struct tm *out);
