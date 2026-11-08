// Hardware-free tests for the WPA2-PSK supplicant (Sources/Shared/eapol.c), run against a simulated access point
// that builds its frames independently from the IEEE 802.11 field layout.
#include "eapol.h"
#include "wpa.h"

#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t AA[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
static const uint8_t SPA[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t AP_RSN[26] = {0x30, 24, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                   2, 0, 0x00, 0x0F, 0xAC, 2, 0x00, 0x0F, 0xAC, 8, 0x80, 0x00};   // PSK + SAE, MFPC
static const uint8_t GTK1[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t GTK2[16] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f};

// --- the simulated AP
static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static size_t ap_frame(uint8_t *out, unsigned key_info, unsigned replay, const uint8_t *nonce, const uint8_t *kd, size_t kdl,
                       const uint8_t *ptk, int encrypt) {
    uint8_t data[512];
    size_t dl = kdl;
    if (kdl) memcpy(data, kd, kdl);
    if (encrypt) {                               // pad (0xDD, zeros) to a multiple of 8, at least 16, then wrap
        if (dl < 16 || dl % 8) { data[dl++] = 0xDD; while (dl < 16 || dl % 8) data[dl++] = 0; }
        uint8_t w[520];
        wpa_aes_wrap(WPA_KEK(ptk), data, dl, w);
        memcpy(data, w, dl + 8);
        dl += 8;
    }
    size_t len = 99 + dl;
    memset(out, 0, len);
    out[0] = 2; out[1] = 3; be16(out + 2, (unsigned)(len - 4));
    out[4] = 2; be16(out + 5, key_info); be16(out + 7, 16);
    out[16] = (uint8_t)replay;
    if (nonce) memcpy(out + 17, nonce, 32);
    be16(out + 97, (unsigned)dl);
    memcpy(out + 99, data, dl);
    if (ptk) wpa_eapol_mic(WPA_KCK(ptk), 2, out, len, out + 81);
    return len;
}
static int ap_mic_ok(const uint8_t *ptk, const uint8_t *f, size_t n) {
    uint8_t t[600], mic[16];
    memcpy(t, f, n);
    memset(t + 81, 0, 16);
    wpa_eapol_mic(WPA_KCK(ptk), 2, t, n, mic);
    return memcmp(mic, f + 81, 16) == 0;
}
static size_t msg3_keydata(uint8_t *kd, const uint8_t *rsn, size_t rsn_len, int keyid, const uint8_t *gtk) {
    size_t n = 0;
    if (rsn_len) memcpy(kd, rsn, rsn_len);
    n += rsn_len;
    kd[n++] = 0xDD; kd[n++] = 22; kd[n++] = 0x00; kd[n++] = 0x0F; kd[n++] = 0xAC; kd[n++] = 1;
    kd[n++] = (uint8_t)keyid; kd[n++] = 0;
    memcpy(kd + n, gtk, 16); n += 16;
    return n;
}

static void setup(WpaSupplicant *s, const char *pass) {
    memset(s, 0, sizeof *s);
    CHECK(wpa_pmk_from_passphrase(pass, (const uint8_t *)"TestNet", 7, s->pmk) == 0);
    memcpy(s->aa, AA, 6);
    memcpy(s->spa, SPA, 6);
    s->rsn_ie_len = wpa_build_rsn_ie(s->rsn_ie, sizeof s->rsn_ie);
    s->ap_rsn_ie = AP_RSN;
    s->ap_rsn_ie_len = sizeof AP_RSN;
}

static void test_rsn_ie(void) {
    uint8_t ie[32];
    static const uint8_t want[22] = {0x30, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0, 0};
    CHECK(wpa_build_rsn_ie(ie, sizeof ie) == 22 && memcmp(ie, want, 22) == 0);
    CHECK(wpa_build_rsn_ie(ie, 21) == 0);
}

static void test_handshake(void) {
    WpaSupplicant s;
    setup(&s, "correct horse battery");
    uint8_t pmk_ap[32];
    wpa_pmk_from_passphrase("correct horse battery", (const uint8_t *)"TestNet", 7, pmk_ap);
    uint8_t anonce[32], snonce[32], f[600], r[600], kd[200];
    for (int i = 0; i < 32; i++) { anonce[i] = (uint8_t)(0xA0 + i); snonce[i] = (uint8_t)(0x50 + i); }
    size_t rl = 0;
    const char *err = NULL;

    // Message 1 -> message 2
    size_t n = ap_frame(f, 0x008A, 1, anonce, NULL, 0, NULL, 0);
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == WPA_EV_SEND && err == NULL);
    EapolKey k;
    CHECK(eapol_parse_key(r, rl, &k) == 0 && k.key_info == 0x010A && k.key_len == 0 && r[0] == 1);
    CHECK(memcmp(k.nonce, snonce, 32) == 0 && k.replay[7] == 1);
    CHECK(k.key_data_len == 22 && memcmp(k.key_data, s.rsn_ie, 22) == 0);
    uint8_t ptk_ap[48];
    CHECK(wpa_derive_ptk(pmk_ap, AA, SPA, anonce, k.nonce, ptk_ap) == 0);
    CHECK(ap_mic_ok(ptk_ap, r, rl));                                    // the AP accepts message 2
    CHECK(memcmp(ptk_ap, s.ptk, 48) == 0);

    // Message 3 -> message 4, keys
    size_t kdl = msg3_keydata(kd, AP_RSN, sizeof AP_RSN, 1, GTK1);
    n = ap_frame(f, 0x13CA, 2, anonce, kd, kdl, ptk_ap, 1);
    int ev = wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err);
    CHECK(ev == (WPA_EV_SEND | WPA_EV_INSTALL_PTK | WPA_EV_INSTALL_GTK | WPA_EV_DONE));
    CHECK(s.gtk_idx == 1 && s.gtk_len == 16 && memcmp(s.gtk, GTK1, 16) == 0);
    CHECK(eapol_parse_key(r, rl, &k) == 0 && k.key_info == 0x030A && k.replay[7] == 2 && k.key_data_len == 0);
    CHECK(ap_mic_ok(ptk_ap, r, rl));                                    // the AP accepts message 4

    // A replay of message 3 is rejected.
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == -1 && strstr(err, "replayed"));

    // Group key rekey: GTK index 2.
    kdl = msg3_keydata(kd, NULL, 0, 2, GTK2);
    n = ap_frame(f, 0x1382, 3, NULL, kd, kdl, ptk_ap, 1);
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == (WPA_EV_SEND | WPA_EV_INSTALL_GTK));
    CHECK(s.gtk_idx == 2 && memcmp(s.gtk, GTK2, 16) == 0);
    CHECK(eapol_parse_key(r, rl, &k) == 0 && k.key_info == 0x0302 && ap_mic_ok(ptk_ap, r, rl));
}

static void test_failures(void) {
    uint8_t anonce[32] = {1}, snonce[32] = {2}, f[600], r[600], kd[200];
    size_t rl;
    const char *err;
    WpaSupplicant s;

    // Wrong password: message 3's MIC doesn't verify.
    setup(&s, "the wrong password");
    uint8_t pmk_ap[32], ptk_ap[48];
    wpa_pmk_from_passphrase("correct horse battery", (const uint8_t *)"TestNet", 7, pmk_ap);
    size_t n = ap_frame(f, 0x008A, 1, anonce, NULL, 0, NULL, 0);
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == WPA_EV_SEND);
    wpa_derive_ptk(pmk_ap, AA, SPA, anonce, snonce, ptk_ap);
    CHECK(!ap_mic_ok(ptk_ap, r, rl));                                   // the AP would reject message 2
    size_t kdl = msg3_keydata(kd, AP_RSN, sizeof AP_RSN, 1, GTK1);
    n = ap_frame(f, 0x13CA, 2, anonce, kd, kdl, ptk_ap, 1);
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == -1 && strstr(err, "MIC"));
    CHECK(!s.ptk_done);

    // Right password, but message 3 carries a different RSN element than the beacon (downgrade).
    setup(&s, "correct horse battery");
    n = ap_frame(f, 0x008A, 1, anonce, NULL, 0, NULL, 0);
    wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err);
    uint8_t weak[26];
    memcpy(weak, AP_RSN, 26);
    weak[23] = 2;                                                       // SAE -> PSK
    kdl = msg3_keydata(kd, weak, sizeof weak, 1, GTK1);
    n = ap_frame(f, 0x13CA, 2, anonce, kd, kdl, ptk_ap, 1);
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == -1 && strstr(err, "downgrade"));

    // ANonce changed between messages 1 and 3.
    setup(&s, "correct horse battery");
    n = ap_frame(f, 0x008A, 1, anonce, NULL, 0, NULL, 0);
    wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err);
    uint8_t other[32] = {9};
    kdl = msg3_keydata(kd, AP_RSN, sizeof AP_RSN, 1, GTK1);
    n = ap_frame(f, 0x13CA, 2, other, kd, kdl, ptk_ap, 1);
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == -1 && strstr(err, "ANonce"));

    // Key data that doesn't unwrap (MIC computed after the damage, so the unwrap check is what catches it).
    setup(&s, "correct horse battery");
    n = ap_frame(f, 0x008A, 1, anonce, NULL, 0, NULL, 0);
    wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err);
    n = ap_frame(f, 0x13CA, 2, anonce, kd, kdl, ptk_ap, 1);
    f[99 + 5] ^= 0x40;
    memset(f + 81, 0, 16);
    wpa_eapol_mic(WPA_KCK(ptk_ap), 2, f, n, f + 81);
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == -1 && strstr(err, "wrap"));

    // Unencrypted key data in message 3 is refused; so is a group message before the 4-way handshake.
    n = ap_frame(f, 0x03CA, 3, anonce, kd, kdl, ptk_ap, 0);
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == -1 && strstr(err, "encrypted"));
    n = ap_frame(f, 0x1382, 4, NULL, kd, kdl, ptk_ap, 1);
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == -1 && strstr(err, "before"));

    // Things that are ignored: not EAPOL-Key, a request from a station, truncated frames.
    uint8_t start[4] = {1, 1, 0, 0};                                    // EAPOL-Start
    CHECK(wpa_supp_rx(&s, start, 4, snonce, r, sizeof r, &rl, &err) == 0);
    n = ap_frame(f, 0x0B0A, 5, NULL, NULL, 0, NULL, 0);                 // request bit, no ACK
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == 0);
    n = ap_frame(f, 0x008A, 6, anonce, NULL, 0, NULL, 0);
    CHECK(wpa_supp_rx(&s, f, n - 1, snonce, r, sizeof r, &rl, &err) == 0);
    EapolKey k;
    CHECK(eapol_parse_key(f, 98, &k) == -1);
    f[4] = 254;                                                         // WPA1 descriptor: not supported
    CHECK(eapol_parse_key(f, n, &k) == -1);
    n = ap_frame(f, 0x0089, 7, anonce, NULL, 0, NULL, 0);               // descriptor version 1 (TKIP)
    CHECK(wpa_supp_rx(&s, f, n, snonce, r, sizeof r, &rl, &err) == -1 && strstr(err, "version"));
}

int main(void) {
    test_rsn_ie();
    test_handshake();
    test_failures();
    printf("eapol: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
