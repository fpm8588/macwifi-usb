// p256.h — NIST P-256 (secp256r1) arithmetic for SAE (WPA3-Personal, group 19). Original code (MIT).
//
// macOS has no public API for raw elliptic-curve point arithmetic (CommonCrypto exposes only ECDH/ECDSA through
// keys), and the project takes no third-party dependencies, so this is a small self-contained implementation:
// 4x64-bit limbs, Montgomery multiplication for both moduli (the field prime p and the group order n), Jacobian
// coordinates, a fixed 256-step double-and-add-always scalar multiplication. Values cross the API as 32-byte
// big-endian strings. Checked in Tests/test_p256.c against OpenSSL (Linux) and fixed vectors.

#ifndef P256_H
#define P256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t x[32], y[32];
    int inf;                 // the point at infinity (x, y unused)
} P256Point;

extern const P256Point kP256G;       // the generator
extern const uint8_t kP256P[32];     // field prime
extern const uint8_t kP256N[32];     // group order

// 1 if the point is on the curve (and not infinity, coordinates < p).
int p256_on_curve(const P256Point *p);
// r = k * p. Returns 0 (r may be infinity).
int p256_mul(P256Point *r, const uint8_t k[32], const P256Point *p);
// r = a + b.
int p256_add(P256Point *r, const P256Point *a, const P256Point *b);
// r = -a.
void p256_neg(P256Point *r, const P256Point *a);
// For x < p: computes t = x^3 - 3x + b; returns 1 and y = sqrt(t) if t is a square, 0 if not; -1 if x >= p.
int p256_y_from_x(const uint8_t x[32], uint8_t y[32]);
// 1 if v < p (as integers).
int p256_lt_p(const uint8_t v[32]);
// Scalars modulo the group order n.
int p256_scalar_ok(const uint8_t s[32]);                       // 1 < s < n
void p256_scalar_add(uint8_t r[32], const uint8_t a[32], const uint8_t b[32]);   // (a + b) mod n, inputs < 2^256
// y := p - y (the other square root).
void p256_field_neg(uint8_t r[32], const uint8_t a[32]);

// For SAE hash-to-element (IEEE 802.11-2020 §12.4.4.2.3):
// a 48-byte big-endian value reduced modulo p;
void p256_reduce48(const uint8_t in[48], uint8_t out[32]);
// the simplified SWU map of u (< p) onto the curve, with z = -10 (as for group 19), the sign of y chosen to match
// the low bit of u;
int p256_sswu(const uint8_t u[32], P256Point *out);
// out = (v mod (n - 1)) + 1, for any 256-bit v.
void p256_scalar_mod_nm1_plus1(const uint8_t v[32], uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
