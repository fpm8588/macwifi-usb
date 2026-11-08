// test_bridge.c — BPF record walking and the bridge's forwarding rules. Original code (MIT).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t kMac[6] = {0x02, 0x00, 0x00, 0xaa, 0xbb, 0xcc};

// Appends one record the way Darwin's bpf does: 18-byte header (hdrlen 18 on 64-bit macOS), packet, pad to 4.
static size_t put_rec(uint8_t *b, size_t off, const uint8_t *pkt, size_t n, uint16_t hdrlen) {
    memset(b + off, 0, hdrlen);
    b[off + 8] = (uint8_t)n; b[off + 9] = (uint8_t)(n >> 8);       // caplen
    b[off + 12] = (uint8_t)n; b[off + 13] = (uint8_t)(n >> 8);     // datalen
    b[off + 16] = (uint8_t)hdrlen; b[off + 17] = (uint8_t)(hdrlen >> 8);
    memcpy(b + off + hdrlen, pkt, n);
    size_t rec = ((size_t)hdrlen + n + 3) & ~(size_t)3;
    memset(b + off + hdrlen + n, 0xEE, rec - hdrlen - n);
    return off + rec;
}

static void test_bpf(void) {
    uint8_t buf[4096], p1[60], p2[61], p3[1514];
    memset(p1, 1, sizeof p1); memset(p2, 2, sizeof p2); memset(p3, 3, sizeof p3);
    size_t len = 0;
    len = put_rec(buf, len, p1, sizeof p1, 18);
    len = put_rec(buf, len, p2, sizeof p2, 18);      // 18 + 61 = 79 -> padded to 80
    len = put_rec(buf, len, p3, sizeof p3, 20);
    size_t off = 0, cl = 0;
    const uint8_t *pk = NULL;
    CHECK(bridge_bpf_next(buf, len, &off, &pk, &cl) == 1 && cl == 60 && pk == buf + 18 && pk[0] == 1);
    CHECK(bridge_bpf_next(buf, len, &off, &pk, &cl) == 1 && cl == 61 && pk[60] == 2 && off == 80 + 80);
    CHECK(bridge_bpf_next(buf, len, &off, &pk, &cl) == 1 && cl == 1514 && pk[0] == 3 && pk[1513] == 3);
    CHECK(bridge_bpf_next(buf, len, &off, &pk, &cl) == 0);
    CHECK(bridge_bpf_next(buf, len, &off, &pk, &cl) == 0);
    // the last record unpadded (buffer ends exactly at the packet)
    len = put_rec(buf, 0, p2, sizeof p2, 18) - 1;
    off = 0;
    CHECK(bridge_bpf_next(buf, len, &off, &pk, &cl) == 1 && cl == 61);
    CHECK(bridge_bpf_next(buf, len, &off, &pk, &cl) == 0);
    // malformed: caplen beyond the buffer, header length too small, truncated header
    len = put_rec(buf, 0, p1, sizeof p1, 18);
    buf[8] = 200;
    off = 0;
    CHECK(bridge_bpf_next(buf, len, &off, &pk, &cl) == -1);
    len = put_rec(buf, 0, p1, sizeof p1, 18);
    buf[16] = 4;
    off = 0;
    CHECK(bridge_bpf_next(buf, len, &off, &pk, &cl) == -1);
    off = 0;
    CHECK(bridge_bpf_next(buf, 10, &off, &pk, &cl) == -1);
    off = 0;
    CHECK(bridge_bpf_next(buf, 0, &off, &pk, &cl) == 0);
    // fuzz: never reads outside the buffer (run under ASan)
    srand(5);
    for (int it = 0; it < 100000; it++) {
        size_t n = (size_t)(rand() % 300);
        uint8_t *b = malloc(n ? n : 1);
        for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rand();
        if (n > 17 && (rand() & 1)) { b[16] = 18; b[17] = 0; b[8] = (uint8_t)(rand() % 64); b[9] = b[10] = b[11] = 0; }
        size_t o = 0;
        int guard = 0;
        while (bridge_bpf_next(b, n, &o, &pk, &cl) == 1 && guard++ < 100) {
            volatile uint8_t x = cl ? pk[cl - 1] : 0;
            (void)x;
        }
        free(b);
    }
}

static size_t frame(uint8_t *f, const uint8_t *da, const uint8_t *sa, uint16_t type, size_t total) {
    memset(f, 0x5a, total);
    memcpy(f, da, 6);
    memcpy(f + 6, sa, 6);
    f[12] = (uint8_t)(type >> 8);
    f[13] = (uint8_t)type;
    return total;
}

static void test_rules(void) {
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t mcast[6] = {0x33, 0x33, 0, 0, 0, 0xfb};
    static const uint8_t router[6] = {0x02, 0x99, 0x99, 0x99, 0x99, 0x01};
    static const uint8_t other[6] = {0x02, 0x77, 0x77, 0x77, 0x77, 0x02};
    uint8_t f[1600];

    // from macOS
    CHECK(bridge_from_host(f, frame(f, router, kMac, 0x0800, 60), kMac) == BRIDGE_PASS);
    CHECK(bridge_from_host(f, frame(f, bcast, kMac, 0x0806, 42), kMac) == BRIDGE_PASS);
    CHECK(bridge_from_host(f, frame(f, router, kMac, 0x0800, 1514), kMac) == BRIDGE_PASS);
    CHECK(bridge_from_host(f, frame(f, router, kMac, 0x8100, 1518), kMac) == BRIDGE_PASS);
    CHECK(bridge_from_host(f, frame(f, router, kMac, 0x0800, 1519), kMac) == BRIDGE_DROP_LONG);
    CHECK(bridge_from_host(f, 13, kMac) == BRIDGE_DROP_SHORT);
    CHECK(bridge_from_host(f, frame(f, router, other, 0x0800, 60), kMac) == BRIDGE_DROP_SOURCE);
    CHECK(bridge_from_host(f, frame(f, router, kMac, 0x888E, 60), kMac) == BRIDGE_DROP_EAPOL);

    // to macOS
    CHECK(bridge_to_host(f, frame(f, kMac, router, 0x0800, 60), kMac, 1, 1) == BRIDGE_PASS);
    CHECK(bridge_to_host(f, frame(f, bcast, router, 0x0806, 60), kMac, 1, 1) == BRIDGE_PASS);
    CHECK(bridge_to_host(f, frame(f, mcast, other, 0x86DD, 90), kMac, 1, 1) == BRIDGE_PASS);
    CHECK(bridge_to_host(f, frame(f, kMac, router, 0x0800, 60), kMac, 0, 1) == BRIDGE_DROP_PLAIN);
    CHECK(bridge_to_host(f, frame(f, kMac, router, 0x0800, 60), kMac, 0, 0) == BRIDGE_PASS);   // open network
    CHECK(bridge_to_host(f, frame(f, bcast, kMac, 0x0806, 60), kMac, 1, 1) == BRIDGE_DROP_ECHO);
    CHECK(bridge_to_host(f, frame(f, mcast, kMac, 0x86DD, 90), kMac, 1, 1) == BRIDGE_DROP_ECHO);
    CHECK(bridge_to_host(f, frame(f, other, router, 0x0800, 60), kMac, 1, 1) == BRIDGE_DROP_NOT_OURS);
    CHECK(bridge_to_host(f, frame(f, kMac, router, 0x888E, 60), kMac, 0, 0) == BRIDGE_DROP_EAPOL);
    CHECK(bridge_to_host(f, frame(f, kMac, router, 0x0800, 1600), kMac, 1, 1) == BRIDGE_DROP_LONG);
    CHECK(bridge_to_host(f, 10, kMac, 1, 1) == BRIDGE_DROP_SHORT);
    CHECK(!strcmp(bridge_verdict_name(BRIDGE_DROP_ECHO), "own echo") && !strcmp(bridge_verdict_name(BRIDGE_DROP_COUNT), "?"));
}

int main(void) {
    test_bpf();
    test_rules();
    printf("bridge: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
