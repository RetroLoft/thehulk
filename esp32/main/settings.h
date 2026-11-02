// The settings of the cartridge, kept in NVS: the drives, the letter of the flash drive,
// the network and the clock. Edited by the configuration program (atari/config, commands
// in gemdrv.c), in a RAM copy until it saves. Drives take effect at the next start.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_MAX_DRIVES   8
#define SETTINGS_NICK_LEN     24
#define SETTINGS_HOST_LEN     64
#define SETTINGS_MOUNT_LEN    32
#define SETTINGS_SD_PATH_LEN  64

#define SETTINGS_DRIVE_EMPTY     0
#define SETTINGS_DRIVE_DISABLED  1
#define SETTINGS_DRIVE_ENABLED   2
#define SETTINGS_TYPE_SD         1
#define SETTINGS_TYPE_TNFS       2

// Status codes of the configuration program (sidetnfs_probe.h)
#define SETTINGS_OK                     0
#define SETTINGS_INVALID_INDEX          1
#define SETTINGS_INVALID_DRIVE_LETTER   3
#define SETTINGS_DUPLICATE_DRIVE_LETTER 4
#define SETTINGS_INVALID_TYPE           5
#define SETTINGS_INVALID_TRANSPORT      6
#define SETTINGS_INVALID_PORT           7
#define SETTINGS_INVALID_HOST           8
#define SETTINGS_INVALID_MOUNT_PATH     9
#define SETTINGS_INVALID_SD_PATH        10
#define SETTINGS_WRITE_FAILED           12
#define SETTINGS_INVALID_DRIVE_STATE    15

#define SETTINGS_NET_OK               0
#define SETTINGS_NET_INVALID_SSID     1
#define SETTINGS_NET_INVALID_PASSWORD 2
#define SETTINGS_NET_INVALID_AUTH     3
#define SETTINGS_NET_INVALID_COUNTRY  4
#define SETTINGS_NET_INVALID_DHCP     5
#define SETTINGS_NET_INVALID_IP       6
#define SETTINGS_NET_INVALID_NETMASK  7
#define SETTINGS_NET_INVALID_GATEWAY  8
#define SETTINGS_NET_INVALID_DNS      9
#define SETTINGS_NET_NOT_STAGED       10
#define SETTINGS_NET_WRITE_FAILED     11

#define SETTINGS_RTC_OK               0
#define SETTINGS_RTC_INVALID_ENABLED  1
#define SETTINGS_RTC_INVALID_NTP      2
#define SETTINGS_RTC_INVALID_OFFSET   3
#define SETTINGS_RTC_NOT_STAGED       4
#define SETTINGS_RTC_WRITE_FAILED     5

typedef struct {
    uint16_t state, letter, type, transport, port;
    char nickname[SETTINGS_NICK_LEN];
    char host[SETTINGS_HOST_LEN];
    char mount_path[SETTINGS_MOUNT_LEN];
    char sd_path[SETTINGS_SD_PATH_LEN];
} settings_drive_t;

typedef struct {
    uint16_t auth_mode, use_dhcp;
    char ssid[36], password[68], country[4];
    char ip_address[16], netmask[16], gateway[16], primary_dns[16];
} settings_net_t;

typedef struct {
    uint16_t enabled;
    char ntp_server[64], utc_offset[4];
} settings_rtc_t;

void settings_init(void);       // after nvs_flash_init

uint16_t settings_flash_letter(void);
const settings_drive_t *settings_drive(int index);
int settings_drive_count(void); // configured: disabled or enabled

int settings_set_drive(uint32_t index, const settings_drive_t *in);
int settings_delete_drive(uint32_t index);
int settings_set_flash_letter(uint32_t letter);
int settings_save(void);

const settings_net_t *settings_net(void);
int settings_net_stage(const settings_net_t *in);
int settings_net_save(void);

const settings_rtc_t *settings_rtc(void);
int settings_rtc_stage(const settings_rtc_t *in);
int settings_rtc_save(void);
