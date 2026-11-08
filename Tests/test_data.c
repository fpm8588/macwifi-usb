// Hardware-free tests for the USB data path (Sources/Shared/aic_data.c). The descriptor and RX header layouts
// were also compared with the Linux driver's own structures (see docs/HARDWARE_LOG.md, phase 3b).
#include "aic_data.h"
#include "aic_msg.h"

#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t AP[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
static const uint8_t ME[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

static void test_tx(void) {
    uint8_t eth[1600], out[2100];
    memcpy(eth, AP, 6);
    memcpy(eth + 6, ME, 6);
    eth[12] = 0x88; eth[13] = 0x8E;                                   // EAPOL, 99-byte body
    for (int i = 0; i < 99; i++) eth[14 + i] = (uint8_t)i;
    size_t n = aic_data_build_tx(out, sizeof out, eth, 14 + 99, 0, 1, 0, 7);
    CHECK(n == 132);                                                   // 4 + 28 + 99 = 131, padded to 132
    CHECK(out[0] == 132 && out[1] == 0 && out[2] == 0x01 && out[3] == 0);
    CHECK(out[4] == 99 && out[5] == 0);                                // packet_len
    CHECK(out[8] == 7 && out[11] == 0x80);                             // status_desc_addr = BIT(31) | 7
    CHECK(memcmp(out + 12, AP, 6) == 0 && memcmp(out + 18, ME, 6) == 0);
    CHECK(out[24] == 0x88 && out[25] == 0x8E);                         // ethertype in network order
    CHECK(out[26] == 1 && out[27] == 0 && out[28] == 0 && out[29] == 1);   // ac BE, tid 0, vif 0, sta 1
    CHECK(memcmp(out + 32, eth + 14, 99) == 0 && out[131] == 0);

    eth[12] = 0x08; eth[13] = 0x00;                                    // IPv4: no confirmation, no padding
    n = aic_data_build_tx(out, sizeof out, eth, 14 + 99, 2, 1, 6, 7);
    CHECK(n == 131 && out[0] == 131 && out[8] == 0 && out[11] == 0);
    CHECK(out[26] == 3 && out[27] == 6 && out[28] == 2);               // tid 6 -> VO

    n = aic_data_build_tx(out, sizeof out, eth, 14 + 512 - 32, 0, 1, 0, 0);   // exactly 512 bytes
    CHECK(n == 513 && out[0] == 0x00 && out[1] == 0x02 && out[512] == 0);    // length field 512, extra byte

    CHECK(aic_data_build_tx(out, sizeof out, eth, 13, 0, 0, 0, 0) == 0);      // shorter than an Ethernet header
    CHECK(aic_data_build_tx(out, 100, eth, 14 + 99, 0, 0, 0, 0) == 0);        // cap too small
    CHECK(aic_data_build_tx(out, sizeof out, eth, 14 + 4064, 0, 0, 0, 0) == 0);   // > 12-bit length
    CHECK(aic_tid_to_ac(0) == 1 && aic_tid_to_ac(1) == 0 && aic_tid_to_ac(4) == 2 && aic_tid_to_ac(7) == 3);
}

// Builds a data packet as the chip sends it: hw_rxhdr (60 bytes) + 802.11 frame.
static size_t rx_packet(uint8_t *pkt, const uint8_t *frame, size_t flen, uint8_t decr, uint8_t flags) {
    memset(pkt, 0, 60);
    pkt[0] = (uint8_t)flen; pkt[1] = (uint8_t)(flen >> 8);
    pkt[17] = (uint8_t)-40;
    pkt[36] = (uint8_t)(decr << 2);
    pkt[48] = flags;
    pkt[49] = 0; pkt[50] = 1; pkt[51] = 0xff;
    memcpy(pkt + 60, frame, flen);
    return 60 + flen;
}

static uint8_t got[8][1600];
static size_t got_len[8];
static int got_n;
static void collect(void *ctx, const uint8_t *eth, size_t len) {
    (void)ctx;
    if (got_n < 8) { memcpy(got[got_n], eth, len); got_len[got_n++] = len; }
}

static void test_rx(void) {
    uint8_t f[400], pkt[600], scratch[1600];
    AicRxInfo ri;
    // QoS data from the AP (FromDS), CCMP: header 26 + CCMP 8 + LLC/SNAP + ARP payload (28 bytes).
    size_t n = 0;
    f[n++] = 0x88; f[n++] = 0x42;                                      // QoS data, FromDS, protected
    f[n++] = 0; f[n++] = 0;
    memcpy(f + n, ME, 6); n += 6;                                      // addr1 = DA
    memcpy(f + n, AP, 6); n += 6;                                      // addr2 = BSSID
    static const uint8_t SA[6] = {0x02, 0x99, 0x88, 0x77, 0x66, 0x55};
    memcpy(f + n, SA, 6); n += 6;                                      // addr3 = SA
    f[n++] = 0x10; f[n++] = 0;                                         // seq
    f[n++] = 0x00; f[n++] = 0x00;                                      // QoS control: TID 0, no A-MSDU
    memset(f + n, 0xEE, 8); n += 8;                                    // CCMP header
    static const uint8_t llc[8] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x08, 0x06};
    memcpy(f + n, llc, 8); n += 8;
    for (int i = 0; i < 28; i++) f[n++] = (uint8_t)(0x40 + i);
    size_t pl = rx_packet(pkt, f, n, 3, 0x40);
    CHECK(aic_data_parse_rx(pkt, pl, &ri) == 0 && ri.frame_len == n && ri.decr_status == 3 && ri.upload);
    CHECK(ri.rssi == -40 && ri.sta_idx == 1 && ri.dst_idx == 0xff);
    got_n = 0;
    CHECK(aic_data_rx_to_eth(&ri, scratch, sizeof scratch, collect, NULL) == 1 && got_n == 1);
    CHECK(got_len[0] == 14 + 28 && memcmp(got[0], ME, 6) == 0 && memcmp(got[0] + 6, SA, 6) == 0);
    CHECK(got[0][12] == 0x08 && got[0][13] == 0x06 && got[0][14] == 0x40 && got[0][14 + 27] == 0x40 + 27);

    // EAPOL message 1 arrives unencrypted (decr_status 0), non-QoS data.
    uint8_t g[200];
    size_t m = 0;
    g[m++] = 0x08; g[m++] = 0x02;                                      // data, FromDS
    g[m++] = 0; g[m++] = 0;
    memcpy(g + m, ME, 6); m += 6;
    memcpy(g + m, AP, 6); m += 6;
    memcpy(g + m, AP, 6); m += 6;
    g[m++] = 0; g[m++] = 0;
    static const uint8_t llc_eapol[8] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x88, 0x8E};
    memcpy(g + m, llc_eapol, 8); m += 8;
    g[m++] = 2; g[m++] = 3; g[m++] = 0; g[m++] = 95;                   // EAPOL header (body truncated here)
    pl = rx_packet(pkt, g, m, 0, 0x40);
    CHECK(aic_data_parse_rx(pkt, pl, &ri) == 0);
    got_n = 0;
    CHECK(aic_data_rx_to_eth(&ri, scratch, sizeof scratch, collect, NULL) == 1);
    CHECK(got_len[0] == 18 && got[0][12] == 0x88 && got[0][13] == 0x8E && memcmp(got[0] + 6, AP, 6) == 0);

    // A-MSDU with two subframes (second one padded).
    m = 0;
    g[m++] = 0x88; g[m++] = 0x02;
    g[m++] = 0; g[m++] = 0;
    memcpy(g + m, ME, 6); m += 6;
    memcpy(g + m, AP, 6); m += 6;
    memcpy(g + m, AP, 6); m += 6;
    g[m++] = 0; g[m++] = 0;
    g[m++] = 0x80; g[m++] = 0x00;                                      // QoS control: A-MSDU present
    for (int s = 0; s < 2; s++) {
        memcpy(g + m, ME, 6); m += 6;
        memcpy(g + m, SA, 6); m += 6;
        g[m++] = 0; g[m++] = 8 + 3;                                    // subframe length (BE)
        memcpy(g + m, llc, 8); m += 8;
        g[m++] = (uint8_t)(0xA0 + s); g[m++] = 1; g[m++] = 2;
        if (s == 0) { g[m++] = 0; g[m++] = 0; g[m++] = 0; }             // pad 25 -> 28
    }
    pl = rx_packet(pkt, g, m, 0, 0x41);
    CHECK(aic_data_parse_rx(pkt, pl, &ri) == 0 && ri.is_amsdu);
    got_n = 0;
    CHECK(aic_data_rx_to_eth(&ri, scratch, sizeof scratch, collect, NULL) == 2 && got_n == 2);
    CHECK(got_len[0] == 17 && got[0][14] == 0xA0 && got_len[1] == 17 && got[1][14] == 0xA1);

    // NETGEAR quirk: A-MSDU bit set on a plain frame (LLC header right after the QoS header).
    g[24] = 0x80;
    m = 26;
    memcpy(g + m, llc, 8); m += 8;
    g[m++] = 0x45;
    pl = rx_packet(pkt, g, m, 0, 0x40);
    CHECK(aic_data_parse_rx(pkt, pl, &ri) == 0);
    got_n = 0;
    CHECK(aic_data_rx_to_eth(&ri, scratch, sizeof scratch, collect, NULL) == 1 && got_len[0] == 15 && got[0][14] == 0x45);

    // QoS null data: nothing to deliver; management frames and 4-address frames: not ours.
    g[0] = 0xC8; g[1] = 0x01;
    pl = rx_packet(pkt, g, 26, 0, 0x40);
    CHECK(aic_data_parse_rx(pkt, pl, &ri) == 0 && aic_data_rx_to_eth(&ri, scratch, sizeof scratch, collect, NULL) == 0);
    g[0] = 0x80;                                                       // beacon
    pl = rx_packet(pkt, g, 40, 0, 0x42);
    CHECK(aic_data_parse_rx(pkt, pl, &ri) == 0 && ri.is_80211_mpdu &&
          aic_data_rx_to_eth(&ri, scratch, sizeof scratch, collect, NULL) == -1);
    g[0] = 0x08; g[1] = 0x03;                                          // ToDS | FromDS
    pl = rx_packet(pkt, g, 40, 0, 0x40);
    CHECK(aic_data_parse_rx(pkt, pl, &ri) == 0 && aic_data_rx_to_eth(&ri, scratch, sizeof scratch, collect, NULL) == -1);

    // Truncation: frame length past the packet, packet shorter than the header.
    pl = rx_packet(pkt, f, n, 3, 0x40);
    CHECK(aic_data_parse_rx(pkt, pl - 1, &ri) == -1 && aic_data_parse_rx(pkt, 59, &ri) == -1);

    // Aggregated transfers: data packets follow each other with no padding (len + 60 exactly), as
    // aicwf_process_rxframes walks them; found on the adapter, where rounding to 4 broke ~9k transfers under load.
    uint8_t two[1600];
    size_t a = rx_packet(two, f, n, 3, 0x40);
    size_t b = rx_packet(two + a, g, 31, 0, 0x40);          // odd frame length
    size_t c = rx_packet(two + a + b, f, n, 3, 0x40);
    CHECK(((a + b) & 3) != 0);                              // the third packet starts unaligned
    size_t total = a + b + c, off = 0;
    AicRxPacket pk;
    AicRxInfo r2;
    CHECK(aic_rx_next(two, total, &off, &pk) == 1 && !pk.is_msg && pk.packet == two && off == a);
    CHECK(aic_rx_next(two, total, &off, &pk) == 1 && pk.packet == two + a && pk.packet_len == b);
    CHECK(aic_data_parse_rx(pk.packet, pk.packet_len, &r2) == 0 && r2.frame_len == 31);
    CHECK(aic_rx_next(two, total, &off, &pk) == 1 && pk.packet == two + a + b && off == total);
    CHECK(aic_data_parse_rx(pk.packet, pk.packet_len, &r2) == 0 && r2.frame_len == n);
    CHECK(aic_rx_next(two, total, &off, &pk) == 0);
}

static void test_tx_cfm(void) {
    const uint8_t c[12] = {8, 0, 0x12, 0, 0x09, 0, 0, 0, 3, 0, 0, 0};
    uint32_t st = 0, idx = 0;
    CHECK(aic_data_parse_tx_cfm(c, sizeof c, &st, &idx) == 0 && st == 9 && (st & AIC_TXST_ACKED) && idx == 3);
    CHECK(aic_data_parse_tx_cfm(c, 11, &st, &idx) == -1);
    uint8_t d[12];
    memcpy(d, c, 12);
    d[2] = 0x11;
    CHECK(aic_data_parse_tx_cfm(d, sizeof d, &st, &idx) == -1);
}

int main(void) {
    test_tx_cfm();
    test_tx();
    test_rx();
    printf("data: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
