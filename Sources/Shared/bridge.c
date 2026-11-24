// bridge.c — see bridge.h. Original code (MIT).

#include "bridge.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int bridge_bpf_next(const uint8_t *buf, size_t len, size_t *off, const uint8_t **pkt, size_t *caplen) {
    if (!buf || !off || !pkt || !caplen) return -1;
    size_t o = *off;
    if (o >= len) return 0;
    if (len - o < 18) return -1;
    uint32_t cap = rd32(buf + o + 8);
    uint16_t hdr = (uint16_t)(buf[o + 16] | (buf[o + 17] << 8));
    if (hdr < 18 || hdr > len - o || cap > len - o - hdr) return -1;
    *pkt = buf + o + hdr;
    *caplen = cap;
    size_t rec = ((size_t)hdr + cap + (BRIDGE_BPF_ALIGN - 1)) & ~(size_t)(BRIDGE_BPF_ALIGN - 1);
    *off = o + rec;   // may step past `len` for the last record: the next call returns 0
    return 1;
}

const char *bridge_verdict_name(BridgeVerdict v) {
    static const char *const names[BRIDGE_DROP_COUNT] = {
        "pass", "short", "too long", "foreign source", "EAPOL", "unencrypted", "not for us", "own echo",
        "unsupported type", "malformed",
    };
    return (unsigned)v < BRIDGE_DROP_COUNT ? names[v] : "?";
}

static int is_eapol(const uint8_t *eth) { return eth[12] == 0x88 && eth[13] == 0x8E; }

BridgeVerdict bridge_from_host(const uint8_t *eth, size_t len, const uint8_t mac[6]) {
    if (!eth || len < 14) return BRIDGE_DROP_SHORT;
    if (len > BRIDGE_MAX_FRAME) return BRIDGE_DROP_LONG;
    if (memcmp(eth + 6, mac, 6) != 0) return BRIDGE_DROP_SOURCE;
    if (is_eapol(eth)) return BRIDGE_DROP_EAPOL;
    return BRIDGE_PASS;
}

// Only plain Ethernet II IPv4, ARP and IPv6 go to macOS, and only when the network header agrees with the frame's
// length: whatever the air delivers (open networks pass through all sorts) is checked here, so nothing odd reaches
// the kernel's network stack through the raw socket. A kernel panic seen with the first open network we used it on
// (macOS 27.2, bounds-safety trap) is why this is strict.
static BridgeVerdict shape_check(const uint8_t *eth, size_t len) {
    unsigned type = ((unsigned)eth[12] << 8) | eth[13];
    const uint8_t *ip = eth + 14;
    size_t room = len - 14;
    if (type == 0x0800) {
        if (room < 20 || (ip[0] >> 4) != 4) return BRIDGE_DROP_SHAPE;
        size_t ihl = (size_t)(ip[0] & 15) * 4, tot = ((size_t)ip[2] << 8) | ip[3];
        return (ihl < 20 || ihl > room || tot < ihl || tot > room) ? BRIDGE_DROP_SHAPE : BRIDGE_PASS;
    }
    if (type == 0x0806) return room < 28 ? BRIDGE_DROP_SHAPE : BRIDGE_PASS;
    if (type == 0x86DD) {
        if (room < 40 || (ip[0] >> 4) != 6) return BRIDGE_DROP_SHAPE;
        size_t plen = ((size_t)ip[4] << 8) | ip[5];
        return 40 + plen > room ? BRIDGE_DROP_SHAPE : BRIDGE_PASS;
    }
    return BRIDGE_DROP_TYPE;
}

BridgeVerdict bridge_to_host(const uint8_t *eth, size_t len, const uint8_t mac[6], int decrypted, int keys_installed) {
    if (!eth || len < 14) return BRIDGE_DROP_SHORT;
    if (len > BRIDGE_MAX_FRAME) return BRIDGE_DROP_LONG;
    if (is_eapol(eth)) return BRIDGE_DROP_EAPOL;
    if (keys_installed && !decrypted) return BRIDGE_DROP_PLAIN;
    if (memcmp(eth + 6, mac, 6) == 0) return BRIDGE_DROP_ECHO;
    if (!(eth[0] & 1) && memcmp(eth, mac, 6) != 0) return BRIDGE_DROP_NOT_OURS;
    if (len > BRIDGE_MAX_TO_HOST) return BRIDGE_DROP_LONG;
    return shape_check(eth, len);
}
