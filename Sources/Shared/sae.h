// sae.h — SAE ("Simultaneous Authentication of Equals", the WPA3-Personal sign-in) for a station.
// Original code (MIT), written from IEEE 802.11-2020 §12.4 (group 19 = NIST P-256). The password element comes
// either from "hunting and pecking" (§12.4.4.2.2) or from "hash-to-element" (H2E, §12.4.4.2.3: PT from the SSID
// and password via HKDF and the simplified SWU map, then PWE = val * PT), which newer access points require.
// Checked against the standard's own test vectors (Annex J.10) for both.
//
// SAE proves both sides know the password without sending anything an eavesdropper could test guesses against,
// and yields a fresh PMK for the 4-way handshake. Exchange (802.11 Authentication frames, algorithm 3):
//   station -> AP   Commit  (seq 1): group, [anti-clogging token], scalar, element
//   AP -> station   Commit  (seq 1)                    (or status 76 + token: "send it again with this token")
//   station -> AP   Confirm (seq 2): send-confirm counter, HMAC over both commits
//   AP -> station   Confirm (seq 2)
// Frame buffers here start at the Authentication frame's fixed fields: algorithm (LE16), sequence (LE16),
// status (LE16), then the SAE fields. The 24-byte 802.11 header is the caller's.

#ifndef SAE_H
#define SAE_H

#include <stddef.h>
#include <stdint.h>

#include "p256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAE_AUTH_ALG 3
#define SAE_PMK_LEN 32
#define SAE_MAX_FRAME 512

typedef void (*SaeRng)(uint8_t *out, size_t n);

typedef enum { SAE_IDLE = 0, SAE_COMMITTED, SAE_CONFIRMED, SAE_ACCEPTED, SAE_FAILED } SaeState;

typedef struct {
    SaeState state;
    int h2e;                 // hash-to-element: commits carry status 126, tokens travel in a container element
    uint8_t own[6], peer[6];
    P256Point pwe;
    uint8_t rand[32], scalar[32];
    P256Point element;
    uint8_t peer_scalar[32];
    P256Point peer_element;
    uint8_t kck[32], pmk[SAE_PMK_LEN], pmkid[16];
    uint8_t token[256];
    size_t token_len;
    uint16_t send_confirm;
} Sae;

enum {
    SAE_EV_SEND = 1,     // `reply` holds an Authentication frame body to send
    SAE_EV_DONE = 2,     // authenticated: s->pmk and s->pmkid are valid
};

#define SAE_STATUS_H2E 126                    // WLAN_STATUS_SAE_HASH_TO_ELEMENT
#define SAE_STATUS_TOKEN 76                   // WLAN_STATUS_ANTI_CLOGGING_TOKEN_REQ

// Derives the password element for this pair of addresses and prepares our Commit. `h2e` selects hash-to-element
// (then the SSID is part of the derivation). Returns 0 or -1.
int sae_start(Sae *s, const uint8_t own[6], const uint8_t peer[6], const uint8_t *password, size_t pw_len,
              const uint8_t *ssid, size_t ssid_len, int h2e, SaeRng rng);
// H2E's password-dependent point PT (password identifier optional), and PWE from it for two addresses.
int sae_derive_pt(const uint8_t *ssid, size_t ssid_len, const uint8_t *password, size_t pw_len, const char *identifier,
                  P256Point *pt);
int sae_pwe_from_pt(const P256Point *pt, const uint8_t a[6], const uint8_t b[6], P256Point *pwe);
// Our Commit frame body (also used to retransmit). Returns its length, 0 on error.
size_t sae_build_commit(const Sae *s, uint8_t *out, size_t cap);
// Handles one Authentication frame body from the AP. Returns SAE_EV_* bits (0: ignored), or -1 with *err set;
// *wrong_password is set when the failure means the two sides don't share the password.
int sae_rx(Sae *s, const uint8_t *auth, size_t len, uint8_t *reply, size_t cap, size_t *reply_len,
           const char **err, int *wrong_password);
// Wipes all key material.
void sae_wipe(Sae *s);

// Test hook: like sae_start, but with given rand and mask values (the standard's test vector).
int sae_start_with(Sae *s, const uint8_t own[6], const uint8_t peer[6], const uint8_t *password, size_t pw_len,
                   const uint8_t rand[32], const uint8_t mask[32]);

#ifdef __cplusplus
}
#endif
#endif
