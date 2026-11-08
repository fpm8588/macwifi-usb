// dhcp.c — see dhcp.h. Original code (MIT).

#include "dhcp.h"

#include <string.h>

#define ETH_HDR 14
#define IP_HDR 20
#define UDP_HDR 8
#define BOOTP_FIXED 236   // op .. file, before the magic cookie

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint16_t ipv4_checksum(const uint8_t *hdr, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += rd16(hdr + i);
    if (len & 1) sum += (uint32_t)hdr[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

size_t dhcp_build_discover(uint8_t *out, size_t cap, const uint8_t mac[6], uint32_t xid) {
    static const uint8_t opts[] = {
        0x63, 0x82, 0x53, 0x63,      // magic cookie
        53, 1, 1,                    // DHCPDISCOVER
        55, 4, 1, 3, 6, 51,          // parameter request list: mask, router, DNS, lease time
        255,
    };
    size_t bootp = BOOTP_FIXED + sizeof opts;
    size_t total = ETH_HDR + IP_HDR + UDP_HDR + bootp;
    if (cap < total) return 0;
    memset(out, 0, total);
    memset(out, 0xff, 6);
    memcpy(out + 6, mac, 6);
    be16(out + 12, 0x0800);

    uint8_t *ip = out + ETH_HDR;
    ip[0] = 0x45;                                       // IPv4, 20-byte header
    be16(ip + 2, (uint16_t)(IP_HDR + UDP_HDR + bootp));
    ip[8] = 64;                                         // TTL
    ip[9] = 17;                                         // UDP
    memset(ip + 16, 0xff, 4);                           // dst 255.255.255.255, src 0.0.0.0
    be16(ip + 10, ipv4_checksum(ip, IP_HDR));

    uint8_t *udp = ip + IP_HDR;
    be16(udp, 68);
    be16(udp + 2, 67);
    be16(udp + 4, (uint16_t)(UDP_HDR + bootp));         // checksum 0: not computed

    uint8_t *b = udp + UDP_HDR;
    b[0] = 1;                                           // BOOTREQUEST
    b[1] = 1;                                           // Ethernet
    b[2] = 6;
    be32(b + 4, xid);
    be16(b + 10, 0x8000);                               // broadcast flag
    memcpy(b + 28, mac, 6);                             // chaddr
    memcpy(b + BOOTP_FIXED, opts, sizeof opts);
    return total;
}

int dhcp_parse_reply(const uint8_t *eth, size_t len, const uint8_t mac[6], uint32_t xid, DhcpReply *r) {
    if (!eth || len < ETH_HDR + IP_HDR + UDP_HDR + BOOTP_FIXED + 4 || rd16(eth + 12) != 0x0800) return -1;
    const uint8_t *ip = eth + ETH_HDR;
    if ((ip[0] >> 4) != 4 || ip[9] != 17) return -1;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
    size_t ip_len = rd16(ip + 2);
    if (ihl < IP_HDR || ip_len > len - ETH_HDR || ip_len < ihl + UDP_HDR + BOOTP_FIXED + 4) return -1;
    const uint8_t *udp = ip + ihl;
    if (rd16(udp) != 67 || rd16(udp + 2) != 68) return -1;
    const uint8_t *b = udp + UDP_HDR;
    const uint8_t *end = ip + ip_len;
    if (b[0] != 2 || rd32(b + 4) != xid || memcmp(b + 28, mac, 6) != 0) return -1;
    if (rd32(b + BOOTP_FIXED) != 0x63825363) return -1;
    memset(r, 0, sizeof *r);
    memcpy(r->yiaddr, b + 16, 4);
    const uint8_t *o = b + BOOTP_FIXED + 4;
    while (o < end) {
        uint8_t code = o[0];
        if (code == 255) break;
        if (code == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) return -1;
        uint8_t n = o[1];
        const uint8_t *v = o + 2;
        if (code == 53 && n == 1) r->type = v[0];
        else if (code == 54 && n == 4) memcpy(r->server, v, 4);
        else if (code == 3 && n >= 4) memcpy(r->router, v, 4);
        else if (code == 1 && n == 4) memcpy(r->mask, v, 4);
        else if (code == 51 && n == 4) r->lease_s = rd32(v);
        o += 2 + n;
    }
    return r->type ? 0 : -1;
}
