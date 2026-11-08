// eapol.h — WPA2-Personal (PSK, CCMP) supplicant: the 4-way handshake and the group key handshake.
// Original code (MIT), written from IEEE 802.11-2020 §12.7.2 (EAPOL-Key frames) and §12.7.6-7 (handshakes).
// Pure C, no I/O; unit tested in Tests/test_eapol.c against a simulated access point.
//
// EAPOL frame (after the Ethernet header, ethertype 0x888E):
//   [0] 802.1X version  [1] type (3 = EAPOL-Key)  [2..3] body length (BE)
//   [4] descriptor type (2 = RSN)  [5..6] key info (BE)  [7..8] key length  [9..16] replay counter
//   [17..48] key nonce  [49..64] key IV  [65..72] key RSC  [73..80] reserved  [81..96] key MIC
//   [97..98] key data length  [99..] key data

#ifndef MACWIFI_EAPOL_H
#define MACWIFI_EAPOL_H

#include <stddef.h>
#include <stdint.h>

#include "wpa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EAPOL_KEY_INFO_VERSION_MASK 0x0007   // 2 = HMAC-SHA1 MIC + AES key wrap (CCMP)
#define EAPOL_KEY_INFO_PAIRWISE     0x0008
#define EAPOL_KEY_INFO_INSTALL      0x0040
#define EAPOL_KEY_INFO_ACK          0x0080
#define EAPOL_KEY_INFO_MIC          0x0100
#define EAPOL_KEY_INFO_SECURE       0x0200
#define EAPOL_KEY_INFO_ERROR        0x0400
#define EAPOL_KEY_INFO_REQUEST      0x0800
#define EAPOL_KEY_INFO_ENC_KEY_DATA 0x1000
#define EAPOL_KEY_HDR_LEN           99        // 802.1X header (4) + key descriptor up to key data (95)

typedef struct {
    uint8_t version, type;
    uint16_t body_len;
    uint8_t desc_type;
    uint16_t key_info, key_len;
    uint8_t replay[8];
    uint8_t nonce[32];
    uint8_t mic[16];
    uint16_t key_data_len;
    const uint8_t *key_data;   // inside the caller's buffer
    size_t frame_len;          // 4 + body_len (what the MIC covers)
} EapolKey;

// Parses an EAPOL-Key frame (802.1X header onward). Returns 0, or -1 if it isn't a well-formed RSN EAPOL-Key frame.
int eapol_parse_key(const uint8_t *p, size_t n, EapolKey *k);

typedef struct {
    // set up by the caller before the handshake
    uint8_t pmk[WPA_PMK_LEN];
    uint8_t aa[6], spa[6];           // authenticator (AP / BSSID) and supplicant (our) MAC addresses
    uint8_t rsn_ie[64];              // the RSN element we sent in the association request (id + len + body)
    size_t rsn_ie_len;
    const uint8_t *ap_rsn_ie;        // optional: the AP's RSN element from its beacon, to check message 3 against
    size_t ap_rsn_ie_len;
    // state
    uint8_t anonce[32], snonce[32];
    uint8_t ptk[WPA_PTK_LEN];
    int have_ptk, ptk_done;
    uint8_t replay[8];
    int have_replay;
    uint8_t gtk[32];
    size_t gtk_len;
    int gtk_idx;
} WpaSupplicant;

enum {
    WPA_EV_SEND        = 1,   // `reply` holds an EAPOL frame to send to the AP (unencrypted until keys are set)
    WPA_EV_INSTALL_PTK = 2,   // install WPA_TK(s->ptk) as the pairwise key (after sending the reply)
    WPA_EV_INSTALL_GTK = 4,   // install s->gtk (s->gtk_len bytes) as group key s->gtk_idx
    WPA_EV_DONE        = 8,   // 4-way handshake complete: open the control port
};

// Handles one EAPOL frame from the AP. `snonce` is 32 random bytes, used when message 1 arrives. Returns a bitmask
// of WPA_EV_* (0 if the frame was ignored), or -1 with *err set if the frame was rejected (bad MIC, replay,
// nonce mismatch, undecryptable key data, RSN element mismatch).
int wpa_supp_rx(WpaSupplicant *s, const uint8_t *eapol, size_t len, const uint8_t snonce[32], uint8_t *reply,
                size_t cap, size_t *reply_len, const char **err);

// Builds the RSN element a WPA2-PSK station sends: CCMP group and pairwise ciphers, AKM PSK, no PMF.
size_t wpa_build_rsn_ie(uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
