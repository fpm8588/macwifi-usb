// Hardware-free unit tests for Sources/Shared. Reference vectors:
//  - PMK: IEEE 802.11i-2004 Annex H.4 ("password", "IEEE")
//  - PRF: IEEE 802.11i-2004 Annex H.3 test case 1
//  - AES Key Wrap: RFC 3394 §4.1
//  - PTK and MIC: computed independently with Python's hashlib/hmac (see docs/RESEARCH.md §6)
#include "ieee80211.h"
#include "wpa.h"

#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_wpa(void) {
    uint8_t pmk[32], want[48];
    CHECK(wpa_pmk_from_passphrase("password", (const uint8_t *)"IEEE", 4, pmk) == 0);
    hex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", want, 32);
    CHECK(memcmp(pmk, want, 32) == 0);
    CHECK(wpa_pmk_from_passphrase("short", (const uint8_t *)"IEEE", 4, pmk) == -1);   // < 8 chars

    uint8_t key[20], prf[24];
    memset(key, 0x0b, sizeof key);
    CHECK(wpa_prf_sha1(key, 20, "prefix", (const uint8_t *)"Hi There", 8, prf, 24) == 0);
    hex("bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606", want, 24);
    CHECK(memcmp(prf, want, 24) == 0);

    uint8_t aa[6], spa[6], an[32], sn[32], ptk[48], ptk2[48];
    hex("a0a1a1a3a4a5", aa, 6);
    hex("b0b1b2b3b4b5", spa, 6);
    for (int i = 0; i < 32; i++) { an[i] = (uint8_t)i; sn[i] = (uint8_t)(0x20 + i); }
    wpa_pmk_from_passphrase("password", (const uint8_t *)"IEEE", 4, pmk);
    CHECK(wpa_derive_ptk(pmk, aa, spa, an, sn, ptk) == 0);
    hex("9c5f622b82f20e42d71c356b0aed4bca67e2fd81f8517311476918a89478082a"
        "fc8c086d396f42760bcb6a9cd6264193", want, 48);
    CHECK(memcmp(ptk, want, 48) == 0);
    // Order independence: swapping roles must give the same PTK.
    CHECK(wpa_derive_ptk(pmk, spa, aa, sn, an, ptk2) == 0 && memcmp(ptk, ptk2, 48) == 0);

    uint8_t eapol[99], mic[16];
    for (int i = 0; i < 99; i++) eapol[i] = (uint8_t)i;
    CHECK(wpa_eapol_mic(WPA_KCK(ptk), 2, eapol, sizeof eapol, mic) == 0);
    hex("cc38032c8c91079fa32138131cc59101", want, 16);
    CHECK(memcmp(mic, want, 16) == 0);
    CHECK(wpa_eapol_mic(WPA_KCK(ptk), 1, eapol, sizeof eapol, mic) == -1);  // TKIP unsupported

    uint8_t kek[16], wrapped[24], plain[16], out[16], rewrap[24];
    hex("000102030405060708090A0B0C0D0E0F", kek, 16);
    hex("1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5", wrapped, 24);
    CHECK(wpa_aes_unwrap(kek, wrapped, 24, out) == 0);
    hex("00112233445566778899AABBCCDDEEFF", plain, 16);
    CHECK(memcmp(out, plain, 16) == 0);
    CHECK(wpa_aes_wrap(kek, plain, 16, rewrap) == 0 && memcmp(rewrap, wrapped, 24) == 0);
    wrapped[3] ^= 1;                                         // tamper -> integrity check fails
    CHECK(wpa_aes_unwrap(kek, wrapped, 24, out) == -1);
}

static size_t build_beacon(uint8_t *f, const char *ssid, int ch, int rsn_akm, int ht40_5g) {
    size_t n = 0;
    f[n++] = 0x80; f[n++] = 0x00;                           // beacon
    f[n++] = 0; f[n++] = 0;
    memset(f + n, 0xff, 6); n += 6;                         // A1 broadcast
    for (int i = 0; i < 6; i++) f[n++] = (uint8_t)(0x10 + i); // A2 = SA
    for (int i = 0; i < 6; i++) f[n++] = (uint8_t)(0x10 + i); // A3 = BSSID
    f[n++] = 0; f[n++] = 0;                                 // seq
    memset(f + n, 0, 8); n += 8;                            // timestamp
    f[n++] = 100; f[n++] = 0;                               // beacon interval 100 TU
    f[n++] = 0x11; f[n++] = 0x04;                           // capability: ESS | Privacy | short slot
    size_t sl = strlen(ssid);
    f[n++] = 0; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;
    if (!ht40_5g) { f[n++] = 3; f[n++] = 1; f[n++] = (uint8_t)ch; }
    f[n++] = 45; f[n++] = 26; memset(f + n, 0, 26); n += 26;               // HT capabilities
    f[n++] = 61; f[n++] = 22; f[n] = (uint8_t)ch; memset(f + n + 1, 0, 21); n += 22;  // HT operation
    if (rsn_akm) {
        uint8_t rsn[] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                         1, 0, 0x00, 0x0F, 0xAC, (uint8_t)rsn_akm, 0x00, 0x00};
        memcpy(f + n, rsn, sizeof rsn); n += sizeof rsn;
    }
    return n;
}

static void test_bss(void) {
    uint8_t f[256];
    WifiBss b;
    size_t n = build_beacon(f, "HomeNet", 6, 2, 0);
    CHECK(wifi_parse_bss(f, n, &b) == 0);
    CHECK(strcmp(b.ssid, "HomeNet") == 0 && b.ssid_len == 7);
    CHECK(b.channel == 6 && b.ht && !b.vht);
    CHECK(b.security == WIFI_SEC_WPA2_PSK && b.rsn_ccmp);
    CHECK(b.bssid[0] == 0x10 && b.bssid[5] == 0x15 && b.beacon_interval == 100);
    CHECK(b.rsn_group_ccmp && b.rsn_akm_psk && !b.rsn_pmf_capable && !b.rsn_pmf_required);
    CHECK(b.rsn_ie_len >= 2 && b.rsn_ie[0] == 48 && b.rsn_ie[1] == b.rsn_ie_len - 2 &&
          memcmp(b.rsn_ie, f + n - b.rsn_ie_len, b.rsn_ie_len) == 0);   // the raw element, for the msg 3 check
    f[n - 2] = 0x80;                                        // RSN capabilities: MFPC
    CHECK(wifi_parse_bss(f, n, &b) == 0 && b.rsn_pmf_capable && !b.rsn_pmf_required);
    f[n - 2] = 0xC0;                                        // MFPC | MFPR
    CHECK(wifi_parse_bss(f, n, &b) == 0 && b.rsn_pmf_capable && b.rsn_pmf_required);
    f[n - 15] = 2;                                          // group cipher TKIP (00-0F-AC:2)
    CHECK(wifi_parse_bss(f, n, &b) == 0 && !b.rsn_group_ccmp && b.rsn_ccmp);

    n = build_beacon(f, "Office5G", 36, 8, 1);             // SAE only, 5 GHz (no DS element)
    CHECK(wifi_parse_bss(f, n, &b) == 0);
    CHECK(b.channel == 36 && b.security == WIFI_SEC_WPA3_SAE && !b.rsn_akm_psk);
    CHECK(wifi_channel_to_mhz(36) == 5180 && wifi_channel_to_mhz(6) == 2437 && wifi_channel_to_mhz(14) == 2484);

    n = build_beacon(f, "Cafe", 1, 0, 0);                  // privacy bit but no RSN -> WEP
    CHECK(wifi_parse_bss(f, n, &b) == 0 && b.security == WIFI_SEC_WEP);

    n = build_beacon(f, "Cut", 1, 2, 0);
    CHECK(wifi_parse_bss(f, n - 3, &b) == -1);              // truncated element
    f[0] = 0x08;                                            // data frame, not a beacon
    CHECK(wifi_parse_bss(f, n, &b) == -1);
}

static void test_data(void) {
    const uint8_t me[6] = {0x02, 0, 0, 0, 0, 1}, ap[6] = {0xaa, 0xbb, 0xcc, 0, 0, 1}, gw[6] = {0x0c, 1, 2, 3, 4, 5};
    uint8_t eth[64], w[128], back[128];
    memcpy(eth, gw, 6); memcpy(eth + 6, me, 6);
    eth[12] = 0x08; eth[13] = 0x00;                         // IPv4
    for (int i = 14; i < 64; i++) eth[i] = (uint8_t)i;

    size_t wl = wifi_eth_to_80211(eth, 64, ap, true, w, sizeof w);
    CHECK(wl == 26 + 8 + 50);
    CHECK(w[0] == 0x88 && w[1] == 0x01);                    // QoS data, To DS
    CHECK(memcmp(w + 4, ap, 6) == 0 && memcmp(w + 10, me, 6) == 0 && memcmp(w + 16, gw, 6) == 0);
    CHECK(w[26] == 0xAA && w[27] == 0xAA && w[28] == 0x03 && w[32] == 0x08 && w[33] == 0x00);
    CHECK(wifi_eth_to_80211(eth, 64, ap, true, w, 40) == 0); // too small

    // The AP's reply: From DS, A1 = us, A2 = BSSID, A3 = original sender.
    uint8_t r[128];
    memcpy(r, w, wl);
    r[1] = 0x02;
    memcpy(r + 4, me, 6); memcpy(r + 10, ap, 6); memcpy(r + 16, gw, 6);
    long el = wifi_80211_to_eth(r, wl, false, back, sizeof back);
    CHECK(el == 64);
    CHECK(memcmp(back, me, 6) == 0 && memcmp(back + 6, gw, 6) == 0);
    CHECK(memcmp(back + 12, eth + 12, 52) == 0);

    r[1] = 0x42;                                            // Protected, not yet decrypted
    CHECK(wifi_80211_to_eth(r, wl, false, back, sizeof back) == -1);
    CHECK(wifi_80211_to_eth(r, wl, true, back, sizeof back) == 64);
    r[1] = 0x02; r[0] = 0xC8;                               // QoS null: no payload
    CHECK(wifi_80211_to_eth(r, wl, false, back, sizeof back) == 0);
    r[0] = 0x88; r[24] = 0x80;                              // A-MSDU flag
    CHECK(wifi_80211_to_eth(r, wl, false, back, sizeof back) == -1);

    eth[12] = 0x80; eth[13] = 0xF3;                         // AARP uses the 802.1H bridge-tunnel OUI
    wl = wifi_eth_to_80211(eth, 64, ap, false, w, sizeof w);
    CHECK(wl == 24 + 8 + 50 && w[0] == 0x08 && w[29] == 0xF8);
}



int main(void) {
    test_wpa();
    test_bss();
    test_data();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
