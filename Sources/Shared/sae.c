// sae.c — see sae.h. Original code (MIT).

#include "sae.h"

#include <string.h>

#include "wpa.h"

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// Hunting and pecking (§12.4.4.2.2): try counter = 1, 2, ... until the derived value is the x coordinate of a
// point; always run 40 rounds so the time doesn't reveal which round succeeded.
static int derive_pwe(P256Point *pwe, const uint8_t a[6], const uint8_t b[6], const uint8_t *pw, size_t pw_len) {
    uint8_t key[12], msg[256 + 1], seed[32], val[32], y[32];
    uint8_t found_x[32], found_y[32], found_seed_lsb = 0;
    int found = 0;
    if (pw_len == 0 || pw_len > 256) return -1;
    int a_bigger = memcmp(a, b, 6) > 0;
    memcpy(key, a_bigger ? a : b, 6);          // MAX || MIN
    memcpy(key + 6, a_bigger ? b : a, 6);
    memcpy(msg, pw, pw_len);
    for (int counter = 1; counter <= 40 || !found; counter++) {
        if (counter > 255) break;
        msg[pw_len] = (uint8_t)counter;
        if (mw_hmac_sha256(key, 12, msg, pw_len + 1, seed)) return -1;
        if (wpa_kdf_sha256(seed, 32, "SAE Hunting and Pecking", kP256P, 32, val, 256)) return -1;
        int ok = p256_lt_p(val) && p256_y_from_x(val, y) == 1;
        if (ok && !found) {
            memcpy(found_x, val, 32);
            memcpy(found_y, y, 32);
            found_seed_lsb = seed[31] & 1;
            found = 1;
        }
    }
    wipe(msg, sizeof msg);
    wipe(seed, sizeof seed);
    wipe(val, sizeof val);
    if (!found) return -1;
    memset(pwe, 0, sizeof *pwe);
    memcpy(pwe->x, found_x, 32);
    if ((found_y[31] & 1) == found_seed_lsb) memcpy(pwe->y, found_y, 32);
    else p256_field_neg(pwe->y, found_y);
    wipe(found_x, 32);
    wipe(found_y, 32);
    return p256_on_curve(pwe) ? 0 : -1;
}

static int make_commit(Sae *s, const uint8_t rand[32], const uint8_t mask[32]) {
    if (!p256_scalar_ok(rand) || !p256_scalar_ok(mask)) return -1;
    memcpy(s->rand, rand, 32);
    p256_scalar_add(s->scalar, rand, mask);
    if (!p256_scalar_ok(s->scalar)) return -1;              // must be > 1
    P256Point m;
    if (p256_mul(&m, mask, &s->pwe) != 0 || m.inf) return -1;
    p256_neg(&s->element, &m);                              // element = inverse(mask * PWE)
    wipe(&m, sizeof m);
    s->state = SAE_COMMITTED;
    s->send_confirm = 0;
    return 0;
}

int sae_start_with(Sae *s, const uint8_t own[6], const uint8_t peer[6], const uint8_t *password, size_t pw_len,
                   const uint8_t rand[32], const uint8_t mask[32]) {
    memset(s, 0, sizeof *s);
    memcpy(s->own, own, 6);
    memcpy(s->peer, peer, 6);
    if (derive_pwe(&s->pwe, own, peer, password, pw_len) != 0) return -1;
    return make_commit(s, rand, mask);
}

int sae_start(Sae *s, const uint8_t own[6], const uint8_t peer[6], const uint8_t *password, size_t pw_len, SaeRng rng) {
    memset(s, 0, sizeof *s);
    memcpy(s->own, own, 6);
    memcpy(s->peer, peer, 6);
    if (derive_pwe(&s->pwe, own, peer, password, pw_len) != 0) return -1;
    uint8_t rand[32], mask[32];
    int r = -1;
    for (int tries = 0; tries < 100 && r != 0; tries++) {
        rng(rand, 32);
        rng(mask, 32);
        r = make_commit(s, rand, mask);                     // rejects values outside 1 < v < n and a scalar <= 1
    }
    wipe(rand, 32);
    wipe(mask, 32);
    return r;
}

size_t sae_build_commit(const Sae *s, uint8_t *out, size_t cap) {
    size_t n = 6 + 2 + s->token_len + 32 + 64;
    if (s->state == SAE_IDLE || s->state == SAE_FAILED || n > cap) return 0;
    put_le16(out, SAE_AUTH_ALG);
    put_le16(out + 2, 1);
    put_le16(out + 4, 0);
    put_le16(out + 6, 19);
    size_t o = 8;
    if (s->token_len) { memcpy(out + o, s->token, s->token_len); o += s->token_len; }
    memcpy(out + o, s->scalar, 32); o += 32;
    memcpy(out + o, s->element.x, 32); o += 32;
    memcpy(out + o, s->element.y, 32); o += 32;
    return o;
}

// confirm = HMAC-SHA256(KCK, send-confirm || scalar A || element A || scalar B || element B)
static int confirm_value(const uint8_t kck[32], uint16_t sc, const uint8_t sa[32], const P256Point *ea,
                         const uint8_t sb[32], const P256Point *eb, uint8_t out[32]) {
    uint8_t m[2 + 96 + 96];
    put_le16(m, sc);
    memcpy(m + 2, sa, 32); memcpy(m + 34, ea->x, 32); memcpy(m + 66, ea->y, 32);
    memcpy(m + 98, sb, 32); memcpy(m + 130, eb->x, 32); memcpy(m + 162, eb->y, 32);
    return mw_hmac_sha256(kck, 32, m, sizeof m, out);
}

static int process_commit(Sae *s, const uint8_t *body, size_t n, const char **err) {
    if (n < 2 + 96) { *err = "the access point's SAE commit is too short"; return -1; }
    if (le16(body) != 19) { *err = "the access point answered with a different SAE group"; return -1; }
    const uint8_t *ps = body + 2;
    P256Point pe;
    memset(&pe, 0, sizeof pe);
    memcpy(pe.x, ps + 32, 32);
    memcpy(pe.y, ps + 64, 32);
    if (!p256_scalar_ok(ps) || !p256_on_curve(&pe)) { *err = "the access point's SAE commit is invalid"; return -1; }
    if (!memcmp(ps, s->scalar, 32) && !memcmp(pe.x, s->element.x, 32) && !memcmp(pe.y, s->element.y, 32)) {
        *err = "SAE reflection attack (our own commit came back)";
        return -1;
    }
    // K = rand * (peer-scalar * PWE + peer-element); k = K.x
    P256Point t, k;
    p256_mul(&t, ps, &s->pwe);
    p256_add(&t, &t, &pe);
    p256_mul(&k, s->rand, &t);
    if (k.inf) { *err = "SAE produced the point at infinity"; return -1; }
    uint8_t zero[32] = {0}, keyseed[32], sum[32], keys[64];
    int r = mw_hmac_sha256(zero, 32, k.x, 32, keyseed);
    p256_scalar_add(sum, s->scalar, ps);
    if (!r) r = wpa_kdf_sha256(keyseed, 32, "SAE KCK and PMK", sum, 32, keys, 512);
    if (!r) {
        memcpy(s->kck, keys, 32);
        memcpy(s->pmk, keys + 32, 32);
        memcpy(s->pmkid, sum, 16);
        memcpy(s->peer_scalar, ps, 32);
        s->peer_element = pe;
    }
    wipe(&t, sizeof t);
    wipe(&k, sizeof k);
    wipe(keyseed, 32);
    wipe(keys, 64);
    if (r) { *err = "SAE key derivation failed"; return -1; }
    return 0;
}

int sae_rx(Sae *s, const uint8_t *a, size_t len, uint8_t *reply, size_t cap, size_t *reply_len, const char **err,
           int *wrong_password) {
    static const char *dummy;
    int dummy_wp;
    if (!err) err = &dummy;
    if (!wrong_password) wrong_password = &dummy_wp;
    *err = NULL;
    *wrong_password = 0;
    *reply_len = 0;
    if (len < 6 || le16(a) != SAE_AUTH_ALG) return 0;
    uint16_t seq = le16(a + 2), status = le16(a + 4);
    const uint8_t *body = a + 6;
    size_t n = len - 6;

    if (seq == 1 && s->state == SAE_COMMITTED) {
        if (status == 76) {                                  // anti-clogging token required: group, token
            if (n < 2 || le16(body) != 19 || n - 2 == 0 || n - 2 > sizeof s->token) {
                *err = "bad SAE anti-clogging token request";
                s->state = SAE_FAILED;
                return -1;
            }
            s->token_len = n - 2;
            memcpy(s->token, body + 2, s->token_len);
            *reply_len = sae_build_commit(s, reply, cap);
            return *reply_len ? SAE_EV_SEND : -1;
        }
        if (status != 0) {
            *err = status == 77 ? "the access point doesn't accept SAE group 19"
                 : status == 126 ? "the access point insists on SAE hash-to-element (not supported yet)"
                                 : "the access point rejected the SAE commit";
            s->state = SAE_FAILED;
            return -1;
        }
        if (process_commit(s, body, n, err) != 0) { s->state = SAE_FAILED; return -1; }
        if (cap < 6 + 2 + 32) { *err = "reply buffer too small"; return -1; }
        s->send_confirm = 1;
        put_le16(reply, SAE_AUTH_ALG);
        put_le16(reply + 2, 2);
        put_le16(reply + 4, 0);
        put_le16(reply + 6, s->send_confirm);
        if (confirm_value(s->kck, s->send_confirm, s->scalar, &s->element, s->peer_scalar, &s->peer_element, reply + 8)) {
            *err = "SAE confirm failed";
            return -1;
        }
        *reply_len = 6 + 2 + 32;
        s->state = SAE_CONFIRMED;
        return SAE_EV_SEND;
    }

    if (seq == 2 && s->state == SAE_CONFIRMED) {
        if (status != 0) {
            *err = "the access point rejected the SAE confirm (wrong password?)";
            *wrong_password = 1;
            s->state = SAE_FAILED;
            return -1;
        }
        if (n < 2 + 32) { *err = "the access point's SAE confirm is too short"; return -1; }
        uint8_t want[32], diff = 0;
        if (confirm_value(s->kck, le16(body), s->peer_scalar, &s->peer_element, s->scalar, &s->element, want)) return -1;
        for (int i = 0; i < 32; i++) diff |= (uint8_t)(want[i] ^ body[2 + i]);
        if (diff) {
            *err = "the access point's SAE confirm doesn't verify (wrong password?)";
            *wrong_password = 1;
            s->state = SAE_FAILED;
            return -1;
        }
        s->state = SAE_ACCEPTED;
        return SAE_EV_DONE;
    }

    if (status != 0 && (s->state == SAE_COMMITTED || s->state == SAE_CONFIRMED)) {
        *err = "the access point rejected SAE authentication";
        *wrong_password = s->state == SAE_CONFIRMED;
        s->state = SAE_FAILED;
        return -1;
    }
    return 0;                                                // duplicates and out-of-order frames: ignore
}

void sae_wipe(Sae *s) { wipe(s, sizeof *s); }
