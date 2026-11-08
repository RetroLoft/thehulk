// The web page and its API, see web.h and docs/WEB.md
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "cart.h"
#include "console.h"
#include "fpga.h"
#include "gemdrv_fs.h"
#include "sd.h"
#include "settings.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "web";

extern const char index_html[] asm("_binary_index_html_start");

#define BODY_MAX 8192

// --- Helpers ------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", code);
    cJSON_AddStringToObject(root, "message", message);
    return send_json(req, status, root);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, "200 OK", root);
}

// A changing request: JSON only (a foreign web page cannot send that without a CORS check,
// which this server never allows), parsed. NULL: the answer has been sent.
static cJSON *read_json(httpd_req_t *req)
{
    char type[48] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type));
    if (strncmp(type, "application/json", 16)) {
        send_error(req, "415 Unsupported Media Type", "UNSUPPORTED_MEDIA_TYPE", "Send application/json.");
        return NULL;
    }
    if (req->content_len > BODY_MAX) {
        send_error(req, "413 Payload Too Large", "TOO_LARGE", "The request is too large.");
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    size_t got = 0;
    while (body && got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            body = NULL;
            break;
        }
        got += n;
    }
    if (!body) {
        send_error(req, "400 Bad Request", "BAD_REQUEST", "The request could not be read.");
        return NULL;
    }
    body[got] = 0;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root)
        send_error(req, "400 Bad Request", "BAD_JSON", "The request is not valid JSON.");
    return root;
}

static const char *str_of(const cJSON *obj, const char *key, const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : dflt;
}

static int int_of(const cJSON *obj, const char *key, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

static int bool_of(const cJSON *obj, const char *key, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static void copy_str(char *dst, size_t size, const char *src)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = 0;
}

// The client is on TheHulk's own network (setup mode): 192.168.4.x
static bool from_setup_network(httpd_req_t *req)
{
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) < 0)
        return false;
    uint32_t ip;
    if (addr.sin6_family == AF_INET)
        ip = ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    else
        ip = addr.sin6_addr.un.u32_addr[3];     // IPv4-mapped
    return (ntohl(ip) & 0xFFFFFF00) == 0xC0A80400;
}

// --- Pages --------------------------------------------------------------------------

static esp_err_t page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, index_html);    // also on TheHulk's own network
}

// Unknown addresses: on TheHulk's own network to the page (phones then open it by themselves)
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (wifi_setup_active() && from_setup_network(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

// --- State and the cartridge --------------------------------------------------------

static esp_err_t get_status(httpd_req_t *req)
{
    wifi_status_t w;
    wifi_status(&w);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", settings_device_name());
    cJSON_AddStringToObject(root, "hostname", w.hostname);
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", w.ssid);
    cJSON_AddStringToObject(wifi, "ip", w.ip);
    cJSON_AddNumberToObject(wifi, "rssi", w.rssi);
    cJSON_AddBoolToObject(wifi, "setup", w.setup);
    cJSON_AddStringToObject(wifi, "setup_ssid", w.ap_ssid);
    cJSON_AddBoolToObject(wifi, "via_setup", from_setup_network(req));
    cJSON_AddBoolToObject(root, "atari_on", fpga_power_present());
    cJSON *cart = cJSON_AddObjectToObject(root, "cartridge");
    cJSON_AddBoolToObject(cart, "on", !settings_cart_off());
    cJSON_AddBoolToObject(cart, "emulator", cart_emulator_on());
    cJSON_AddStringToObject(cart, "image", cart_active_name());
    // busy: the choice is still on its way into the cartridge
    cJSON_AddBoolToObject(cart, "busy", cart_select_busy() ||
                                        (fpga_power_present() && cart_change_pending()));
    cJSON_AddStringToObject(cart, "running", cart_running_name());
    cJSON_AddBoolToObject(cart, "load_failed", cart_load_failed());
    int job = cart_select_busy() ? 0 : cart_select_result();
    cJSON_AddStringToObject(cart, "error", job ? cart_error(job) : "");
    return send_json(req, "200 OK", root);
}

static esp_err_t put_cartridge(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    if (!root)
        return ESP_OK;
    int on = bool_of(root, "on", -1);
    cJSON_Delete(root);
    if (on < 0)
        return send_error(req, "400 Bad Request", "BAD_REQUEST", "\"on\" (true or false) is missing.");
    console_set_cart_off(!on);
    return send_ok(req);
}

// --- Settings -----------------------------------------------------------------------

static const char *drive_error(int code)
{
    switch (code) {
    case SETTINGS_INVALID_DRIVE_LETTER:   return "invalid drive letter";
    case SETTINGS_DUPLICATE_DRIVE_LETTER: return "drive letter already used";
    case SETTINGS_INVALID_TYPE:           return "invalid type";
    case SETTINGS_INVALID_TRANSPORT:      return "only UDP is supported";
    case SETTINGS_INVALID_PORT:           return "invalid port";
    case SETTINGS_INVALID_HOST:           return "host is empty";
    case SETTINGS_INVALID_MOUNT_PATH:     return "mount path is empty";
    case SETTINGS_INVALID_SD_PATH:        return "SD path is empty";
    case SETTINGS_INVALID_DRIVE_STATE:    return "invalid state";
    case SETTINGS_INVALID_NAME:           return "the device name is empty";
    default:                              return "invalid setting";
    }
}

static const char *net_error(int code)
{
    switch (code) {
    case SETTINGS_NET_INVALID_SSID:     return "Wi-Fi: network name too long";
    case SETTINGS_NET_INVALID_PASSWORD: return "Wi-Fi: password too long";
    case SETTINGS_NET_INVALID_AUTH:     return "Wi-Fi: invalid security";
    case SETTINGS_NET_INVALID_COUNTRY:  return "Wi-Fi: country must be two letters";
    case SETTINGS_NET_INVALID_IP:       return "Wi-Fi: invalid IP address";
    case SETTINGS_NET_INVALID_NETMASK:  return "Wi-Fi: invalid subnet mask";
    case SETTINGS_NET_INVALID_GATEWAY:  return "Wi-Fi: invalid gateway";
    case SETTINGS_NET_INVALID_DNS:      return "Wi-Fi: invalid DNS server";
    default:                            return "Wi-Fi: invalid setting";
    }
}

static esp_err_t get_settings(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    char host[33];
    settings_hostname(host, sizeof(host));
    cJSON_AddStringToObject(root, "name", settings_device_name());
    cJSON_AddStringToObject(root, "hostname", host);
    char letter[2] = { (char)settings_flash_letter(), 0 };
    cJSON_AddStringToObject(root, "flash_letter", letter);

    cJSON *drives = cJSON_AddArrayToObject(root, "drives");
    for (int i = 0; i < SETTINGS_MAX_DRIVES; i++) {
        const settings_drive_t *d = settings_drive(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "state", d->state == SETTINGS_DRIVE_ENABLED ? "enabled" :
                                            d->state == SETTINGS_DRIVE_DISABLED ? "disabled" : "empty");
        if (d->state != SETTINGS_DRIVE_EMPTY) {
            char l[2] = { (char)d->letter, 0 };
            cJSON_AddStringToObject(o, "letter", l);
            cJSON_AddStringToObject(o, "type", d->type == SETTINGS_TYPE_TNFS ? "tnfs" : "sd");
            cJSON_AddStringToObject(o, "name", d->nickname);
            cJSON_AddStringToObject(o, "sd_path", d->sd_path);
            cJSON_AddStringToObject(o, "host", d->host);
            cJSON_AddNumberToObject(o, "port", d->port);
            cJSON_AddStringToObject(o, "mount", d->mount_path);
        }
        cJSON_AddItemToArray(drives, o);
    }

    const settings_net_t *n = settings_net();
    cJSON *net = cJSON_AddObjectToObject(root, "network");
    cJSON_AddStringToObject(net, "ssid", n->ssid);
    cJSON_AddBoolToObject(net, "password_set", n->password[0] != 0);   // never the password
    cJSON_AddNumberToObject(net, "auth_mode", n->auth_mode);
    cJSON_AddBoolToObject(net, "dhcp", n->use_dhcp);
    cJSON_AddStringToObject(net, "ip", n->ip_address);
    cJSON_AddStringToObject(net, "netmask", n->netmask);
    cJSON_AddStringToObject(net, "gateway", n->gateway);
    cJSON_AddStringToObject(net, "dns", n->primary_dns);
    cJSON_AddStringToObject(net, "country", n->country);

    const settings_rtc_t *r = settings_rtc();
    cJSON *clock = cJSON_AddObjectToObject(root, "clock");
    cJSON_AddBoolToObject(clock, "enabled", r->enabled);
    cJSON_AddStringToObject(clock, "ntp_server", r->ntp_server);
    cJSON_AddStringToObject(clock, "utc_offset", r->utc_offset);
    return send_json(req, "200 OK", root);
}

static esp_err_t bad_setting(httpd_req_t *req, const char *message)
{
    settings_restore();
    return send_error(req, "422 Unprocessable Entity", "INVALID_SETTING", message);
}

// Everything at once, like Save in THEHULK.PRG. Left out: stays as it is. The password
// only changes when one is sent.
static esp_err_t put_settings(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    if (!root)
        return ESP_OK;
    char msg[96];
    int r;
    bool net_changed = false, clock_changed = false;
    settings_snapshot();

    const char *name = str_of(root, "name", NULL);
    const cJSON *drives = cJSON_GetObjectItemCaseSensitive(root, "drives");
    const char *flash = str_of(root, "flash_letter", NULL);
    if (cJSON_IsArray(drives)) {
        for (int i = 0; i < SETTINGS_MAX_DRIVES; i++)
            settings_delete_drive(i);
    }
    if (flash) {
        char l = flash[0] >= 'a' && flash[0] <= 'z' ? flash[0] - 'a' + 'A' : flash[0];
        if ((r = settings_set_flash_letter(l)) != SETTINGS_OK) {
            snprintf(msg, sizeof(msg), "Flash drive %c: %s.", l ? l : '?', drive_error(r));
            cJSON_Delete(root);
            return bad_setting(req, msg);
        }
    }
    if (cJSON_IsArray(drives)) {
        int i = 0;
        const cJSON *o;
        cJSON_ArrayForEach(o, drives) {
            if (i >= SETTINGS_MAX_DRIVES)
                break;
            const char *state = str_of(o, "state", "empty");
            settings_drive_t d = { 0 };
            if (!strcmp(state, "empty")) {
                i++;
                continue;
            }
            d.state = !strcmp(state, "enabled") ? SETTINGS_DRIVE_ENABLED :
                      !strcmp(state, "disabled") ? SETTINGS_DRIVE_DISABLED : 99;
            const char *l = str_of(o, "letter", "");
            d.letter = l[0] >= 'a' && l[0] <= 'z' ? l[0] - 'a' + 'A' : l[0];
            d.type = !strcmp(str_of(o, "type", ""), "tnfs") ? SETTINGS_TYPE_TNFS : SETTINGS_TYPE_SD;
            copy_str(d.nickname, sizeof(d.nickname), str_of(o, "name", ""));
            copy_str(d.sd_path, sizeof(d.sd_path), str_of(o, "sd_path", ""));
            copy_str(d.host, sizeof(d.host), str_of(o, "host", ""));
            copy_str(d.mount_path, sizeof(d.mount_path), str_of(o, "mount", ""));
            d.port = int_of(o, "port", 16384);
            if ((r = settings_set_drive(i, &d)) != SETTINGS_OK) {
                snprintf(msg, sizeof(msg), "Slot %d (%c:): %s.", i + 1, d.letter ? d.letter : '?', drive_error(r));
                cJSON_Delete(root);
                return bad_setting(req, msg);
            }
            i++;
        }
    }

    const cJSON *net = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (cJSON_IsObject(net)) {
        settings_net_t n = *settings_net();
        copy_str(n.ssid, sizeof(n.ssid), str_of(net, "ssid", n.ssid));
        const char *pw = str_of(net, "password", NULL);
        if (pw && pw[0])
            copy_str(n.password, sizeof(n.password), pw);
        if (bool_of(net, "clear_password", 0))
            n.password[0] = 0;
        n.auth_mode = int_of(net, "auth_mode", n.auth_mode);
        n.use_dhcp = bool_of(net, "dhcp", n.use_dhcp);
        copy_str(n.ip_address, sizeof(n.ip_address), str_of(net, "ip", n.ip_address));
        copy_str(n.netmask, sizeof(n.netmask), str_of(net, "netmask", n.netmask));
        copy_str(n.gateway, sizeof(n.gateway), str_of(net, "gateway", n.gateway));
        copy_str(n.primary_dns, sizeof(n.primary_dns), str_of(net, "dns", n.primary_dns));
        copy_str(n.country, sizeof(n.country), str_of(net, "country", n.country));
        if (n.country[0] >= 'a') n.country[0] -= 'a' - 'A';
        if (n.country[1] >= 'a') n.country[1] -= 'a' - 'A';
        net_changed = memcmp(&n, settings_net(), sizeof(n)) != 0;
        if (net_changed && (r = settings_net_stage(&n)) != SETTINGS_NET_OK) {
            snprintf(msg, sizeof(msg), "%s.", net_error(r));
            cJSON_Delete(root);
            return bad_setting(req, msg);
        }
    }

    const cJSON *clock = cJSON_GetObjectItemCaseSensitive(root, "clock");
    if (cJSON_IsObject(clock)) {
        settings_rtc_t c = *settings_rtc();
        c.enabled = bool_of(clock, "enabled", c.enabled);
        copy_str(c.ntp_server, sizeof(c.ntp_server), str_of(clock, "ntp_server", c.ntp_server));
        copy_str(c.utc_offset, sizeof(c.utc_offset), str_of(clock, "utc_offset", c.utc_offset));
        clock_changed = memcmp(&c, settings_rtc(), sizeof(c)) != 0;
        if (clock_changed && (r = settings_rtc_stage(&c)) != SETTINGS_RTC_OK) {
            cJSON_Delete(root);
            return bad_setting(req, r == SETTINGS_RTC_INVALID_NTP ? "Clock: the time server is empty." :
                                    "Clock: the time zone must be whole hours from -12 to +14.");
        }
    }

    bool name_changed = name && strcmp(name, settings_device_name());
    if (name_changed && (r = settings_set_device_name(name)) != SETTINGS_OK) {
        cJSON_Delete(root);
        return bad_setting(req, "The device name is empty.");
    }
    cJSON_Delete(root);

    // All valid: saved
    if (settings_save() != SETTINGS_OK ||
        (net_changed && settings_net_save() != SETTINGS_NET_OK) ||
        (clock_changed && settings_rtc_save() != SETTINGS_RTC_OK))
        return send_error(req, "500 Internal Server Error", "WRITE_FAILED", "The settings could not be saved.");
    if (clock_changed)
        wifi_clock_changed();
    if (name_changed)
        wifi_name_changed();
    ESP_LOGI(TAG, "settings saved from the web page%s", net_changed ? " (new network settings)" : "");
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    cJSON_AddBoolToObject(res, "network_changed", net_changed);
    esp_err_t err = send_json(req, "200 OK", res);
    if (net_changed)
        wifi_apply();                   // after the answer: the connection may go
    return err;
}

static esp_err_t post_sd_folder(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    if (!root)
        return ESP_OK;
    char path[SETTINGS_SD_PATH_LEN];
    copy_str(path, sizeof(path), str_of(root, "path", ""));
    bool create = bool_of(root, "create", 0);
    cJSON_Delete(root);
    sd_lock();
    int r = fs_sd_folder(path, create);
    sd_unlock();
    static const char *names[] = { "there", "missing", "no_card", "created", "failed", "is_file" };
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", r >= 0 && r <= 5 ? names[r] : "failed");
    return send_json(req, "200 OK", res);
}

// --- Setup mode ---------------------------------------------------------------------

static esp_err_t get_setup(httpd_req_t *req)
{
    wifi_status_t w;
    wifi_status(&w);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", w.setup);
    cJSON_AddStringToObject(root, "name", settings_device_name());
    cJSON_AddStringToObject(root, "setup_ssid", w.ap_ssid);
    cJSON_AddStringToObject(root, "ssid", w.ssid);
    cJSON_AddStringToObject(root, "ip", w.ip);
    cJSON_AddStringToObject(root, "hostname", w.hostname);
    cJSON_AddStringToObject(root, "link", wifi_link_state() == WIFI_STATE_READY ? "connected" :
                                          wifi_link_state() == WIFI_STATE_BUSY ? "connecting" : "not_connected");
    cJSON_AddBoolToObject(root, "atari_on", fpga_power_present());
    return send_json(req, "200 OK", root);
}

static esp_err_t get_scan(httpd_req_t *req)
{
    static wifi_scan_entry_t found[24];
    int n = wifi_scan(found, 24);
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", found[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", found[i].rssi);
        cJSON_AddBoolToObject(o, "open", found[i].open);
        cJSON_AddItemToArray(list, o);
    }
    return send_json(req, "200 OK", root);
}

// Only the network name and password: the rest stays (DHCP unless set otherwise)
static esp_err_t put_setup_wifi(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    if (!root)
        return ESP_OK;
    settings_net_t n = *settings_net();
    copy_str(n.ssid, sizeof(n.ssid), str_of(root, "ssid", ""));
    copy_str(n.password, sizeof(n.password), str_of(root, "password", ""));
    cJSON_Delete(root);
    if (!n.ssid[0])
        return send_error(req, "422 Unprocessable Entity", "INVALID_SETTING", "Choose a network.");
    int r = settings_net_stage(&n);
    if (r == SETTINGS_NET_OK)
        r = settings_net_save();
    if (r != SETTINGS_NET_OK)
        return send_error(req, "422 Unprocessable Entity", "INVALID_SETTING", net_error(r));
    esp_err_t err = send_ok(req);
    wifi_apply();
    return err;
}

static esp_err_t post_setup_leave(httpd_req_t *req)
{
    esp_err_t err = send_ok(req);
    wifi_setup_stop();
    return err;
}

static esp_err_t post_setup_start(httpd_req_t *req)
{
    esp_err_t err = send_ok(req);
    wifi_setup_start();
    return err;
}

// --- Cartridge emulator (cart.c) ----------------------------------------------------

#define CATALOG_URL   "http://roms.sidecartridge.com/roms.json"
#define CATALOG_HOST  "http://roms.sidecartridge.com/"
#define CATALOG_MAX   65536
#define MAX_CARTS     100

static esp_err_t send_cart_error(httpd_req_t *req, int e)
{
    const char *status = e == CART_E_NO_CARD ? "409 Conflict" : e == CART_E_NOT_FOUND ? "404 Not Found" :
                         e == CART_E_EXISTS || e == CART_E_BUSY ? "409 Conflict" :
                         e == CART_E_IO ? "500 Internal Server Error" : "422 Unprocessable Entity";
    return send_error(req, status, "CARTRIDGE", cart_error(e));
}

static esp_err_t get_carts(httpd_req_t *req)
{
    cart_file_t *files = malloc(MAX_CARTS * sizeof(cart_file_t));
    if (!files)
        return send_error(req, "500 Internal Server Error", "NO_MEMORY", "Out of memory.");
    int n = cart_list(files, MAX_CARTS);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "emulator", cart_emulator_on());
    cJSON_AddStringToObject(root, "image", cart_active_name());
    cJSON_AddBoolToObject(root, "card", n >= 0);
    cJSON *list = cJSON_AddArrayToObject(root, "files");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", files[i].name);
        cJSON_AddNumberToObject(o, "size", files[i].size);
        cJSON_AddStringToObject(o, "kind", cart_kind_name(files[i].kind));
        cJSON_AddItemToArray(list, o);
    }
    free(files);
    return send_json(req, "200 OK", root);
}

// {"mode": "gemdrive"} or {"mode": "emulator", "image": "XENON.STC"}
static esp_err_t put_mode(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    if (!root)
        return ESP_OK;
    char mode[16], image[CART_NAME_LEN + 4];
    copy_str(mode, sizeof(mode), str_of(root, "mode", ""));
    copy_str(image, sizeof(image), str_of(root, "image", ""));
    cJSON_Delete(root);
    int r;
    if (!strcmp(mode, "gemdrive")) {
        r = cart_select(NULL);
        if (!r)
            cart_apply_now();
    }
    else if (!strcmp(mode, "emulator"))
        r = cart_select_start(image);   // status tells when it is in the cartridge
    else
        return send_error(req, "422 Unprocessable Entity", "INVALID_SETTING", "Mode: gemdrive or emulator.");
    if (r)
        return send_cart_error(req, r);
    if (settings_cart_off())
        console_set_cart_off(false);    // a cartridge is chosen: the Atari has to see it
    ESP_LOGI(TAG, "mode from the web page: %s %s", mode, image);
    return send_ok(req);
}

static bool query_name(httpd_req_t *req, char *out, size_t size)
{
    char q[64];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, "name", out, size) == ESP_OK;
}

// The file itself as the body (application/octet-stream), ?name=XENON.STC
static esp_err_t post_cart_upload(httpd_req_t *req)
{
    char type[48] = "", name[CART_NAME_LEN + 4];
    httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type));
    if (strcmp(type, "application/octet-stream"))   // as read_json: never from a foreign page
        return send_error(req, "415 Unsupported Media Type", "UNSUPPORTED_MEDIA_TYPE",
                          "Send application/octet-stream.");
    if (!query_name(req, name, sizeof(name)))
        return send_cart_error(req, CART_E_NAME);
    int r = cart_create(name, req->content_len);
    if (r)
        return send_cart_error(req, r);
    char *buf = malloc(4096);
    size_t got = 0;
    while (!r && buf && got < req->content_len) {
        int n = httpd_req_recv(req, buf, 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0) {
            r = CART_E_IO;
            break;
        }
        r = cart_write(buf, n);
        got += n;
    }
    free(buf);
    int c = cart_close(!r && got == req->content_len);
    if (!r)
        r = c;
    return r ? send_cart_error(req, r) : send_ok(req);
}

static esp_err_t delete_cart(httpd_req_t *req)
{
    char name[CART_NAME_LEN + 4];
    if (!query_name(req, name, sizeof(name)))
        return send_cart_error(req, CART_E_NAME);
    int r = cart_delete(name);
    return r ? send_cart_error(req, r) : send_ok(req);
}

static esp_err_t put_cart_rename(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    if (!root)
        return ESP_OK;
    int r = cart_rename(str_of(root, "from", ""), str_of(root, "to", ""));
    cJSON_Delete(root);
    return r ? send_cart_error(req, r) : send_ok(req);
}

// An HTTP GET from the internet: 'sink' gets the body in parts; the length or -1
static int fetch(const char *url, int max, int (*sink)(void *ctx, const char *data, int len, int total),
                 void *ctx)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000, .buffer_size = 2048 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c)
        return -1;
    int total = -1;
    char *buf = malloc(4096);
    if (buf && esp_http_client_open(c, 0) == ESP_OK) {
        int len = esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200 && len > 0 && len <= max) {
            int done = 0;
            while (done < len) {
                int n = esp_http_client_read(c, buf, 4096);
                if (n <= 0 || sink(ctx, buf, n, len))
                    break;
                done += n;
            }
            if (done == len)
                total = len;
        }
        esp_http_client_close(c);
    }
    free(buf);
    esp_http_client_cleanup(c);
    return total;
}

typedef struct { char *data; int used; } mem_sink_t;

static int to_memory(void *ctx, const char *data, int len, int total)
{
    mem_sink_t *m = ctx;
    if (!m->data && !(m->data = malloc(total + 1)))
        return -1;
    memcpy(m->data + m->used, data, len);
    m->used += len;
    return 0;
}

// The list of roms.sidecartridge.com (the browser cannot fetch it itself: no CORS there, and
// in AP mode the phone has no internet)
static esp_err_t get_catalog(httpd_req_t *req)
{
    mem_sink_t m = { 0 };
    int len = fetch(CATALOG_URL, CATALOG_MAX, to_memory, &m);
    if (len < 0) {
        free(m.data);
        return send_error(req, "502 Bad Gateway", "NO_CATALOG",
                          "The list from roms.sidecartridge.com could not be fetched. Is TheHulk connected to the internet?");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, m.data, len);
    free(m.data);
    return err;
}

typedef struct {
    int r;                              // 1: nothing came yet, then 0 or a CART_E_* error
    char name[CART_NAME_LEN + 4];
} download_t;

static int to_card(void *ctx, const char *data, int len, int total)
{
    download_t *d = ctx;
    if (d->r == 1)                      // the first part: now the size is known
        d->r = cart_create(d->name, total);
    if (!d->r)
        d->r = cart_write(data, len);
    return d->r;
}

// {"url": "http://roms.sidecartridge.com/Xenon.STC", "name": "XENON.STC"}
static esp_err_t post_cart_download(httpd_req_t *req)
{
    cJSON *root = read_json(req);
    if (!root)
        return ESP_OK;
    char url[256];
    download_t st = { .r = 1 };
    copy_str(url, sizeof(url), str_of(root, "url", ""));
    copy_str(st.name, sizeof(st.name), str_of(root, "name", ""));
    cJSON_Delete(root);
    if (strncmp(url, CATALOG_HOST, strlen(CATALOG_HOST)) || strchr(url, '?'))
        return send_error(req, "422 Unprocessable Entity", "INVALID_URL", "Only images from roms.sidecartridge.com.");
    char check[CART_NAME_LEN + 4];
    copy_str(check, sizeof(check), st.name);
    if (strlen(st.name) >= CART_NAME_LEN || !cart_name_ok(check))
        return send_cart_error(req, CART_E_NAME);
    int len = fetch(url, CART_STC_SIZE, to_card, &st);
    if (st.r == 1)                      // nothing came
        return send_error(req, "502 Bad Gateway", "DOWNLOAD_FAILED",
                          "The image could not be downloaded. Is TheHulk connected to the internet?");
    int c = cart_close(!st.r && len > 0);
    int r = st.r ? st.r : len < 0 ? CART_E_IO : c;
    return r ? send_cart_error(req, r) : send_ok(req);
}

// --- Start --------------------------------------------------------------------------

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.core_id = 1;                    // the first core answers the Atari
    cfg.max_uri_handlers = 20;
    cfg.lru_purge_enable = true;
    httpd_handle_t server;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "no web server");
        return;
    }
    static const httpd_uri_t uris[] = {
        { "/",                       HTTP_GET,  page,             NULL },
        { "/api/v1/status",          HTTP_GET,  get_status,       NULL },
        { "/api/v1/cartridge",       HTTP_PUT,  put_cartridge,    NULL },
        { "/api/v1/settings",        HTTP_GET,  get_settings,     NULL },
        { "/api/v1/settings",        HTTP_PUT,  put_settings,     NULL },
        { "/api/v1/sd-folder",       HTTP_POST, post_sd_folder,   NULL },
        { "/api/v1/setup",           HTTP_GET,  get_setup,        NULL },
        { "/api/v1/setup/scan",      HTTP_GET,  get_scan,         NULL },
        { "/api/v1/setup/wifi",      HTTP_PUT,  put_setup_wifi,   NULL },
        { "/api/v1/setup/leave",     HTTP_POST, post_setup_leave, NULL },
        { "/api/v1/setup/start",     HTTP_POST, post_setup_start, NULL },
        { "/api/v1/mode",            HTTP_PUT,  put_mode,         NULL },
        { "/api/v1/carts",           HTTP_GET,  get_carts,        NULL },
        { "/api/v1/carts",           HTTP_DELETE, delete_cart,    NULL },
        { "/api/v1/carts/upload",    HTTP_POST, post_cart_upload, NULL },
        { "/api/v1/carts/rename",    HTTP_PUT,  put_cart_rename,  NULL },
        { "/api/v1/carts/catalog",   HTTP_GET,  get_catalog,      NULL },
        { "/api/v1/carts/download",  HTTP_POST, post_cart_download, NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "web server on port 80");
}
