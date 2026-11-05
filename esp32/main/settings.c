// The settings of the cartridge, see settings.h. The rules follow the firmware the
// configuration program was written for (sidetnfs_config.c, _netconfig.c, _rtcconfig.c).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "settings.h"

static const char *TAG = "settings";

#define NVS_NS       "settings"
#define NVS_KEY      "all"
#define BLOB_VERSION 1

typedef struct {
    uint32_t version;
    uint16_t flash_letter;
    settings_drive_t drives[SETTINGS_MAX_DRIVES];
    settings_net_t net;
    settings_rtc_t rtc;
} blob_t;

static blob_t s_cfg;            // what the program sees, saved as a whole
static settings_net_t s_net_staged;
static settings_rtc_t s_rtc_staged;
static bool s_net_is_staged, s_rtc_is_staged;

static void load_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.version = BLOB_VERSION;
    s_cfg.flash_letter = 'S';
    settings_drive_t *d = &s_cfg.drives[0];
    d->state = SETTINGS_DRIVE_ENABLED;
    d->letter = 'N';
    d->type = SETTINGS_TYPE_SD;
    strcpy(d->nickname, "SD card");
    strcpy(d->sd_path, "/HD");
    s_cfg.net.use_dhcp = 1;
    s_cfg.net.auth_mode = 3;
    strcpy(s_cfg.net.country, "XX");
    strcpy(s_cfg.net.primary_dns, "8.8.8.8");
    s_cfg.rtc.enabled = 1;
    strcpy(s_cfg.rtc.ntp_server, "pool.ntp.org");
    strcpy(s_cfg.rtc.utc_offset, "+1");
}

void settings_init(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_cfg);
    load_defaults();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return;                 // nothing saved yet
    blob_t *saved = malloc(sizeof(*saved));
    if (saved && nvs_get_blob(h, NVS_KEY, saved, &len) == ESP_OK &&
        len == sizeof(*saved) && saved->version == BLOB_VERSION)
        s_cfg = *saved;
    free(saved);
    nvs_close(h);
    ESP_LOGI(TAG, "%d drives, flash drive %c:", settings_drive_count(), s_cfg.flash_letter);
}

static bool write_blob(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_blob(h, NVS_KEY, &s_cfg, sizeof(s_cfg)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

uint16_t settings_flash_letter(void)
{
    return s_cfg.flash_letter;
}

const settings_drive_t *settings_drive(int index)
{
    return &s_cfg.drives[index];
}

int settings_drive_count(void)
{
    int n = 0;
    for (int i = 0; i < SETTINGS_MAX_DRIVES; i++)
        n += s_cfg.drives[i].state != SETTINGS_DRIVE_EMPTY;
    return n;
}

static bool valid_letter(uint32_t letter)
{
    return letter >= 'C' && letter <= 'Z';  // not the floppy drives
}

int settings_set_drive(uint32_t index, const settings_drive_t *in)
{
    if (index >= SETTINGS_MAX_DRIVES)
        return SETTINGS_INVALID_INDEX;
    if (in->state == SETTINGS_DRIVE_EMPTY)
        return settings_delete_drive(index);
    if (in->state != SETTINGS_DRIVE_DISABLED && in->state != SETTINGS_DRIVE_ENABLED)
        return SETTINGS_INVALID_DRIVE_STATE;

    settings_drive_t d = *in;
    d.nickname[sizeof(d.nickname) - 1] = 0;
    d.host[sizeof(d.host) - 1] = 0;
    d.mount_path[sizeof(d.mount_path) - 1] = 0;
    d.sd_path[sizeof(d.sd_path) - 1] = 0;
    if (!valid_letter(d.letter))
        return SETTINGS_INVALID_DRIVE_LETTER;
    if (d.type == SETTINGS_TYPE_TNFS) {
        if (d.transport > 1)
            return SETTINGS_INVALID_TRANSPORT;
        if (!d.port)
            return SETTINGS_INVALID_PORT;
        if (!d.host[0])
            return SETTINGS_INVALID_HOST;
        if (!d.mount_path[0])
            return SETTINGS_INVALID_MOUNT_PATH;
        memset(d.sd_path, 0, sizeof(d.sd_path));
    } else if (d.type == SETTINGS_TYPE_SD) {
        if (!d.sd_path[0])
            return SETTINGS_INVALID_SD_PATH;
        d.transport = d.port = 0;
        memset(d.host, 0, sizeof(d.host));
        memset(d.mount_path, 0, sizeof(d.mount_path));
    } else {
        return SETTINGS_INVALID_TYPE;
    }
    if (d.letter == s_cfg.flash_letter)
        return SETTINGS_DUPLICATE_DRIVE_LETTER;
    for (uint32_t i = 0; i < SETTINGS_MAX_DRIVES; i++)
        if (i != index && s_cfg.drives[i].state != SETTINGS_DRIVE_EMPTY && s_cfg.drives[i].letter == d.letter)
            return SETTINGS_DUPLICATE_DRIVE_LETTER;
    s_cfg.drives[index] = d;
    return SETTINGS_OK;
}

int settings_delete_drive(uint32_t index)
{
    if (index >= SETTINGS_MAX_DRIVES)
        return SETTINGS_INVALID_INDEX;
    memset(&s_cfg.drives[index], 0, sizeof(s_cfg.drives[index]));
    return SETTINGS_OK;
}

int settings_set_flash_letter(uint32_t letter)
{
    if (!valid_letter(letter))
        return SETTINGS_INVALID_DRIVE_LETTER;
    for (int i = 0; i < SETTINGS_MAX_DRIVES; i++)
        if (s_cfg.drives[i].state != SETTINGS_DRIVE_EMPTY && s_cfg.drives[i].letter == letter)
            return SETTINGS_DUPLICATE_DRIVE_LETTER;
    s_cfg.flash_letter = letter;
    return SETTINGS_OK;
}

int settings_save(void)
{
    return write_blob() ? SETTINGS_OK : SETTINGS_WRITE_FAILED;
}

// --- Network ------------------------------------------------------------------------

static bool valid_ipv4(const char *s, size_t size)
{
    if (strnlen(s, size) >= size)
        return false;
    int parts = 0;
    while (*s) {
        if (!isdigit((unsigned char)*s))
            return false;
        int v = 0;
        while (isdigit((unsigned char)*s) && v < 1000)
            v = v * 10 + (*s++ - '0');
        if (v > 255 || ++parts > 4)
            return false;
        if (*s == '.' && *++s == 0)
            return false;
    }
    return parts == 4;
}

const settings_net_t *settings_net(void)
{
    return &s_cfg.net;
}

int settings_net_stage(const settings_net_t *in)
{
    if (strnlen(in->ssid, sizeof(in->ssid)) > 32)
        return SETTINGS_NET_INVALID_SSID;
    if (strnlen(in->password, sizeof(in->password)) > 64)
        return SETTINGS_NET_INVALID_PASSWORD;
    if (in->auth_mode > 8)
        return SETTINGS_NET_INVALID_AUTH;
    if (strnlen(in->country, sizeof(in->country)) != 2 ||
        !isalpha((unsigned char)in->country[0]) || !isalpha((unsigned char)in->country[1]))
        return SETTINGS_NET_INVALID_COUNTRY;
    if (in->use_dhcp > 1)
        return SETTINGS_NET_INVALID_DHCP;
    if (!in->use_dhcp) {
        if (!valid_ipv4(in->ip_address, sizeof(in->ip_address)))
            return SETTINGS_NET_INVALID_IP;
        if (!valid_ipv4(in->netmask, sizeof(in->netmask)))
            return SETTINGS_NET_INVALID_NETMASK;
        if (!valid_ipv4(in->gateway, sizeof(in->gateway)))
            return SETTINGS_NET_INVALID_GATEWAY;
        if (!valid_ipv4(in->primary_dns, sizeof(in->primary_dns)))
            return SETTINGS_NET_INVALID_DNS;
    }
    s_net_staged = *in;
    s_net_is_staged = true;
    return SETTINGS_NET_OK;
}

int settings_net_save(void)
{
    if (!s_net_is_staged)
        return SETTINGS_NET_NOT_STAGED;
    s_cfg.net = s_net_staged;
    s_net_is_staged = false;
    return write_blob() ? SETTINGS_NET_OK : SETTINGS_NET_WRITE_FAILED;
}

// --- Clock --------------------------------------------------------------------------

const settings_rtc_t *settings_rtc(void)
{
    return &s_cfg.rtc;
}

// "+1", "-5", "2": whole hours from -12 to +14. Stored with its sign.
static bool normalize_utc_offset(char field[4])
{
    const char *p = field;
    int sign = 1;
    if (strnlen(field, 4) >= 4)
        return false;
    if (*p == '+' || *p == '-')
        sign = *p++ == '-' ? -1 : 1;
    if (!isdigit((unsigned char)p[0]) || (p[1] && (!isdigit((unsigned char)p[1]) || p[2])))
        return false;
    int value = sign * atoi(p);
    if (value < -12 || value > 14)
        return false;
    snprintf(field, 4, "%+d", value);
    return true;
}

int settings_rtc_stage(const settings_rtc_t *in)
{
    settings_rtc_t r = *in;
    if (r.enabled > 1)
        return SETTINGS_RTC_INVALID_ENABLED;
    if (strnlen(r.ntp_server, sizeof(r.ntp_server)) >= sizeof(r.ntp_server) || (r.enabled && !r.ntp_server[0]))
        return SETTINGS_RTC_INVALID_NTP;
    if (!normalize_utc_offset(r.utc_offset))
        return SETTINGS_RTC_INVALID_OFFSET;
    s_rtc_staged = r;
    s_rtc_is_staged = true;
    return SETTINGS_RTC_OK;
}

int settings_rtc_save(void)
{
    if (!s_rtc_is_staged)
        return SETTINGS_RTC_NOT_STAGED;
    s_cfg.rtc = s_rtc_staged;
    s_rtc_is_staged = false;
    return write_blob() ? SETTINGS_RTC_OK : SETTINGS_RTC_WRITE_FAILED;
}

// Separate from the blob: switching the cartridge off must not reset the settings
#define NVS_KEY_CART_OFF "cart_off"

static int s_cart_off = -1;     // -1: not read from NVS yet

bool settings_cart_off(void)
{
    if (s_cart_off < 0) {
        nvs_handle_t h;
        uint8_t off = 0;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, NVS_KEY_CART_OFF, &off);
            nvs_close(h);
        }
        s_cart_off = off != 0;
    }
    return s_cart_off;
}

bool settings_set_cart_off(bool off)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_u8(h, NVS_KEY_CART_OFF, off) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok)
        s_cart_off = off;
    return ok;
}
