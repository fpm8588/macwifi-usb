// test_sae.c — SAE against the IEEE 802.11-2020 Annex J.10 vector and a second instance playing the access point;
// AES-CMAC and KDF-SHA256 vectors. Original code (MIT).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sae.h"
#include "sae_vectors.h"
#include "wpa.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void rng(uint8_t *o, size_t n) { for (size_t i = 0; i < n; i++) o[i] = (uint8_t)rand(); }

static void test_cmac(void) {
    // RFC 4493 examples
    static const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    static const uint8_t msg[40] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
                                    0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
                                    0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11};
    static const uint8_t m0[16] = {0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28, 0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46};
    static const uint8_t m16[16] = {0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44, 0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c};
    static const uint8_t m40[16] = {0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30, 0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27};
    uint8_t mac[16];
    CHECK(wpa_aes_cmac(key, msg, 0, mac) == 0 && !memcmp(mac, m0, 16));
    CHECK(wpa_aes_cmac(key, msg, 16, mac) == 0 && !memcmp(mac, m16, 16));
    CHECK(wpa_aes_cmac(key, msg, 40, mac) == 0 && !memcmp(mac, m40, 16));
}

static void test_vector(void) {
    Sae s;
    uint8_t out[SAE_MAX_FRAME], reply[SAE_MAX_FRAME], frame[6 + sizeof kJ10_peer_commit];
    size_t rl = 0;
    const char *err = NULL;
    int wp = 0;
    CHECK(sae_start_with(&s, kJ10_addr1, kJ10_addr2, (const uint8_t *)"mekmitasdigoat", 14, kJ10_local_rand, kJ10_local_mask) == 0);
    size_t n = sae_build_commit(&s, out, sizeof out);
    CHECK(n == 6 + sizeof kJ10_local_commit && out[0] == 3 && out[2] == 1 && out[4] == 0 &&
          !memcmp(out + 6, kJ10_local_commit, sizeof kJ10_local_commit));
    frame[0] = 3; frame[1] = 0; frame[2] = 1; frame[3] = 0; frame[4] = 0; frame[5] = 0;
    memcpy(frame + 6, kJ10_peer_commit, sizeof kJ10_peer_commit);
    CHECK(sae_rx(&s, frame, sizeof frame, reply, sizeof reply, &rl, &err, &wp) == SAE_EV_SEND && rl == 40);
    CHECK(!memcmp(s.kck, kJ10_kck, 32) && !memcmp(s.pmk, kJ10_pmk, 32) && !memcmp(s.pmkid, kJ10_pmkid, 16));
    CHECK(reply[0] == 3 && reply[2] == 2 && reply[6] == 1 && s.state == SAE_CONFIRMED);
    // the address order doesn't matter for the password element
    Sae t;
    CHECK(sae_start_with(&t, kJ10_addr2, kJ10_addr1, (const uint8_t *)"mekmitasdigoat", 14, kJ10_local_rand, kJ10_local_mask) == 0);
    CHECK(!memcmp(t.pwe.x, s.pwe.x, 32) && !memcmp(t.pwe.y, s.pwe.y, 32));
    sae_wipe(&s);
    CHECK(s.state == SAE_IDLE && s.pmk[0] == 0);
}

// One full exchange between a station and an "access point" instance. Returns 1 if both accepted with equal PMKs.
static int exchange(const char *sta_pw, const char *ap_pw, int token, int *wrong) {
    static const uint8_t sta[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}, ap[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
    Sae s, a;
    uint8_t c1[SAE_MAX_FRAME], c2[SAE_MAX_FRAME], r1[SAE_MAX_FRAME], r2[SAE_MAX_FRAME], tmp[SAE_MAX_FRAME];
    size_t n1, n2, l1 = 0, l2 = 0, tl = 0;
    const char *err = NULL;
    *wrong = 0;
    if (sae_start(&s, sta, ap, (const uint8_t *)sta_pw, strlen(sta_pw), rng)) return 0;
    if (sae_start(&a, ap, sta, (const uint8_t *)ap_pw, strlen(ap_pw), rng)) return 0;
    n1 = sae_build_commit(&s, c1, sizeof c1);
    if (token) {   // AP asks for a token first; the station's second commit must carry it
        uint8_t req[6 + 2 + 20] = {3, 0, 1, 0, 76, 0, 19, 0};
        memset(req + 8, 0xA5, 20);
        if (sae_rx(&s, req, sizeof req, tmp, sizeof tmp, &tl, &err, wrong) != SAE_EV_SEND) return 0;
        if (tl != n1 + 20 || memcmp(tmp + 8, req + 8, 20) || memcmp(tmp + 28, c1 + 8, 96)) return 0;
        n1 = tl - 20;                                        // what the AP parses: the commit without the token
        memmove(tmp + 8, tmp + 28, 96);
        memcpy(c1, tmp, n1);
    }
    n2 = sae_build_commit(&a, c2, sizeof c2);
    if (sae_rx(&a, c1, n1, r2, sizeof r2, &l2, &err, wrong) != SAE_EV_SEND) return 0;     // AP: commit in, confirm out
    if (sae_rx(&s, c2, n2, r1, sizeof r1, &l1, &err, wrong) != SAE_EV_SEND) return 0;     // station likewise
    if (sae_rx(&s, c2, n2, tmp, sizeof tmp, &tl, &err, wrong) != 0) return 0;             // a duplicate commit is ignored
    int ra = sae_rx(&a, r1, l1, tmp, sizeof tmp, &tl, &err, wrong);
    int rs = sae_rx(&s, r2, l2, tmp, sizeof tmp, &tl, &err, wrong);
    if (ra != SAE_EV_DONE || rs != SAE_EV_DONE) return 0;
    return s.state == SAE_ACCEPTED && !memcmp(s.pmk, a.pmk, 32) && !memcmp(s.pmkid, a.pmkid, 16);
}

static void test_exchange(void) {
    int wp = 0;
    srand(21);
    for (int i = 0; i < 5; i++) CHECK(exchange("correct horse battery", "correct horse battery", i & 1, &wp) == 1);
    CHECK(exchange("incorrect horse battery", "correct horse battery", 0, &wp) == 0 && wp == 1);

    // rejects: bad group, invalid element, scalar out of range, reflection, rejection statuses, short frames
    static const uint8_t sta[6] = {2, 1, 1, 1, 1, 1}, ap[6] = {2, 2, 2, 2, 2, 2};
    Sae s;
    uint8_t c[SAE_MAX_FRAME], f[SAE_MAX_FRAME], rep[SAE_MAX_FRAME];
    size_t rl;
    const char *err;
    for (int kind = 0; kind < 7; kind++) {
        Sae a;
        sae_start(&s, sta, ap, (const uint8_t *)"pw-pw-pw", 8, rng);
        sae_start(&a, ap, sta, (const uint8_t *)"pw-pw-pw", 8, rng);
        size_t n = sae_build_commit(&a, f, sizeof f);
        if (kind == 0) f[6] = 20;                              // group
        if (kind == 1) f[8 + 32 + 40] ^= 1;                    // element off the curve
        if (kind == 2) memset(f + 8, 0, 32);                   // scalar 0
        if (kind == 3) n = sae_build_commit(&s, f, sizeof f);  // our own commit reflected
        if (kind == 4) f[4] = 77;                              // unsupported group
        if (kind == 5) n = 50;                                 // truncated
        if (kind == 6) memcpy(f + 8, kP256N, 32);              // scalar = n
        err = NULL;
        CHECK(sae_rx(&s, f, n, rep, sizeof rep, &rl, &err, &wp) == -1 && err && s.state == SAE_FAILED && rl == 0);
        CHECK(sae_build_commit(&s, c, sizeof c) == 0);
    }
    // not SAE, wrong state: ignored
    sae_start(&s, sta, ap, (const uint8_t *)"pw-pw-pw", 8, rng);
    uint8_t open[6] = {0, 0, 2, 0, 0, 0}, conf[40] = {3, 0, 2, 0, 0, 0};
    CHECK(sae_rx(&s, open, 6, rep, sizeof rep, &rl, &err, &wp) == 0);
    CHECK(sae_rx(&s, conf, 40, rep, sizeof rep, &rl, &err, &wp) == 0 && s.state == SAE_COMMITTED);   // confirm before commit
    CHECK(sae_start(&s, sta, ap, (const uint8_t *)"", 0, rng) == -1);
    // fuzz: random frames never crash or get accepted
    srand(5);
    int accepted = 0;
    for (int it = 0; it < 300; it++) {
        if (it % 30 == 0) sae_start(&s, sta, ap, (const uint8_t *)"pw-pw-pw", 8, rng);
        size_t n = (size_t)(rand() % 140);
        for (size_t i = 0; i < n; i++) f[i] = (uint8_t)rand();
        if (n > 6) { f[0] = 3; f[1] = 0; f[2] = (uint8_t)(1 + rand() % 2); f[3] = 0; f[4] = 0; f[5] = 0; f[6] = 19; f[7] = 0; }
        int r = sae_rx(&s, f, n, rep, sizeof rep, &rl, &err, &wp);
        if (r > 0 && (r & SAE_EV_DONE)) accepted++;
    }
    CHECK(accepted == 0);
}

static void test_kdf(void) {
    // PTK derivation for SHA-256 AKMs is symmetric in addresses and nonces, differs from the SHA-1 one, and the
    // KDF's output depends on the requested length (the length is part of the input)
    uint8_t pmk[32], aa[6] = {1, 2, 3, 4, 5, 6}, spa[6] = {9, 8, 7, 6, 5, 4}, an[32], sn[32], p1[48], p2[48], p3[48], k1[32], k2[64];
    memset(pmk, 7, 32); memset(an, 1, 32); memset(sn, 2, 32);
    CHECK(wpa_derive_ptk_sha256(pmk, aa, spa, an, sn, p1) == 0 && wpa_derive_ptk_sha256(pmk, aa, spa, sn, an, p2) == 0);
    CHECK(!memcmp(p1, p2, 48));
    CHECK(wpa_derive_ptk(pmk, aa, spa, an, sn, p3) == 0 && memcmp(p1, p3, 48) != 0);
    CHECK(wpa_kdf_sha256(pmk, 32, "x", aa, 6, k1, 256) == 0 && wpa_kdf_sha256(pmk, 32, "x", aa, 6, k2, 512) == 0 &&
          memcmp(k1, k2, 32) != 0);
}

int main(void) {
    test_cmac();
    test_vector();
    test_exchange();
    test_kdf();
    printf("sae: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
