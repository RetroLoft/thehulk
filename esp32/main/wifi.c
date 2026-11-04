// Wi-Fi and NTP, see wifi.h. The first connection attempt gets a limited time, because
// GEMDRIVE waits for it during the boot (18 s for the network, 15 s for the time). The
// ESP32 keeps trying to connect afterwards. NTP gets three requests of 5 s each, once
// connected, and is then skipped until the next start.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define CONNECT_WAIT_MS   15000
#define NTP_WAIT_MS       5000
#define NTP_ATTEMPTS      3
#define BIT_GOT_IP        BIT0

static volatile wifi_state_t s_link = WIFI_STATE_BUSY;
static volatile wifi_state_t s_time = WIFI_STATE_BUSY;
static EventGroupHandle_t s_events;
static int s_utc_offset_s;

wifi_state_t wifi_link_state(void)
{
    return s_link;
}

wifi_state_t wifi_time_state(void)
{
    return s_time;
}

bool wifi_local_time(struct tm *out)
{
    if (s_time != WIFI_STATE_READY)
        return false;
    time_t now = time(NULL) + s_utc_offset_s;
    gmtime_r(&now, out);
    return true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        if (s_link == WIFI_STATE_READY)
            s_link = WIFI_STATE_FAILED;
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, address " IPSTR, IP2STR(&e->ip_info.ip));
        s_link = WIFI_STATE_READY;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static void set_static_ip(esp_netif_t *netif, const settings_net_t *n)
{
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_dns_info_t dns = { 0 };
    ip.ip.addr = ipaddr_addr(n->ip_address);
    ip.netmask.addr = ipaddr_addr(n->netmask);
    ip.gw.addr = ipaddr_addr(n->gateway);
    esp_netif_dhcpc_stop(netif);
    esp_netif_set_ip_info(netif, &ip);
    dns.ip.u_addr.ip4.addr = ipaddr_addr(n->primary_dns);
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
}

static void wifi_task(void *arg)
{
    const settings_net_t *n = settings_net();
    const settings_rtc_t *r = settings_rtc();
    s_utc_offset_s = atoi(r->utc_offset) * 3600;

    if (!n->ssid[0]) {
        ESP_LOGI(TAG, "no network configured");
        s_link = s_time = WIFI_STATE_FAILED;
        vTaskDelete(NULL);
    }

    s_events = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, n->ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, n->password, sizeof(cfg.sta.password));
    if (!n->use_dhcp)
        set_static_ip(netif, n);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_set_ps(WIFI_PS_NONE);      // no delays on the drives later (TNFS)
    esp_wifi_start();
    ESP_LOGI(TAG, "connecting to \"%s\"", n->ssid);

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_WAIT_MS));
    if (!(bits & BIT_GOT_IP)) {
        ESP_LOGW(TAG, "no connection yet, still trying");
        s_link = WIFI_STATE_FAILED;
    }

    if (!r->enabled) {
        s_time = WIFI_STATE_FAILED;
        vTaskDelete(NULL);
    }

    // The boot of the Atari does not wait for ever: report a failure, the time can still
    // come after a late connection (THEHULK.PRG takes it then)
    if (!(bits & BIT_GOT_IP))
        s_time = WIFI_STATE_FAILED;
    xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdFALSE, portMAX_DELAY);
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(r->ntp_server);
    esp_netif_sntp_init(&sntp);
    for (int i = 0; i < NTP_ATTEMPTS; i++) {
        if (i)
            esp_sntp_restart();         // a new request
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_WAIT_MS)) == ESP_OK) {
            s_time = WIFI_STATE_READY;
            ESP_LOGI(TAG, "time from %s, UTC offset %s", r->ntp_server, r->utc_offset);
            vTaskDelete(NULL);
        }
    }
    ESP_LOGW(TAG, "no answer from %s, skipped", r->ntp_server);
    esp_netif_sntp_deinit();
    s_time = WIFI_STATE_FAILED;
    vTaskDelete(NULL);
}

void wifi_start(void)
{
    // For the dates FatFs puts on the card. In the POSIX form the sign is the other way round.
    char tz[20];
    snprintf(tz, sizeof(tz), "UTC%+d", -atoi(settings_rtc()->utc_offset));
    setenv("TZ", tz, 1);
    tzset();

#ifdef HULK_NO_WIFI
    s_link = s_time = WIFI_STATE_FAILED;    // diagnostics: the radio stays off
    return;
#endif
    // On the second core: the main loop (core 0) answers the Atari
    xTaskCreatePinnedToCore(wifi_task, "wifi", 4096, NULL, 2, NULL, 1);
}
