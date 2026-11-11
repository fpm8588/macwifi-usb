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
// the macOS interface and service, release USB) and waits for it, at most 20 s; macOS sleeps right after. After
// wake the main thread finds the adapter again (whatever state it woke up in) and auto-joins as usual.
static atomic_int gSleepReq, gWake;
static pthread_mutex_t gPmMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gPmCv = PTHREAD_COND_INITIALIZER;
static int gSleepReady;

static void d_will_sleep(void) {
    pthread_mutex_lock(&gPmMu);
    gSleepReady = 0;
    gSleepReq = 1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 20;
    while (!gSleepReady)
        if (pthread_cond_timedwait(&gPmCv, &gPmMu, &ts) != 0) break;
    pthread_mutex_unlock(&gPmMu);
}

static void d_did_wake(void) { gWake = 1; }

typedef enum { DC_NONE, DC_SCAN, DC_JOIN, DC_DISCONNECT, DC_FORGET, DC_POWER } DCmd;

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    int rssi, freq, secured, usable, known, sae, sae_only;
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
        jo_str(j, "security", !v->secured ? "open" : v->sae ? (v->sae_only ? "wpa3" : "wpa2/wpa3") : "wpa2");
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
    DaemonHooks h = {real_adapter_present, real_acquire, real_release, MW_DIR, MW_SOCK};
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
