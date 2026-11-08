// dhcp.h — just enough DHCPv4 (RFC 2131 / 2132) for the Phase 3b connectivity probe: build a DHCPDISCOVER as an
// Ethernet frame and recognise the matching DHCPOFFER. Original code (MIT). No lease is requested: the probe
// never sends DHCPREQUEST, so the server only makes an offer and nothing is assigned.

#ifndef DHCP_H
#define DHCP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Ethernet (broadcast) + IPv4 (0.0.0.0 -> 255.255.255.255, checksummed) + UDP 68 -> 67 (checksum 0, allowed for
// IPv4) + BOOTP request with the broadcast flag, so the offer comes back to the broadcast address.
// Returns the frame length, or 0 if `cap` is too small.
size_t dhcp_build_discover(uint8_t *out, size_t cap, const uint8_t mac[6], uint32_t xid);

typedef struct {
    uint8_t type;            // option 53: 2 OFFER, 5 ACK, 6 NAK, ...
    uint8_t yiaddr[4];       // address offered
    uint8_t server[4];       // option 54 (server identifier), 0 if absent
    uint8_t router[4];       // option 3, first router, 0 if absent
    uint8_t mask[4];         // option 1, 0 if absent
    uint32_t lease_s;        // option 51, 0 if absent
} DhcpReply;

// Parses an Ethernet frame. Returns 0 if it is a DHCP reply (UDP 67 -> 68) for `mac` with transaction id `xid`,
// -1 otherwise.
int dhcp_parse_reply(const uint8_t *eth, size_t len, const uint8_t mac[6], uint32_t xid, DhcpReply *r);

// The IPv4 header checksum (RFC 791) over `len` bytes, as stored in the header (big-endian when written out).
uint16_t ipv4_checksum(const uint8_t *hdr, size_t len);

#ifdef __cplusplus
}
#endif
#endif
