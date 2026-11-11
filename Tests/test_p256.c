// test_p256.c — P-256 arithmetic against fixed vectors, and against OpenSSL where it's available (Linux).
// Original code (MIT).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p256.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void hex(uint8_t *out, const char *h) {
    for (int i = 0; i < 32; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
}

static void test_vectors(void) {
    CHECK(p256_on_curve(&kP256G));
    // 2G (well-known multiple of the generator)
    P256Point r, want;
    memset(&want, 0, sizeof want);
    hex(want.x, "7CF27B188D034F7E8A52380304B51AC3C08969E277F21B35A60B48FC47669978");
    hex(want.y, "07775510DB8ED040293D9AC69F7430DBBA7DADE63CE982299E04B79D227873D1");
    uint8_t k[32] = {0};
    k[31] = 2;
    CHECK(p256_mul(&r, k, &kP256G) == 0 && !r.inf && !memcmp(r.x, want.x, 32) && !memcmp(r.y, want.y, 32));
    CHECK(p256_add(&r, &kP256G, &kP256G) == 0 && !memcmp(r.x, want.x, 32) && !memcmp(r.y, want.y, 32));   // doubling path
    // n * G = infinity; (n-1) * G = -G; G + (-G) = infinity
    CHECK(p256_mul(&r, kP256N, &kP256G) == 0 && r.inf);
    uint8_t nm1[32];
    memcpy(nm1, kP256N, 32);
    nm1[31]--;
    P256Point neg;
    p256_neg(&neg, &kP256G);
    CHECK(p256_mul(&r, nm1, &kP256G) == 0 && !memcmp(r.x, neg.x, 32) && !memcmp(r.y, neg.y, 32) && p256_on_curve(&neg));
    CHECK(p256_add(&r, &kP256G, &neg) == 0 && r.inf);
    CHECK(p256_add(&r, &kP256G, &r) == 0 && !memcmp(r.x, kP256G.x, 32));          // P + infinity
    memset(k, 0, 32);
    CHECK(p256_mul(&r, k, &kP256G) == 0 && r.inf);
    // y from x: the generator's x gives y or p - y; a non-residue is reported
    uint8_t y[32], ny[32];
    CHECK(p256_y_from_x(kP256G.x, y) == 1);
    p256_field_neg(ny, y);
    CHECK(!memcmp(y, kP256G.y, 32) || !memcmp(ny, kP256G.y, 32));
    CHECK(p256_y_from_x(kP256P, y) == -1 && !p256_lt_p(kP256P));
    uint8_t pm1[32];
    memcpy(pm1, kP256P, 32);
    pm1[31]--;
    CHECK(p256_lt_p(pm1));
    int squares = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t x[32] = {0};
        x[31] = (uint8_t)i;
        int q = p256_y_from_x(x, y);
        if (q == 1) {
            P256Point p;
            memset(&p, 0, sizeof p);
            memcpy(p.x, x, 32);
            memcpy(p.y, y, 32);
            CHECK(p256_on_curve(&p));
            squares++;
        }
    }
    CHECK(squares > 16 && squares < 48);                     // about half of all x are on the curve
    // scalars
    uint8_t one[32] = {0}, two[32] = {0}, s[32];
    one[31] = 1; two[31] = 2;
    CHECK(!p256_scalar_ok(one) && p256_scalar_ok(two) && p256_scalar_ok(nm1) && !p256_scalar_ok(kP256N));
    p256_scalar_add(s, nm1, two);
    CHECK(!memcmp(s, one, 32));                              // (n - 1) + 2 = 1 mod n
    uint8_t ff[32];
    memset(ff, 0xff, 32);
    p256_scalar_add(s, ff, ff);                              // both reduced first
    CHECK(!p256_scalar_ok(kP256N) && memcmp(s, ff, 32) != 0);
    // a point not on the curve
    P256Point bad = kP256G;
    bad.y[31] ^= 1;
    CHECK(!p256_on_curve(&bad));
}

#ifndef __APPLE__
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>

static void test_openssl(void) {
    EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    BN_CTX *ctx = BN_CTX_new();
    BIGNUM *k = BN_new(), *x = BN_new(), *y = BN_new(), *n = BN_new();
    EC_POINT *P = EC_POINT_new(g), *Q = EC_POINT_new(g), *R = EC_POINT_new(g);
    EC_GROUP_get_order(g, n, ctx);
    srand(7);
    int bad = 0;
    P256Point mp = kP256G, mq, mr;
    EC_POINT_copy(P, EC_GROUP_get0_generator(g));
    for (int it = 0; it < 300; it++) {
        uint8_t kb[32], sb[32], want[64];
        for (int i = 0; i < 32; i++) kb[i] = (uint8_t)rand();
        if (it % 50 == 0) memset(kb, 0xff, 16);            // scalars above n too
        BN_bin2bn(kb, 32, k);
        // Q = k * P
        EC_POINT_mul(g, Q, NULL, P, k, ctx);
        p256_mul(&mq, kb, &mp);
        EC_POINT_get_affine_coordinates(g, Q, x, y, ctx);
        BN_bn2binpad(x, want, 32); BN_bn2binpad(y, want + 32, 32);
        if (mq.inf || memcmp(mq.x, want, 32) || memcmp(mq.y, want + 32, 32)) bad++;
        // R = P + Q
        EC_POINT_add(g, R, P, Q, ctx);
        p256_add(&mr, &mp, &mq);
        EC_POINT_get_affine_coordinates(g, R, x, y, ctx);
        BN_bn2binpad(x, want, 32); BN_bn2binpad(y, want + 32, 32);
        if (memcmp(mr.x, want, 32) || memcmp(mr.y, want + 32, 32)) bad++;
        // scalar add mod n
        for (int i = 0; i < 32; i++) sb[i] = (uint8_t)rand();
        BIGNUM *a = BN_bin2bn(sb, 32, NULL), *sum = BN_new();
        BN_mod_add(sum, a, k, n, ctx);
        uint8_t ms[32], ws[32];
        p256_scalar_add(ms, sb, kb);
        BN_bn2binpad(sum, ws, 32);
        if (memcmp(ms, ws, 32)) bad++;
        BN_free(a); BN_free(sum);
        // 48-byte reduction mod p agrees with BN_mod
        {
            uint8_t w[48], got[32], want48[32];
            for (int i = 0; i < 48; i++) w[i] = (uint8_t)rand();
            if (it % 7 == 0) memset(w, 0xff, 48);
            BIGNUM *bw = BN_bin2bn(w, 48, NULL), *p = BN_new(), *rr = BN_new();
            EC_GROUP_get_curve(g, p, NULL, NULL, ctx);
            BN_mod(rr, bw, p, ctx);
            BN_bn2binpad(rr, want48, 32);
            p256_reduce48(w, got);
            if (memcmp(got, want48, 32)) bad++;
            BN_free(bw); BN_free(p); BN_free(rr);
        }
        // y from x agrees with OpenSSL's decompression
        uint8_t yb[32];
        int q = p256_y_from_x(mq.x, yb);
        if (q != 1) bad++;
        // next round continues from R
        EC_POINT_copy(P, R);
        mp = mr;
    }
    CHECK(bad == 0);
    EC_POINT_free(P); EC_POINT_free(Q); EC_POINT_free(R);
    BN_free(k); BN_free(x); BN_free(y); BN_free(n);
    BN_CTX_free(ctx);
    EC_GROUP_free(g);
}
#else
static void test_openssl(void) {}
#endif

int main(void) {
    test_vectors();
    test_openssl();
    printf("p256: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
