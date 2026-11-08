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

BridgeVerdict bridge_to_host(const uint8_t *eth, size_t len, const uint8_t mac[6], int decrypted, int keys_installed) {
    if (!eth || len < 14) return BRIDGE_DROP_SHORT;
    if (len > BRIDGE_MAX_FRAME) return BRIDGE_DROP_LONG;
    if (is_eapol(eth)) return BRIDGE_DROP_EAPOL;
    if (keys_installed && !decrypted) return BRIDGE_DROP_PLAIN;
    if (memcmp(eth + 6, mac, 6) == 0) return BRIDGE_DROP_ECHO;
    if (!(eth[0] & 1) && memcmp(eth, mac, 6) != 0) return BRIDGE_DROP_NOT_OURS;
    return BRIDGE_PASS;
}
