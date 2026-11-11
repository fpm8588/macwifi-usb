// daemon.c — macwifid: the background service that owns the adapter (Phase 5). Compiled as part of main.c
// (#included near its end) so it can reuse main.c's static helpers; not built on its own.
// Original code, GPL-2.0-only like the rest of main.c's commands (see NOTICE.md).
//
// Threads: the main thread owns the adapter (USB, firmware messages, association, bridge housekeeping); the
// control thread serves the Unix socket and hands commands to the main thread through a one-slot mailbox; the
// bridge thread (bridge_start) copies macOS's frames to the air.
//
// Control protocol: one JSON object per line, one request per connection. Requests:
//   {"cmd":"status"}                         -> state, adapter, power, ssid, rssi, freq, ip, error
//   {"cmd":"networks"}                       -> the latest scan (one entry per network name, strongest AP)
//   {"cmd":"scan"}                           -> scans, then answers like "networks"
//   {"cmd":"join","ssid":S[,"password":P]}  -> joins (no password: open network, or a remembered one)
//   {"cmd":"disconnect"} {"cmd":"forget","ssid":S} {"cmd":"power","on":true|false} {"cmd":"known"}
// Every answer has "ok" (bool) and, when false, "error" (text for the user).

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define MW_DIR "/Library/Application Support/macwifi"
#define MW_SOCK "/var/run/macwifid.sock"

typedef enum { DS_OFF, DS_NO_ADAPTER, DS_STARTING, DS_IDLE, DS_SCANNING, DS_CONNECTING, DS_CONNECTED, DS_ERROR, DS_ASLEEP } DState;
static const char *const kStateName[] = {"off", "no_adapter", "starting", "idle", "scanning", "connecting", "connected", "error", "asleep"};

// ---------------------------------------------------------------- sleep / wake (power.c calls these)
//
// Before sleep the power callback asks the main thread to shut the adapter down cleanly (leave the network, remove
// the macOS interface and service, release USB) and waits for it, at most MW_SLEEP_WAIT_MS; macOS sleeps right
// after. The cap is short on purpose: closing the lid must put the Mac to sleep promptly even if the adapter is
// stuck, and the adapter's firmware survives sleep anyway (hardware log). macwifid never takes a power assertion,
// so nothing here can keep the Mac awake. After wake the main thread finds the adapter again (whatever state it
// woke up in) and auto-joins as usual.
#define MW_SLEEP_WAIT_MS 3000
static atomic_int gSleepReq, gWake;
static atomic_int gBtSleep, gBtOpen;   // Bluetooth thread: asked to close for sleep / has the interface open
static pthread_mutex_t gPmMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gPmCv = PTHREAD_COND_INITIALIZER;
static int gSleepReady;

static void d_will_sleep(void) {
    pthread_mutex_lock(&gPmMu);
    gSleepReady = 0;
    gSleepReq = 1;
    struct timespec ts, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += MW_SLEEP_WAIT_MS / 1000;
    ts.tv_nsec += (long)(MW_SLEEP_WAIT_MS % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (!gSleepReady)
        if (pthread_cond_timedwait(&gPmCv, &gPmMu, &ts) != 0) break;
    int ready = gSleepReady;
    pthread_mutex_unlock(&gPmMu);
    gBtSleep = 1;                                   // Bluetooth closes too, within what is left of the budget
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (!gBtOpen || (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= MW_SLEEP_WAIT_MS) break;
        usleep(20000);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf(ready ? "[macwifid] ready for sleep after %.0f ms\n"
                 : "[macwifid] adapter still busy after %.0f ms; letting the Mac sleep anyway\n", ms);
    fflush(stdout);
}

static void d_did_wake(void) { gWake = 1; gBtSleep = 0; }

typedef enum { DC_NONE, DC_SCAN, DC_JOIN, DC_DISCONNECT, DC_FORGET, DC_POWER } DCmd;

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    int rssi, freq, secured, usable, known, sae, sae_only, akm_sae, akm_psk;
    const char *why;
    double seen;              // when a scan last saw it
} NetView;

#define NET_KEEP_S 180        // a network stays listed this long after the last scan that saw it

typedef struct {
    int (*adapter_present)(void);
    int (*acquire)(Link *l, uint8_t mac[6], uint8_t *vif);
    void (*release)(Link *l);
    const char *dir;          // known networks + power state
    const char *sock;
    int bluetooth;            // run the Bluetooth thread (real adapter only)
} DaemonHooks;

typedef struct {
    DaemonHooks h;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    // status (mu)
    DState state;
    int power, adapter;
    uint8_t ssid[32];
    uint8_t ssid_len;
    int rssi, freq;
    char ip[64], error[200];
    const char *sec;          // "wpa3", "wpa2" or "open" for the current connection
    int captive;              // CaptiveState of the current connection
    char portal[512];
    NetView nets[48];
    int nnets;
    double scan_time;
    KnownList known;          // written by the main thread under mu
    // mailbox (mu)
    DCmd cmd;
    uint8_t cmd_ssid[32];
    size_t cmd_ssid_len;
    char cmd_pass[64];
    int cmd_has_pass, cmd_on, cmd_done, cmd_ok;
    char cmd_msg[200];
    unsigned long cmd_seq, done_seq;
    // main thread only
    int listen_fd;
} Daemon;

static Daemon gD;

static void d_set_state(Daemon *d, DState s) {
    pthread_mutex_lock(&d->mu);
    if (d->state != s) printf("[macwifid] state: %s\n", kStateName[s]);
    d->state = s;
    pthread_mutex_unlock(&d->mu);
    fflush(stdout);
}

static void d_set_error(Daemon *d, const char *e) {
    pthread_mutex_lock(&d->mu);
    snprintf(d->error, sizeof d->error, "%s", e ? e : "");
    pthread_mutex_unlock(&d->mu);
    if (e && *e) { printf("[macwifid] %s\n", e); fflush(stdout); }
}

// ---------------------------------------------------------------- persistence (root-only files)

static void d_path(const Daemon *d, const char *name, char *out, size_t cap) { snprintf(out, cap, "%s/%s", d->h.dir, name); }

static int write_private(const char *path, const char *data, size_t len) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, data, len) == (ssize_t)len && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

// Reads a small file (absent = NULL, quietly).
static char *read_small(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *b = malloc(16384);
    ssize_t n = b ? read(fd, b, 16383) : -1;
    close(fd);
    if (n < 0) { free(b); return NULL; }
    b[n] = 0;
    *len = (size_t)n;
    return b;
}

static void d_load(Daemon *d) {
    char p[1024];
    mkdir(d->h.dir, 0755);
    d_path(d, "networks", p, sizeof p);
    size_t n = 0;
    char *t = read_small(p, &n);
    if (t) { kn_parse(&d->known, t, n); secure_zero(t, n); free(t); }
    d_path(d, "power", p, sizeof p);
    t = read_small(p, &n);
    d->power = !(t && n && t[0] == '0');
    free(t);
    printf("[macwifid] %d known network(s); Wi-Fi %s\n", d->known.count, d->power ? "on" : "off");
}

static void d_save_known(Daemon *d) {
    char p[1024], buf[KN_MAX * 270];
    d_path(d, "networks", p, sizeof p);
    pthread_mutex_lock(&d->mu);
    size_t n = kn_format(&d->known, buf, sizeof buf);
    pthread_mutex_unlock(&d->mu);
    if (write_private(p, buf, n) != 0) printf("[macwifid] couldn't save %s\n", p);
    secure_zero(buf, sizeof buf);
}

static void d_save_power(Daemon *d) {
    char p[1024];
    d_path(d, "power", p, sizeof p);
    write_private(p, d->power ? "1\n" : "0\n", 2);
}

// ---------------------------------------------------------------- control socket (its own thread)

static void reply_status(Daemon *d, JsonOut *j) {
    jo_str(j, "state", kStateName[d->state]);
    jo_bool(j, "adapter", d->adapter);
    jo_bool(j, "power", d->power);
    if (d->state == DS_CONNECTED || d->state == DS_CONNECTING) {
        jo_strn(j, "ssid", d->ssid, d->ssid_len);
        jo_int(j, "rssi", d->rssi);
        jo_int(j, "freq", d->freq);
        jo_str(j, "ip", d->ip);
        if (d->state == DS_CONNECTED && d->sec) jo_str(j, "security", d->sec);
        jo_str(j, "captive", d->captive == CAPTIVE_PORTAL ? "yes" : d->captive == CAPTIVE_NONE ? "no" : "unknown");
        if (d->captive == CAPTIVE_PORTAL) jo_str(j, "portal", d->portal);
    }
    jo_str(j, "error", d->error);
    jo_str(j, "version", MW_VERSION);
}

static void reply_networks(Daemon *d, JsonOut *j) {
    jo_int(j, "age", d->scan_time > 0 ? (long long)(now_s() - d->scan_time) : -1);
    jo_key_arr(j, "networks");
    for (int i = 0; i < d->nnets; i++) {
        const NetView *v = &d->nets[i];
        jo_begin_obj(j);
        jo_strn(j, "ssid", v->ssid, v->ssid_len);
        jo_int(j, "rssi", v->rssi);
        jo_int(j, "freq", v->freq);
        jo_bool(j, "secured", v->secured);
        jo_str(j, "security", !v->secured ? "open"
                              : v->akm_sae ? (v->akm_psk ? "wpa2/wpa3" : "wpa3")
                              : v->akm_psk ? "wpa2" : "other");
        jo_bool(j, "usable", v->usable);
        jo_bool(j, "known", kn_find(&d->known, v->ssid, v->ssid_len) != NULL);   // as of now, not the scan
        if (!v->usable) jo_str(j, "why", v->why);
        jo_end_obj(j);
    }
    jo_end_arr(j);
}

// Posts a command to the main thread and waits for it (up to `timeout_s`).
static int d_post(Daemon *d, DCmd cmd, const uint8_t *ssid, size_t ssid_len, const char *pass, size_t pass_len, int on,
                  int timeout_s, char *msg, size_t msgcap) {
    pthread_mutex_lock(&d->mu);
    while (d->cmd != DC_NONE) pthread_cond_wait(&d->cv, &d->mu);   // one at a time
    d->cmd = cmd;
    d->cmd_ssid_len = ssid_len > 32 ? 32 : ssid_len;
    if (ssid) memcpy(d->cmd_ssid, ssid, d->cmd_ssid_len);
    d->cmd_has_pass = pass != NULL;
    if (pass) { memcpy(d->cmd_pass, pass, pass_len); d->cmd_pass[pass_len] = 0; }
    d->cmd_on = on;
    unsigned long seq = ++d->cmd_seq;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_s;
    int r = 0;
    while (d->done_seq < seq && r == 0) r = pthread_cond_timedwait(&d->cv, &d->mu, &ts);
    int ok = d->done_seq >= seq ? d->cmd_ok : 0;
    snprintf(msg, msgcap, "%s", d->done_seq >= seq ? d->cmd_msg : "timed out waiting for the adapter");
    pthread_mutex_unlock(&d->mu);
    return ok;
}

// ---------------------------------------------------------------- Bluetooth (phase B2)
//
// Its own thread: the Bluetooth controller is interface 0 of the same USB device, independent of the Wi-Fi
// interface the main thread drives, but it only runs once our firmware is on the chip, so it follows the Wi-Fi
// side's state (adapter present and brought up). It closes the interface whenever the Mac is about to sleep, the
// user turns Bluetooth off, or the adapter goes away. No power assertions.

#define BT_DEV_KEEP_S 90      // a device stays listed this long after it was last heard
#define BT_SCAN_S     11      // ~5 s classic inquiry alone (+ name requests), then at least 3 s LE scan

typedef struct {
    pthread_mutex_t mu;
    int power;                // user setting (persisted), default on
    int up, scanning, scan_req;
    char controller[96], error[160];
    int last_classic, last_le;   // responses in the last scan (shown in the menu)
    HciDevice dev[HCI_SEEN_MAX];
    double seen[HCI_SEEN_MAX];
    int ndev;
} BtState;

static BtState gBt = {PTHREAD_MUTEX_INITIALIZER, 1, 0, 0, 0, "", "", -1, -1, {{0}}, {0}, 0};

// ---- connections (phase B3/B4): paired devices, per-device status, requests from clients, input to the app

#define BT_PAIRED_MAX 16
typedef struct {
    uint8_t addr[6];
    uint8_t key[16];
    uint8_t key_type;
    uint32_t cod;
    char name[64];
    char type[24];
} BtPaired;

typedef struct {
    uint8_t addr[6];
    char state[16];            // "connecting", "pairing", "connected", "failed", "" = none
    uint32_t passkey;          // shown while pairing a keyboard
    int has_passkey;
    char why[64];              // after "failed"
    double t;
} BtDevStatus;

enum { BT_ACT_CONNECT = 1, BT_ACT_DISCONNECT, BT_ACT_FORGET, BT_ACT_SOUND };

static struct {
    BtPaired paired[BT_PAIRED_MAX];
    int npaired;
    BtDevStatus st[BT_PAIRED_MAX];
    struct { int op; uint8_t addr[6]; } act[8];
    int nact;
} gBp;                         // guarded by gBt.mu

static BtHost gHost;           // Bluetooth thread only
static BtLink *gBtLink;        // Bluetooth thread only
static struct {
    uint8_t addr[6];
    int used;
    HidKbState kb;
    uint8_t buttons;
    int unknown_logged;
    // mouse report timing (logged every 20 s while it moves): how often reports come, the longest pause
    unsigned reports;
    double win_start, last, max_gap;
} gHid[BTH_MAX_CONN];

// Input for the menu app (it posts the events into the user's session): a ring of text lines, read by
// `bt_input` streams. "k <macOS key code> <1 down|0 up> <modifier bits>" and "m <buttons> <dx> <dy> <wheel>".
#define BT_IN_RING 1024
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char line[BT_IN_RING][40];
    unsigned long seq;
    int readers;
} gIn = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {{0}}, 0, 0};

static void in_push(const char *s) {
    pthread_mutex_lock(&gIn.mu);
    snprintf(gIn.line[gIn.seq % BT_IN_RING], sizeof gIn.line[0], "%s", s);
    gIn.seq++;
    pthread_cond_broadcast(&gIn.cv);
    pthread_mutex_unlock(&gIn.mu);
}

static int hexnib(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// "AA:BB:CC:DD:EE:FF" (as shown) -> addr (wire order). 0 or -1.
static int parse_addr(const char *s, size_t n, uint8_t a[6]) {
    if (n != 17) return -1;
    for (int i = 0; i < 6; i++) {
        int hi = hexnib(s[i * 3]), lo = hexnib(s[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && s[i * 3 + 2] != ':')) return -1;
        a[5 - i] = (uint8_t)(hi * 16 + lo);
    }
    return 0;
}

static void hexenc(const uint8_t *p, size_t n, char *out) {
    static const char k[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = k[p[i] >> 4]; out[2 * i + 1] = k[p[i] & 15]; }
    out[2 * n] = 0;
}

static size_t hexdec(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        int hi = hexnib(s[0]), lo = hexnib(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)(hi * 16 + lo);
        s += 2;
    }
    return n;
}

// Paired devices file (root-only, like the Wi-Fi keys): addr key type cod name-hex type-hex per line.
static void bp_save(void) {
    char path[512];
    d_path(&gD, "bt_paired", path, sizeof path);
    char *buf = malloc(BT_PAIRED_MAX * 400);
    if (!buf) return;
    size_t o = 0;
    for (int i = 0; i < gBp.npaired; i++) {
        const BtPaired *p = &gBp.paired[i];
        char a[13], k[33], nm[129], ty[49];
        hexenc(p->addr, 6, a);
        hexenc(p->key, 16, k);
        hexenc((const uint8_t *)p->name, strlen(p->name), nm);
        hexenc((const uint8_t *)p->type, strlen(p->type), ty);
        o += (size_t)snprintf(buf + o, BT_PAIRED_MAX * 400 - o, "%s %s %u %06x %s %s\n", a, k, p->key_type,
                              (unsigned)p->cod, nm[0] ? nm : "-", ty[0] ? ty : "-");
    }
    write_private(path, buf, o);
    secure_zero(buf, BT_PAIRED_MAX * 400);
    free(buf);
}

static void bp_load(void) {
    char path[512];
    d_path(&gD, "bt_paired", path, sizeof path);
    size_t n = 0;
    char *t = read_small(path, &n);
    if (!t) return;
    char *save = NULL;
    for (char *line = strtok_r(t, "\n", &save); line && gBp.npaired < BT_PAIRED_MAX; line = strtok_r(NULL, "\n", &save)) {
        char a[16], k[40], nm[160], ty[64];
        unsigned kt = 0, cod = 0;
        if (sscanf(line, "%15s %39s %u %x %159s %63s", a, k, &kt, &cod, nm, ty) != 6) continue;
        BtPaired *p = &gBp.paired[gBp.npaired];
        memset(p, 0, sizeof *p);
        if (hexdec(a, p->addr, 6) != 6 || hexdec(k, p->key, 16) != 16) continue;
        p->key_type = (uint8_t)kt;
        p->cod = cod;
        if (strcmp(nm, "-")) p->name[hexdec(nm, (uint8_t *)p->name, sizeof p->name - 1)] = 0;
        if (strcmp(ty, "-")) p->type[hexdec(ty, (uint8_t *)p->type, sizeof p->type - 1)] = 0;
        gBp.npaired++;
    }
    secure_zero(t, n);
    free(t);
}

static BtPaired *bp_find(const uint8_t a[6]) {
    for (int i = 0; i < gBp.npaired; i++) if (!memcmp(gBp.paired[i].addr, a, 6)) return &gBp.paired[i];
    return NULL;
}

static void bp_remove(const uint8_t a[6]) {
    for (int i = 0; i < gBp.npaired; i++)
        if (!memcmp(gBp.paired[i].addr, a, 6)) {
            secure_zero(&gBp.paired[i], sizeof gBp.paired[i]);
            gBp.paired[i] = gBp.paired[--gBp.npaired];
            bp_save();
            return;
        }
}

static BtDevStatus *bs_get(const uint8_t a[6], int create) {
    BtDevStatus *oldest = &gBp.st[0];
    for (int i = 0; i < BT_PAIRED_MAX; i++) {
        if (gBp.st[i].t > 0 && !memcmp(gBp.st[i].addr, a, 6)) return &gBp.st[i];
        if (gBp.st[i].t < oldest->t) oldest = &gBp.st[i];
    }
    if (!create) return NULL;
    memset(oldest, 0, sizeof *oldest);
    memcpy(oldest->addr, a, 6);
    oldest->t = now_s();
    return oldest;
}

// The device's kind for logs (never names or addresses there).
static const char *bt_kind_for_log(const uint8_t a[6]) {
    const BtPaired *p = bp_find(a);
    if (p && p->type[0]) return p->type;
    for (int i = 0; i < gBt.ndev; i++)
        if (!memcmp(gBt.dev[i].addr, a, 6)) {
            const char *k = gBt.dev[i].kind == HCI_DEV_CLASSIC ? hci_cod_kind(gBt.dev[i].cod) : NULL;
            if (k) return k;
        }
    return "device";
}

static void hid_release_all(int slot) {
    if (!gHid[slot].used) return;
    HidKeyEvent ev[32];
    uint8_t zero[8] = {0};
    int n = hid_kb_diff(&gHid[slot].kb, zero, 8, ev);
    for (int i = 0; i < n; i++) {
        int kc = hid_usage_to_mac(ev[i].usage);
        char line[40];
        if (kc >= 0) { snprintf(line, sizeof line, "k %d 0 0", kc); in_push(line); }
    }
    if (gHid[slot].buttons) in_push("m 0 0 0 0");
    memset(&gHid[slot], 0, sizeof gHid[slot]);
}

static int hid_slot(const uint8_t a[6], int create) {
    int free_slot = -1;
    for (int i = 0; i < BTH_MAX_CONN; i++) {
        if (gHid[i].used && !memcmp(gHid[i].addr, a, 6)) return i;
        if (!gHid[i].used && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    memset(&gHid[free_slot], 0, sizeof gHid[free_slot]);
    gHid[free_slot].used = 1;
    memcpy(gHid[free_slot].addr, a, 6);
    return free_slot;
}

// ---- BtHostOps (all on the Bluetooth thread)

static int bh_send_cmd(void *ctx, uint16_t op, const uint8_t *p, size_t n) {
    (void)ctx;
    return gBtLink ? bt_send_cmd(gBtLink, op, p, n) : -1;
}
static int bh_send_acl(void *ctx, const uint8_t *pkt, size_t n) {
    (void)ctx;
    return gBtLink ? bt_send_acl(gBtLink, pkt, n) : -1;
}
static int bh_key_load(void *ctx, const uint8_t a[6], uint8_t key[16]) {
    (void)ctx;
    pthread_mutex_lock(&gBt.mu);
    const BtPaired *p = bp_find(a);
    if (p) memcpy(key, p->key, 16);
    pthread_mutex_unlock(&gBt.mu);
    return p ? 0 : -1;
}
static void bh_key_store(void *ctx, const uint8_t a[6], const uint8_t key[16], uint8_t type) {
    (void)ctx;
    pthread_mutex_lock(&gBt.mu);
    BtPaired *p = bp_find(a);
    if (!p && gBp.npaired < BT_PAIRED_MAX) {
        p = &gBp.paired[gBp.npaired++];
        memset(p, 0, sizeof *p);
        memcpy(p->addr, a, 6);
    }
    if (p) {
        memcpy(p->key, key, 16);
        p->key_type = type;
        for (int i = 0; i < gBt.ndev; i++)
            if (!memcmp(gBt.dev[i].addr, a, 6)) {
                if (gBt.dev[i].name[0]) snprintf(p->name, sizeof p->name, "%s", gBt.dev[i].name);
                if (gBt.dev[i].cod) p->cod = gBt.dev[i].cod;
                const char *k = gBt.dev[i].kind == HCI_DEV_CLASSIC ? hci_cod_kind(gBt.dev[i].cod)
                                                                   : hci_appearance_kind(gBt.dev[i].appearance);
                if (k) snprintf(p->type, sizeof p->type, "%s", k);
            }
        bp_save();
        printf("[macwifid] Bluetooth: paired a %s (key type %u)\n", p->type[0] ? p->type : "device", type);
        fflush(stdout);
    }
    pthread_mutex_unlock(&gBt.mu);
}
static void bh_key_forget(void *ctx, const uint8_t a[6]) {
    (void)ctx;
    pthread_mutex_lock(&gBt.mu);
    bp_remove(a);
    pthread_mutex_unlock(&gBt.mu);
}
static int bh_accept(void *ctx, const uint8_t a[6], uint32_t cod) {
    (void)ctx;
    pthread_mutex_lock(&gBt.mu);
    BtPaired *p = bp_find(a);
    if (p && cod) p->cod = cod;
    pthread_mutex_unlock(&gBt.mu);
    return p != NULL;              // only paired devices may connect to us
}
static int audio_is(const uint8_t a[6]);
static void audio_reset(void);

static void bh_event(void *ctx, const uint8_t a[6], BtHostEvent ev, uint32_t v) {
    (void)ctx;
    if (ev == BTH_EV_MODE) {                          // only logged: sniff mode explains a laggy mouse
        pthread_mutex_lock(&gBt.mu);
        const char *kind = bt_kind_for_log(a);
        pthread_mutex_unlock(&gBt.mu);
        unsigned mode = v & 0xFF, slots = v >> 8;
        printf("[macwifid] Bluetooth %s: link %s", kind, mode == 0 ? "active" : mode == 2 ? "in sniff mode" : "in another mode");
        if (mode == 2) printf(" (every %.1f ms; asking to leave it)", slots * 0.625);
        printf("\n");
        fflush(stdout);
        return;
    }
    pthread_mutex_lock(&gBt.mu);
    BtDevStatus *s = bs_get(a, 1);
    const char *kind = bt_kind_for_log(a);
    s->t = now_s();
    switch (ev) {
    case BTH_EV_CONNECTING: snprintf(s->state, sizeof s->state, "connecting"); s->has_passkey = 0; break;
    case BTH_EV_PAIRING: snprintf(s->state, sizeof s->state, "pairing"); break;
    case BTH_EV_PASSKEY: snprintf(s->state, sizeof s->state, "pairing"); s->passkey = v; s->has_passkey = 1; break;
    case BTH_EV_CONNECTED: snprintf(s->state, sizeof s->state, "connected"); s->has_passkey = 0; s->why[0] = 0; break;
    case BTH_EV_DISCONNECTED: s->state[0] = 0; s->has_passkey = 0; break;
    case BTH_EV_UNPLUGGED: s->state[0] = 0; s->has_passkey = 0; break;
    case BTH_EV_MODE: break;
    case BTH_EV_FAILED:
        snprintf(s->state, sizeof s->state, "failed");
        s->has_passkey = 0;
        snprintf(s->why, sizeof s->why, "%s", v == 0x04 ? "it didn't answer (is it on and in pairing mode?)"
                                              : v == 0x05 || v == 0x06 ? "pairing was refused"
                                              : v == 0x08 ? "it went out of range"
                                              : v == 0x13 || v == 0x16 ? "it disconnected"
                                              : "it couldn't connect");
        break;
    }
    pthread_mutex_unlock(&gBt.mu);
    static const char *names[] = {"", "connecting", "pairing", "passkey shown", "connected", "disconnected", "failed",
                                  "unplugged (forgotten)"};
    printf("[macwifid] Bluetooth %s: %s", kind, names[ev <= BTH_EV_UNPLUGGED ? ev : 0]);
    if (ev == BTH_EV_FAILED || ev == BTH_EV_DISCONNECTED) printf(" (0x%02x)", v);
    printf("\n");
    fflush(stdout);
    if (ev == BTH_EV_DISCONNECTED || ev == BTH_EV_FAILED || ev == BTH_EV_UNPLUGGED) {
        int slot = hid_slot(a, 0);
        if (slot >= 0) hid_release_all(slot);      // never leave a key or button held down
        if (audio_is(a)) audio_reset();
    }
}
static void bh_hid_input(void *ctx, const uint8_t a[6], const uint8_t *r, size_t n) {
    (void)ctx;
    int slot = hid_slot(a, 1);
    if (slot < 0 || n < 1) return;
    char line[40];
    if (r[0] == HID_REPORT_KEYBOARD) {
        HidKeyEvent ev[32];
        int k = hid_kb_diff(&gHid[slot].kb, r + 1, n - 1, ev);
        for (int i = 0; i < k; i++) {
            int kc = hid_usage_to_mac(ev[i].usage);
            if (kc < 0) continue;
            snprintf(line, sizeof line, "k %d %d %u", kc, ev[i].down, gHid[slot].kb.mods);
            in_push(line);
        }
        return;
    }
    HidMouse m;
    if (r[0] == HID_REPORT_MOUSE && hid_mouse_parse(r + 1, n - 1, &m) == 0) {
        double t = now_s();
        if (gHid[slot].reports == 0) gHid[slot].win_start = t;
        else if (t - gHid[slot].last < 0.5 && t - gHid[slot].last > gHid[slot].max_gap) gHid[slot].max_gap = t - gHid[slot].last;
        gHid[slot].reports++;
        gHid[slot].last = t;
        if (t - gHid[slot].win_start >= 20) {
            printf("[macwifid] Bluetooth mouse: %u reports in %.0f s, longest pause while moving %.0f ms\n",
                   gHid[slot].reports, t - gHid[slot].win_start, gHid[slot].max_gap * 1000);
            fflush(stdout);
            gHid[slot].reports = 0;
            gHid[slot].max_gap = 0;
        }
        gHid[slot].buttons = m.buttons;
        snprintf(line, sizeof line, "m %u %d %d %d", m.buttons, m.dx, m.dy, m.wheel);
        in_push(line);
        return;
    }
    if (!gHid[slot].unknown_logged) {               // report protocol data we don't decode yet: shape only
        gHid[slot].unknown_logged = 1;
        printf("[macwifid] Bluetooth: input report ID 0x%02x, %zu bytes, not decoded yet\n", r[0], n);
        fflush(stdout);
    }
}

// ---- audio (A2DP source, phase B5): one stream at a time

typedef struct {
    int active;                 // a device with an AVDTP signaling channel
    uint8_t addr[6];
    uint16_t sig_cid, media_cid;
    Avdtp av;
    int we_started;             // we opened the signaling channel (we drive AVDTP)
    int streaming;
    SbcEncoder enc;
    SbcConfig cfg;
    double next_due, rate;
    uint32_t ts;
    uint16_t seq;
    double tone_until, idle_since, avdtp_kick;
    double phase;
    unsigned long packets, dropped;
} BtAudio;
static BtAudio gAudio;

static int audio_is(const uint8_t a[6]) { return gAudio.active && !memcmp(gAudio.addr, a, 6); }

static int av_send(void *ctx, const uint8_t *d, size_t n) {
    (void)ctx;
    if (!gAudio.sig_cid) return -1;
    int r = bthost_l2cap_send(&gHost, gAudio.addr, gAudio.sig_cid, d, n);
    if (r == -2) {                                      // buffers full of media: drain a little, then try once more
        uint8_t ev[HCI_MAX_EVENT];
        size_t m = 0;
        if (gBtLink && bt_next_event(gBtLink, ev, &m, 20) == 1) {
            HciEvent e;
            if (hci_event(ev, m, &e) == 0) bthost_event(&gHost, &e);
        }
        r = bthost_l2cap_send(&gHost, gAudio.addr, gAudio.sig_cid, d, n);
    }
    return r;
}
static void av_open_media(void *ctx) {
    (void)ctx;
    int cid = bthost_l2cap_connect(&gHost, gAudio.addr, L2CAP_PSM_AVDTP);
    if (cid < 0) { printf("[macwifid] Bluetooth audio: couldn't open the media channel\n"); fflush(stdout); }
}
static void av_streaming(void *ctx, int on, const SbcConfig *cfg) {
    (void)ctx;
    if (on) {
        gAudio.cfg = *cfg;
        if (sbc_init(&gAudio.enc, cfg) != 0) { printf("[macwifid] Bluetooth audio: bad SBC settings\n"); return; }
        static const double fs[4] = {16000, 32000, 44100, 48000};
        gAudio.rate = fs[cfg->fs & 3];
        gAudio.streaming = 1;
        gAudio.next_due = now_s();
        gAudio.idle_since = now_s();
        if (gAudio.tone_until < now_s()) gAudio.tone_until = now_s() + 1.2;   // a short chime: it works
        printf("[macwifid] Bluetooth audio: streaming SBC %.1f kHz, %s, %d blocks, bitpool %d (%zu-byte frames)\n",
               gAudio.rate / 1000, cfg->mode == SBC_MODE_MONO ? "mono" : cfg->mode == SBC_MODE_JOINT ? "joint stereo"
                                   : cfg->mode == SBC_MODE_DUAL ? "dual channel" : "stereo",
               cfg->blocks, cfg->bitpool, sbc_frame_length(cfg));
    } else if (gAudio.streaming) {
        gAudio.streaming = 0;
        printf("[macwifid] Bluetooth audio: stream paused (%lu packets sent, %lu dropped)\n", gAudio.packets, gAudio.dropped);
    }
    fflush(stdout);
}
static void av_failed(void *ctx, const char *why) {
    (void)ctx;
    printf("[macwifid] Bluetooth audio: %s\n", why);
    fflush(stdout);
    pthread_mutex_lock(&gBt.mu);
    BtDevStatus *s = bs_get(gAudio.addr, 1);
    snprintf(s->state, sizeof s->state, "failed");
    snprintf(s->why, sizeof s->why, "%s", why);
    s->t = now_s();
    pthread_mutex_unlock(&gBt.mu);
}
static const AvdtpOps kAvOps = {NULL, av_send, av_open_media, av_streaming, av_failed};

static void audio_reset(void) {
    if (gAudio.streaming) av_streaming(NULL, 0, &gAudio.cfg);
    memset(&gAudio, 0, sizeof gAudio);
}

// One media packet: RTP header (12 bytes, payload type 96), the SBC payload header (number of frames), frames.
// Paced by the sample clock; when the controller's buffers are full the packet is dropped, never queued (late audio
// is worse than a gap). Silence after the chime; after 5 s of nothing to play the stream is suspended.
static void audio_pump(void) {
    if (!gAudio.streaming || !gAudio.media_cid) return;
    double t = now_s();
    if (t < gAudio.next_due) return;
    if (t - gAudio.next_due > 0.25) gAudio.next_due = t;          // we stalled (e.g. a scan): don't burst
    size_t flen = sbc_frame_length(&gAudio.cfg);
    int spf = sbc_frame_samples(&gAudio.cfg);
    uint16_t mtu = bthost_l2cap_mtu(&gHost, gAudio.addr, gAudio.media_cid);
    int frames = (int)((mtu > 13 ? mtu - 13 : 0) / flen);
    if (frames > 15) frames = 15;
    if (frames < 1) return;
    int nch = gAudio.enc.channels;
    int16_t pcm[16 * 8 * 2];
    uint8_t pkt[13 + 15 * 520];
    size_t o = 13;
    int playing = t < gAudio.tone_until;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < spf; i++) {
            double v = 0;
            if (playing) {            // two soft tones (E5 then A5) with a fade, like a connect chime
                double el = 1.2 - (gAudio.tone_until - t);
                double fr = el < 0.45 ? 659.25 : 880.0;
                double env = el < 0.45 ? (el / 0.45) : (1.2 - el) / 0.75;
                if (env < 0) env = 0;
                v = 0.25 * env * sin(gAudio.phase);
                gAudio.phase += 2 * M_PI * fr / gAudio.rate;
                if (gAudio.phase > 2 * M_PI) gAudio.phase -= 2 * M_PI;
            }
            int16_t s = (int16_t)(v * 32767);
            for (int c = 0; c < nch; c++) pcm[i * nch + c] = s;
        }
        if (!sbc_encode(&gAudio.enc, pcm, pkt + o, sizeof pkt - o)) return;
        o += flen;
    }
    pkt[0] = 0x80;                                   // RTP version 2
    pkt[1] = 0x60;                                   // payload type 96 (dynamic)
    pkt[2] = (uint8_t)(gAudio.seq >> 8);
    pkt[3] = (uint8_t)gAudio.seq;
    pkt[4] = (uint8_t)(gAudio.ts >> 24);
    pkt[5] = (uint8_t)(gAudio.ts >> 16);
    pkt[6] = (uint8_t)(gAudio.ts >> 8);
    pkt[7] = (uint8_t)gAudio.ts;
    pkt[8] = 0; pkt[9] = 0; pkt[10] = 0; pkt[11] = 1; // SSRC
    pkt[12] = (uint8_t)frames;                       // SBC payload header: not fragmented, frame count
    gAudio.seq++;
    gAudio.ts += (uint32_t)(frames * spf);
    gAudio.next_due += frames * spf / gAudio.rate;
    if (bthost_l2cap_send(&gHost, gAudio.addr, gAudio.media_cid, pkt, o) == 0) gAudio.packets++;
    else gAudio.dropped++;
    if (playing) gAudio.idle_since = t;
    else if (t - gAudio.idle_since > 5) avdtp_suspend(&gAudio.av);   // nothing to play: let the earbuds rest
}

static int bh_l2_accept(void *ctx, const uint8_t a[6], uint16_t psm) {
    (void)ctx;
    if (psm == L2CAP_PSM_SDP || psm == L2CAP_PSM_AVCTP) return 1;
    if (psm == L2CAP_PSM_AVDTP) return !gAudio.active || audio_is(a);   // one audio device at a time
    return 0;
}
static void bh_l2_open(void *ctx, const uint8_t a[6], uint16_t psm, uint16_t cid) {
    (void)ctx;
    if (psm != L2CAP_PSM_AVDTP) return;
    if (!gAudio.active) {                               // the first AVDTP channel is signaling
        memset(&gAudio, 0, sizeof gAudio);
        gAudio.active = 1;
        memcpy(gAudio.addr, a, 6);
        gAudio.sig_cid = cid;
        avdtp_init(&gAudio.av, &kAvOps);
        BtConnState cs = bthost_state(&gHost, a);
        (void)cs;
        for (int i = 0; i < BTH_MAX_CONN; i++)
            if (gHost.c[i].state != BTC_FREE && !memcmp(gHost.c[i].addr, a, 6) && gHost.c[i].initiator) gAudio.we_started = 1;
        if (gAudio.we_started) avdtp_start(&gAudio.av);
        else gAudio.avdtp_kick = now_s() + 1.5;         // give the device a moment to drive it; then we do
        printf("[macwifid] Bluetooth audio: signaling channel open\n");
    } else if (audio_is(a) && !gAudio.media_cid) {      // the second is the media transport
        gAudio.media_cid = cid;
        printf("[macwifid] Bluetooth audio: media channel open (MTU %u)\n", bthost_l2cap_mtu(&gHost, a, cid));
        avdtp_media_open(&gAudio.av, 1);
    }
    fflush(stdout);
}
static void bh_l2_data(void *ctx, const uint8_t a[6], uint16_t psm, uint16_t cid, const uint8_t *d, size_t n) {
    (void)ctx;
    if (psm == L2CAP_PSM_SDP) {
        uint8_t rsp[512];
        size_t m = sdp_server_handle(d, n, rsp, sizeof rsp);
        if (m) bthost_l2cap_send(&gHost, a, cid, rsp, m);
        return;
    }
    if (psm == L2CAP_PSM_AVDTP && audio_is(a) && cid == gAudio.sig_cid) {
        gAudio.avdtp_kick = 0;                          // the device is driving AVDTP
        avdtp_rx(&gAudio.av, d, n);
    }
    // AVCTP (remote control): accepted so the device is happy; commands aren't acted on yet.
}
static void bh_l2_closed(void *ctx, const uint8_t a[6], uint16_t psm, uint16_t cid) {
    (void)ctx;
    if (psm != L2CAP_PSM_AVDTP || !audio_is(a)) return;
    if (cid == gAudio.media_cid) {
        gAudio.media_cid = 0;
        avdtp_media_open(&gAudio.av, 0);
    } else if (cid == gAudio.sig_cid) {
        audio_reset();
    }
}

static const BtHostOps kBtOps = {NULL, bh_send_cmd, bh_send_acl, bh_key_load, bh_key_store, bh_key_forget,
                                 bh_accept, bh_event, bh_hid_input, bh_l2_accept, bh_l2_open, bh_l2_data, bh_l2_closed};

static void bt_acl_in(void *ctx, const uint8_t *pkt, size_t n) {
    (void)ctx;
    bthost_acl(&gHost, pkt, n);
}

static int bt_busy_connecting(void) {
    for (int i = 0; i < BTH_MAX_CONN; i++) {
        int s = gHost.c[i].state;
        if (s == BTC_PAGING || s == BTC_SECURING || s == BTC_CHANNELS) return 1;
    }
    return 0;
}

static void bt_save_power(Daemon *d) {
    char path[512];
    d_path(d, "bt_power", path, sizeof path);
    write_private(path, gBt.power ? "1\n" : "0\n", 2);
}

static void bt_load_power(Daemon *d) {
    char path[512];
    d_path(d, "bt_power", path, sizeof path);
    size_t n = 0;
    char *t = read_small(path, &n);
    if (t) { gBt.power = !(n && t[0] == '0'); free(t); }
}

static void bt_set_error(const char *e) {
    pthread_mutex_lock(&gBt.mu);
    if (strcmp(gBt.error, e)) { printf("[macwifid] Bluetooth: %s\n", e[0] ? e : "ok"); fflush(stdout); }
    snprintf(gBt.error, sizeof gBt.error, "%s", e);
    pthread_mutex_unlock(&gBt.mu);
}

// Merges one scan result into the published list (gBt.mu held by the caller).
static void bt_publish(const HciDevice *d) {
    double t = now_s();
    for (int i = 0; i < gBt.ndev; i++) {
        HciDevice *e = &gBt.dev[i];
        if (e->kind != d->kind || memcmp(e->addr, d->addr, 6)) continue;
        hci_merge(e, d);                     // same merge rules as a single scan
        gBt.seen[i] = t;
        return;
    }
    int slot = gBt.ndev < HCI_SEEN_MAX ? gBt.ndev++ : -1;
    if (slot < 0) {                          // full: replace the oldest
        slot = 0;
        for (int i = 1; i < gBt.ndev; i++) if (gBt.seen[i] < gBt.seen[slot]) slot = i;
    }
    gBt.dev[slot] = *d;
    gBt.seen[slot] = t;
}

static void bt_expire(void) {
    double t = now_s();
    int w = 0;
    for (int i = 0; i < gBt.ndev; i++)
        if (t - gBt.seen[i] <= BT_DEV_KEEP_S) { gBt.dev[w] = gBt.dev[i]; gBt.seen[w] = gBt.seen[i]; w++; }
    gBt.ndev = w;
}

typedef struct { int inquiry_done, n_classic, n_le; } BtRun;
static void bt_d_device(void *ctx, const HciDevice *dv) {
    (void)ctx;
    pthread_mutex_lock(&gBt.mu);
    bt_publish(dv);
    pthread_mutex_unlock(&gBt.mu);
}
static void bt_d_event(void *ctx, const HciEvent *ev) {
    BtRun *r = ctx;
    if (ev->code == HCI_EV_INQUIRY_COMPLETE) { if (r) r->inquiry_done = 1; return; }
    int k = hci_scan_event(ev, bt_d_device, NULL);
    if (k > 0 && r) { if (ev->code == HCI_EV_LE_META) r->n_le += k; else r->n_classic += k; }
    if (k == 0 && gBtLink) bthost_event(&gHost, ev);      // connections, pairing, flow control
}

static int bt_wifi_ready(Daemon *d) {
    pthread_mutex_lock(&d->mu);
    int ok = d->adapter && (d->state == DS_IDLE || d->state == DS_SCANNING || d->state == DS_CONNECTING ||
                            d->state == DS_CONNECTED);
    pthread_mutex_unlock(&d->mu);
    return ok;
}

static int bt_should_run(Daemon *d) {
    pthread_mutex_lock(&gBt.mu);
    int on = gBt.power;
    pthread_mutex_unlock(&gBt.mu);
    return on && !gBtSleep && !gStop && bt_wifi_ready(d);
}

static void bt_down(BtLink **b, const char *why) {
    if (*b) {
        // HCI Reset drops every link at once (fast, for sleep); the devices notice and reconnect later.
        bt_call(*b, HCI_OP_RESET, NULL, 0, NULL, 0, NULL, 800, NULL, NULL);
        bt_set_acl_handler(*b, NULL, NULL);
        audio_reset();
        gBtLink = NULL;
        bthost_reset(&gHost);
        for (int i = 0; i < BTH_MAX_CONN; i++) hid_release_all(i);
        pthread_mutex_lock(&gBt.mu);
        for (int i = 0; i < BT_PAIRED_MAX; i++)
            if (!strcmp(gBp.st[i].state, "connected") || !strcmp(gBp.st[i].state, "connecting") ||
                !strcmp(gBp.st[i].state, "pairing")) gBp.st[i].state[0] = 0;
        pthread_mutex_unlock(&gBt.mu);
        bt_close(*b);
        *b = NULL;
        printf("[macwifid] Bluetooth closed (%s)\n", why);
        fflush(stdout);
    }
    gBtOpen = 0;
    pthread_mutex_lock(&gBt.mu);
    gBt.up = 0;
    gBt.scanning = 0;
    pthread_mutex_unlock(&gBt.mu);
}

static BtLink *bt_up(void) {
    io_service_t svc = find_adapter(AIC_MODE_WIFI, NULL);
    if (!svc) { bt_set_error("adapter not found"); return NULL; }
    char err[160];
    gBtOpen = 1;                              // count it as open while we try, so sleep waits for us
    BtLink *b = bt_open(svc, gDebug, err, sizeof err);
    IOObjectRelease(svc);
    if (!b) { gBtOpen = 0; bt_set_error(err); return NULL; }
    uint8_t ret[260];
    size_t rl = 0;
    BtRun run = {0};
    if (bt_call(b, HCI_OP_RESET, NULL, 0, NULL, 0, NULL, 3000, bt_d_event, &run) != 0 ||
        bt_call(b, HCI_OP_READ_LOCAL_VERSION, NULL, 0, ret, sizeof ret, &rl, 2000, bt_d_event, &run) != 0 || rl < 8) {
        bt_close(b);
        gBtOpen = 0;
        bt_set_error("the Bluetooth controller didn't answer");
        return NULL;
    }
    uint8_t mask[8];
    for (int i = 0; i < 8; i++) mask[i] = (uint8_t)(HCI_EVENT_MASK_SCAN >> (8 * i));
    bt_call(b, HCI_OP_SET_EVENT_MASK, mask, 8, NULL, 0, NULL, 2000, bt_d_event, &run);
    uint8_t mode = 2;
    bt_call(b, HCI_OP_WRITE_INQUIRY_MODE, &mode, 1, NULL, 0, NULL, 2000, bt_d_event, &run);
    // Host role for keyboards and mice: Secure Simple Pairing on, a computer's class of device, a name, and page
    // scan on (connectable, not discoverable) so paired devices can reconnect to us by themselves.
    uint8_t one = 1;
    bt_call(b, HCI_OP_WRITE_SSP_MODE, &one, 1, NULL, 0, NULL, 2000, bt_d_event, &run);
    const uint8_t cod[3] = {0x0C, 0x01, 0x00};          // major class Computer, minor Laptop
    bt_call(b, HCI_OP_WRITE_CLASS_OF_DEVICE, cod, 3, NULL, 0, NULL, 2000, bt_d_event, &run);
    uint8_t name[248];
    memset(name, 0, sizeof name);
    snprintf((char *)name, sizeof name, "Mac (UGREEN Bluetooth)");
    bt_call(b, HCI_OP_WRITE_LOCAL_NAME, name, 248, NULL, 0, NULL, 2000, bt_d_event, &run);
    uint8_t scan = 0x02;                                 // page scan only
    bt_call(b, HCI_OP_WRITE_SCAN_ENABLE, &scan, 1, NULL, 0, NULL, 2000, bt_d_event, &run);
    bthost_init(&gHost, &kBtOps);
    uint8_t bs[16];
    size_t bsl = 0;
    if (bt_call(b, HCI_OP_READ_BUFFER_SIZE, NULL, 0, bs, sizeof bs, &bsl, 2000, bt_d_event, &run) == 0 && bsl >= 7) {
        uint16_t acl_len = (uint16_t)(bs[0] | (bs[1] << 8)), acl_num = (uint16_t)(bs[3] | (bs[4] << 8));
        bthost_set_acl(&gHost, acl_len, acl_num);
        printf("[macwifid] Bluetooth ACL buffers: %u x %u bytes\n", acl_num, acl_len);
    }
    gBtLink = b;
    bt_set_acl_handler(b, bt_acl_in, NULL);
    pthread_mutex_lock(&gBt.mu);
    snprintf(gBt.controller, sizeof gBt.controller, "Bluetooth %s", hci_version_name(ret[0]));
    gBt.up = 1;
    pthread_mutex_unlock(&gBt.mu);
    bt_set_error("");
    printf("[macwifid] Bluetooth up: %s, manufacturer 0x%04x\n", gBt.controller, ret[4] | (ret[5] << 8));
    fflush(stdout);
    return b;
}

// Waits for events until `secs` pass or `stop_on_inquiry_done`; 0, -1 if the link died, 1 if asked to stop.
static int bt_listen(Daemon *d, BtLink *b, BtRun *run, double secs, int stop_on_inquiry_done) {
    double end = now_s() + secs;
    uint8_t ev[HCI_MAX_EVENT];
    size_t n = 0;
    while (now_s() < end) {
        if (!bt_should_run(d)) return 1;
        if (stop_on_inquiry_done && run->inquiry_done) return 0;
        int r = bt_next_event(b, ev, &n, 250);
        if (r < 0) return -1;
        HciEvent e;
        if (r == 1 && hci_event(ev, n, &e) == 0) bt_d_event(run, &e);
    }
    return 0;
}

// Classic devices often leave their name out of the inquiry response; ask each unnamed one (up to a few, each
// with its own timeout). Names arrive in Remote Name Request Complete events.
static int bt_fetch_names(Daemon *d, BtLink *b, BtRun *run) {
    HciDevice want[6];
    int nw = 0;
    pthread_mutex_lock(&gBt.mu);
    for (int i = 0; i < gBt.ndev && nw < 6; i++)
        if (gBt.dev[i].kind == HCI_DEV_CLASSIC && !gBt.dev[i].name[0] && now_s() - gBt.seen[i] < 15) want[nw++] = gBt.dev[i];
    pthread_mutex_unlock(&gBt.mu);
    int got = 0;
    for (int k = 0; k < nw; k++) {
        uint8_t prm[10];
        hci_remote_name_params(&want[k], prm);
        if (bt_call(b, HCI_OP_REMOTE_NAME_REQ, prm, 10, NULL, 0, NULL, 2000, bt_d_event, run) != 0) continue;
        double end = now_s() + 4;
        uint8_t ev[HCI_MAX_EVENT];
        size_t n = 0;
        int done = 0;
        while (!done && now_s() < end) {
            if (!bt_should_run(d)) return got;
            int r = bt_next_event(b, ev, &n, 250);
            if (r < 0) return -1;
            HciEvent e;
            if (r != 1 || hci_event(ev, n, &e)) continue;
            if (e.code == HCI_EV_REMOTE_NAME_COMPLETE && e.len >= 7 && !memcmp(e.p + 1, want[k].addr, 6)) {
                uint8_t a[6];
                char name[HCI_NAME_MAX + 1];
                if (hci_parse_remote_name(&e, a, name) == 0 && name[0]) {
                    HciDevice upd = want[k];
                    snprintf(upd.name, sizeof upd.name, "%s", name);
                    upd.name_complete = 1;
                    upd.rssi = 127;
                    pthread_mutex_lock(&gBt.mu);
                    bt_publish(&upd);
                    pthread_mutex_unlock(&gBt.mu);
                    got++;
                }
                done = 1;
            } else {
                bt_d_event(run, &e);
            }
        }
        if (!done) bt_call(b, HCI_OP_REMOTE_NAME_CANCEL, want[k].addr, 6, NULL, 0, NULL, 1000, bt_d_event, run);
    }
    return got;
}

static void bt_count(int *le, int *classic) {
    *le = *classic = 0;
    for (int i = 0; i < gBt.ndev; i++) { if (gBt.dev[i].kind == HCI_DEV_LE) (*le)++; else (*classic)++; }
}

// One scan: classic inquiry on its own first (keyboards, mice, headphones in pairing mode: these only answer
// inquiries, and running an LE scan at the same time can starve the inquiry on a single controller), then an LE
// scan. Stops early for sleep / off / unplug. 0, or -1 if the link died.
static int bt_scan_once(Daemon *d, BtLink *b) {
    BtRun run = {0};
    pthread_mutex_lock(&gBt.mu);
    gBt.scanning = 1;
    int le0, cl0;
    bt_count(&le0, &cl0);
    pthread_mutex_unlock(&gBt.mu);
    double t0 = now_s();
    uint8_t inq[5] = {(uint8_t)HCI_GIAC_LAP, (uint8_t)(HCI_GIAC_LAP >> 8), (uint8_t)(HCI_GIAC_LAP >> 16), 4, 0};
    int ist = bt_call(b, HCI_OP_INQUIRY, inq, 5, NULL, 0, NULL, 2000, bt_d_event, &run);   // 4 x 1.28 s
    int rc = ist == 0 ? bt_listen(d, b, &run, 6.0, 1) : 0;
    if (ist == 0 && rc == 0 && !run.inquiry_done)
        bt_call(b, HCI_OP_INQUIRY_CANCEL, NULL, 0, NULL, 0, NULL, 1000, bt_d_event, &run);
    int names = 0;
    if (rc == 0) {
        names = bt_fetch_names(d, b, &run);
        if (names < 0) rc = -1;
    }
    pthread_mutex_lock(&gBt.mu);
    int le1, cl1;
    bt_count(&le1, &cl1);
    pthread_mutex_unlock(&gBt.mu);
    int lst = -3;
    if (rc == 0) {
        const uint8_t sp[7] = {1, 0x60, 0x00, 0x30, 0x00, 0, 0};
        const uint8_t on[2] = {1, 1}, off[2] = {0, 0};
        lst = bt_call(b, HCI_OP_LE_SET_SCAN_PARAMS, sp, 7, NULL, 0, NULL, 2000, bt_d_event, &run);
        if (lst == 0) lst = bt_call(b, HCI_OP_LE_SET_SCAN_ENABLE, on, 2, NULL, 0, NULL, 2000, bt_d_event, &run);
        if (lst == 0) {
            rc = bt_listen(d, b, &run, BT_SCAN_S - (now_s() - t0) > 3 ? BT_SCAN_S - (now_s() - t0) : 3, 0);
            if (rc >= 0) bt_call(b, HCI_OP_LE_SET_SCAN_ENABLE, off, 2, NULL, 0, NULL, 1000, bt_d_event, &run);
        }
    }
    pthread_mutex_lock(&gBt.mu);
    gBt.scanning = 0;
    gBt.last_classic = run.n_classic;
    gBt.last_le = run.n_le;
    int le2, cl2;
    bt_count(&le2, &cl2);
    bt_expire();
    pthread_mutex_unlock(&gBt.mu);
    // Counts only (no names or addresses in the log).
    printf("[macwifid] Bluetooth scan %.1f s: classic inquiry status %d%s, %d responses, %d new classic devices; "
           "%d names fetched; LE scan status %d, %d reports, %d new LE devices\n", now_s() - t0, ist,
           run.inquiry_done ? " (completed)" : "", run.n_classic, cl2 - cl0, names > 0 ? names : 0, lst, run.n_le, le2 - le0);
    fflush(stdout);
    if (ist != 0 && lst != 0) bt_set_error("the controller refused to scan");
    return rc < 0 ? -1 : 0;
}

static void *bt_thread(void *arg) {
    Daemon *d = arg;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // input latency matters here
#endif
    BtLink *b = NULL;
    double next_try = 0;
    while (!gStop) {
        if (!bt_should_run(d)) {
            bt_down(&b, gBtSleep ? "the Mac is going to sleep" : !gBt.power ? "turned off" : "adapter not ready");
            usleep(100000);
            continue;
        }
        if (!b) {
            if (now_s() < next_try) { usleep(200000); continue; }
            b = bt_up();
            if (!b) { next_try = now_s() + 10; continue; }
            pthread_mutex_lock(&gBt.mu);
            gBt.scan_req = 1;                  // a first look around after coming up
            pthread_mutex_unlock(&gBt.mu);
        }
        // Requests from the menu (connect / disconnect / forget).
        pthread_mutex_lock(&gBt.mu);
        int nact = gBp.nact;
        struct { int op; uint8_t addr[6]; } act[8];
        memcpy(act, gBp.act, sizeof act);
        gBp.nact = 0;
        pthread_mutex_unlock(&gBt.mu);
        for (int i = 0; i < nact; i++) {
            if (act[i].op == BT_ACT_CONNECT) {
                uint8_t psrm = 1;
                uint16_t co = 0;
                int audio = 0;
                pthread_mutex_lock(&gBt.mu);
                for (int k = 0; k < gBt.ndev; k++)
                    if (gBt.dev[k].kind == HCI_DEV_CLASSIC && !memcmp(gBt.dev[k].addr, act[i].addr, 6)) {
                        psrm = gBt.dev[k].psrm;
                        co = gBt.dev[k].clock_offset;
                        if (((gBt.dev[k].cod >> 8) & 0x1F) == 4) audio = 1;           // major class Audio/Video
                    }
                const BtPaired *pp = bp_find(act[i].addr);
                if (pp && (((pp->cod >> 8) & 0x1F) == 4 || !strcmp(pp->type, "Audio"))) audio = 1;
                pthread_mutex_unlock(&gBt.mu);
                if (audio && gAudio.active && !audio_is(act[i].addr)) {
                    pthread_mutex_lock(&gBt.mu);
                    BtDevStatus *s = bs_get(act[i].addr, 1);
                    snprintf(s->state, sizeof s->state, "failed");
                    snprintf(s->why, sizeof s->why, "another audio device is connected");
                    pthread_mutex_unlock(&gBt.mu);
                    continue;
                }
                if (bthost_connect_profile(&gHost, act[i].addr, psrm, co, audio ? L2CAP_PSM_AVDTP : L2CAP_PSM_HID_CTRL) != 0) {
                    pthread_mutex_lock(&gBt.mu);
                    BtDevStatus *s = bs_get(act[i].addr, 1);
                    if (bthost_state(&gHost, act[i].addr) == BTC_FREE) {
                        snprintf(s->state, sizeof s->state, "failed");
                        snprintf(s->why, sizeof s->why, "too many Bluetooth connections");
                    }
                    pthread_mutex_unlock(&gBt.mu);
                }
            } else if (act[i].op == BT_ACT_SOUND) {
                if (audio_is(act[i].addr)) {
                    gAudio.tone_until = now_s() + 1.2;
                    gAudio.phase = 0;
                    if (!gAudio.streaming) {
                        if (avdtp_resume(&gAudio.av) != 0 && gAudio.av.state == AV_IDLE) avdtp_start(&gAudio.av);
                    }
                }
            } else {
                bthost_disconnect(&gHost, act[i].addr);
                if (act[i].op == BT_ACT_FORGET) {
                    pthread_mutex_lock(&gBt.mu);
                    bp_remove(act[i].addr);
                    BtDevStatus *s = bs_get(act[i].addr, 0);
                    if (s) s->state[0] = 0;
                    pthread_mutex_unlock(&gBt.mu);
                }
            }
        }
        bthost_poll(&gHost, now_s());
        if (gAudio.active && gAudio.avdtp_kick > 0 && now_s() >= gAudio.avdtp_kick) {
            gAudio.avdtp_kick = 0;
            if (gAudio.av.state == AV_IDLE) avdtp_start(&gAudio.av);   // the device didn't set the stream up itself
        }
        audio_pump();
        pthread_mutex_lock(&gBt.mu);
        // an inquiry would slow paging down and starve the audio stream
        int scan = gBt.scan_req && !bt_busy_connecting() && !gAudio.streaming;
        if (scan) gBt.scan_req = 0;
        pthread_mutex_unlock(&gBt.mu);
        if (scan && bt_scan_once(d, b) != 0) { bt_down(&b, "lost the adapter"); next_try = now_s() + 2; continue; }
        uint8_t ev[HCI_MAX_EVENT];
        size_t n = 0;
        int wait_ms = 100;
        if (gAudio.streaming) {                   // wake up in time for the next audio packet
            double dt = (gAudio.next_due - now_s()) * 1000;
            wait_ms = dt < 1 ? 1 : dt > 20 ? 20 : (int)dt;
        }
        int r = bt_next_event(b, ev, &n, wait_ms);   // handle events (connections, input), notice unplugging
        if (r < 0) { bt_down(&b, "lost the adapter"); next_try = now_s() + 2; continue; }
        HciEvent e;
        if (r == 1 && hci_event(ev, n, &e) == 0) bt_d_event(NULL, &e);
    }
    bt_down(&b, "service stopping");
    return NULL;
}

static void reply_bt(JsonOut *j) {
    pthread_mutex_lock(&gBt.mu);
    bt_expire();
    jo_bool(j, "power", gBt.power);
    jo_bool(j, "up", gBt.up);
    jo_bool(j, "scanning", gBt.scanning);
    jo_str(j, "controller", gBt.controller);
    jo_str(j, "error", gBt.error);
    jo_int(j, "last_classic", gBt.last_classic);
    jo_int(j, "last_le", gBt.last_le);
    jo_key_arr(j, "devices");
    double t = now_s();
    for (int i = 0; i < gBt.ndev; i++) {
        const HciDevice *v = &gBt.dev[i];
        char a[18];
        hci_addr_str(v->addr, a);
        const char *kind = v->kind == HCI_DEV_CLASSIC ? hci_cod_kind(v->cod) : hci_appearance_kind(v->appearance);
        const BtPaired *pp = bp_find(v->addr);
        const BtDevStatus *s = bs_get(v->addr, 0);
        jo_begin_obj(j);
        jo_str(j, "id", a);
        jo_str(j, "name", v->name[0] ? v->name : pp ? pp->name : "");
        jo_str(j, "transport", v->kind == HCI_DEV_CLASSIC ? "classic" : "le");
        jo_str(j, "type", kind ? kind : pp ? pp->type : "");
        jo_int(j, "rssi", v->rssi == 127 ? -127 : v->rssi);
        jo_bool(j, "connectable", v->connectable);
        jo_int(j, "age", (long long)(t - gBt.seen[i]));
        jo_bool(j, "paired", pp != NULL);
        jo_str(j, "state", s ? s->state : "");
        if (audio_is(v->addr)) jo_str(j, "audio", gAudio.streaming ? "playing" : gAudio.media_cid ? "ready" : "setting up");
        if (s && s->has_passkey) jo_int(j, "passkey", s->passkey);
        if (s && !strcmp(s->state, "failed")) jo_str(j, "why", s->why);
        jo_end_obj(j);
    }
    // Paired devices that aren't in the nearby list (off, asleep, or simply not advertising)
    for (int i = 0; i < gBp.npaired; i++) {
        const BtPaired *pp = &gBp.paired[i];
        int listed = 0;
        for (int k = 0; k < gBt.ndev; k++) if (!memcmp(gBt.dev[k].addr, pp->addr, 6)) listed = 1;
        if (listed) continue;
        char a[18];
        hci_addr_str(pp->addr, a);
        const BtDevStatus *s = bs_get(pp->addr, 0);
        jo_begin_obj(j);
        jo_str(j, "id", a);
        jo_str(j, "name", pp->name);
        jo_str(j, "transport", "classic");
        jo_str(j, "type", pp->type);
        jo_int(j, "rssi", -127);
        jo_bool(j, "connectable", 1);
        jo_int(j, "age", -1);
        jo_bool(j, "paired", 1);
        jo_str(j, "state", s ? s->state : "");
        if (audio_is(pp->addr)) jo_str(j, "audio", gAudio.streaming ? "playing" : gAudio.media_cid ? "ready" : "setting up");
        if (s && s->has_passkey) jo_int(j, "passkey", s->passkey);
        if (s && !strcmp(s->state, "failed")) jo_str(j, "why", s->why);
        jo_end_obj(j);
    }
    jo_end_arr(j);
    pthread_mutex_lock(&gIn.mu);
    jo_bool(j, "input_app", gIn.readers > 0);
    pthread_mutex_unlock(&gIn.mu);
    pthread_mutex_unlock(&gBt.mu);
}

// Streams input lines to the menu app until it goes away (or the service stops).
static void bt_input_stream(int fd) {
    pthread_mutex_lock(&gIn.mu);
    unsigned long next = gIn.seq;
    gIn.readers++;
    pthread_mutex_unlock(&gIn.mu);
    signal(SIGPIPE, SIG_IGN);
    struct timeval stv = {2, 0};                       // an app that stops reading doesn't hold this thread forever
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
    char buf[BT_IN_RING * 41];
    for (;;) {
        pthread_mutex_lock(&gIn.mu);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 500000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (gIn.seq == next && !gStop)
            if (pthread_cond_timedwait(&gIn.cv, &gIn.mu, &ts) != 0) break;
        if (gIn.seq - next > BT_IN_RING) next = gIn.seq - BT_IN_RING;   // fell behind: drop the oldest
        size_t o = 0;
        for (; next != gIn.seq; next++) o += (size_t)snprintf(buf + o, sizeof buf - o, "%s\n", gIn.line[next % BT_IN_RING]);
        pthread_mutex_unlock(&gIn.mu);
        if (gStop) break;
        if (!o) {                                     // keepalive, so a closed client is noticed
            if (write(fd, "\n", 1) != 1) break;
            continue;
        }
        size_t w = 0;
        while (w < o) {
            ssize_t r = write(fd, buf + w, o - w);
            if (r <= 0) break;
            w += (size_t)r;
        }
        if (w < o) break;
    }
    pthread_mutex_lock(&gIn.mu);
    gIn.readers--;
    pthread_mutex_unlock(&gIn.mu);
}

static void serve_client(Daemon *d, int fd) {
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[4096];
    size_t n = 0;
    while (n < sizeof req - 1) {
        ssize_t r = read(fd, req + n, sizeof req - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (memchr(req, '\n', n)) break;
    }
    char *nl = memchr(req, '\n', n);
    if (nl) n = (size_t)(nl - req);
    char *out = malloc(32768);
    if (!out) return;
    JsonOut j;
    jo_init(&j, out, 32768);
    jo_begin_obj(&j);
    JsonObj o;
    int stream = 0;
    if (json_parse_flat(req, n, &o) != 0) {
        jo_bool(&j, "ok", 0);
        jo_str(&j, "error", "bad request");
    } else {
        const char *cmd = json_str(&o, "cmd", NULL);
        size_t sl = 0, pl = 0;
        const char *ssid = json_str(&o, "ssid", &sl);
        const char *pass = json_str(&o, "password", &pl);
        const JsonVal *on = json_get(&o, "on");
        char msg[200] = "";
        if (!cmd) {
            jo_bool(&j, "ok", 0);
            jo_str(&j, "error", "no cmd");
        } else if (!strcmp(cmd, "status")) {
            jo_bool(&j, "ok", 1);
            pthread_mutex_lock(&d->mu);
            reply_status(d, &j);
            pthread_mutex_unlock(&d->mu);
        } else if (!strcmp(cmd, "networks") || !strcmp(cmd, "scan")) {
            int ok = 1;
            if (!strcmp(cmd, "scan")) ok = d_post(d, DC_SCAN, NULL, 0, NULL, 0, 0, 30, msg, sizeof msg);
            jo_bool(&j, "ok", ok);
            if (!ok) jo_str(&j, "error", msg);
            pthread_mutex_lock(&d->mu);
            reply_networks(d, &j);
            pthread_mutex_unlock(&d->mu);
        } else if (!strcmp(cmd, "known")) {
            jo_bool(&j, "ok", 1);
            jo_key_arr(&j, "known");
            pthread_mutex_lock(&d->mu);
            for (int i = 0; i < d->known.count; i++) {
                jo_begin_obj(&j);
                jo_strn(&j, "ssid", d->known.n[i].ssid, d->known.n[i].ssid_len);
                jo_bool(&j, "secured", d->known.n[i].has_pmk);
                jo_end_obj(&j);
            }
            pthread_mutex_unlock(&d->mu);
            jo_end_arr(&j);
        } else if (!strcmp(cmd, "join")) {
            if (!ssid || !sl || sl > 32 || (pass && (pl < 8 || pl > 63))) {
                jo_bool(&j, "ok", 0);
                jo_str(&j, "error", !ssid || !sl || sl > 32 ? "bad network name" : "a WPA2 password has 8 to 63 characters");
            } else {
                int ok = d_post(d, DC_JOIN, (const uint8_t *)ssid, sl, pass, pl, 0, 60, msg, sizeof msg);
                jo_bool(&j, "ok", ok);
                if (!ok) jo_str(&j, "error", msg);
            }
        } else if (!strcmp(cmd, "disconnect") || !strcmp(cmd, "forget") || !strcmp(cmd, "power")) {
            DCmd c = !strcmp(cmd, "disconnect") ? DC_DISCONNECT : !strcmp(cmd, "forget") ? DC_FORGET : DC_POWER;
            if ((c == DC_FORGET && (!ssid || !sl || sl > 32)) || (c == DC_POWER && (!on || on->type != JSON_BOOL))) {
                jo_bool(&j, "ok", 0);
                jo_str(&j, "error", "missing argument");
            } else {
                int ok = d_post(d, c, (const uint8_t *)ssid, sl, NULL, 0, on ? (int)on->num : 0, 30, msg, sizeof msg);
                jo_bool(&j, "ok", ok);
                if (!ok) jo_str(&j, "error", msg);
            }
        } else if (!strcmp(cmd, "bt") || !strcmp(cmd, "bt_scan")) {
            if (!strcmp(cmd, "bt_scan")) {
                pthread_mutex_lock(&gBt.mu);
                gBt.scan_req = 1;
                pthread_mutex_unlock(&gBt.mu);
            }
            jo_bool(&j, "ok", 1);
            reply_bt(&j);
        } else if (!strcmp(cmd, "bt_connect") || !strcmp(cmd, "bt_disconnect") || !strcmp(cmd, "bt_forget") ||
                   !strcmp(cmd, "bt_test_sound")) {
            size_t il = 0;
            const char *id = json_str(&o, "id", &il);
            uint8_t a[6];
            if (!id || parse_addr(id, il, a) != 0) {
                jo_bool(&j, "ok", 0);
                jo_str(&j, "error", "bad device id");
            } else {
                int op = !strcmp(cmd, "bt_connect") ? BT_ACT_CONNECT : !strcmp(cmd, "bt_disconnect") ? BT_ACT_DISCONNECT
                       : !strcmp(cmd, "bt_forget") ? BT_ACT_FORGET : BT_ACT_SOUND;
                pthread_mutex_lock(&gBt.mu);
                int le_only = 0;
                for (int k = 0; k < gBt.ndev; k++)
                    if (!memcmp(gBt.dev[k].addr, a, 6) && gBt.dev[k].kind == HCI_DEV_LE && !bp_find(a)) le_only = 1;
                int ok = gBt.up && gBp.nact < 8 && !(op == BT_ACT_CONNECT && le_only);
                if (ok) {
                    gBp.act[gBp.nact].op = op;
                    memcpy(gBp.act[gBp.nact].addr, a, 6);
                    gBp.nact++;
                    if (op == BT_ACT_CONNECT) {
                        BtDevStatus *s = bs_get(a, 1);
                        snprintf(s->state, sizeof s->state, "connecting");
                        s->t = now_s();
                    }
                }
                pthread_mutex_unlock(&gBt.mu);
                jo_bool(&j, "ok", ok);
                if (!ok) jo_str(&j, "error", le_only ? "Bluetooth LE devices can't be connected yet (classic ones can)"
                                                     : "Bluetooth isn't ready");
            }
        } else if (!strcmp(cmd, "bt_input")) {
            stream = 1;
            jo_bool(&j, "ok", 1);
        } else if (!strcmp(cmd, "bt_power")) {
            if (!on || on->type != JSON_BOOL) {
                jo_bool(&j, "ok", 0);
                jo_str(&j, "error", "missing argument");
            } else {
                pthread_mutex_lock(&gBt.mu);
                gBt.power = on->num != 0;
                if (!gBt.power) gBt.ndev = 0;
                pthread_mutex_unlock(&gBt.mu);
                bt_save_power(d);
                jo_bool(&j, "ok", 1);
                reply_bt(&j);
            }
        } else {
            jo_bool(&j, "ok", 0);
            jo_str(&j, "error", "unknown cmd");
        }
        json_wipe(&o);
    }
    secure_zero(req, sizeof req);
    jo_end_obj(&j);
    size_t len = jo_finish(&j);
    if (!len) len = (size_t)snprintf(out, 32768, "{\"ok\":false,\"error\":\"reply too long\"}\n");
    for (size_t w = 0; w < len;) {
        ssize_t r = write(fd, out + w, len - w);
        if (r <= 0) break;
        w += (size_t)r;
    }
    free(out);
    if (stream) bt_input_stream(fd);
}

typedef struct { Daemon *d; int fd; } ClientArg;
static void *client_thread(void *arg) {
    ClientArg *ca = (ClientArg *)arg;
    serve_client(ca->d, ca->fd);
    close(ca->fd);
    free(ca);
    return NULL;
}

// Accepts clients (poll with a timeout, so a stop is noticed); each gets its own short-lived thread, so a
// status request isn't stuck behind a join that takes many seconds.
static void *control_thread(void *arg) {
    Daemon *d = (Daemon *)arg;
    while (!gStop) {
        struct pollfd pf = {d->listen_fd, POLLIN, 0};
        if (poll(&pf, 1, 300) <= 0) continue;
        int fd = accept(d->listen_fd, NULL, NULL);
        if (fd < 0) continue;
        ClientArg *ca = malloc(sizeof *ca);
        pthread_t t;
        if (!ca) { close(fd); continue; }
        ca->d = d;
        ca->fd = fd;
        if (pthread_create(&t, NULL, client_thread, ca) != 0) { close(fd); free(ca); continue; }
        pthread_detach(t);
    }
    return NULL;
}

static int open_control_socket(Daemon *d) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(d->h.sock) >= sizeof a.sun_path) return -1;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", d->h.sock);
    unlink(a.sun_path);
    d->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (d->listen_fd < 0) return -1;
    mode_t old = umask(0117);   // created 0660
    int r = bind(d->listen_fd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r != 0 || listen(d->listen_fd, 8) != 0) { perror("control socket"); return -1; }
    struct group *g = getgrnam("admin");   // the menu bar app runs as an admin user
    if (g && geteuid() == 0 && chown(a.sun_path, 0, g->gr_gid) != 0) perror("chown control socket");
    chmod(a.sun_path, 0660);
    return 0;
}

// ---------------------------------------------------------------- the adapter (main thread)

static void d_finish(Daemon *d, int ok, const char *msg) {
    pthread_mutex_lock(&d->mu);
    d->cmd_ok = ok;
    snprintf(d->cmd_msg, sizeof d->cmd_msg, "%s", msg ? msg : "");
    secure_zero(d->cmd_pass, sizeof d->cmd_pass);
    d->cmd = DC_NONE;
    d->done_seq = d->cmd_seq;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
}

// Takes a pending command (copies it out), or returns DC_NONE.
static DCmd d_take(Daemon *d, uint8_t ssid[32], size_t *ssid_len, char pass[64], int *has_pass, int *on) {
    pthread_mutex_lock(&d->mu);
    DCmd c = d->done_seq < d->cmd_seq ? d->cmd : DC_NONE;
    if (c != DC_NONE) {
        memcpy(ssid, d->cmd_ssid, 32);
        *ssid_len = d->cmd_ssid_len;
        memcpy(pass, d->cmd_pass, 64);
        *has_pass = d->cmd_has_pass;
        *on = d->cmd_on;
    }
    pthread_mutex_unlock(&d->mu);
    return c;
}

// Merges a scan into the client-visible list: one entry per name (the strongest AP of this scan), hidden networks
// skipped; networks no scan has seen for NET_KEEP_S drop out (as macOS's own Wi-Fi menu ages its list).
static void d_publish_scan(Daemon *d, const ScanList *sl) {
    double now = now_s();
    pthread_mutex_lock(&d->mu);
    int keep = 0;
    for (int i = 0; i < d->nnets; i++)
        if (now - d->nets[i].seen < NET_KEEP_S) d->nets[keep++] = d->nets[i];
    d->nnets = keep;
    uint8_t fresh[48] = {0};   // entries already updated by this scan
    for (int i = 0; i < sl->n; i++) {
        const ScanEntry *e = &sl->e[i];
        int blank = 1;
        for (int k = 0; k < e->bss.ssid_len; k++) if (e->bss.ssid[k]) blank = 0;
        if (blank) continue;
        int sec;
        const char *why;
        int usable = bss_usable(&e->bss, &sec, &why);
        NetView *v = NULL;
        int vi = -1;
        for (int k = 0; k < d->nnets; k++)
            if (d->nets[k].ssid_len == e->bss.ssid_len && !memcmp(d->nets[k].ssid, e->bss.ssid, e->bss.ssid_len)) { v = &d->nets[k]; vi = k; }
        // within this scan keep the best AP of a name; an older entry is simply refreshed
        if (v && fresh[vi] && (v->usable > usable || (v->usable == usable && v->rssi >= e->rssi))) continue;
        if (!v) {
            if (d->nnets >= (int)(sizeof d->nets / sizeof d->nets[0])) continue;
            vi = d->nnets;
            v = &d->nets[d->nnets++];
        }
        fresh[vi] = 1;
        v->seen = now;
        memcpy(v->ssid, e->bss.ssid, e->bss.ssid_len);
        v->ssid_len = e->bss.ssid_len;
        v->rssi = e->rssi;
        v->freq = e->freq;
        v->secured = sec;
        v->sae = bss_can_sae(&e->bss);
        v->akm_sae = e->bss.rsn_akm_sae;
        v->akm_psk = e->bss.security == WIFI_SEC_WPA2_PSK || e->bss.security == WIFI_SEC_WPA2_WPA3;
        v->sae_only = v->sae && !bss_can_psk(&e->bss);
        v->usable = usable;
        v->why = why;
        v->known = kn_find(&d->known, v->ssid, v->ssid_len) != NULL;
    }
    // strongest first
    for (int a = 1; a < d->nnets; a++)
        for (int b = a; b > 0 && d->nets[b].rssi > d->nets[b - 1].rssi; b--) {
            NetView t = d->nets[b]; d->nets[b] = d->nets[b - 1]; d->nets[b - 1] = t;
        }
    d->scan_time = now;
    pthread_mutex_unlock(&d->mu);
}

// ---------------------------------------------------------------- captive portal checks (own thread each)
// Started once macOS has an address; repeated every gCaptiveRecheck seconds while a portal is in the way (so the
// menu notices when sign-in is done) and every 5 minutes otherwise. A result from an earlier connection is
// ignored (generation counter).
static double gCaptiveRecheck = 15;
static atomic_int gCaptiveBusy;
static atomic_uint gConnGen;
typedef struct { Daemon *d; unsigned gen; } ProbeArg;
static void *captive_thread(void *arg) {
    ProbeArg *p = (ProbeArg *)arg;
    char url[512];
    int r = feth_captive_probe(FETH_NAME, url, sizeof url);
    pthread_mutex_lock(&p->d->mu);
    if (p->gen == gConnGen && r != CAPTIVE_UNKNOWN) {
        if (r != p->d->captive)
            printf("[macwifid] captive portal: %s\n", r == CAPTIVE_PORTAL ? "yes, sign-in needed" : "none");
        p->d->captive = r;
        snprintf(p->d->portal, sizeof p->d->portal, "%s", url);
    }
    pthread_mutex_unlock(&p->d->mu);
    gCaptiveBusy = 0;
    free(p);
    return NULL;
}

static void captive_maybe_probe(Daemon *d, int have_ip, double *next) {
    static unsigned probed_gen = ~0u;
    if (!have_ip || gCaptiveBusy) return;
    if (gConnGen != probed_gen) { probed_gen = gConnGen; *next = 0; }   // new connection: check right away
    if (now_s() < *next) return;
    pthread_mutex_lock(&d->mu);
    int settled = d->captive == CAPTIVE_NONE;
    pthread_mutex_unlock(&d->mu);
    *next = now_s() + (settled ? 300 : gCaptiveRecheck);
    ProbeArg *p = malloc(sizeof *p);
    pthread_t t;
    if (!p) return;
    p->d = d;
    p->gen = gConnGen;
    gCaptiveBusy = 1;
    if (pthread_create(&t, NULL, captive_thread, p) != 0) { gCaptiveBusy = 0; free(p); return; }
    pthread_detach(t);
}

typedef struct {
    Link l;
    Conn c;
    Feth f;
    WpaSupplicant ws;
    ScanList sl;
    uint8_t mac[6];
    int up, connected, feth_open;
    int sae_capable;          // the access point we last joined offers WPA3
} Adapter;

static Adapter gA;

static void d_disconnect(Daemon *d, Adapter *a) {
    if (a->connected) {
        bridge_stop(&a->c);
        a->connected = 0;
    }
    conn_leave(&a->c);
    if (a->feth_open) { feth_close(&a->f); a->feth_open = 0; }
    secure_zero(&a->ws, sizeof a->ws);
    pthread_mutex_lock(&d->mu);
    d->ssid_len = 0;
    d->ip[0] = 0;
    d->captive = CAPTIVE_UNKNOWN;
    gConnGen++;
    pthread_mutex_unlock(&d->mu);
}

static void d_release(Daemon *d, Adapter *a) {
    if (!a->up) return;
    d_disconnect(d, a);
    pthread_mutex_destroy(&a->c.tx_lock);
    link_rx_stop(&a->l);
    d->h.release(&a->l);
    a->up = 0;
}

static int d_scan(Daemon *d, Adapter *a, const uint8_t *ssid, size_t ssid_len) {
    DState prev = d->state;
    if (!a->connected) d_set_state(d, DS_SCANNING);
    char name[33];
    if (ssid) { memcpy(name, ssid, ssid_len); name[ssid_len] = 0; }
    double t0 = now_s();
    int r = conn_scan(&a->c, ssid ? name : NULL, &a->sl);
    AicScanCfm sc = {0, 0xff, 0};
    if (r == 0) aic_fmac_parse_scan_cfm(a->c.params, a->c.plen, &sc);
    int names = 0;
    for (int i = 0; i < a->sl.n; i++) {
        int dup = 0;
        for (int k = 0; k < i; k++) dup |= ssid_is(&a->sl.e[k].bss, (const uint8_t *)a->sl.e[i].bss.ssid, a->sl.e[i].bss.ssid_len);
        names += !dup;
    }
    printf("[macwifid] %s scan%s in %.1f s: firmware status %u, %u results; %d frames, %d BSSs, %d names, %d unparsed\n",
           ssid ? "directed" : "full", a->connected ? " while connected" : "", now_s() - t0, sc.status, sc.result_cnt,
           a->sl.frames, a->sl.n, names, a->sl.unparsed);
    if (r == 0 && !ssid) d_publish_scan(d, &a->sl);
    if (!a->connected) d_set_state(d, prev == DS_SCANNING ? DS_IDLE : prev);
    return r;
}

// Joins `ssid` with what we know (cred: the WPA2 key and/or the password for WPA3; both NULL = open). Returns 0, or
// -1 with `err` set.
static int d_join(Daemon *d, Adapter *a, const uint8_t *ssid, size_t ssid_len, const JoinCred *cred, char *err, size_t errcap,
                  int *wrong_password) {
    *wrong_password = 0;
    if (a->connected || a->c.associated) d_disconnect(d, a);
    pthread_mutex_lock(&d->mu);
    memcpy(d->ssid, ssid, ssid_len);
    d->ssid_len = (uint8_t)ssid_len;
    pthread_mutex_unlock(&d->mu);
    d_set_state(d, DS_CONNECTING);
    if (d_scan(d, a, ssid, ssid_len) != 0) {
        snprintf(err, errcap, "scan failed");
        d_set_state(d, DS_IDLE);
        return -1;
    }
    int found = 0;
    const ScanEntry *best = scan_pick(&a->sl, ssid, ssid_len, 0, &found);
    if (!best) {
        snprintf(err, errcap, "%s", found ? "this network's security isn't supported yet" : "network not found");
        d_set_state(d, DS_IDLE);
        return -1;
    }
    int secured;
    const char *why;
    bss_usable(&best->bss, &secured, &why);
    if (secured && !cred->pmk && !cred->pass) {
        snprintf(err, errcap, "a password is needed");
        d_set_state(d, DS_IDLE);
        return -1;
    }
    memset(&a->ws, 0, sizeof a->ws);
    pthread_mutex_lock(&d->mu);
    d->freq = best->freq;
    d->rssi = best->rssi;
    pthread_mutex_unlock(&d->mu);
    a->sae_capable = secured && bss_can_sae(&best->bss);
    int had_cred = secured && ((bss_can_sae(&best->bss) && cred->pass) || (bss_can_psk(&best->bss) && cred->pmk));
    JoinResult jr = secured ? conn_join_secured(&a->c, best, ssid, ssid_len, &a->ws, cred, err, errcap)
                            : conn_associate(&a->c, best, ssid, ssid_len, NULL, NULL, 0, err, errcap);
    pthread_mutex_lock(&d->mu);
    d->sec = !secured ? "open" : a->c.used_sae ? "wpa3" : "wpa2";
    pthread_mutex_unlock(&d->mu);
    if (jr != JOIN_OK) {
        *wrong_password = jr == JOIN_PASSWORD && had_cred;   // not when we simply lacked the password for WPA3
        conn_leave(&a->c);
        secure_zero(&a->ws, sizeof a->ws);
        d_set_state(d, a->c.usb_error ? DS_ERROR : DS_IDLE);
        return -1;
    }
    if (feth_open(&a->f, a->mac) != 0) {
        snprintf(err, errcap, "couldn't create the network interface");
        conn_leave(&a->c);
        d_set_state(d, DS_IDLE);
        return -1;
    }
    a->feth_open = 1;
    a->c.feth = &a->f;
    if (bridge_start(&a->c, 0) != 0) {
        snprintf(err, errcap, "couldn't start the bridge");
        conn_leave(&a->c);
        feth_close(&a->f);
        a->feth_open = 0;
        d_set_state(d, DS_IDLE);
        return -1;
    }
    a->connected = 1;
    pthread_mutex_lock(&d->mu);
    d->captive = CAPTIVE_UNKNOWN;
    gConnGen++;
    pthread_mutex_unlock(&d->mu);
    d_set_state(d, DS_CONNECTED);
    d_set_error(d, "");
    return 0;
}

// Runs the command the control thread posted.
static void d_handle(Daemon *d, Adapter *a, int *manual_off, double *next_autojoin) {
    uint8_t ssid[32];
    size_t ssid_len = 0;
    char pass[64];
    int has_pass = 0, on = 0;
    DCmd cmd = d_take(d, ssid, &ssid_len, pass, &has_pass, &on);
    if (cmd == DC_NONE) return;
    char err[200] = "";
    switch (cmd) {
        case DC_POWER:
            d->power = on;
            d_save_power(d);
            if (!on) { d_release(d, a); d_set_state(d, DS_OFF); }
            *manual_off = 0;
            d_finish(d, 1, "");
            break;
        case DC_FORGET: {
            pthread_mutex_lock(&d->mu);
            int removed = kn_remove(&d->known, ssid, ssid_len);
            for (int i = 0; i < d->nnets; i++)
                if (d->nets[i].ssid_len == ssid_len && !memcmp(d->nets[i].ssid, ssid, ssid_len)) d->nets[i].known = 0;
            int current = d->ssid_len == ssid_len && !memcmp(d->ssid, ssid, ssid_len);
            pthread_mutex_unlock(&d->mu);
            if (removed) d_save_known(d);
            if (current && a->connected) { d_disconnect(d, a); d_set_state(d, DS_IDLE); }
            d_finish(d, 1, removed ? "" : "not a known network");
            break;
        }
        case DC_DISCONNECT:
            if (a->up) { d_disconnect(d, a); d_set_state(d, DS_IDLE); }
            *manual_off = 1;   // stay off until the user joins again (or the adapter is replugged)
            d_finish(d, 1, "");
            break;
        case DC_SCAN:
            if (!a->up) { d_finish(d, 0, d->power ? "the adapter isn't ready" : "Wi-Fi is off"); break; }
            d_finish(d, d_scan(d, a, NULL, 0) == 0, "scan failed");
            break;
        case DC_JOIN: {
            if (!a->up) { d_finish(d, 0, d->power ? "the adapter isn't ready" : "Wi-Fi is off"); break; }
            uint8_t pmk[32];
            int have_pmk = 0;
            size_t pass_len = 0;
            if (has_pass) {
                have_pmk = wpa_pmk_from_passphrase(pass, ssid, ssid_len, pmk) == 0;
                pass_len = strlen(pass);
            } else {
                pthread_mutex_lock(&d->mu);
                KnownNet *k = kn_find(&d->known, ssid, ssid_len);
                if (k && k->has_pmk) { memcpy(pmk, k->pmk, 32); have_pmk = 1; }
                if (k && k->pass_len) { memcpy(pass, k->pass, k->pass_len); pass[k->pass_len] = 0; pass_len = k->pass_len; }
                pthread_mutex_unlock(&d->mu);
            }
            int wrong = 0;
            JoinCred cred = {pass_len ? pass : NULL, pass_len, have_pmk ? pmk : NULL};
            int r = d_join(d, a, ssid, ssid_len, &cred, err, sizeof err, &wrong);
            if (r == 0) {
                pthread_mutex_lock(&d->mu);
                if (has_pass || !kn_find(&d->known, ssid, ssid_len)) {
                    kn_put(&d->known, ssid, ssid_len, have_pmk ? pmk : NULL);
                    // WPA3 needs the password itself at every sign-in, so for networks that offer WPA3 it is kept
                    // (root-only file) next to the WPA2 key; for WPA2-only networks the key alone is enough
                    if (has_pass && a->sae_capable) kn_set_pass(kn_find(&d->known, ssid, ssid_len), pass, pass_len);
                }
                pthread_mutex_unlock(&d->mu);
                d_save_known(d);
                *manual_off = 0;
            } else {
                d_set_error(d, err);
                if (wrong && !has_pass) {   // a remembered key stopped working: stop auto-joining it
                    pthread_mutex_lock(&d->mu);
                    KnownNet *k = kn_find(&d->known, ssid, ssid_len);
                    if (k) k->autojoin = 0;
                    pthread_mutex_unlock(&d->mu);
                    d_save_known(d);
                }
            }
            secure_zero(pmk, sizeof pmk);
            secure_zero(pass, sizeof pass);
            *next_autojoin = now_s() + 30;
            d_finish(d, r == 0, err);
            break;
        }
        default:
            d_finish(d, 0, "unknown");
    }
    secure_zero(pass, sizeof pass);
}

// launchd points stdout/stderr at /Library/Logs/macwifid.log and never reopens it, so the daemon rotates it
// itself: past 2 MB it keeps three old copies (.1 .. .3) and reopens the file.
#define MW_LOG "/Library/Logs/macwifid.log"
static void d_rotate_log(void) {
    struct stat st;
    if (stat(MW_LOG, &st) != 0 || st.st_size < 2 * 1024 * 1024) return;
    fflush(stdout);
    fflush(stderr);
    char a[64], b[64];
    for (int i = 2; i >= 1; i--) {
        snprintf(a, sizeof a, "%s.%d", MW_LOG, i);
        snprintf(b, sizeof b, "%s.%d", MW_LOG, i + 1);
        rename(a, b);
    }
    rename(MW_LOG, MW_LOG ".1");
    if (freopen(MW_LOG, "a", stdout)) setvbuf(stdout, NULL, _IOLBF, 0);
    if (freopen(MW_LOG, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    chmod(MW_LOG, 0644);
    printf("[macwifid] log rotated, version %s\n", MW_VERSION);
}

static int daemon_main(const DaemonHooks *hooks) {
    Daemon *d = &gD;
    memset(d, 0, sizeof *d);
    d->h = *hooks;
    pthread_mutex_init(&d->mu, NULL);
    pthread_cond_init(&d->cv, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[macwifid] starting, version %s (pid %d)\n", MW_VERSION, (int)getpid());
    d_load(d);
    if (geteuid() == 0) {
        int n = feth_cleanup();
        if (n > 0) printf("[macwifid] removed %d item(s) left over from an earlier run\n", n);
    }
    if (open_control_socket(d) != 0) return 1;
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGHUP, on_sigint);
    signal(SIGPIPE, SIG_IGN);
    pthread_t ct;
    pthread_create(&ct, NULL, control_thread, d);
    pthread_t btt;
    int bt_on = 0;
    if (d->h.bluetooth) {
        bt_load_power(d);
        bp_load();
        bt_on = pthread_create(&btt, NULL, bt_thread, d) == 0;
    }

    Adapter *a = &gA;
    memset(a, 0, sizeof *a);
    double next_try = 0, next_autojoin = 0, autojoin_backoff = 5, next_log_check = 0, next_captive = 0;
    int manual_off = 0, sleeping = 0, quick_retries = 0;
    d->state = d->power ? DS_NO_ADAPTER : DS_OFF;
    while (!gStop) {
        if (geteuid() == 0 && now_s() >= next_log_check) {
            next_log_check = now_s() + 60;
            d_rotate_log();
        }
        if (gSleepReq) {
            printf("[macwifid] shutting the adapter down for sleep\n");
            if (a->up) d_release(d, a);
            pthread_mutex_lock(&d->mu);
            d->adapter = 0;
            pthread_mutex_unlock(&d->mu);
            sleeping = 1;
            d_set_state(d, DS_ASLEEP);
            pthread_mutex_lock(&gPmMu);
            gSleepReq = 0;
            gSleepReady = 1;
            pthread_cond_broadcast(&gPmCv);
            pthread_mutex_unlock(&gPmMu);
        }
        if (gWake) {
            gWake = 0;
            if (sleeping) {
                sleeping = 0;
                next_try = now_s() + 1;   // give USB a moment to bring the adapter back
                quick_retries = 5;
                d_set_state(d, d->power ? DS_NO_ADAPTER : DS_OFF);
            }
        }
        if (sleeping) {
            d_handle(d, a, &manual_off, &next_autojoin);
            usleep(200000);
            continue;
        }
        if (!d->power) {
            if (a->up) d_release(d, a);
            d_set_state(d, DS_OFF);
            d_handle(d, a, &manual_off, &next_autojoin);
            usleep(200000);
            continue;
        }
        if (!a->up) {
            int present = d->h.adapter_present();
            pthread_mutex_lock(&d->mu);
            d->adapter = present;
            pthread_mutex_unlock(&d->mu);
            if (present && now_s() >= next_try) {
                d_set_state(d, DS_STARTING);
                d_set_error(d, "");
                uint8_t vif = 0;
                if (d->h.acquire(&a->l, a->mac, &vif) == 0) {
                    link_rx_start(&a->l);
                    pthread_mutex_lock(&d->mu);
                    d->adapter = 1;
                    pthread_mutex_unlock(&d->mu);
                    memset(&a->c, 0, sizeof a->c);
                    a->c.l = &a->l;
                    memcpy(a->c.mac, a->mac, 6);
                    a->c.vif = vif;
                    pthread_mutex_init(&a->c.tx_lock, NULL);
                    a->up = 1;
                    manual_off = 0;
                    next_autojoin = 0;
                    autojoin_backoff = 5;
                    d_set_state(d, DS_IDLE);
                } else if (access("firmware/aic8800D80-legacy-mcu1/fmacfw_8800d80_u02.bin", R_OK) != 0 &&
                           access("firmware/aic8800D80/fmacfw_8800d80_u02.bin", R_OK) != 0) {
                    d_set_error(d, "the adapter firmware isn't installed: run sudo MW_FW_ROOT=\"" MW_DIR "\" \"" MW_DIR
                                   "/fetch-firmware.sh\"");
                    d_set_state(d, DS_ERROR);
                    next_try = now_s() + 30;
                } else {
                    d_set_error(d, "the adapter didn't start; unplug it and plug it back in");
                    d_set_state(d, DS_ERROR);
                    next_try = now_s() + (quick_retries > 0 ? 3 : 15);   // right after wake USB may still be settling
                    if (quick_retries > 0) quick_retries--;
                }
            } else if (!present) {
                d_set_state(d, DS_NO_ADAPTER);
                next_try = 0;
                quick_retries = 4;   // a freshly plugged adapter can need a moment (e.g. its virtual CD isn't mounted yet)
            }
            d_handle(d, a, &manual_off, &next_autojoin);
            if (!a->up) usleep(500000);
            continue;
        }

        // adapter up
        pump(&a->c, 250, a->connected ? w_pending : NULL);
        if (a->c.usb_error) {
            printf("[macwifid] lost the adapter (unplugged or reset)\n");
            a->c.disconnected = 1;
            d_release(d, a);
            pthread_mutex_lock(&d->mu);
            d->adapter = 0;
            pthread_mutex_unlock(&d->mu);
            d_set_state(d, DS_NO_ADAPTER);
            next_try = now_s() + 2;
            continue;
        }
        if (a->connected) {
            if (a->c.pending && !a->c.open_net && handle_wpa_events(&a->c, &a->ws)) a->c.disconnected = 1;
            bridge_poll(&a->c, 300);
            pthread_mutex_lock(&d->mu);
            _Static_assert(sizeof d->ip == sizeof a->c.br.ip, "ip buffers");
            memcpy(d->ip, a->c.br.ip, sizeof d->ip);
            d->ip[sizeof d->ip - 1] = 0;
            if (a->c.last_rssi) d->rssi = a->c.last_rssi;
            pthread_mutex_unlock(&d->mu);
            captive_maybe_probe(d, a->c.br.ip[0] != 0, &next_captive);
            if (a->c.disconnected) {
                char e[160];
                snprintf(e, sizeof e, "the network dropped us: reason %u (%s)", a->c.disc_reason, reason_text(a->c.disc_reason));
                d_set_error(d, e);
                d_disconnect(d, a);
                d_set_state(d, DS_IDLE);
                next_autojoin = now_s() + 1;   // try again right away (backoff grows if it keeps failing)
            }
        }
        d_handle(d, a, &manual_off, &next_autojoin);
        int any_autojoin = 0;
        pthread_mutex_lock(&d->mu);
        for (int i = 0; i < d->known.count; i++) any_autojoin |= d->known.n[i].autojoin;
        pthread_mutex_unlock(&d->mu);
        if (a->up && !a->connected && !manual_off && any_autojoin && now_s() >= next_autojoin) {
            // auto-join: scan, pick the strongest remembered network
            int ok = 0;
            if (d_scan(d, a, NULL, 0) == 0) {
                KnSeen seen[48];
                int ns = 0;
                pthread_mutex_lock(&d->mu);
                for (int i = 0; i < d->nnets && ns < 48; i++) {
                    seen[ns].ssid = d->nets[i].ssid;
                    seen[ns].ssid_len = d->nets[i].ssid_len;
                    seen[ns].rssi = d->nets[i].rssi;
                    seen[ns].usable = d->nets[i].usable;
                    seen[ns].secured = d->nets[i].secured;
                    seen[ns].sae_only = d->nets[i].sae_only;
                    ns++;
                }
                int pick = kn_choose(&d->known, seen, ns);
                uint8_t ssid[32], pmk[32];
                char kpass[64] = "";
                size_t sl = 0, kpl = 0;
                int has_pmk = 0;
                if (pick >= 0) {
                    KnownNet *k = kn_find(&d->known, seen[pick].ssid, seen[pick].ssid_len);
                    sl = seen[pick].ssid_len;
                    memcpy(ssid, seen[pick].ssid, sl);
                    has_pmk = k->has_pmk;
                    if (has_pmk) memcpy(pmk, k->pmk, 32);
                    kpl = k->pass_len;
                    memcpy(kpass, k->pass, sizeof kpass);
                }
                pthread_mutex_unlock(&d->mu);
                if (pick >= 0) {
                    char err[200] = "";
                    int wrong = 0;
                    printf("[macwifid] auto-joining a remembered network\n");
                    JoinCred cred = {kpl ? kpass : NULL, kpl, has_pmk ? pmk : NULL};
                    ok = d_join(d, a, ssid, sl, &cred, err, sizeof err, &wrong) == 0;
                    if (!ok) d_set_error(d, err);
                    if (wrong) {
                        pthread_mutex_lock(&d->mu);
                        KnownNet *k = kn_find(&d->known, ssid, sl);
                        if (k) k->autojoin = 0;
                        pthread_mutex_unlock(&d->mu);
                        d_save_known(d);
                    }
                }
                secure_zero(pmk, sizeof pmk);
                secure_zero(kpass, sizeof kpass);
            }
            autojoin_backoff = ok ? 5 : (autojoin_backoff < 60 ? autojoin_backoff * 2 : 60);
            next_autojoin = now_s() + (ok ? 5 : autojoin_backoff);
        }
    }

    printf("[macwifid] stopping\n");
    d_release(d, a);
    pthread_join(ct, NULL);                 // notices gStop within 300 ms
    if (bt_on) pthread_join(btt, NULL);     // notices gStop within ~250 ms (a scan stops early)
    close(d->listen_fd);
    unlink(d->h.sock);
    pthread_mutex_lock(&d->mu);
    d->done_seq = d->cmd_seq;   // release any waiting client
    d->cmd = DC_NONE;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
    usleep(100000);                         // let client threads answer
    pthread_mutex_lock(&d->mu);
    kn_wipe(&d->known);
    pthread_mutex_unlock(&d->mu);
    return 0;
}

// ---------------------------------------------------------------- real hooks and commands

static int real_adapter_present(void) {
    const AicId *id = NULL;
    io_service_t s = find_adapter(AIC_MODE_NONE, &id);
    if (s) IOObjectRelease(s);
    return s != 0;
}

static int real_acquire(Link *l, uint8_t mac[6], uint8_t *vif) {
    char *argv[] = {"macwifid", "daemon", NULL};
    if (up_link(2, argv, l, mac) != 0) return -1;
    if (radio_start(l, mac, vif) != 0) { link_close(l); return -1; }
    return 0;
}

static void real_release(Link *l) { link_close(l); }

static int cmd_daemon(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "macwifid runs as root (it's started by launchd; see README).\n");
        return 2;
    }
    if (chdir(MW_DIR) != 0) fprintf(stderr, "note: %s not found; firmware is looked up relative to the current folder\n", MW_DIR);
    power_watch_start(d_will_sleep, d_did_wake);   // no power assertions: never keeps the Mac awake
    DaemonHooks h = {real_adapter_present, real_acquire, real_release, MW_DIR, MW_SOCK, 1};
    return daemon_main(&h);
}

// Client side: sends one request, prints the reply. Returns 0 if "ok" was true.
static int ctl_request(const char *sock, const char *json, char *reply, size_t cap) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { if (fd >= 0) close(fd); return -1; }
    size_t n = strlen(json);
    int ok = write(fd, json, n) == (ssize_t)n && write(fd, "\n", 1) == 1;
    size_t got = 0;
    while (ok && got < cap - 1) {
        ssize_t r = read(fd, reply + got, cap - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
        if (memchr(reply, '\n', got)) break;
    }
    reply[got] = 0;
    close(fd);
    return ok && got ? 0 : -1;
}

static int daemon_running(void) {
    char r[256];
    return ctl_request(MW_SOCK, "{\"cmd\":\"status\"}", r, sizeof r) == 0;
}

static int cmd_ctl(int argc, char **argv) {
    char req[1024];
    const char *c = argv[1];
    if (!strcmp(c, "status")) snprintf(req, sizeof req, "{\"cmd\":\"status\"}");
    else if (!strcmp(c, "networks")) snprintf(req, sizeof req, "{\"cmd\":\"scan\"}");
    else if (!strcmp(c, "disconnect")) snprintf(req, sizeof req, "{\"cmd\":\"disconnect\"}");
    else if (!strcmp(c, "ctl") && argc > 2) snprintf(req, sizeof req, "%s", argv[2]);
    else { fprintf(stderr, "usage: macwifictl status | networks | disconnect | ctl '<json request>'\n"); return 2; }
    static char reply[65536];
    if (ctl_request(MW_SOCK, req, reply, sizeof reply) != 0) {
        fprintf(stderr, "macwifid isn't running (no answer on %s).\n", MW_SOCK);
        return 3;
    }
    fputs(reply, stdout);
    return strstr(reply, "\"ok\":true") ? 0 : 1;
}
