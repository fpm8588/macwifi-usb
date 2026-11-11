// sim_connect.c — runs macwifictl's `connect` logic (connect_run in main.c) against a simulated AIC8800 firmware
// and WPA2 access point, behind stand-in IOKit pipes. Linux only (uses the header stand-ins in Tests/macstubs).
// Checks the wire format of every message and data frame the tool sends, the handshake, the key order (pairwise
// key only after message 4 has been sent), the control port, the DHCP probe and the failure paths.
// Original code (MIT).

#include "sae.h"
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
static const uint8_t kIgtk[16] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
enum { AP_WPA2 = 0, AP_TRANSITION, AP_WPA3 };   // what the simulated access point offers
static size_t ap_rsn(uint8_t *out);
static uint8_t gApRsn[40];
static size_t gApRsnLen;
static const uint8_t kGtk[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
#define AP_IDX 2

typedef struct {
    const char *ssid;           // network the simulated AP serves
    const char *ap_pass;
    uint16_t assoc_status;      // SM_CONNECT_IND status
    int pmf_required;
    int open;                   // no RSN, no privacy: open network
    int own_only_when_assoc, associated;
    int mode;                   // AP_WPA2 (default), AP_TRANSITION, AP_WPA3
    int h2e;                    // 1: advertises H2E (RSNXE); 2: H2E only (also membership selector 123)
    int saw_rsnxe_assoc, saw_rsnxe_msg2;
    int sae_reject, sae_token, fw_no_ext_auth;   // AP refuses SAE (status 77) / asks for a token / firmware never asks
    int sae_session;            // this association's PMK came from SAE
    Sae ap_sae;
    uint8_t ap_confirm[SAE_MAX_FRAME];
    size_t ap_confirm_len;
    int sae_commits, sae_ok, sae_bad, sae_token_ok, igtk_ok, ext_auth_rsp, sae_frames_bad;
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
// CCMP header and an 8-byte MIC left in place. `sn` >= 0 also flags it "need reorder" (block-ack session).
static void in_data_sn(const uint8_t *eth, size_t len, int ccmp, int sn);
static void in_data(const uint8_t *eth, size_t len, int ccmp) { in_data_sn(eth, len, ccmp, -1); }
static void in_data_sn(const uint8_t *eth, size_t len, int ccmp, int sn) {
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
    if (sn >= 0) {
        f[22] = (uint8_t)(sn << 4); f[23] = (uint8_t)(sn >> 4);   // sequence control
        b[48] |= 0x20;                                             // need reorder
    }
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
    if (S.sae_session) key_info &= ~7u;                    // key descriptor version 0 with AKM SAE
    out[4] = 2; put_be16(out + 5, key_info); put_be16(out + 7, 16);
    out[16] = (uint8_t)++S.replay;
    memcpy(out + 17, S.anonce, 32);
    put_be16(out + 97, (unsigned)dl);
    memcpy(out + 99, data, dl);
    if (with_mic) wpa_eapol_mic(WPA_KCK(S.ptk), S.sae_session ? 0 : 2, out, len, out + 81);
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
    wpa_eapol_mic(WPA_KCK(S.ptk), S.sae_session ? 0 : 2, t, n, mic);
    return memcmp(mic, f + 81, 16) == 0;
}

static void beacon_result_for(const char *ssid, const uint8_t *bssid);
static void beacon_result(void) {
    beacon_result_for(S.ssid, kBssid);
    // A neighbour's network; with own_only_when_assoc set, the "firmware" reports only our AP while associated
    // (the behaviour suspected on the adapter, which the menu's merged list must survive).
    static const uint8_t nb[6] = {0x02, 0x99, 0x88, 0x77, 0x66, 0x55};
    if (!(S.own_only_when_assoc && S.associated)) beacon_result_for("Neighbor", nb);
}
static void beacon_result_for(const char *ssid, const uint8_t *bssid) {
    uint8_t f[256] = {0};
    size_t n = 0;
    f[0] = 0x50;                                            // probe response
    memcpy(f + 4, kMac, 6); memcpy(f + 10, bssid, 6); memcpy(f + 16, bssid, 6);
    n = 24;
    f[n + 8] = 100; f[n + 10] = S.open ? 0x01 : 0x11; f[n + 11] = 0x04;   // interval, capability: ESS (+ privacy)
    n += 12;
    size_t sl = strlen(ssid);
    f[n++] = 0; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;
    if (S.h2e == 2) { f[n++] = 1; f[n++] = 2; f[n++] = 0x8c; f[n++] = 0x80 | 123; }   // rates: 6 Mb/s, H2E-only selector
    f[n++] = 3; f[n++] = 1; f[n++] = 1;                     // channel 1
    if (!S.open) {
        gApRsnLen = ap_rsn(gApRsn);
        memcpy(f + n, gApRsn, gApRsnLen);
        n += gApRsnLen;
        if (S.h2e) { f[n++] = 244; f[n++] = 1; f[n++] = 0x20; }   // RSNXE: SAE hash-to-element
    }
    uint8_t p[300] = {0};
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = 0x50; p[4] = 0x6c; p[5] = 0x09;                  // 2412 MHz
    p[9] = (uint8_t)-40;
    memcpy(p + 12, f, n);
    in_msg(AIC_SCANU_RESULT_IND, p, (uint16_t)(12 + n));
}

// The AP's RSN element for its mode: WPA2 (PSK, no PMF unless pmf_required), transition (PSK + SAE, PMF capable),
// WPA3 (SAE, PMF required, BIP-CMAC-128 stated).
static size_t ap_rsn(uint8_t *o) {
    size_t n = 0;
    static const uint8_t head[12] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4};
    o[n++] = 0x30; o[n++] = 0;
    memcpy(o + n, head, 12); n += 12;
    int psk = S.mode != AP_WPA3, sae = S.mode != AP_WPA2;
    o[n++] = (uint8_t)(psk + sae); o[n++] = 0;
    if (psk) { o[n++] = 0x00; o[n++] = 0x0F; o[n++] = 0xAC; o[n++] = 2; }
    if (sae) { o[n++] = 0x00; o[n++] = 0x0F; o[n++] = 0xAC; o[n++] = 8; }
    o[n++] = S.mode == AP_WPA3 || S.pmf_required ? 0xC0 : S.mode == AP_TRANSITION ? 0x80 : 0x00;
    o[n++] = 0;
    if (S.mode == AP_WPA3) { o[n++] = 0; o[n++] = 0; o[n++] = 0x00; o[n++] = 0x0F; o[n++] = 0xAC; o[n++] = 6; }
    o[1] = (uint8_t)(n - 2);
    return n;
}

// A management frame for the station, as the firmware forwards it: RX header flagged "802.11 MPDU", the frame as is.
static void in_auth(const uint8_t *body, size_t n) {
    uint8_t b[60 + 24 + SAE_MAX_FRAME] = {0};
    uint8_t *f = b + 60;
    f[0] = 0xB0;
    memcpy(f + 4, kMac, 6); memcpy(f + 10, kBssid, 6); memcpy(f + 16, kBssid, 6);
    memcpy(f + 24, body, n);
    size_t fl = 24 + n;
    b[0] = (uint8_t)fl; b[1] = (uint8_t)(fl >> 8);
    b[17] = (uint8_t)-42;
    b[48] = 0x02;                                            // is_80211_mpdu
    b[49] = 0; b[50] = 0xff; b[51] = 0xff;
    q_push_data(b, 60 + fl);
}

static void sim_rng(uint8_t *o, size_t n) { for (size_t i = 0; i < n; i++) o[i] = (uint8_t)rand(); }

// The access point's side of SAE (the same sae.c, playing the other role).
static void ap_on_auth(const uint8_t *body, size_t n) {
    uint8_t reply[SAE_MAX_FRAME];
    size_t rl = 0;
    const char *err = NULL;
    int wp = 0;
    if (n < 6 || body[0] != 3) { S.sae_frames_bad++; return; }
    if (body[2] == 1) {                                       // commit
        if (S.ap_sae.state != SAE_IDLE) return;               // a retransmission
        S.sae_commits++;
        if (S.sae_reject) {
            uint8_t rej[8] = {3, 0, 1, 0, 77, 0, 19, 0};
            in_auth(rej, sizeof rej);
            return;
        }
        uint8_t commit[SAE_MAX_FRAME];
        memcpy(commit, body, n);
        int h2e_commit = body[4] == 126;
        if (S.h2e == 2 && !h2e_commit) {                     // an H2E-only AP refuses hunting-and-pecking
            uint8_t rej[6] = {3, 0, 1, 0, 1, 0};
            in_auth(rej, sizeof rej);
            return;
        }
        if (S.sae_token && h2e_commit) {                     // H2E: token in a container element after the element
            if (n == 6 + 2 + 96) {
                uint8_t req[8 + 3 + 16] = {3, 0, 1, 0, 76, 0, 19, 0, 255, 17, 93};
                memset(req + 11, 0x5A, 16);
                in_auth(req, sizeof req);
                S.sae_commits--;
                return;
            }
            int ok = n == 6 + 2 + 96 + 3 + 16 && body[104] == 255 && body[105] == 17 && body[106] == 93;
            for (int i = 0; ok && i < 16; i++) ok = body[107 + i] == 0x5A;
            if (!ok) { S.sae_frames_bad++; return; }
            S.sae_token_ok = 1;
            n = 6 + 2 + 96;
        } else if (S.sae_token) {
            if (n == 6 + 2 + 96) {                             // no token yet: ask for one
                uint8_t req[8 + 16] = {3, 0, 1, 0, 76, 0, 19, 0};
                memset(req + 8, 0x5A, 16);
                in_auth(req, sizeof req);
                S.sae_commits--;
                return;
            }
            int ok = n == 6 + 2 + 16 + 96;
            for (int i = 0; ok && i < 16; i++) ok = body[8 + i] == 0x5A;
            if (!ok) { S.sae_frames_bad++; return; }
            S.sae_token_ok = 1;
            memmove(commit + 8, commit + 24, 96);
            n -= 16;
        }
        if (sae_start(&S.ap_sae, kBssid, kMac, (const uint8_t *)S.ap_pass, strlen(S.ap_pass), (const uint8_t *)S.ssid,
                      strlen(S.ssid), h2e_commit, sim_rng)) return;
        size_t cl = sae_build_commit(&S.ap_sae, reply, sizeof reply);
        in_auth(reply, cl);
        if (sae_rx(&S.ap_sae, commit, n, S.ap_confirm, sizeof S.ap_confirm, &S.ap_confirm_len, &err, &wp) != SAE_EV_SEND)
            S.sae_frames_bad++;
    } else if (body[2] == 2) {                                // confirm
        if (S.ap_sae.state != SAE_CONFIRMED) return;
        if (sae_rx(&S.ap_sae, body, n, reply, sizeof reply, &rl, &err, &wp) == SAE_EV_DONE) {
            S.sae_ok++;
            in_auth(S.ap_confirm, S.ap_confirm_len);
        } else {
            S.sae_bad++;
            uint8_t rej[6] = {3, 0, 2, 0, 1, 0};              // rejected (a real AP may also just stay silent)
            in_auth(rej, sizeof rej);
        }
    }
}

// The firmware's part after authentication: association result, then the AP starts the 4-way handshake.
static void sim_associate(void) {
    uint8_t r[900] = {0};
    r[0] = (uint8_t)S.assoc_status; r[1] = (uint8_t)(S.assoc_status >> 8);
    memcpy(r + 2, kBssid, 6);
    r[10] = AP_IDX; r[12] = 1; r[820] = 1; r[824] = 0x6c; r[825] = 0x09;
    in_msg(AIC_SM_CONNECT_IND, r, 852);
    if (S.assoc_status) return;
    S.associated = 1;
    if (S.open) { S.port_open = 1; return; }               // no handshake, no control port
    for (int i = 0; i < 32; i++) S.anonce[i] = (uint8_t)(0x80 + i);
    uint8_t e[200];
    size_t n = ap_eapol(e, 0x008A, NULL, 0, 0, 0);          // message 1
    ap_send_eapol(e, n, 0);
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
            uint8_t rsn[22], rsn3[28];
            wpa_build_rsn_ie(rsn, sizeof rsn);
            wpa_build_rsn_ie_sae(rsn3, sizeof rsn3);
            int want_sae = plen == 320 && p[59] == 3;
            S.saw_rsnxe_assoc = want_sae && p[54] == 31 && p[64 + 28] == 244 && p[64 + 29] == 1 && p[64 + 30] == 0x20;
            S.connect_req_ok = plen == 320 && p[0] == strlen(S.ssid) && !memcmp(p + 1, S.ssid, p[0]) &&
                               !memcmp(p + 34, kBssid, 6) && p[40] == 0x6c && p[41] == 0x09 && p[42] == 0 &&
                               p[52] == 0x88 && p[53] == 0x8E && p[60] == 1 && p[61] == 0 &&
                               (S.open ? p[48] == 0 && p[54] == 0 && p[64] == 0
                                : want_sae ? p[48] == 0x19 && (p[54] == 28 || S.saw_rsnxe_assoc) && !memcmp(p + 64, rsn3, 28)
                                           : p[48] == 9 && p[54] == 22 && p[59] == 0 && !memcmp(p + 64, rsn, 22));
            in_msg(AIC_SM_CONNECT_CFM, r, 4);
            S.sae_session = 0;
            sae_wipe(&S.ap_sae);
            if (want_sae) {                                // hand authentication to the host
                if (S.fw_no_ext_auth) break;
                uint8_t ind[AIC_SZ_EXT_AUTH_IND] = {0};
                ind[1] = (uint8_t)strlen(S.ssid);
                memcpy(ind + 2, S.ssid, ind[1]);
                memcpy(ind + 34, kBssid, 6);
                ind[40] = 0x08; ind[41] = 0xAC; ind[42] = 0x0F; ind[43] = 0x00;
                in_msg(AIC_SM_EXTERNAL_AUTH_REQUIRED_IND, ind, sizeof ind);
                break;
            }
            sim_associate();
            break;
        }
        case AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP: {
            S.ext_auth_rsp++;
            in_msg(AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP_CFM, r, 4);
            uint16_t st = plen == 4 ? (uint16_t)(p[2] | (p[3] << 8)) : 1;
            if (st == 0 && S.ap_sae.state == SAE_ACCEPTED) {
                S.sae_session = 1;
                memcpy(S.pmk, S.ap_sae.pmk, 32);
                sim_associate();
            } else {                                       // authentication failed: the connect attempt ends
                uint8_t ci[852] = {1, 0};
                memcpy(ci + 2, kBssid, 6);
                in_msg(AIC_SM_CONNECT_IND, ci, 852);
            }
            break;
        }
        case AIC_MM_KEY_ADD_REQ: {
            S.key_dumped = 0;
            if (plen == 44 && p[40] == AIC_MAC_CIPHER_BIP_CMAC_128) {
                S.igtk_ok = p[0] == 4 && p[1] == 0xFF && p[4] == 16 && !memcmp(p + 8, kIgtk, 16) && p[43] == 0;
            } else if (plen == 44 && p[43] == 1) {
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
            // a block-ack burst arriving out of order (sequence 20, 22, 21): macOS must see 20, 21, 22
            for (int k = 0; k < 3; k++) {
                static const int order[3] = {20, 22, 21};
                eth[15] = (uint8_t)(0xC0 + order[k] - 20);
                in_data_sn(eth, 14 + 40, 1, order[k]);
            }
            break;
        }
        case AIC_SM_DISCONNECT_REQ:
            S.discs++;
            S.associated = 0;
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
    if (total > n || plen > pl) { S.data_bad++; return; }
    if (b[30] & 0x08) {                                     // TXU_CNTRL_MGMT: a raw 802.11 management frame
        if (b[29] != 0xFF || b[27] != 0xFF || b[26] != 3 || b[28] != 0 || sda != 0 || et != 0 || plen < 30 ||
            payload[0] != 0xB0 || memcmp(payload + 4, kBssid, 6) || memcmp(payload + 10, kMac, 6) ||
            memcmp(payload + 16, kBssid, 6)) {
            S.sae_frames_bad++;
            return;
        }
        ap_on_auth(payload + 24, (size_t)plen - 24);
        return;
    }
    if (memcmp(b + 18, kMac, 6) != 0) { S.data_bad++; return; }
    if (et == 0x888E) {
        if (memcmp(b + 12, kBssid, 6) != 0 || b[29] != AP_IDX || b[28] != 0) { S.data_bad++; return; }
        S.eapol_sta_ok = 1;
        if (!(sda & 0x80000000u)) return;
        S.eapol_cfm_req = 1;
        in_tx_cfm(0x9, sda & 0xffff);
        EapolKey k;
        if (eapol_parse_key(payload, plen, &k) != 0) { S.data_bad++; return; }
        const uint16_t v = S.sae_session ? 0 : 2;
        if (k.key_info == (0x0108 | v)) {                  // message 2
            uint8_t spa_nonce[32];
            memcpy(spa_nonce, k.nonce, 32);
            if (S.sae_session) {
                wpa_derive_ptk_sha256(S.pmk, kBssid, kMac, S.anonce, spa_nonce, S.ptk);
            } else {
                wpa_pmk_from_passphrase(S.ap_pass, (const uint8_t *)S.ssid, strlen(S.ssid), S.pmk);
                wpa_derive_ptk(S.pmk, kBssid, kMac, S.anonce, spa_nonce, S.ptk);
            }
            S.saw_rsnxe_msg2 = k.key_data_len == 31 && k.key_data[28] == 244 && k.key_data[30] == 0x20;
            if (!mic_ok(payload, k.frame_len)) {
                S.msg2_bad++;
                S.associated = 0;
                uint8_t r[6] = {15, 0, 0, 0, 0, 0};          // the AP gives up: 4-way handshake timeout
                in_msg(AIC_SM_DISCONNECT_IND, r, 6);
                return;
            }
            S.msg2_ok++;
            uint8_t kd[160], e[400];
            size_t kdl = 0;
            memcpy(kd, gApRsn, gApRsnLen); kdl = gApRsnLen;
            kd[kdl++] = 0xDD; kd[kdl++] = 22; kd[kdl++] = 0x00; kd[kdl++] = 0x0F; kd[kdl++] = 0xAC; kd[kdl++] = 1;
            kd[kdl++] = 1; kd[kdl++] = 0;
            memcpy(kd + kdl, kGtk, 16); kdl += 16;
            if (S.sae_session) {                           // IGTK KDE: key id 4, IPN, key
                kd[kdl++] = 0xDD; kd[kdl++] = 28; kd[kdl++] = 0x00; kd[kdl++] = 0x0F; kd[kdl++] = 0xAC; kd[kdl++] = 9;
                kd[kdl++] = 4; kd[kdl++] = 0;
                memset(kd + kdl, 0, 6); kdl += 6;
                memcpy(kd + kdl, kIgtk, 16); kdl += 16;
            }
            size_t en = ap_eapol(e, 0x13CA, kd, kdl, 1, 1);  // message 3
            ap_send_eapol(e, en, 0);
        } else if (k.key_info == (0x0308 | v)) {           // message 4
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

// ---------------------------------------------------------------- stand-in for the async USB reader
// Same interface as rx_async.c: a thread keeps reading the simulated device and fills the RxQ.
struct RxAsync {
    pthread_t th;
    atomic_int stop;
    RxQ *q;
};
static IOReturn f_read(void *self, UInt8 pipe, void *buf, UInt32 *n, UInt32 a, UInt32 b);
static atomic_int gRxStarted;
static void *feeder(void *arg) {
    RxAsync *r = (RxAsync *)arg;
    uint8_t *buf = malloc(32768);
    while (!r->stop) {
        UInt32 n = 32768;
        IOReturn kr = f_read(NULL, 2, buf, &n, 0, 0);
        if (kr == kIOUSBTransactionTimeout) continue;
        if (kr != kIOReturnSuccess) { rxq_push_error(r->q, kr); break; }
        rxq_push(r->q, buf, n);
    }
    free(buf);
    return NULL;
}
RxAsync *rx_async_start(void *intf, uint8_t pipe, RxQ *q) {
    (void)intf; (void)pipe;
    RxAsync *r = calloc(1, sizeof *r);
    r->q = q;
    pthread_create(&r->th, NULL, feeder, r);
    gRxStarted++;
    return r;
}
// TX: the simulated device takes writes synchronously; same contract (copy, in order).
static atomic_int gTxQueued;
int rx_async_tx(RxAsync *r, uint8_t pipe, const void *buf, size_t len) {
    (void)r;
    if (len > TX_ASYNC_BUFSIZE) return -1;
    gTxQueued++;
    return f_write(NULL, pipe, (void *)buf, (UInt32)len, 0, 0) == kIOReturnSuccess ? 0 : -1;
}
int rx_async_tx_drain(RxAsync *r, int timeout_ms) { (void)r; (void)timeout_ms; return 0; }
void rx_async_tx_stats(RxAsync *r, TxAsyncStats *s) { (void)r; memset(s, 0, sizeof *s); s->sent = (unsigned long)gTxQueued; }
void rx_async_stop(RxAsync *r) {
    r->stop = 1;
    pthread_join(r->th, NULL);
    free(r);
    gRxStarted--;
}

// ---------------------------------------------------------------- a fake feth pair ("macOS" side of join)
static struct {
    atomic_int dhcp_set, none_set, offer_to_host, arp_to_host, v6_to_host, echo_to_host, plain_to_host, eapol_to_host, reads;
    atomic_int ba_seen;   // order of the block-ack burst as macOS saw it: base-4 digits, first frame most significant
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
    H.ba_seen = 0;
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
    if (eth[12] == 0x08 && eth[13] == 0x00 && len > 15 && eth[15] >= 0xC0 && eth[15] <= 0xC2) H.ba_seen = H.ba_seen * 4 + (eth[15] - 0xC0 + 1);
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
static atomic_int gSimCaptive = CAPTIVE_NONE, gProbes;
int feth_captive_probe(const char *ifname, char *url, size_t cap) {
    (void)ifname;
    gProbes++;
    snprintf(url, cap, "%s", gSimCaptive == CAPTIVE_PORTAL ? "https://portal.example.net/login" : CAPTIVE_URL);
    return gSimCaptive;
}

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
    pthread_mutex_lock(&wlock);
    S.own_only_when_assoc = 1;
    pthread_mutex_unlock(&wlock);
    CHECK(ask("{\"cmd\":\"scan\"}", r, sizeof r) == 0 && strstr(r, "\"ok\":true") && strstr(r, "\"ssid\":\"ExampleNet\"") &&
          strstr(r, "\"usable\":true") && strstr(r, "\"known\":false") && strstr(r, "\"ssid\":\"Neighbor\""));
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
    CHECK(wait_for("\"captive\":\"no\"", 3));
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
    // a scan while connected that only reports our own AP keeps the neighbour listed (merged, aged list)
    CHECK(ask("{\"cmd\":\"scan\"}", r, sizeof r) == 0 && strstr(r, "\"ok\":true") && strstr(r, "\"ssid\":\"Neighbor\"") &&
          strstr(r, "\"ssid\":\"ExampleNet\""));

    // captive portal: after a (re)connection the check finds one and reports where to sign in; once signed in
    // the re-check (every gCaptiveRecheck s while a portal is in the way) clears it
    gCaptiveRecheck = 0.5;
    gSimCaptive = CAPTIVE_PORTAL;
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\"}", r, sizeof r) == 0 && strstr(r, "\"ok\":true"));
    CHECK(wait_for("\"captive\":\"yes\",\"portal\":\"https://portal.example.net/login\"", 5));
    gSimCaptive = CAPTIVE_NONE;
    CHECK(wait_for("\"captive\":\"no\"", 5));

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

    // sleep and wake: before sleep the adapter is shut down cleanly (leave, remove interface, release USB) and
    // the power callback returns only then; after wake it comes back and rejoins the remembered network.
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\",\"password\":\"correct horse battery\"}", r, sizeof r) == 0 &&
          strstr(r, "\"ok\":true"));
    CHECK(wait_for("\"state\":\"connected\"", 3));
    int discs = S_get(&S.discs), rel = gReleased, acq = gAcquired, conns = S_get(&S.connects);
    double ts = now_s();
    d_will_sleep();                                            // what power.c does on kIOMessageSystemWillSleep
    CHECK(now_s() - ts < 5);                                   // answered quickly, not by the 20 s timeout
    CHECK(S_get(&S.discs) == discs + 1 && gReleased == rel + 1 && gFethOpen == 0);
    CHECK(wait_for("\"state\":\"asleep\",\"adapter\":false", 1));
    usleep(1500000);
    CHECK(gAcquired == acq && S_get(&S.connects) == conns);   // nothing happens while asleep
    d_did_wake();                                              // kIOMessageSystemHasPoweredOn
    CHECK(wait_for("\"state\":\"connected\"", 10));
    CHECK(gAcquired == acq + 1 && S_get(&S.connects) == conns + 1 && gFethOpen == 1);
    CHECK(wait_for("\"ip\":\"192.0.2.77\"", 5));

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

static void test_daemon_wpa3(void) {
    printf("---- macwifid with WPA3\n");
    reset("ExampleNet", "correct horse battery");
    S.mode = AP_WPA3;
    snprintf(gDir, sizeof gDir, "/tmp/mw-sim3-%d", (int)getpid());
    snprintf(gSock, sizeof gSock, "/tmp/mw-sim3-%d.sock", (int)getpid());
    mkdir(gDir, 0700);
    // a remembered entry from before WPA3 support: the WPA2 key only. It can't join a WPA3-only network.
    char path[128], line[200];
    snprintf(path, sizeof path, "%s/networks", gDir);
    int n = snprintf(line, sizeof line, "4578616d706c654e6574 %064d 1\n", 0);
    CHECK(write_private(path, line, (size_t)n) == 0);
    gStop = 0;
    gAdapterPresent = 1;
    gUnplugged = 0;
    gSimCaptive = CAPTIVE_NONE;
    pthread_t th;
    pthread_create(&th, NULL, daemon_thread, NULL);
    char r[8192];
    CHECK(wait_for("\"state\":\"idle\"", 5));
    usleep(1500000);
    CHECK(S_get(&S.connects) == 0);                           // no auto-join attempt without the password
    CHECK(ask("{\"cmd\":\"scan\"}", r, sizeof r) == 0 && strstr(r, "\"security\":\"wpa3\"") && strstr(r, "\"usable\":true"));
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\"}", r, sizeof r) == 0 && strstr(r, "\"ok\":false") &&
          strstr(r, "enter its password again"));
    CHECK(ask("{\"cmd\":\"known\"}", r, sizeof r) == 0 && strstr(r, "ExampleNet"));   // still remembered, still auto-join
    // wrong, then right password
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\",\"password\":\"incorrect horse battery\"}", r, sizeof r) == 0 &&
          strstr(r, "\"ok\":false") && strstr(r, "wrong password"));
    CHECK(ask("{\"cmd\":\"join\",\"ssid\":\"ExampleNet\",\"password\":\"correct horse battery\"}", r, sizeof r) == 0 &&
          strstr(r, "\"ok\":true"));
    CHECK(wait_for("\"security\":\"wpa3\"", 3) && wait_for("\"ip\":\"192.0.2.77\"", 5));
    CHECK(S_get(&S.sae_ok) == 1 && S_get(&S.igtk_ok) && S_get(&S.sae_frames_bad) == 0);
    // the password is stored for WPA3 (hex, root-only file), never in the clear
    struct stat st;
    size_t fl = 0;
    char *ft = read_small(path, &fl);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600 && ft && !strstr(ft, "horse") &&
          strstr(ft, " 1 636f727265637420686f7273652062617474657279\n") && fl == 20 + 1 + 64 + 3 + 1 + 42);
    free(ft);
    // the AP drops us: the remembered password signs in again by itself, with a fresh SAE exchange
    uint8_t dr[6] = {4, 0, 0, 0, 0, 0};
    in_msg(AIC_SM_DISCONNECT_IND, dr, 6);
    double t0 = now_s();
    while (S_get(&S.sae_ok) < 2 && now_s() - t0 < 10) usleep(50000);
    CHECK(S_get(&S.sae_ok) == 2 && wait_for("\"state\":\"connected\"", 10));
    gStop = 1;
    pthread_join(th, NULL);
    unlink(path);
    snprintf(path, sizeof path, "%s/power", gDir);
    unlink(path);
    rmdir(gDir);
    gStop = 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("---- band choice\n");
    {
        static ScanList bl;
        memset(&bl, 0, sizeof bl);
        bl.n = 2;
        for (int i = 0; i < 2; i++) {
            memcpy(bl.e[i].bss.ssid, "Home", 4);
            bl.e[i].bss.ssid_len = 4;
            bl.e[i].bss.security = WIFI_SEC_WPA2_WPA3;
            bl.e[i].bss.rsn_akm_psk = bl.e[i].bss.rsn_ccmp = bl.e[i].bss.rsn_group_ccmp = 1;
            bl.e[i].bss.rsn_ie_len = 26;
        }
        bl.e[0].freq = 2437; bl.e[1].freq = 5745;
        int found = 0;
        bl.e[0].rssi = -46; bl.e[1].rssi = -52;                       // 5 GHz a bit weaker: still 5 GHz
        CHECK(scan_pick(&bl, (const uint8_t *)"Home", 4, 0, &found) == &bl.e[1] && found == 2);
        bl.e[0].rssi = -50; bl.e[1].rssi = -70;                       // 5 GHz 20 dB weaker: 2.4 GHz
        CHECK(scan_pick(&bl, (const uint8_t *)"Home", 4, 0, &found) == &bl.e[0]);
        bl.e[1].bss.rsn_pmf_required = 1; bl.e[1].rssi = -30;         // unusable 5 GHz AP never wins
        CHECK(scan_pick(&bl, (const uint8_t *)"Home", 4, 0, &found) == &bl.e[0]);
    }

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

    printf("---- WPA3-only network\n");
    reset("ExampleNet", "correct horse battery");
    S.mode = AP_WPA3;
    gPass = "correct horse battery";
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 0 && S.connects == 1 && S.connect_req_ok);
    CHECK(S.sae_commits == 1 && S.sae_ok == 1 && S.sae_bad == 0 && S.sae_frames_bad == 0 && S.ext_auth_rsp == 1);
    CHECK(S.sae_session && S.msg2_ok == 1 && S.msg4_ok == 1 && S.ptk_ok && S.gtk_ok && S.igtk_ok && !S.ptk_before_msg4);
    CHECK(S.port_open && S.dhcp_seen >= 1 && S.discs == 1 && S.data_bad == 0);

    printf("---- WPA3: wrong password\n");
    reset("ExampleNet", "correct horse battery");
    S.mode = AP_WPA3;
    gPass = "incorrect horse battery";
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 1 && S.sae_bad == 1 && S.sae_ok == 0 && !S.associated && S.msg2_ok == 0 && !S.ptk_ok && S.dhcp_seen == 0);
    CHECK(S.ext_auth_rsp == 1 && S.connects == 1);          // told the firmware it failed; no WPA2 retry

    printf("---- WPA2/WPA3 transition network: WPA3 is used (with an anti-clogging token)\n");
    reset("ExampleNet", "correct horse battery");
    S.mode = AP_TRANSITION;
    S.sae_token = 1;
    gPass = "correct horse battery";
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 0 && S.sae_ok == 1 && S.sae_token_ok && S.sae_session && S.igtk_ok && S.connects == 1 && S.sae_frames_bad == 0);

    printf("---- transition network where WPA3 fails: falls back to WPA2\n");
    reset("ExampleNet", "correct horse battery");
    S.mode = AP_TRANSITION;
    S.sae_reject = 1;
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 0 && S.connects == 2 && S.sae_ok == 0 && !S.sae_session && S.msg2_ok == 1 && S.ptk_ok && S.port_open);

    printf("---- transition network with MACWIFI_NO_WPA3=1: WPA2 only\n");
    reset("ExampleNet", "correct horse battery");
    S.mode = AP_TRANSITION;
    setenv("MACWIFI_NO_WPA3", "1", 1);
    rc = sim_run("ExampleNet", 1);
    unsetenv("MACWIFI_NO_WPA3");
    CHECK(rc == 0 && S.connects == 1 && S.sae_commits == 0 && S.msg2_ok == 1 && S.connect_req_ok);

    printf("---- WPA3 hash-to-element only (with an anti-clogging token), like the school's networks\n");
    reset("SchoolNet", "correct horse battery");
    S.mode = AP_WPA3;
    S.h2e = 2;
    S.sae_token = 1;
    gPass = "correct horse battery";
    rc = sim_run("SchoolNet", 1);
    CHECK(rc == 0 && S.connects == 1 && S.connect_req_ok && S.saw_rsnxe_assoc && S.saw_rsnxe_msg2);
    CHECK(S.sae_ok == 1 && S.sae_token_ok && S.sae_session && S.igtk_ok && S.dhcp_seen >= 1 && S.sae_frames_bad == 0);

    printf("---- WPA3 hash-to-element only: wrong password\n");
    reset("SchoolNet", "correct horse battery");
    S.mode = AP_WPA3;
    S.h2e = 2;
    gPass = "incorrect horse battery";
    rc = sim_run("SchoolNet", 1);
    CHECK(rc == 1 && S.sae_bad == 1 && S.sae_ok == 0 && S.dhcp_seen == 0);

    printf("---- transition network that also offers H2E: H2E is used\n");
    reset("ExampleNet", "correct horse battery");
    S.mode = AP_TRANSITION;
    S.h2e = 1;
    gPass = "correct horse battery";
    rc = sim_run("ExampleNet", 1);
    CHECK(rc == 0 && S.sae_ok == 1 && S.saw_rsnxe_assoc && S.saw_rsnxe_msg2 && S.connects == 1);

    printf("---- what can be joined\n");
    {
        WifiBss b;
        memset(&b, 0, sizeof b);
        int sec;
        const char *why;
        b.security = WIFI_SEC_WPA3_SAE;
        b.rsn_akm_sae = b.rsn_pmf_capable = b.rsn_pmf_required = b.rsn_mgmt_bip_cmac = b.rsn_ccmp = b.rsn_group_ccmp = 1;
        b.rsn_ie_len = 28;
        CHECK(bss_usable(&b, &sec, &why) == 1 && bss_can_sae(&b) && !bss_can_psk(&b));
        b.sae_h2e_only = 1;
        CHECK(bss_usable(&b, &sec, &why) == 1 && bss_sae_h2e(&b));
        b.sae_h2e_only = 0;
        b.rsn_mgmt_bip_cmac = 0;                              // e.g. BIP-GMAC-256
        CHECK(bss_usable(&b, &sec, &why) == 0);
    }

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
    CHECK(H.ba_seen == 1 * 16 + 2 * 4 + 3);   // out-of-order block-ack burst delivered as 20, 21, 22
    CHECK(H.echo_to_host == 0 && H.plain_to_host == 0 && H.eapol_to_host == 0);
    CHECK(S.discs == 1);

    test_daemon();
    test_daemon_wpa3();
    CHECK(gRxStarted == 0);    // every reader that was started was stopped

    CHECK(gTxQueued > 0);   // bridged data went through the queued (async) TX path
    printf("sim_connect: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
