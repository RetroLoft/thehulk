// Wi-Fi and NTP, see wifi.h. The first connection attempt gets a limited time, because
// GEMDRIVE waits for it during the boot (18 s for the network, 15 s for the time). The
// ESP32 keeps trying to connect afterwards. NTP gets three requests of 5 s each, once
// connected, and is then skipped until the next start.
//
// Setup mode adds an access point (APSTA). While it is on, the station does not keep
// trying to connect by itself: its search over all channels would take the access point
// with it, and the phone on it would lose the page. A connection that is up stays.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "dhcpserver/dhcpserver.h"
#include "netbridge.h"
#include "settings.h"
#include "status_led.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define CONNECT_WAIT_MS   15000
#define NTP_WAIT_MS       5000
#define NTP_ATTEMPTS      3
#define BIT_GOT_IP        BIT0
#define BIT_SCAN_DONE     BIT1

static volatile wifi_state_t s_link = WIFI_STATE_BUSY;
static volatile wifi_state_t s_time = WIFI_STATE_BUSY;
static EventGroupHandle_t s_events;
static int s_utc_offset_s;
static esp_netif_t *s_sta, *s_ap;
static volatile bool s_started, s_setup, s_scanning;
static char s_ip[16];
static char s_ap_ssid[33];
static TaskHandle_t s_dns_task;

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

static void set_tz(void)
{
    // For the dates FatFs puts on the card. In the POSIX form the sign is the other way round.
    char tz[20];
    s_utc_offset_s = atoi(settings_rtc()->utc_offset) * 3600;
    snprintf(tz, sizeof(tz), "UTC%+d", -atoi(settings_rtc()->utc_offset));
    setenv("TZ", tz, 1);
    tzset();
}

void wifi_clock_changed(void)
{
    set_tz();
}

void wifi_name_changed(void)
{
    char host[33];
    settings_hostname(host, sizeof(host));
    if (s_sta)
        esp_netif_set_hostname(s_sta, host);    // DHCP: from the next connection
    mdns_hostname_set(host);
    mdns_instance_name_set(settings_device_name());
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (settings_net()->ssid[0] && !s_setup)
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        s_ip[0] = 0;
        if (s_link == WIFI_STATE_READY)
            s_link = WIFI_STATE_FAILED;
        if (!s_setup && !s_scanning && settings_net()->ssid[0]) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_events, BIT_SCAN_DONE);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, address " IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        net_bridge_install(e->esp_netif, &e->ip_info);
        s_link = WIFI_STATE_READY;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static void set_ip_config(const settings_net_t *n)
{
    if (n->use_dhcp) {
        esp_netif_dhcpc_start(s_sta);
        return;
    }
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_dns_info_t dns = { 0 };
    ip.ip.addr = ipaddr_addr(n->ip_address);
    ip.netmask.addr = ipaddr_addr(n->netmask);
    ip.gw.addr = ipaddr_addr(n->gateway);
    esp_netif_dhcpc_stop(s_sta);
    esp_netif_set_ip_info(s_sta, &ip);
    dns.ip.u_addr.ip4.addr = ipaddr_addr(n->primary_dns);
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns);
}

static void set_sta_config(const settings_net_t *n)
{
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, n->ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, n->password, sizeof(cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    set_ip_config(n);
}

void wifi_apply(void)
{
    if (!s_started)
        return;
    const settings_net_t *n = settings_net();
    ESP_LOGI(TAG, "new network settings: connecting to \"%s\"", n->ssid);
    esp_wifi_disconnect();
    s_link = WIFI_STATE_BUSY;
    set_sta_config(n);
    if (n->ssid[0])
        esp_wifi_connect();
}

// --- Setup mode ---------------------------------------------------------------------

// Every name is our own address, so phones open the page by themselves
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53),
                                .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "setup mode: no DNS server");
        if (sock >= 0)
            close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    static uint8_t buf[512];
    while (s_setup) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1)
            continue;                   // not a query with one question
        int q = 12;
        while (q < n && buf[q])
            q += buf[q] + 1;
        q += 5;                         // the end of the name, type, class
        if (q > n)
            continue;
        buf[2] = 0x81;                  // a response, recursion desired
        buf[3] = 0x80;                  // recursion available, no error
        buf[6] = 0; buf[7] = 1;         // one answer
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        static const uint8_t answer[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4,
                                            192, 168, 4, 1 };
        memcpy(buf + q, answer, sizeof(answer));
        sendto(sock, buf, q + sizeof(answer), 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

void wifi_setup_start(void)
{
    if (!s_started || s_setup)
        return;
    if (settings_net()->ssid[0])
        settings_set_ap_on(true);       // remembered; without a network it starts anyway
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%.24s-%02X%02X", settings_device_name(), mac[4], mac[5]);
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    wifi_ap_record_t sta;
    if (esp_wifi_sta_get_ap_info(&sta) == ESP_OK)
        ap.ap.channel = sta.primary;    // one radio: the channel of the connection
    else
        esp_wifi_disconnect();          // no searching while the AP is on, see above
    s_setup = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (!s_dns_task)
        xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 2, &s_dns_task, 1);
    status_led_setup(true);
    ESP_LOGW(TAG, "setup mode: network \"%s\" (open), http://192.168.4.1/", s_ap_ssid);
}

void wifi_setup_stop(void)
{
    if (!s_setup)
        return;
    s_setup = false;                    // the DNS task ends within a second
    settings_set_ap_on(false);
    esp_wifi_set_mode(WIFI_MODE_STA);
    status_led_setup(false);
    ESP_LOGW(TAG, "setup mode off");
    if (settings_net()->ssid[0] && s_link != WIFI_STATE_READY)
        esp_wifi_connect();
}

bool wifi_setup_active(void)
{
    return s_setup;
}

int wifi_scan(wifi_scan_entry_t *out, int max)
{
    if (!s_started)
        return 0;
    s_scanning = true;
    xEventGroupClearBits(s_events, BIT_SCAN_DONE);
    int n = 0;
    if (esp_wifi_scan_start(NULL, false) == ESP_OK &&
        (xEventGroupWaitBits(s_events, BIT_SCAN_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000)) & BIT_SCAN_DONE)) {
        uint16_t count = 30;
        static wifi_ap_record_t rec[30];
        if (esp_wifi_scan_get_ap_records(&count, rec) == ESP_OK) {
            for (int i = 0; i < count && n < max; i++) {
                if (!rec[i].ssid[0])
                    continue;
                bool dup = false;       // one line per network name (several access points)
                for (int j = 0; j < n && !dup; j++)
                    dup = !strcmp(out[j].ssid, (char *)rec[i].ssid);
                if (dup)
                    continue;
                strncpy(out[n].ssid, (char *)rec[i].ssid, sizeof(out[n].ssid) - 1);
                out[n].ssid[sizeof(out[n].ssid) - 1] = 0;
                out[n].rssi = rec[i].rssi;
                out[n].open = rec[i].authmode == WIFI_AUTH_OPEN;
                n++;
            }
        }
    }
    s_scanning = false;
    return n;                           // the driver sorts them strongest first
}

void wifi_status(wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->ssid, settings_net()->ssid, sizeof(out->ssid) - 1);
    strcpy(out->ip, s_ip);
    settings_hostname(out->hostname, sizeof(out->hostname));
    wifi_ap_record_t sta;
    if (s_ip[0] && esp_wifi_sta_get_ap_info(&sta) == ESP_OK)
        out->rssi = sta.rssi;
    out->setup = s_setup;
    strcpy(out->ap_ssid, s_ap_ssid);
    if (s_setup) {
        wifi_sta_list_t list;
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK)
            out->ap_clients = list.num;
    }
}

// --- Start --------------------------------------------------------------------------

static void wifi_task(void *arg)
{
    const settings_net_t *n = settings_net();
    const settings_rtc_t *r = settings_rtc();

    s_events = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    // The own network hands out itself as DNS server (dns_task answers every name with
    // 192.168.4.1): without one a phone sees no captive portal, decides the network has no
    // internet and sends everything, 192.168.4.1 too, over its mobile data
    esp_netif_dhcps_stop(s_ap);
    esp_netif_dns_info_t ap_dns = { .ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1),
                                    .ip.type = ESP_IPADDR_TYPE_V4 };
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &ap_dns);
    uint8_t offer_dns = OFFER_DNS;
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns,
                           sizeof(offer_dns));
    esp_netif_dhcps_start(s_ap);
    char host[33];
    settings_hostname(host, sizeof(host));
    esp_netif_set_hostname(s_sta, host);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    esp_wifi_set_mode(WIFI_MODE_STA);  // the mode first: set_config needs it
    set_sta_config(n);
    esp_wifi_set_ps(WIFI_PS_NONE);      // no delays on the drives later (TNFS)
    esp_wifi_start();
    s_started = true;
    web_start();
    // <host name>.local and the web page announced in the network
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(host);
        mdns_instance_name_set(settings_device_name());
        mdns_service_add(settings_device_name(), "_http", "_tcp", 80, NULL, 0);
    }

    EventBits_t bits = 0;
    if (!n->ssid[0]) {
        ESP_LOGI(TAG, "no network configured: setup mode");
        s_link = s_time = WIFI_STATE_FAILED;
        wifi_setup_start();
    } else {
        ESP_LOGI(TAG, "connecting to \"%s\" as %s", n->ssid, host);
        bits = xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(CONNECT_WAIT_MS));
        if (!(bits & BIT_GOT_IP)) {
            ESP_LOGW(TAG, "no connection yet, still trying");
            s_link = WIFI_STATE_FAILED;
        }
        // Setup mode was on: on again, next to the connection when there is one (on its
        // channel), otherwise alone (no more searching, see wifi_setup_start)
        if (settings_ap_on()) {
            ESP_LOGI(TAG, "setup mode was on: on again");
            wifi_setup_start();
        }
    }

    // The boot of the Atari does not wait for ever: report a failure, the time can still
    // come after a late connection (THEHULK.PRG takes it then)
    if (!r->enabled || !(bits & BIT_GOT_IP))
        s_time = WIFI_STATE_FAILED;
    xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdFALSE, portMAX_DELAY);
    if (!r->enabled)
        vTaskDelete(NULL);
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
    set_tz();
#ifdef HULK_NO_WIFI
    s_link = s_time = WIFI_STATE_FAILED;    // diagnostics: the radio stays off
    return;
#endif
    // On the second core: the main loop (core 0) answers the Atari
    xTaskCreatePinnedToCore(wifi_task, "wifi", 4096, NULL, 2, NULL, 1);
}
