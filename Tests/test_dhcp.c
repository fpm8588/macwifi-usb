// test_dhcp.c — DHCPDISCOVER layout and DHCPOFFER parsing. Original code (MIT).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dhcp.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t kMac[6] = {0x02, 0x00, 0x00, 0xaa, 0xbb, 0xcc};

static void test_discover(void) {
    uint8_t f[600];
    size_t n = dhcp_build_discover(f, sizeof f, kMac, 0x11223344);
    CHECK(n == 14 + 20 + 8 + 236 + 14);
    CHECK(dhcp_build_discover(f, n - 1, kMac, 1) == 0);
    n = dhcp_build_discover(f, sizeof f, kMac, 0x11223344);
    CHECK(f[0] == 0xff && f[5] == 0xff && !memcmp(f + 6, kMac, 6) && f[12] == 0x08 && f[13] == 0x00);
    const uint8_t *ip = f + 14;
    CHECK(ip[0] == 0x45 && ip[9] == 17 && ((ip[2] << 8) | ip[3]) == (int)(n - 14));
    CHECK(ipv4_checksum(ip, 20) == 0);                        // a correct header sums to 0xffff
    CHECK(ip[12] == 0 && ip[15] == 0 && ip[16] == 0xff && ip[19] == 0xff);
    const uint8_t *udp = ip + 20;
    CHECK(udp[1] == 68 && udp[3] == 67 && ((udp[4] << 8) | udp[5]) == (int)(n - 34) && udp[6] == 0 && udp[7] == 0);
    const uint8_t *b = udp + 8;
    CHECK(b[0] == 1 && b[1] == 1 && b[2] == 6 && b[4] == 0x11 && b[7] == 0x44 && b[10] == 0x80);
    CHECK(!memcmp(b + 28, kMac, 6) && b[236] == 0x63 && b[239] == 0x63 && b[240] == 53 && b[242] == 1);
    CHECK(f[n - 1] == 255);
    // RFC 1071 example: 00 01 f2 03 f4 f5 f6 f7 sums to ddf2, checksum 220d
    const uint8_t ex[8] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(ipv4_checksum(ex, 8) == 0x220d);
}

static size_t make_offer(uint8_t *f, uint32_t xid, uint8_t type, int truncate_opt) {
    memset(f, 0, 600);
    memset(f, 0xff, 6);
    f[6] = 0x02; f[11] = 0x01;
    f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
    b[0] = 2; b[1] = 1; b[2] = 6;
    b[4] = (uint8_t)(xid >> 24); b[5] = (uint8_t)(xid >> 16); b[6] = (uint8_t)(xid >> 8); b[7] = (uint8_t)xid;
    b[16] = 192; b[17] = 0; b[18] = 2; b[19] = 50;          // yiaddr (TEST-NET-1)
    memcpy(b + 28, kMac, 6);
    uint8_t *o = b + 236;
    *o++ = 0x63; *o++ = 0x82; *o++ = 0x53; *o++ = 0x63;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 0;                                               // pad
    *o++ = 54; *o++ = 4; *o++ = 192; *o++ = 0; *o++ = 2; *o++ = 1;
    *o++ = 1; *o++ = 4; *o++ = 255; *o++ = 255; *o++ = 255; *o++ = 0;
    *o++ = 3; *o++ = 8; *o++ = 192; *o++ = 0; *o++ = 2; *o++ = 1; *o++ = 192; *o++ = 0; *o++ = 2; *o++ = 2;
    *o++ = 51; *o++ = 4; *o++ = 0; *o++ = 0; *o++ = 0x0e; *o++ = 0x10;
    if (truncate_opt) { *o++ = 12; *o++ = 40; }               // claims 40 bytes that aren't there
    else *o++ = 255;
    size_t ip_len = (size_t)(o - ip);
    ip[0] = 0x45; ip[2] = (uint8_t)(ip_len >> 8); ip[3] = (uint8_t)ip_len; ip[9] = 17;
    udp[0] = 0; udp[1] = 67; udp[2] = 0; udp[3] = 68;
    return 14 + ip_len;
}

static void test_offer(void) {
    uint8_t f[600];
    DhcpReply r;
    size_t n = make_offer(f, 0xdeadbeef, 2, 0);
    CHECK(dhcp_parse_reply(f, n, kMac, 0xdeadbeef, &r) == 0);
    CHECK(r.type == 2 && r.yiaddr[0] == 192 && r.yiaddr[3] == 50 && r.server[3] == 1 && r.router[3] == 1 &&
          r.mask[2] == 255 && r.mask[3] == 0 && r.lease_s == 3600);
    CHECK(dhcp_parse_reply(f, n, kMac, 0xdeadbeee, &r) == -1);            // other transaction
    uint8_t other[6] = {0x02, 0, 0, 0, 0, 1};
    CHECK(dhcp_parse_reply(f, n, other, 0xdeadbeef, &r) == -1);           // other client
    CHECK(dhcp_parse_reply(f, n - 30, kMac, 0xdeadbeef, &r) == -1);       // IP length beyond the frame
    n = make_offer(f, 7, 2, 1);
    CHECK(dhcp_parse_reply(f, n, kMac, 7, &r) == -1);                     // option runs past the end
    n = make_offer(f, 7, 5, 0);
    CHECK(dhcp_parse_reply(f, n, kMac, 7, &r) == 0 && r.type == 5);
    f[14 + 20 + 1] = 68;                                                  // not from port 67
    CHECK(dhcp_parse_reply(f, n, kMac, 7, &r) == -1);
    // our own discover is not a reply
    n = dhcp_build_discover(f, sizeof f, kMac, 7);
    CHECK(dhcp_parse_reply(f, n, kMac, 7, &r) == -1);
}

static void fuzz(void) {
    uint8_t base[600], f[600];
    size_t n = make_offer(base, 9, 2, 0);
    DhcpReply r;
    srand(3);
    for (int it = 0; it < 100000; it++) {
        memcpy(f, base, n);
        int k = 1 + rand() % 6;
        for (int j = 0; j < k; j++) f[rand() % n] = (uint8_t)rand();
        size_t len = (size_t)(rand() % (int)(n + 1));
        (void)dhcp_parse_reply(f, len, kMac, 9, &r);
    }
    printf("dhcp fuzz ok\n");
}

int main(void) {
    test_discover();
    test_offer();
    fuzz();
    printf("dhcp: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
