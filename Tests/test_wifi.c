// Hardware-free unit tests for Sources/Shared. Reference vectors:
//  - PMK: IEEE 802.11i-2004 Annex H.4 ("password", "IEEE")
//  - PRF: IEEE 802.11i-2004 Annex H.3 test case 1
//  - AES Key Wrap: RFC 3394 §4.1
//  - PTK and MIC: computed independently with Python's hashlib/hmac (see docs/RESEARCH.md §6)
#include "ieee80211.h"
#include "mt76x02_frames.h"
#include "mt76x2u.h"
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

static void test_mt76x2u(void) {
    CHECK(mt76x2u_lookup(0x0e8d, 0x7612) != NULL);
    CHECK(mt76x2u_lookup(0x0846, 0x9053) != NULL);          // Netgear A6210
    CHECK(mt76x2u_lookup(0x0e8d, 0x7961) == NULL);          // MT7921AU: different family, not supported
    CHECK(mt76x2u_read_req(MT_ASIC_VERSION) == MT_VEND_MULTI_READ);
    CHECK(mt76x2u_read_req(MT_WLAN_MTC_CTRL_CFG) == MT_VEND_READ_CFG);
    CHECK(mt76x2u_write_req(MT_WLAN_MTC_CTRL_CFG) == MT_VEND_WRITE_CFG);
    CHECK(mt76x2u_write_req(MT_MAC_CSR0) == MT_VEND_MULTI_WRITE);
    CHECK(mt76x2u_wvalue(MT_WLAN_MTC_CTRL_CFG) == 0 && mt76x2u_windex(MT_WLAN_MTC_CTRL_CFG) == 0x148);
    CHECK(mt76x2u_wvalue(0x00120034) == 0x0012 && mt76x2u_windex(0x00120034) == 0x0034);
    CHECK(mt76x2u_is_supported_chip(0x76120044));
    CHECK(mt76x2u_is_supported_chip(0x76620000));
    CHECK(!mt76x2u_is_supported_chip(0x76010001));          // MT7601U
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

    n = build_beacon(f, "Office5G", 36, 8, 1);             // SAE only, 5 GHz (no DS element)
    CHECK(wifi_parse_bss(f, n, &b) == 0);
    CHECK(b.channel == 36 && b.security == WIFI_SEC_WPA3_SAE);
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


static void test_frames(void) {
    // A QoS data frame (26-byte header -> needs a 2-byte pad to align the body).
    uint8_t f[64];
    memset(f, 0, sizeof f);
    f[0] = 0x88; f[1] = 0x01;
    for (int i = 26; i < 60; i++) f[i] = (uint8_t)i;
    CHECK(mt76_80211_hdrlen(f, 60) == 26);
    uint8_t beacon[40] = {0x80, 0x00};
    CHECK(mt76_80211_hdrlen(beacon, 40) == 24);
    uint8_t ack[10] = {0xD4, 0x00};
    CHECK(mt76_80211_hdrlen(ack, 10) == 10);

    Mt76TxParams p = {.wcid = 1, .rate = MT_RATE_OFDM_6M, .ack = true, .hw_seq = true, .hw_key = false,
                      .qsel = MT_QSEL_EDCA, .pktid = 7};
    uint8_t out[160];
    size_t n = mt76x2u_tx_pack(f, 60, &p, out, sizeof out);
    // TXWI(20) + frame(60) + pad(2) = 82 -> aligned 84; + TXINFO 4 + trailing 4 = 92
    CHECK(n == 92);
    uint32_t txinfo = (uint32_t)out[0] | ((uint32_t)out[1] << 8) | ((uint32_t)out[2] << 16) | ((uint32_t)out[3] << 24);
    CHECK((txinfo & 0xFFFF) == 84);
    CHECK(txinfo & MT_TXD_INFO_80211);
    CHECK(txinfo & MT_TXD_INFO_WIV);                          // no hardware key yet
    CHECK(((txinfo >> MT_TXD_INFO_QSEL_SHIFT) & 3) == MT_QSEL_EDCA);
    CHECK(out[4 + 5] == 1 && out[4 + 4] == (MT_TXWI_ACK_CTL_REQ | MT_TXWI_ACK_CTL_NSEQ));
    CHECK((out[4 + 6] | (out[4 + 7] << 8)) == 60);            // len_ctl excludes the pad
    CHECK(out[4 + 19] == 7 && out[4 + 2] == 0x00 && out[4 + 3] == 0x20);  // pktid; rate = OFDM idx 0
    CHECK(memcmp(out + 24, f, 26) == 0);                      // header
    CHECK(out[24 + 26] == 0 && out[24 + 27] == 0);            // pad
    CHECK(memcmp(out + 24 + 28, f + 26, 34) == 0);            // body after the pad
    CHECK(out[88] == 0 && out[91] == 0);
    CHECK(mt76x2u_tx_pack(f, 60, &p, out, 80) == 0);          // too small
    p.hw_key = true;
    n = mt76x2u_tx_pack(beacon, 40, &p, out, sizeof out);
    txinfo = (uint32_t)out[0] | ((uint32_t)out[1] << 8) | ((uint32_t)out[2] << 16) | ((uint32_t)out[3] << 24);
    CHECK(n > 0 && !(txinfo & MT_TXD_INFO_WIV) && (out[4] & MT_TXWI_FLAGS_TS));

    // RX: same QoS frame, decrypted by hardware, with L2 pad and an 8-byte CCMP IV still in the buffer.
    uint8_t rx[256];
    memset(rx, 0, sizeof rx);
    size_t mpdu = 26 + 8 + 34;                                // header + IV + body (pad excluded)
    size_t frame_bytes = 26 + 2 + 8 + 34;
    size_t dma = 32 + ((frame_bytes + 3) & ~(size_t)3);
    rx[0] = (uint8_t)dma; rx[1] = (uint8_t)(dma >> 8);
    uint32_t rxinfo = MT_RXINFO_DATA | MT_RXINFO_UNICAST | MT_RXINFO_L2PAD | MT_RXINFO_DECRYPT |
                      (2u << MT_RXINFO_PN_LEN_SHIFT);
    uint32_t ctl = 3u | ((uint32_t)mpdu << 16);               // wcid 3
    for (int i = 0; i < 4; i++) { rx[4 + i] = (uint8_t)(rxinfo >> (8 * i)); rx[8 + i] = (uint8_t)(ctl >> (8 * i)); }
    rx[4 + 10] = 0x07; rx[4 + 11] = 0x40;                     // rate: HT (phy 2) MCS 7
    rx[4 + 12] = (uint8_t)-40;
    uint8_t *fr = rx + 4 + 32;
    memcpy(fr, f, 26);
    memset(fr + 28, 0xEE, 8);                                 // IV
    memcpy(fr + 36, f + 26, 34);
    size_t rxlen = 4 + dma + 4;
    uint8_t got[128];
    Mt76RxInfo ri;
    long gl = mt76x2u_rx_unpack(rx, rxlen, &ri, got, sizeof got);
    CHECK(gl == 60);
    CHECK(memcmp(got, f, 60) == 0);                           // pad and IV removed
    CHECK(ri.wcid == 3 && ri.decrypted && ri.unicast && ri.phy == MT_PHY_HT && ri.mcs == 7 && ri.rssi[0] == -40);

    rx[0] = 0x03;                                             // dma_len not a multiple of 4
    CHECK(mt76x2u_rx_unpack(rx, rxlen, &ri, got, sizeof got) == -1);
    rx[0] = (uint8_t)dma;
    CHECK(mt76x2u_rx_unpack(rx, 20, &ri, got, sizeof got) == -1);   // too short
    ctl = 3u | (200u << 16);                                  // MPDU longer than the buffer
    for (int i = 0; i < 4; i++) rx[8 + i] = (uint8_t)(ctl >> (8 * i));
    CHECK(mt76x2u_rx_unpack(rx, rxlen, &ri, got, sizeof got) == -1);
}

int main(void) {
    test_mt76x2u();
    test_wpa();
    test_bss();
    test_data();
    test_frames();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
