// eapol.c — see eapol.h. Original code (MIT). Behaviour follows IEEE 802.11-2020 §12.7 and, where the standard
// leaves room, what wpa_supplicant does (EAPOL version 1 in our frames, key length 0 in message 2, replay counter
// committed only after a valid MIC).

#include "eapol.h"

#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

int eapol_parse_key(const uint8_t *p, size_t n, EapolKey *k) {
    if (!p || !k || n < EAPOL_KEY_HDR_LEN) return -1;
    memset(k, 0, sizeof *k);
    k->version = p[0];
    k->type = p[1];
    k->body_len = be16(p + 2);
    if (k->type != 3 || 4u + k->body_len > n || k->body_len < EAPOL_KEY_HDR_LEN - 4) return -1;
    k->desc_type = p[4];
    if (k->desc_type != 2) return -1;   // RSN key descriptor
    k->key_info = be16(p + 5);
    k->key_len = be16(p + 7);
    memcpy(k->replay, p + 9, 8);
    memcpy(k->nonce, p + 17, 32);
    memcpy(k->mic, p + 81, 16);
    k->key_data_len = be16(p + 97);
    if (EAPOL_KEY_HDR_LEN + (size_t)k->key_data_len > 4u + k->body_len) return -1;
    k->key_data = p + EAPOL_KEY_HDR_LEN;
    k->frame_len = 4u + k->body_len;
    return 0;
}

// Builds an EAPOL-Key frame from the supplicant, MIC included when `kck` is given.
static size_t build_key(uint8_t *out, size_t cap, uint16_t key_info, const uint8_t replay[8], const uint8_t *nonce,
                        const uint8_t *key_data, size_t kdl, const uint8_t *kck) {
    size_t len = EAPOL_KEY_HDR_LEN + kdl;
    if (!out || len > cap || kdl > 0xffff) return 0;
    memset(out, 0, len);
    out[0] = 1;                                   // 802.1X-2001, as wpa_supplicant's default eapol_version
    out[1] = 3;                                   // EAPOL-Key
    put_be16(out + 2, (uint16_t)(len - 4));
    out[4] = 2;                                   // RSN descriptor
    put_be16(out + 5, key_info);
    put_be16(out + 7, 0);                         // key length: 0 from the supplicant
    memcpy(out + 9, replay, 8);
    if (nonce) memcpy(out + 17, nonce, 32);
    put_be16(out + 97, (uint16_t)kdl);
    if (kdl) memcpy(out + EAPOL_KEY_HDR_LEN, key_data, kdl);
    if (kck && wpa_eapol_mic(kck, 2, out, len, out + 81) != 0) return 0;
    return len;
}

static int mic_ok(const uint8_t kck[16], const uint8_t *frame, const EapolKey *k) {
    uint8_t tmp[2048], mic[16];
    if (k->frame_len > sizeof tmp) return 0;
    memcpy(tmp, frame, k->frame_len);
    memset(tmp + 81, 0, 16);
    if (wpa_eapol_mic(kck, 2, tmp, k->frame_len, mic) != 0) return 0;
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(mic[i] ^ k->mic[i]);
    return diff == 0;
}

static int replay_newer(const WpaSupplicant *s, const uint8_t r[8]) {
    return !s->have_replay || memcmp(r, s->replay, 8) > 0;   // big-endian counter
}

// Decrypts (if needed) the key data of a message 3 / group message 1 and extracts the GTK, checking the AP's RSN
// element when one is present and the caller gave us the beacon's. Returns 0 or -1.
static int handle_key_data(WpaSupplicant *s, const EapolKey *k, int want_gtk, const char **err) {
    uint8_t plain[1024];
    const uint8_t *d = k->key_data;
    size_t n = k->key_data_len;
    if (k->key_info & EAPOL_KEY_INFO_ENC_KEY_DATA) {
        if (n < 16 || (n % 8) != 0 || n - 8 > sizeof plain) { *err = "encrypted key data has a bad length"; return -1; }
        if (wpa_aes_unwrap(WPA_KEK(s->ptk), d, n, plain) != 0) { *err = "key data failed the AES key wrap check"; return -1; }
        d = plain;
        n -= 8;
    }
    int got_gtk = 0, got_rsn = 0;
    size_t p = 0;
    while (p + 2 <= n) {
        uint8_t id = d[p], len = d[p + 1];
        if (p + 2 + len > n) break;
        const uint8_t *v = d + p + 2;
        if (id == 0x30) {
            got_rsn = 1;
            if (s->ap_rsn_ie && (s->ap_rsn_ie_len != (size_t)len + 2 || memcmp(s->ap_rsn_ie, d + p, s->ap_rsn_ie_len))) {
                *err = "the AP's RSN element in message 3 differs from its beacon (possible downgrade attack)";
                return -1;
            }
        } else if (id == 0xDD) {
            if (len < 4) break;                                     // padding: 0xDD followed by zeros
            if (v[0] == 0x00 && v[1] == 0x0F && v[2] == 0xAC && v[3] == 1) {   // GTK KDE
                if (len < 6 + 16 || (size_t)len - 6 > sizeof s->gtk) { *err = "GTK KDE has a bad length"; return -1; }
                s->gtk_idx = v[4] & 3;
                s->gtk_len = (size_t)len - 6;
                memcpy(s->gtk, v + 6, s->gtk_len);
                got_gtk = 1;
            }
        }
        p += 2 + (size_t)len;
    }
    if (want_gtk && !got_gtk) { *err = "no GTK in the key data"; return -1; }
    (void)got_rsn;
    return 0;
}

int wpa_supp_rx(WpaSupplicant *s, const uint8_t *eapol, size_t len, const uint8_t snonce[32], uint8_t *reply,
                size_t cap, size_t *reply_len, const char **err) {
    static const char *dummy;
    if (!err) err = &dummy;
    *err = NULL;
    if (reply_len) *reply_len = 0;
    EapolKey k;
    if (eapol_parse_key(eapol, len, &k) != 0) return 0;              // not an RSN EAPOL-Key frame: ignore
    if (!(k.key_info & EAPOL_KEY_INFO_ACK) || (k.key_info & EAPOL_KEY_INFO_REQUEST)) return 0;   // not from an AP
    if ((k.key_info & EAPOL_KEY_INFO_VERSION_MASK) != 2) { *err = "unsupported key descriptor version"; return -1; }
    if (!replay_newer(s, k.replay)) { *err = "replayed EAPOL-Key frame"; return -1; }
    int pairwise = (k.key_info & EAPOL_KEY_INFO_PAIRWISE) != 0;

    if (pairwise && !(k.key_info & EAPOL_KEY_INFO_MIC)) {
        // Message 1: ANonce. Derive the PTK and answer with message 2 (SNonce + our RSN element).
        memcpy(s->anonce, k.nonce, 32);
        memcpy(s->snonce, snonce, 32);
        if (wpa_derive_ptk(s->pmk, s->aa, s->spa, s->anonce, s->snonce, s->ptk) != 0) { *err = "PTK derivation failed"; return -1; }
        s->have_ptk = 1;
        size_t n = build_key(reply, cap, 2 | EAPOL_KEY_INFO_PAIRWISE | EAPOL_KEY_INFO_MIC, k.replay, s->snonce,
                             s->rsn_ie, s->rsn_ie_len, WPA_KCK(s->ptk));
        if (!n) { *err = "reply buffer too small"; return -1; }
        *reply_len = n;
        return WPA_EV_SEND;
    }

    if (pairwise) {
        // Message 3: same ANonce, valid MIC, encrypted key data with the GTK; answer with message 4.
        const uint16_t need = EAPOL_KEY_INFO_MIC | EAPOL_KEY_INFO_INSTALL | EAPOL_KEY_INFO_SECURE |
                              EAPOL_KEY_INFO_ENC_KEY_DATA;
        if ((k.key_info & need) != need) { *err = "message 3 is missing the MIC/install/secure/encrypted bits"; return -1; }
        if (!s->have_ptk) { *err = "message 3 before message 1"; return -1; }
        if (memcmp(k.nonce, s->anonce, 32) != 0) { *err = "message 3 ANonce differs from message 1"; return -1; }
        if (!mic_ok(WPA_KCK(s->ptk), eapol, &k)) { *err = "message 3 MIC is wrong (wrong password?)"; return -1; }
        memcpy(s->replay, k.replay, 8);
        s->have_replay = 1;
        if (handle_key_data(s, &k, 1, err) != 0) return -1;
        uint8_t zero[32] = {0};
        size_t n = build_key(reply, cap, 2 | EAPOL_KEY_INFO_PAIRWISE | EAPOL_KEY_INFO_MIC | EAPOL_KEY_INFO_SECURE,
                             k.replay, zero, NULL, 0, WPA_KCK(s->ptk));
        if (!n) { *err = "reply buffer too small"; return -1; }
        *reply_len = n;
        s->ptk_done = 1;
        return WPA_EV_SEND | WPA_EV_INSTALL_PTK | WPA_EV_INSTALL_GTK | WPA_EV_DONE;
    }

    // Group key handshake message 1 (GTK rekey).
    if (!s->ptk_done) { *err = "group key message before the 4-way handshake finished"; return -1; }
    if (!(k.key_info & EAPOL_KEY_INFO_MIC) || !(k.key_info & EAPOL_KEY_INFO_SECURE) ||
        !(k.key_info & EAPOL_KEY_INFO_ENC_KEY_DATA)) {
        *err = "group key message is missing the MIC/secure/encrypted bits";
        return -1;
    }
    if (!mic_ok(WPA_KCK(s->ptk), eapol, &k)) { *err = "group key message MIC is wrong"; return -1; }
    memcpy(s->replay, k.replay, 8);
    s->have_replay = 1;
    if (handle_key_data(s, &k, 1, err) != 0) return -1;
    size_t n = build_key(reply, cap, 2 | EAPOL_KEY_INFO_MIC | EAPOL_KEY_INFO_SECURE, k.replay, NULL, NULL, 0,
                         WPA_KCK(s->ptk));
    if (!n) { *err = "reply buffer too small"; return -1; }
    *reply_len = n;
    return WPA_EV_SEND | WPA_EV_INSTALL_GTK;
}

size_t wpa_build_rsn_ie(uint8_t *out, size_t cap) {
    static const uint8_t ie[22] = {
        0x30, 20,
        0x01, 0x00,                     // version 1
        0x00, 0x0F, 0xAC, 0x04,         // group cipher CCMP
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,   // 1 pairwise cipher: CCMP
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,   // 1 AKM: PSK
        0x00, 0x00,                     // RSN capabilities: no PMF
    };
    if (!out || cap < sizeof ie) return 0;
    memcpy(out, ie, sizeof ie);
    return sizeof ie;
}
