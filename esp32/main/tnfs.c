// TNFS client, see tnfs.h. Runs in the main task: each call waits for its answer, while
// lwIP runs on the second core. Three tries of 400 ms stay well within the 5 s GEMDRIVE
// waits for an answer.
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "gemdrv_layout.h"
#include "status_led.h"
#include "tnfs.h"
#include "wifi.h"

static const char *TAG = "tnfs";

#define CMD_MOUNT     0x00
#define CMD_OPENDIRX  0x17
#define CMD_READDIRX  0x18
#define CMD_CLOSEDIR  0x12
#define CMD_READ      0x21
#define CMD_WRITE     0x22
#define CMD_CLOSE     0x23
#define CMD_STAT      0x24
#define CMD_LSEEK     0x25
#define CMD_UNLINK    0x26
#define CMD_RENAME    0x28
#define CMD_MKDIR     0x13
#define CMD_RMDIR     0x14
#define CMD_FREE      0x31
#define CMD_OPEN      0x29

#define ST_OK             0x00
#define ST_EPERM          0x01
#define ST_ENOENT         0x02
#define ST_EEXIST         0x11
#define ST_ENOTEMPTY      0x27
#define ST_EACCES         0x06
#define ST_EOF            0x21
#define ST_BAD_SESSION    0xFF

#define TRIES        3
#define TIMEOUT_MS   400
#define READ_MAX     512
#define RETRY_US     (5 * 1000000)  // after a failed mount, so the Atari is not held up

typedef struct {
    char host[64], mount[32];
    uint16_t port;
    bool mounted;
    uint16_t conn;
    struct sockaddr_in addr;
    bool have_addr;
    int64_t next_try;
    tnfs_problem_t problem;         // why the last mount failed
    uint8_t refused;                // the TNFS error of a refused mount
} slot_t;

static slot_t s_slots[TNFS_MAX_SLOTS];
static int s_sock = -1;
static uint8_t s_seq;
static uint8_t s_rx[READ_MAX + 64];

void tnfs_set_slot(int slot, const char *host, uint16_t port, const char *mount)
{
    slot_t *s = &s_slots[slot];
    memset(s, 0, sizeof(*s));
    strncpy(s->host, host, sizeof(s->host) - 1);
    strncpy(s->mount, mount, sizeof(s->mount) - 1);
    s->port = port;
}

static int to_gemdos(uint8_t status)
{
    switch (status) {
    case ST_ENOENT: return GEMDOS_EFILNF;
    case ST_EPERM:
    case ST_EACCES:
    case ST_EEXIST:
    case ST_ENOTEMPTY: return GEMDOS_EACCDN;
    default:        return GEMDOS_ERROR;
    }
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// One request and its answer in s_rx: its length, or -1 without an answer
static int request(slot_t *s, uint8_t cmd, const void *data, size_t len)
{
    static uint8_t tx[READ_MAX + 64];
    if (s_sock < 0 || len + 4 > sizeof(tx))
        return -1;
    uint8_t seq = ++s_seq;
    tx[0] = s->conn & 0xFF;
    tx[1] = s->conn >> 8;
    tx[2] = seq;
    tx[3] = cmd;
    if (len)
        memcpy(tx + 4, data, len);
    for (int t = 0; t < TRIES; t++) {
        if (sendto(s_sock, tx, len + 4, 0, (struct sockaddr *)&s->addr, sizeof(s->addr)) < 0)
            return -1;
        int64_t until = esp_timer_get_time() + TIMEOUT_MS * 1000;
        while (esp_timer_get_time() < until) {
            int n = recv(s_sock, s_rx, sizeof(s_rx), 0);
            if (n >= 5 && s_rx[2] == seq && s_rx[3] == cmd)
                return n;           // an older answer is skipped
            if (n < 0)
                break;
        }
    }
    return -1;
}

static bool open_socket(void)
{
    if (s_sock >= 0)
        return true;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0)
        return false;
    struct timeval tv = { 0, TIMEOUT_MS * 1000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

static bool resolve(slot_t *s)
{
    if (s->have_addr)
        return true;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(s->host, NULL, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "host %s not found", s->host);
        return false;
    }
    s->addr = *(struct sockaddr_in *)res->ai_addr;
    s->addr.sin_port = htons(s->port);
    freeaddrinfo(res);
    s->have_addr = true;
    return true;
}

static bool mount(slot_t *s)
{
    if (s->mounted)
        return true;
    if (wifi_link_state() != WIFI_STATE_READY) {
        s->problem = TNFS_NO_WIFI;
        return false;
    }
    if (esp_timer_get_time() < s->next_try)
        return false;
    s->next_try = esp_timer_get_time() + RETRY_US;
    if (!open_socket()) {
        s->problem = TNFS_NO_ANSWER;
        return false;
    }
    if (!resolve(s)) {
        s->problem = TNFS_NO_HOST;
        return false;
    }
    uint8_t m[64];
    size_t pl = strlen(s->mount);
    m[0] = 0x02;                    // version 1.2: minor, major
    m[1] = 0x01;
    memcpy(m + 2, s->mount, pl + 1);
    m[3 + pl] = 0;                  // no user
    m[4 + pl] = 0;                  // no password
    s->conn = 0;
    int n = request(s, CMD_MOUNT, m, 5 + pl);
    if (n < 0) {
        ESP_LOGW(TAG, "no answer from %s:%u", s->host, s->port);
        s->problem = TNFS_NO_ANSWER;
        return false;
    }
    if (s_rx[4] != ST_OK) {
        ESP_LOGW(TAG, "mount of %s refused: 0x%02x", s->mount, s_rx[4]);
        s->problem = TNFS_REFUSED;
        s->refused = s_rx[4];
        return false;
    }
    s->conn = s_rx[0] | (s_rx[1] << 8);
    s->mounted = true;
    ESP_LOGI(TAG, "mounted %s:%s", s->host, s->mount);
    return true;
}

bool tnfs_ready(int slot)
{
    return slot >= 0 && slot < TNFS_MAX_SLOTS && mount(&s_slots[slot]);
}

// Shown on the Atari as NET_ERR.TXT (or NET_WAIT.TXT while Wi-Fi still connects), the way
// acsi2tnfs and SideTNFS do it
size_t tnfs_problem_text(int slot, char *out, size_t size, bool *waiting)
{
    if (slot < 0 || slot >= TNFS_MAX_SLOTS || !size)
        return 0;
    const slot_t *s = &s_slots[slot];
    wifi_status_t w;
    wifi_status(&w);
    char reason[96], advice[320];
    *waiting = false;
    if (s->problem == TNFS_NO_WIFI && w.setup) {
        snprintf(reason, sizeof(reason), "TheHulk is in AP mode, no Wi-Fi");
        snprintf(advice, sizeof(advice),
                 "TheHulk has its own network \"%s\" for its web\r\n"
                 "page (http://192.168.4.1) and is not connected to your\r\n"
                 "Wi-Fi. Switch AP mode off on the web page, or hold\r\n"
                 "button 1 for 10 seconds, then reset the Atari.\r\n", w.ap_ssid);
    } else if (s->problem == TNFS_NO_WIFI && !w.ssid[0]) {
        snprintf(reason, sizeof(reason), "no Wi-Fi network set");
        snprintf(advice, sizeof(advice),
                 "Set the Wi-Fi network in THEHULK.PRG or on the web\r\n"
                 "page, then reset the Atari.\r\n");
    } else if (s->problem == TNFS_NO_WIFI && wifi_link_state() == WIFI_STATE_BUSY) {
        *waiting = true;
        snprintf(reason, sizeof(reason), "still connecting to Wi-Fi \"%s\"", w.ssid);
        snprintf(advice, sizeof(advice),
                 "Close this window and open the drive again in a\r\n"
                 "moment.\r\n");
    } else if (s->problem == TNFS_NO_WIFI) {
        snprintf(reason, sizeof(reason), "no connection with Wi-Fi \"%s\"", w.ssid);
        snprintf(advice, sizeof(advice),
                 "Check that the network is in range and that the\r\n"
                 "password is right. TheHulk keeps trying: close this\r\n"
                 "window and open the drive again later.\r\n");
    } else if (s->problem == TNFS_NO_HOST) {
        snprintf(reason, sizeof(reason), "server name not found");
        snprintf(advice, sizeof(advice),
                 "Check the server name in THEHULK.PRG or on the web\r\n"
                 "page.\r\n");
    } else if (s->problem == TNFS_REFUSED) {
        snprintf(reason, sizeof(reason), "mount refused (TNFS error 0x%02X)", s->refused);
        snprintf(advice, sizeof(advice),
                 "Check that the folder exists on the server.\r\n");
    } else {
        snprintf(reason, sizeof(reason), "no answer from the server");
        snprintf(advice, sizeof(advice),
                 "Check that the TNFS server runs and can be reached,\r\n"
                 "and that the address and port are right. Close this\r\n"
                 "window and open the drive again to try once more.\r\n");
    }
    int n = snprintf(out, size,
                     "TheHulk: network drive not %s\r\n"
                     "\r\n"
                     "Reason : %s\r\n"
                     "Server : %s:%u\r\n"
                     "Folder : %s\r\n"
                     "\r\n"
                     "%s",
                     *waiting ? "ready yet" : "available", reason, s->host, s->port,
                     s->mount[0] ? s->mount : "/", advice);
    return n < 0 ? 0 : (size_t)n >= size ? size - 1 : (size_t)n;
}

// A request on a mounted session: mounts again when the server lost the session
static int call(int slot, uint8_t cmd, const void *data, size_t len)
{
    slot_t *s = &s_slots[slot];
    int n = -1;
    if (slot >= 0 && slot < TNFS_MAX_SLOTS && mount(s)) {
        n = request(s, cmd, data, len);
        if (n >= 5 && s_rx[4] == ST_BAD_SESSION) {
            s->mounted = false;
            s->next_try = 0;
            n = mount(s) ? request(s, cmd, data, len) : -1;
        }
    }
    if (n < 0)
        status_led_error();
    return n;
}

int tnfs_stat(int slot, const char *path, uint32_t *size, uint32_t *mtime, bool *is_dir)
{
    int n = call(slot, CMD_STAT, path, strlen(path) + 1);
    if (n < 0)
        return GEMDOS_EPTHNF;
    if (s_rx[4] != ST_OK || n < 5 + 22)
        return to_gemdos(s_rx[4]);
    *is_dir = ((s_rx[5] | (s_rx[6] << 8)) & 0170000) == 0040000;
    *size = le32(s_rx + 11);
    *mtime = le32(s_rx + 19);
    return 0;
}

int tnfs_dir_open(int slot, const char *path, uint8_t *handle)
{
    uint8_t req[5 + 256];
    size_t pl = strlen(path);
    if (pl >= 256)
        return GEMDOS_EPTHNF;
    memset(req, 0, 5);              // no ., .. or hidden names; default order; no pattern
    memcpy(req + 5, path, pl + 1);
    int n = call(slot, CMD_OPENDIRX, req, 5 + pl + 1);
    if (n < 0 || s_rx[4] != ST_OK || n < 8)
        return GEMDOS_EPTHNF;
    *handle = s_rx[5];
    return 0;
}

int tnfs_dir_next(int slot, uint8_t handle, tnfs_entry_t *e)
{
    uint8_t req[2] = { handle, 1 }; // one entry per request
    int n = call(slot, CMD_READDIRX, req, sizeof(req));
    if (n < 0)
        return GEMDOS_ERROR;
    if (s_rx[4] == ST_EOF)
        return 0;
    if (s_rx[4] != ST_OK || n < 9 || s_rx[5] == 0)
        return s_rx[4] == ST_OK ? 0 : to_gemdos(s_rx[4]);
    const uint8_t *p = s_rx + 9;
    if (n < 9 + 14)
        return GEMDOS_ERROR;
    e->is_dir = p[0] & 1;
    e->size = le32(p + 1);
    e->mtime = le32(p + 5);
    strncpy(e->name, (const char *)p + 13, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = 0;
    return 1;
}

void tnfs_dir_close(int slot, uint8_t handle)
{
    call(slot, CMD_CLOSEDIR, &handle, 1);
}

int tnfs_file_open(int slot, const char *path, uint16_t flags, uint8_t *handle)
{
    uint8_t req[4 + 256];
    size_t pl = strlen(path);
    if (pl >= 256)
        return GEMDOS_EFILNF;
    req[0] = flags & 0xFF;
    req[1] = flags >> 8;
    req[2] = 0xA4;                  // mode 0644, only used with O_CREAT
    req[3] = 0x01;
    memcpy(req + 4, path, pl + 1);
    int n = call(slot, CMD_OPEN, req, 4 + pl + 1);
    if (n < 0)
        return GEMDOS_EFILNF;
    if (s_rx[4] != ST_OK || n < 6)
        return to_gemdos(s_rx[4]);
    *handle = s_rx[5];
    return 0;
}

int tnfs_file_read(int slot, uint8_t handle, uint8_t *buf, uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        uint32_t want = len - done > READ_MAX ? READ_MAX : len - done;
        uint8_t req[3] = { handle, want & 0xFF, want >> 8 };
        int n = call(slot, CMD_READ, req, sizeof(req));
        if (n < 0)
            return done ? (int)done : GEMDOS_ERROR;
        if (s_rx[4] == ST_EOF)
            break;
        if (s_rx[4] != ST_OK || n < 7)
            return done ? (int)done : to_gemdos(s_rx[4]);
        uint32_t got = s_rx[5] | (s_rx[6] << 8);
        if (got > (uint32_t)n - 7 || !got)
            break;
        memcpy(buf + done, s_rx + 7, got);
        done += got;
    }
    return done;
}

int tnfs_file_write(int slot, uint8_t handle, const uint8_t *buf, uint32_t len)
{
    static uint8_t req[3 + READ_MAX];
    uint32_t done = 0;
    while (done < len) {
        uint32_t want = len - done > READ_MAX ? READ_MAX : len - done;
        req[0] = handle;
        req[1] = want & 0xFF;
        req[2] = want >> 8;
        memcpy(req + 3, buf + done, want);
        int n = call(slot, CMD_WRITE, req, 3 + want);
        if (n < 0)
            return done ? (int)done : GEMDOS_ERROR;
        if (s_rx[4] != ST_OK || n < 7)
            return done ? (int)done : to_gemdos(s_rx[4]);
        uint32_t put = s_rx[5] | (s_rx[6] << 8);
        if (!put)
            break;
        done += put;
    }
    return done;
}

int tnfs_file_seek(int slot, uint8_t handle, uint32_t pos)
{
    uint8_t req[6] = { handle, 0, pos & 0xFF, (pos >> 8) & 0xFF, (pos >> 16) & 0xFF, pos >> 24 };
    int n = call(slot, CMD_LSEEK, req, sizeof(req));
    if (n < 0)
        return GEMDOS_ERROR;
    return s_rx[4] == ST_OK ? 0 : to_gemdos(s_rx[4]);
}

void tnfs_file_close(int slot, uint8_t handle)
{
    call(slot, CMD_CLOSE, &handle, 1);
}

static int path_call(int slot, uint8_t cmd, const char *path)
{
    int n = call(slot, cmd, path, strlen(path) + 1);
    if (n < 0)
        return GEMDOS_ERROR;
    return s_rx[4] == ST_OK ? 0 : to_gemdos(s_rx[4]);
}

int tnfs_unlink(int slot, const char *path)
{
    return path_call(slot, CMD_UNLINK, path);
}

int tnfs_mkdir(int slot, const char *path)
{
    return path_call(slot, CMD_MKDIR, path);
}

int tnfs_rmdir(int slot, const char *path)
{
    return path_call(slot, CMD_RMDIR, path);
}

int tnfs_rename(int slot, const char *from, const char *to)
{
    uint8_t req[2 * 256];
    size_t a = strlen(from) + 1, b = strlen(to) + 1;
    if (a + b > sizeof(req))
        return GEMDOS_EPTHNF;
    memcpy(req, from, a);
    memcpy(req + a, to, b);
    int n = call(slot, CMD_RENAME, req, a + b);
    if (n < 0)
        return GEMDOS_ERROR;
    return s_rx[4] == ST_OK ? 0 : to_gemdos(s_rx[4]);
}

int64_t tnfs_free_kb(int slot)
{
    int n = call(slot, CMD_FREE, NULL, 0);
    if (n < 9 || s_rx[4] != ST_OK)
        return GEMDOS_ERROR;
    return le32(s_rx + 5);
}
