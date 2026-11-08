// sim_connect.c — runs macwifictl's `connect` logic (connect_run in main.c) against a simulated AIC8800 firmware
// and WPA2 access point, behind stand-in IOKit pipes. Linux only (uses the header stand-ins in Tests/macstubs).
// Checks the wire format of every message and data frame the tool sends, the handshake, the key order (pairwise
// key only after message 4 has been sent), the control port, the DHCP probe and the failure paths.
// Original code (MIT).

#define main macwifictl_main
#include "../Sources/macwifictl/main.c"
#undef main

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------- stand-ins for the macOS functions used
static const char *gPass = "correct horse battery";
char *readpassphrase(const char *prompt, char *buf, unsigned long bufsiz, int flags) {
    (void)prompt; (void)flags;
    snprintf(buf, bufsiz, "%s", gPass);
    return buf;
}

// ---------------------------------------------------------------- the simulated network
static const uint8_t kMac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kBssid[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
static const uint8_t kApRsn[26] = {0x30, 24, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                   2, 0, 0x00, 0x0F, 0xAC, 2, 0x00, 0x0F, 0xAC, 8, 0x80, 0x00};   // PSK + SAE, MFPC
static const uint8_t kGtk[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
#define AP_IDX 2

typedef struct {
    const char *ssid;           // network the simulated AP serves
    const char *ap_pass;
    uint16_t assoc_status;      // SM_CONNECT_IND status
    int pmf_required;
    int open;                   // no RSN, no privacy: open network
    // what happened
    int scans, connects, msg2_ok, msg2_bad, msg4_ok, ptk_ok, gtk_ok, ptk_before_msg4, port_open, dhcp_seen;
    int discs, data_bad, eapol_sta_ok, eapol_cfm_req, dhcp_sta_ok, key_dumped, connect_req_ok;
    uint8_t anonce[32], ptk[48], pmk[32];
    unsigned replay;
} Sim;
static Sim S;

static uint8_t qbuf[64][4096];
static UInt32 qlen[64];
static int qh, qt;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;   // join writes from two threads, like the real device
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;   // the simulated device handles one transfer at a time

static void q_push(const uint8_t *p, size_t n) {
    pthread_mutex_lock(&qlock);
    if (qt - qh >= 64 || n > sizeof qbuf[0]) {
        fprintf(stderr, "sim queue overflow\n");
        failures++;
    } else {
        memcpy(qbuf[qt % 64], p, n);
        qlen[qt % 64] = (UInt32)n;
        qt++;
    }
    pthread_mutex_unlock(&qlock);
}

// Data packets queued back to back go into one transfer (up to 4 KB), as the chip aggregates RX on USB.
static int qlast_is_data;
static void q_push_data(const uint8_t *p, size_t n) {
    pthread_mutex_lock(&qlock);
    int last = (qt - 1) % 64;
    if (qt > qh && qlast_is_data && qlen[last] + n <= sizeof qbuf[0]) {
        memcpy(qbuf[last] + qlen[last], p, n);
        qlen[last] += (UInt32)n;
        pthread_mutex_unlock(&qlock);
        return;
    }
    pthread_mutex_unlock(&qlock);
    q_push(p, n);
    qlast_is_data = 1;
}

static void in_msg(uint16_t id, const uint8_t *params, uint16_t plen) {
    uint8_t b[2048] = {0};
    uint16_t len = (uint16_t)(12 + plen);
    b[0] = (uint8_t)len; b[1] = (uint8_t)(len >> 8); b[2] = 0x11;
    b[4] = (uint8_t)id; b[5] = (uint8_t)(id >> 8);
    b[10] = (uint8_t)plen; b[11] = (uint8_t)(plen >> 8);
    if (plen) memcpy(b + 16, params, plen);
    q_push(b, 4 + (((size_t)len + 3) & ~(size_t)3));
    qlast_is_data = 0;
}

static void in_tx_cfm(uint32_t status, uint32_t idx) {
    uint8_t b[12] = {8, 0, 0x12, 0};
    for (int i = 0; i < 4; i++) { b[4 + i] = (uint8_t)(status >> (8 * i)); b[8 + i] = (uint8_t)(idx >> (8 * i)); }
    q_push(b, sizeof b);
    qlast_is_data = 0;
}

// A From-DS (QoS) data frame carrying `eth` (DA, SA, type, payload), optionally "decrypted CCMP": protected bit,
// CCMP header and an 8-byte MIC left in place.
static void in_data(const uint8_t *eth, size_t len, int ccmp) {
    uint8_t b[4096] = {0};
    uint8_t *f = b + 60;
    size_t n = 0;
    f[0] = 0x88; f[1] = 0x02 | (ccmp ? 0x40 : 0);          // QoS data, From DS
    memcpy(f + 4, eth, 6);                                  // A1 = DA
    memcpy(f + 10, kBssid, 6);                              // A2 = BSSID
    memcpy(f + 16, eth + 6, 6);                             // A3 = SA
    n = 26;                                                 // + QoS control (TID 0)
    if (ccmp) { f[n] = 1; f[n + 3] = 0x20; n += 8; }
    const uint8_t llc[6] = {0xAA, 0xAA, 0x03, 0, 0, 0};
    memcpy(f + n, llc, 6); n += 6;
    f[n++] = eth[12]; f[n++] = eth[13];
    memcpy(f + n, eth + 14, len - 14); n += len - 14;
    if (ccmp) { memset(f + n, 0xEE, 8); n += 8; }
    b[0] = (uint8_t)n; b[1] = (uint8_t)(n >> 8);
    b[17] = (uint8_t)-42;
    b[36] = (uint8_t)((ccmp ? 3 : 0) << 2);
    b[42] = 0x6c; b[43] = 0x09;                             // 2412 MHz
    b[49] = 0; b[50] = AP_IDX; b[51] = 0xff;
    q_push_data(b, 60 + n);
}

static void put_be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static size_t ap_eapol(uint8_t *out, unsigned key_info, const uint8_t *kd, size_t kdl, int with_mic, int encrypt) {
    uint8_t data[512];
    size_t dl = kdl;
    if (kdl) memcpy(data, kd, kdl);
    if (encrypt) {
        if (dl < 16 || dl % 8) { data[dl++] = 0xDD; while (dl < 16 || dl % 8) data[dl++] = 0; }
        uint8_t w[520];
        wpa_aes_wrap(WPA_KEK(S.ptk), data, dl, w);
        memcpy(data, w, dl + 8);
        dl += 8;
    }
    size_t len = 99 + dl;
    memset(out, 0, len);
    out[0] = 2; out[1] = 3; put_be16(out + 2, (unsigned)(len - 4));
    out[4] = 2; put_be16(out + 5, key_info); put_be16(out + 7, 16);
    out[16] = (uint8_t)++S.replay;
    memcpy(out + 17, S.anonce, 32);
    put_be16(out + 97, (unsigned)dl);
    memcpy(out + 99, data, dl);
    if (with_mic) wpa_eapol_mic(WPA_KCK(S.ptk), 2, out, len, out + 81);
    return len;
}

static void ap_send_eapol(const uint8_t *eapol, size_t n, int ccmp) {
    uint8_t eth[600];
    memcpy(eth, kMac, 6);
    memcpy(eth + 6, kBssid, 6);
    eth[12] = 0x88; eth[13] = 0x8E;
    memcpy(eth + 14, eapol, n);
    in_data(eth, 14 + n, ccmp);
}

static int mic_ok(const uint8_t *f, size_t n) {
    uint8_t t[600], mic[16];
    if (n > sizeof t || n < 97) return 0;
    memcpy(t, f, n);
    memset(t + 81, 0, 16);
    wpa_eapol_mic(WPA_KCK(S.ptk), 2, t, n, mic);
    return memcmp(mic, f + 81, 16) == 0;
}

static void beacon_result(void) {
    uint8_t f[256] = {0};
    size_t n = 0;
    f[0] = 0x50;                                            // probe response
    memcpy(f + 4, kMac, 6); memcpy(f + 10, kBssid, 6); memcpy(f + 16, kBssid, 6);
    n = 24;
    f[n + 8] = 100; f[n + 10] = S.open ? 0x01 : 0x11; f[n + 11] = 0x04;   // interval, capability: ESS (+ privacy)
    n += 12;
    size_t sl = strlen(S.ssid);
    f[n++] = 0; f[n++] = (uint8_t)sl; memcpy(f + n, S.ssid, sl); n += sl;
    f[n++] = 3; f[n++] = 1; f[n++] = 1;                     // channel 1
    if (!S.open) {
        memcpy(f + n, kApRsn, sizeof kApRsn);
        if (S.pmf_required) f[n + sizeof kApRsn - 2] = 0xC0;
        n += sizeof kApRsn;
    }
    uint8_t p[300] = {0};
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = 0x50; p[4] = 0x6c; p[5] = 0x09;                  // 2412 MHz
    p[9] = (uint8_t)-40;
    memcpy(p + 12, f, n);
    in_msg(AIC_SCANU_RESULT_IND, p, (uint16_t)(12 + n));
}

static void on_msg(uint16_t id, const uint8_t *p, uint16_t plen) {
    uint8_t r[900] = {0};
    switch (id) {
        case AIC_SCANU_START_REQ:
            S.scans++;
            in_msg(AIC_SCANU_START_CFM_ADDTIONAL, r, 4);
            beacon_result();
            r[0] = 0; r[1] = 0; r[2] = 1;
            in_msg(AIC_SCANU_START_CFM, r, 4);
            break;
        case AIC_SM_CONNECT_REQ: {
            S.connects++;
            uint8_t rsn[22];
            wpa_build_rsn_ie(rsn, sizeof rsn);
            S.connect_req_ok = plen == 320 && p[0] == strlen(S.ssid) && !memcmp(p + 1, S.ssid, p[0]) &&
                               !memcmp(p + 34, kBssid, 6) && p[40] == 0x6c && p[41] == 0x09 && p[42] == 0 &&
                               p[52] == 0x88 && p[53] == 0x8E && p[60] == 1 && p[61] == 0 &&
                               (S.open ? p[48] == 0 && p[54] == 0 && p[64] == 0
                                       : p[48] == 9 && p[54] == 22 && !memcmp(p + 64, rsn, 22));
            in_msg(AIC_SM_CONNECT_CFM, r, 4);
            r[0] = (uint8_t)S.assoc_status; r[1] = (uint8_t)(S.assoc_status >> 8);
            memcpy(r + 2, kBssid, 6);
            r[10] = AP_IDX; r[12] = 1; r[820] = 1; r[824] = 0x6c; r[825] = 0x09;
            in_msg(AIC_SM_CONNECT_IND, r, 852);
            if (S.assoc_status) break;
            if (S.open) { S.port_open = 1; break; }        // no handshake, no control port
            for (int i = 0; i < 32; i++) S.anonce[i] = (uint8_t)(0x80 + i);
            uint8_t e[200];
            size_t n = ap_eapol(e, 0x008A, NULL, 0, 0, 0);   // message 1
            ap_send_eapol(e, n, 0);
            break;
        }
        case AIC_MM_KEY_ADD_REQ: {
            S.key_dumped = 0;
            if (plen == 44 && p[43] == 1) {
                S.ptk_ok = p[1] == AP_IDX && p[4] == 16 && !memcmp(p + 8, WPA_TK(S.ptk), 16) && p[40] == 2;
                if (!S.msg4_ok) S.ptk_before_msg4 = 1;
            } else if (plen == 44) {
                S.gtk_ok = p[0] == 1 && p[1] == 0xFF && p[4] == 16 && !memcmp(p + 8, kGtk, 16) && p[40] == 2;
            }
            r[0] = 0; r[1] = p[43] ? 8 : 1;
            in_msg(AIC_MM_KEY_ADD_CFM, r, 4);
            break;
        }
        case AIC_ME_SET_CONTROL_PORT_REQ: {
            S.port_open = plen == 2 && p[0] == AP_IDX && p[1] == 1;
            in_msg(AIC_ME_SET_CONTROL_PORT_CFM, r, 4);
            // some network chatter: an ARP request and an IPv6 multicast, both CCMP
            uint8_t eth[80] = {0};
            memset(eth, 0xff, 6); memcpy(eth + 6, "\x02\x00\x00\x00\x00\x09", 6);
            eth[12] = 0x08; eth[13] = 0x06;
            in_data(eth, 14 + 28, 1);
            memcpy(eth, "\x33\x33\x00\x00\x00\x01", 6);
            eth[12] = 0x86; eth[13] = 0xDD;
            in_data(eth, 14 + 40, 1);
            // our own broadcast reflected by the AP (must not reach macOS), and a plaintext frame after the
            // keys are in place (must not reach macOS either)
            memset(eth, 0xff, 6); memcpy(eth + 6, kMac, 6);
            eth[12] = 0x08; eth[13] = 0x06;
            in_data(eth, 14 + 28, 1);
            memcpy(eth, kMac, 6); memcpy(eth + 6, "\x02\x00\x00\x00\x00\x09", 6);
            eth[12] = 0x08; eth[13] = 0x00; eth[14] = 0x45; eth[15] = 0xAB;
            in_data(eth, 14 + 40, 0);
            break;
        }
        case AIC_SM_DISCONNECT_REQ:
            S.discs++;
            in_msg(AIC_SM_DISCONNECT_CFM, r, 4);
            break;
        default:
            break;
    }
}

static void on_data(const uint8_t *b, size_t n) {
    if (n < 32 || b[2] != 0x01) { S.data_bad++; return; }
    size_t total = (size_t)(b[0] | ((b[1] & 0x0f) << 8));
    uint16_t plen = (uint16_t)(b[4] | (b[5] << 8));
    uint32_t sda = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
    uint16_t et = (uint16_t)((b[24] << 8) | b[25]);
    const uint8_t *payload = b + 32;
    size_t pl = n - 32;
    if (total > n || plen > pl || memcmp(b + 18, kMac, 6) != 0) { S.data_bad++; return; }
    if (et == 0x888E) {
        if (memcmp(b + 12, kBssid, 6) != 0 || b[29] != AP_IDX || b[28] != 0) { S.data_bad++; return; }
        S.eapol_sta_ok = 1;
        if (!(sda & 0x80000000u)) return;
        S.eapol_cfm_req = 1;
        in_tx_cfm(0x9, sda & 0xffff);
        EapolKey k;
        if (eapol_parse_key(payload, plen, &k) != 0) { S.data_bad++; return; }
        if (k.key_info == 0x010A) {                        // message 2
            uint8_t spa_nonce[32];
            memcpy(spa_nonce, k.nonce, 32);
            wpa_pmk_from_passphrase(S.ap_pass, (const uint8_t *)S.ssid, strlen(S.ssid), S.pmk);
            wpa_derive_ptk(S.pmk, kBssid, kMac, S.anonce, spa_nonce, S.ptk);
            if (!mic_ok(payload, k.frame_len)) {
                S.msg2_bad++;
                uint8_t r[6] = {15, 0, 0, 0, 0, 0};          // the AP gives up: 4-way handshake timeout
                in_msg(AIC_SM_DISCONNECT_IND, r, 6);
                return;
            }
            S.msg2_ok++;
            uint8_t kd[100], e[300];
            size_t kdl = 0;
            memcpy(kd, kApRsn, sizeof kApRsn); kdl = sizeof kApRsn;
            kd[kdl++] = 0xDD; kd[kdl++] = 22; kd[kdl++] = 0x00; kd[kdl++] = 0x0F; kd[kdl++] = 0xAC; kd[kdl++] = 1;
            kd[kdl++] = 1; kd[kdl++] = 0;
            memcpy(kd + kdl, kGtk, 16); kdl += 16;
            size_t en = ap_eapol(e, 0x13CA, kd, kdl, 1, 1);  // message 3
            ap_send_eapol(e, en, 0);
        } else if (k.key_info == 0x030A) {                 // message 4
            if (mic_ok(payload, k.frame_len)) S.msg4_ok++;
        }
        return;
    }
    if (et == 0x0800 && pl >= 28 && payload[9] == 17 && payload[22] == 0 && payload[23] == 67) {
        S.dhcp_seen++;
        S.dhcp_sta_ok = b[29] == AP_IDX && !memcmp(b + 12, "\xff\xff\xff\xff\xff\xff", 6) && sda == 0;
        const uint8_t *bootp = payload + 28;
        uint32_t xid = ((uint32_t)bootp[4] << 24) | ((uint32_t)bootp[5] << 16) | ((uint32_t)bootp[6] << 8) | bootp[7];
        // the offer: reuse the discover's layout, flip it into a reply
        uint8_t eth[600];
        size_t dl = dhcp_build_discover(eth, sizeof eth, kMac, xid);
        memcpy(eth + 6, "\x02\x00\x00\x00\x00\x01", 6);
        uint8_t *ip = eth + 14, *udp = ip + 20, *bo = udp + 8;
        ip[12] = 192; ip[13] = 0; ip[14] = 2; ip[15] = 1;
        udp[1] = 67; udp[3] = 68;
        bo[0] = 2;
        bo[16] = 192; bo[17] = 0; bo[18] = 2; bo[19] = 77;
        uint8_t *o = bo + 236 + 4;
        o[2] = 2;                                          // DHCPOFFER
        o[3] = 54; o[4] = 4; o[5] = 192; o[6] = 0; o[7] = 2; o[8] = 1; o[9] = 255;
        in_data(eth, dl, !S.open);
        return;
    }
}

static IOReturn f_write_locked(void *self, UInt8 pipe, void *buf, UInt32 n, UInt32 a, UInt32 b);
static IOReturn f_write(void *self, UInt8 pipe, void *buf, UInt32 n, UInt32 a, UInt32 b) {
    pthread_mutex_lock(&wlock);
    IOReturn r = f_write_locked(self, pipe, buf, n, a, b);
    pthread_mutex_unlock(&wlock);
    return r;
}
static IOReturn f_write_locked(void *self, UInt8 pipe, void *buf, UInt32 n, UInt32 a, UInt32 b) {
    (void)self; (void)a; (void)b;
    const uint8_t *p = (const uint8_t *)buf;
    if (pipe == 3) {
        if (n < 16 || p[2] != 0x11) { failures++; return kIOReturnError; }
        uint16_t id = (uint16_t)(p[8] | (p[9] << 8)), plen = (uint16_t)(p[14] | (p[15] << 8));
        if (16u + plen > n) { failures++; return kIOReturnError; }
        on_msg(id, p + 16, plen);
    } else if (pipe == 1) {
        on_data(p, n);
    } else {
        failures++;
    }
    return kIOReturnSuccess;
}

static atomic_int gUnplugged;
static IOReturn f_read(void *self, UInt8 pipe, void *buf, UInt32 *n, UInt32 a, UInt32 b) {
    (void)self; (void)pipe; (void)a; (void)b;
    if (gUnplugged) { usleep(2000); return kIOReturnNoDevice; }
    pthread_mutex_lock(&qlock);
    if (qh == qt) {
        pthread_mutex_unlock(&qlock);
        usleep(2000);
        return kIOUSBTransactionTimeout;
    }
    UInt32 len = qlen[qh % 64];
    if (len > *n) len = *n;
    memcpy(buf, qbuf[qh % 64], len);
    *n = len;
    qh++;
    pthread_mutex_unlock(&qlock);
    return kIOReturnSuccess;
}
static IOReturn f_clear(void *self, UInt8 pipe) { (void)self; (void)pipe; return kIOReturnSuccess; }

// ---------------------------------------------------------------- a fake feth pair ("macOS" side of join)
static struct {
    atomic_int dhcp_set, none_set, offer_to_host, arp_to_host, v6_to_host, echo_to_host, plain_to_host, eapol_to_host, reads;
} H;
static uint8_t hbuf[8192];

static size_t bpf_rec(uint8_t *b, size_t off, const uint8_t *pkt, size_t n) {
    memset(b + off, 0, 18);
    b[off + 8] = (uint8_t)n; b[off + 9] = (uint8_t)(n >> 8);
    b[off + 12] = b[off + 8]; b[off + 13] = b[off + 9];
    b[off + 16] = 18;
    memcpy(b + off + 18, pkt, n);
    return off + ((18 + n + 3) & ~(size_t)3);
}

static atomic_int gFethOpen;
int feth_open(Feth *f, const uint8_t mac[6]) {
    (void)mac;
    memset(f, 0, sizeof *f);
    f->rbuf = hbuf;
    f->blen = sizeof hbuf;
    H.reads = 0;              // each new interface: macOS asks for DHCP again
    H.offer_to_host = 0;
    gFethOpen++;
    return 0;
}
long feth_read(Feth *f) {
    f->rbuf = hbuf;
    if (H.reads++ == 0) {   // what macOS sends right after the interface comes up
        uint8_t fr[600];
        size_t len = 0, n = dhcp_build_discover(fr, sizeof fr, kMac, 0x51515151);
        len = bpf_rec(hbuf, len, fr, n);
        memcpy(fr + 6, "\x02\x66\x66\x66\x66\x66", 6);          // foreign source: must be dropped
        len = bpf_rec(hbuf, len, fr, n);
        uint8_t ea[60] = {0};
        memcpy(ea, kBssid, 6); memcpy(ea + 6, kMac, 6); ea[12] = 0x88; ea[13] = 0x8E;   // EAPOL from macOS: dropped
        len = bpf_rec(hbuf, len, ea, sizeof ea);
        return (long)len;
    }
    usleep(20000);
    return 0;
}
int feth_write(Feth *f, const uint8_t *eth, size_t len) {
    (void)f;
    if (len < 14) return -1;
    if (!memcmp(eth + 6, kMac, 6)) H.echo_to_host++;
    if (eth[12] == 0x88 && eth[13] == 0x8E) H.eapol_to_host++;
    if (eth[12] == 0x08 && eth[13] == 0x06) H.arp_to_host++;
    if (eth[12] == 0x86 && eth[13] == 0xDD) H.v6_to_host++;
    if (eth[12] == 0x08 && eth[13] == 0x00 && len > 15 && eth[15] == 0xAB) H.plain_to_host++;
    DhcpReply r;
    if (dhcp_parse_reply(eth, len, kMac, 0x51515151, &r) == 0 && r.type == 2) H.offer_to_host++;
    return 0;
}
void feth_close(Feth *f) { (void)f; gFethOpen--; }
void feth_ipv4(const Feth *f, char *out, size_t cap) { (void)f; snprintf(out, cap, "%s", H.offer_to_host ? "192.0.2.77" : ""); }
int feth_run(const char *const argv[]) {
    if (argv[3] && !strcmp(argv[3], "DHCP")) H.dhcp_set++;
    if (argv[3] && !strcmp(argv[3], "NONE")) H.none_set++;
    return 0;
}
int feth_spawn_sh(const char *script) { (void)script; return 0; }
static atomic_int gSvcAdded, gSvcRemoved;
int feth_service_add(const char *ifname) { (void)ifname; gSvcAdded++; return 0; }
int feth_service_remove(void) { gSvcRemoved++; return 0; }
int feth_cleanup(void) { return 0; }

static int S_get(const int *field) {
    pthread_mutex_lock(&wlock);
    int v = *field;
    pthread_mutex_unlock(&wlock);
    return v;
}

static IOUSBInterfaceInterface300 gVt;
static IOUSBInterfaceInterface300 *gVtp = &gVt;
static const AicId gId = {0xa69c, 0x8d81, AIC_MODE_WIFI, "sim", 1};

static int sim_run_feth(const char *ssid_wanted, int listen_s, Feth *feth) {
    qh = qt = 0;
    qlast_is_data = 0;
    gStop = 0;
    gVt.WritePipeTO = f_write;
    gVt.ReadPipeTO = f_read;
    gVt.ClearPipeStallBothEnds = f_clear;
    Link l;
    memset(&l, 0, sizeof l);
    l.intf = &gVtp;
    l.in_pipe = 2; l.out_pipe = 1; l.msg_pipe = 3; l.out_ep = 1;
    l.id = &gId;
    return connect_run(&l, kMac, 0, ssid_wanted, listen_s, feth);
}
static int sim_run(const char *ssid_wanted, int listen_s) { return sim_run_feth(ssid_wanted, listen_s, NULL); }

static void reset(const char *ssid, const char *ap_pass) {
    memset(&S, 0, sizeof S);
    S.ssid = ssid;
    S.ap_pass = ap_pass;
}

// ---------------------------------------------------------------- macwifid against the simulated adapter

static atomic_int gAdapterPresent, gAcquired, gReleased;
static int sim_present(void) { return gAdapterPresent; }
static int sim_acquire(Link *l, uint8_t mac[6], uint8_t *vif) {
    qh = qt = 0;
    qlast_is_data = 0;
    gVt.WritePipeTO = f_write;
    gVt.ReadPipeTO = f_read;
    gVt.ClearPipeStallBothEnds = f_clear;
    memset(l, 0, sizeof *l);
    l->intf = &gVtp;
    l->in_pipe = 2; l->out_pipe = 1; l->msg_pipe = 3; l->out_ep = 1;
    l->id = &gId;
    memcpy(mac, kMac, 6);
    *vif = 0;
    gAcquired++;
    return 0;
}
static void sim_release(Link *l) { (void)l; gReleased++; }

static char gSock[64], gDir[64];
static void *daemon_thread(void *arg) {
    (void)arg;
    DaemonHooks h = {sim_present, sim_acquire, sim_release, gDir, gSock};
    daemon_main(&h);
    return NULL;
}

static int ask(const char *req, char *reply, size_t cap) { return ctl_request(gSock, req, reply, cap); }

// Polls status until `want` appears in the reply (or `secs` pass).
static int wait_for(const char *want, double secs) {
    char r[4096];
    double t0 = now_s();
    while (now_s() - t0 < secs) {
        if (ask("{\"cmd\":\"status\"}", r, sizeof r) == 0 && strstr(r, want)) return 1;
        usleep(50000);
    }
    fprintf(stderr, "waited for %s; last status: %s", want, r);
    return 0;
}

static void test_daemon(void) {
    printf("---- macwifid\n");
    reset("ExampleNet", "correct horse battery");
    snprintf(gDir, sizeof gDir, "/tmp/mw-sim-%d", (int)getpid());
    snprintf(gSock, sizeof gSock, "/tmp/mw-sim-%d.sock", (int)getpid());
    mkdir(gDir, 0700);
    gStop = 0;
    gAdapterPresent = 1;
    pthread_t th;
    pthread_create(&th, NULL, daemon_thread, NULL);
    char r[8192];
    CHECK(wait_for("\"state\":\"idle\"", 5));
    CHECK(gAcquired == 1);

    // the menu's scan
    CHECK(ask("{\"cmd\":\"scan\"}", r, sizeof r) == 0 && strstr(r, "\"ok\":true") && strstr(r, "\"ssid\":\"ExampleNet\"") &&
          strstr(r, "\"usable\":true") && strstr(r, "\"known\":false"));
    // bad requests
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\",\"password\":\"short\"}", r, sizeof r) == 0 && strstr(r, "\"ok\":false"));
    CHECK(ask("not json", r, sizeof r) == 0 && strstr(r, "bad request"));
    CHECK(ask("{\"cmd\":\"teleport\"}", r, sizeof r) == 0 && strstr(r, "unknown cmd"));
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\"}", r, sizeof r) == 0 && strstr(r, "password is needed"));

    // wrong password: refused, nothing remembered
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\",\"password\":\"incorrect horse battery\"}", r, sizeof r) == 0 &&
          strstr(r, "\"ok\":false") && strstr(r, "password seems wrong"));
    CHECK(ask("{\"cmd\":\"known\"}", r, sizeof r) == 0 && strstr(r, "\"known\":[]"));

    // right password: connected, macOS gets an address, the network is remembered (as a PMK, root-only file)
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\",\"password\":\"correct horse battery\"}", r, sizeof r) == 0 &&
          strstr(r, "\"ok\":true"));
    CHECK(wait_for("\"ip\":\"192.0.2.77\"", 5));
    CHECK(wait_for("\"state\":\"connected\",\"adapter\":true,\"power\":true,\"ssid\":\"ExampleNet\"", 1));
    CHECK(gSvcAdded >= 1 && gFethOpen == 1);
    CHECK(ask("{\"cmd\":\"known\"}", r, sizeof r) == 0 && strstr(r, "{\"ssid\":\"ExampleNet\",\"secured\":true}"));
    char path[128];
    snprintf(path, sizeof path, "%s/networks", gDir);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    size_t fl = 0;
    char *ft = read_small(path, &fl);
    CHECK(ft && !strstr(ft, "horse") && strstr(ft, "4578616d706c654e6574 ") && fl == 20 + 1 + 64 + 3);
    free(ft);
    CHECK(ask("{\"cmd\":\"networks\"}", r, sizeof r) == 0 && strstr(r, "\"known\":true"));

    // the AP drops us: macwifid rejoins the remembered network by itself
    int before = S_get(&S.connects);
    uint8_t dr[6] = {4, 0, 0, 0, 0, 0};
    in_msg(AIC_SM_DISCONNECT_IND, dr, 6);
    double t0 = now_s();
    while (S_get(&S.connects) == before && now_s() - t0 < 10) usleep(50000);
    CHECK(S_get(&S.connects) == before + 1);
    CHECK(wait_for("\"state\":\"connected\"", 10));
    CHECK(gFethOpen == 1);                                     // old interface closed, new one open

    // user disconnect: stays off (no auto-join)
    CHECK(ask("{\"cmd\":\"disconnect\"}", r, sizeof r) == 0 && strstr(r, "\"ok\":true"));
    CHECK(wait_for("\"state\":\"idle\"", 3));
    int after = S_get(&S.connects);
    usleep(1500000);
    CHECK(S_get(&S.connects) == after && gFethOpen == 0);
    // join a remembered network without a password
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\"}", r, sizeof r) == 0 && strstr(r, "\"ok\":true"));
    CHECK(wait_for("\"state\":\"connected\"", 3));
    // forget: disconnects and drops the key
    CHECK(ask("{\"cmd\":\"forget\",\"ssid\":\"ExampleNet\"}", r, sizeof r) == 0 && strstr(r, "\"ok\":true"));
    CHECK(wait_for("\"state\":\"idle\"", 3));
    CHECK(ask("{\"cmd\":\"known\"}", r, sizeof r) == 0 && strstr(r, "\"known\":[]"));
    ft = read_small(path, &fl);
    CHECK(ft && fl == 0);
    free(ft);

    // power off / on
    CHECK(ask("{\"cmd\":\"power\",\"on\":false}", r, sizeof r) == 0 && strstr(r, "\"ok\":true"));
    CHECK(wait_for("\"state\":\"off\"", 3) && gReleased >= 1);
    CHECK(ask("{\"cmd\":\"scan\"}", r, sizeof r) == 0 && strstr(r, "Wi-Fi is off"));
    CHECK(ask("{\"cmd\":\"power\",\"on\":true}", r, sizeof r) == 0);
    CHECK(wait_for("\"state\":\"idle\"", 5) && gAcquired == 2);

    // unplugged: back to waiting for the adapter
    gAdapterPresent = 0;
    gUnplugged = 1;
    CHECK(wait_for("\"state\":\"no_adapter\",\"adapter\":false", 5));
    gUnplugged = 0;

    gStop = 1;
    pthread_join(th, NULL);
    CHECK(access(gSock, F_OK) != 0);                          // socket removed on exit
    unlink(path);
    snprintf(path, sizeof path, "%s/power", gDir);
    unlink(path);
    rmdir(gDir);
    gStop = 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("---- happy path\n");
    reset("ExampleNet", "correct horse battery");
    gPass = "correct horse battery";
    int rc = sim_run("ExampleNet", 1);
    CHECK(rc == 0);
    CHECK(S.scans == 1 && S.connects == 1 && S.connect_req_ok);
    CHECK(S.eapol_sta_ok && S.eapol_cfm_req && S.data_bad == 0);
    CHECK(S.msg2_ok == 1 && S.msg4_ok == 1);
    CHECK(S.ptk_ok && S.gtk_ok && !S.ptk_before_msg4);
    CHECK(S.port_open);
    CHECK(S.dhcp_seen >= 1 && S.dhcp_sta_ok);
    CHECK(S.discs == 1);

    printf("---- wrong password\n");
    reset("ExampleNet", "correct horse battery");
    gPass = "incorrect horse battery";
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 1 && S.msg2_bad == 1 && S.msg2_ok == 0 && !S.ptk_ok && !S.port_open && S.dhcp_seen == 0);
    CHECK(S.discs == 0);                                    // the AP already disconnected us

    printf("---- network not found\n");
    reset("OtherNet", "x");
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 1 && S.connects == 0);

    printf("---- PMF required\n");
    reset("ExampleNet", "correct horse battery");
    S.pmf_required = 1;
    gPass = "correct horse battery";
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 1 && S.connects == 0);

    printf("---- association refused (AP full)\n");
    reset("ExampleNet", "correct horse battery");
    S.assoc_status = 17;
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 1 && S.connects == 1 && S.msg2_ok == 0 && S.discs == 0);

    printf("---- password too short\n");
    reset("ExampleNet", "correct horse battery");
    gPass = "short";
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 2 && S.connects == 0);

    printf("---- open network\n");
    reset("GuestNet", "");
    S.open = 1;
    rc = sim_run("GuestNet", 1);
    CHECK(rc == 0 && S.connect_req_ok && S.msg2_ok == 0 && !S.ptk_ok && S.dhcp_seen >= 1 && S.discs == 1);

    printf("---- join: bridge to a (fake) macOS interface\n");
    reset("ExampleNet", "correct horse battery");
    gPass = "correct horse battery";
    memset(&H, 0, sizeof H);
    Feth fe;
    feth_open(&fe, kMac);
    rc = sim_run_feth("ExampleNet", 2, &fe);
    feth_close(&fe);
    CHECK(rc == 0);
    CHECK(S.ptk_ok && S.gtk_ok && S.port_open && !S.ptk_before_msg4);
    CHECK(gSvcAdded == 1 && gSvcRemoved == 1);              // the network service, not `ipconfig set`
    CHECK(H.dhcp_set == 0 && H.none_set == 0);
    CHECK(S.dhcp_seen == 1 && S.dhcp_sta_ok && S.data_bad == 0);   // only macOS's DISCOVER went out
    CHECK(H.offer_to_host == 1 && H.arp_to_host == 1);
    CHECK(H.v6_to_host == 1);   // 2nd packet of an aggregated transfer, starting at an unaligned offset
    CHECK(H.echo_to_host == 0 && H.plain_to_host == 0 && H.eapol_to_host == 0);
    CHECK(S.discs == 1);

    test_daemon();

    printf("sim_connect: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
