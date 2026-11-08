// bridge.h — pure logic for joining the adapter to macOS through a feth pair (Phase 4). Original code (MIT).
//
// macOS side: frames macOS sends on the feth interface are read with BPF on its peer; frames for macOS are
// written to the peer with AF_NDRV (the approach ZeroTier's MacEthernetTapAgent documents).

#ifndef BRIDGE_H
#define BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Walks the records of one BPF read buffer. Darwin's struct bpf_hdr: struct timeval32 (8 bytes), u32 bh_caplen at
// 8, u32 bh_datalen at 12, u16 bh_hdrlen at 16; each record (hdrlen + caplen) is padded to 4 bytes
// (BPF_WORDALIGN). Returns 1 with the next packet, 0 at the end, -1 if the buffer is malformed.
#define BRIDGE_BPF_ALIGN 4
int bridge_bpf_next(const uint8_t *buf, size_t len, size_t *off, const uint8_t **pkt, size_t *caplen);

// Largest Ethernet frame passed either way: 1500-byte MTU + 14-byte header + one 802.1Q tag.
#define BRIDGE_MAX_FRAME 1518

typedef enum {
    BRIDGE_PASS = 0,
    BRIDGE_DROP_SHORT,       // shorter than an Ethernet header
    BRIDGE_DROP_LONG,        // larger than BRIDGE_MAX_FRAME
    BRIDGE_DROP_SOURCE,      // from macOS: source address isn't the adapter's
    BRIDGE_DROP_EAPOL,       // EAPOL: handled by our supplicant, never bridged
    BRIDGE_DROP_PLAIN,       // to macOS: arrived unencrypted although keys are installed
    BRIDGE_DROP_NOT_OURS,    // to macOS: unicast for another address
    BRIDGE_DROP_ECHO,        // to macOS: our own broadcast/multicast reflected by the AP (SA = our address)
    BRIDGE_DROP_COUNT
} BridgeVerdict;

const char *bridge_verdict_name(BridgeVerdict v);

// A frame macOS sent on the feth interface, about to go to the air.
BridgeVerdict bridge_from_host(const uint8_t *eth, size_t len, const uint8_t mac[6]);

// A frame from the air (already converted to Ethernet), about to go to macOS. `decrypted` is the RX header's
// decryption status != 0; `keys_installed` says whether the pairwise key is in place.
BridgeVerdict bridge_to_host(const uint8_t *eth, size_t len, const uint8_t mac[6], int decrypted, int keys_installed);

#ifdef __cplusplus
}
#endif
#endif
