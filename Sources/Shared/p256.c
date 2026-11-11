// p256.c — see p256.h. Original code (MIT).

#include "p256.h"

#include <pthread.h>
#include <string.h>

__extension__ typedef unsigned __int128 u128;
typedef struct { uint64_t v[4]; } U;      // little-endian limbs

const uint8_t kP256P[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t kP256N[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                            0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51};
static const uint8_t kB[32] = {0x5A, 0xC6, 0x35, 0xD8, 0xAA, 0x3A, 0x93, 0xE7, 0xB3, 0xEB, 0xBD, 0x55, 0x76, 0x98, 0x86, 0xBC,
                               0x65, 0x1D, 0x06, 0xB0, 0xCC, 0x53, 0xB0, 0xF6, 0x3B, 0xCE, 0x3C, 0x3E, 0x27, 0xD2, 0x60, 0x4B};
const P256Point kP256G = {
    {0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47, 0xF8, 0xBC, 0xE6, 0xE5, 0x63, 0xA4, 0x40, 0xF2,
     0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0, 0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96},
    {0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B, 0x8E, 0xE7, 0xEB, 0x4A, 0x7C, 0x0F, 0x9E, 0x16,
     0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE, 0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5},
    0};

static U from_be(const uint8_t b[32]) {
    U r;
    for (int i = 0; i < 4; i++) {
        uint64_t w = 0;
        for (int k = 0; k < 8; k++) w = (w << 8) | b[(3 - i) * 8 + k];
        r.v[i] = w;
    }
    return r;
}
static void to_be(uint8_t b[32], const U *a) {
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 8; k++) b[(3 - i) * 8 + k] = (uint8_t)(a->v[i] >> (56 - 8 * k));
}

// r = a + b, returns the carry out.
static uint64_t add4(U *r, const U *a, const U *b) {
    u128 c = 0;
    for (int i = 0; i < 4; i++) { c += (u128)a->v[i] + b->v[i]; r->v[i] = (uint64_t)c; c >>= 64; }
    return (uint64_t)c;
}
// r = a - b, returns the borrow out (1 if a < b).
static uint64_t sub4(U *r, const U *a, const U *b) {
    uint64_t br = 0;
    for (int i = 0; i < 4; i++) {
        u128 d = (u128)a->v[i] - b->v[i] - br;
        r->v[i] = (uint64_t)d;
        br = (uint64_t)(d >> 64) & 1;
    }
    return br;
}
// r = cond ? a : b, without branching on cond (0 or 1).
static void sel(U *r, uint64_t cond, const U *a, const U *b) {
    uint64_t m = 0 - cond;
    for (int i = 0; i < 4; i++) r->v[i] = (a->v[i] & m) | (b->v[i] & ~m);
}
static int is_zero(const U *a) { return (a->v[0] | a->v[1] | a->v[2] | a->v[3]) == 0; }
static int equal(const U *a, const U *b) {
    return ((a->v[0] ^ b->v[0]) | (a->v[1] ^ b->v[1]) | (a->v[2] ^ b->v[2]) | (a->v[3] ^ b->v[3])) == 0;
}

// A modulus with what Montgomery multiplication needs: n0 = -m^-1 mod 2^64, rr = 2^512 mod m, one = 2^256 mod m.
typedef struct { U m; uint64_t n0; U rr, one; } Mod;
static Mod gP, gN;
static U gBm, gThree;     // curve b and 3 in Montgomery form (mod p)

static void mod_add(U *r, const U *a, const U *b, const Mod *M) {
    U s, t;
    uint64_t c = add4(&s, a, b);
    uint64_t br = sub4(&t, &s, &M->m);
    sel(r, c | (1 - br), &t, &s);          // take s - m if the sum overflowed or s >= m
}
static void mod_sub(U *r, const U *a, const U *b, const Mod *M) {
    U d, t;
    uint64_t br = sub4(&d, a, b);
    add4(&t, &d, &M->m);
    sel(r, br, &t, &d);
}

// Montgomery product a * b / 2^256 mod m (CIOS). Inputs < m.
static void mmul(U *r, const U *a, const U *b, const Mod *M) {
    uint64_t t[6] = {0};
    for (int i = 0; i < 4; i++) {
        u128 c = 0;
        for (int j = 0; j < 4; j++) {
            c += (u128)a->v[j] * b->v[i] + t[j];
            t[j] = (uint64_t)c;
            c >>= 64;
        }
        c += t[4];
        t[4] = (uint64_t)c;
        t[5] = (uint64_t)(c >> 64);
        uint64_t q = t[0] * M->n0;
        c = (u128)q * M->m.v[0] + t[0];
        c >>= 64;
        for (int j = 1; j < 4; j++) {
            c += (u128)q * M->m.v[j] + t[j];
            t[j - 1] = (uint64_t)c;
            c >>= 64;
        }
        c += t[4];
        t[3] = (uint64_t)c;
        t[4] = t[5] + (uint64_t)(c >> 64);
    }
    U s = {{t[0], t[1], t[2], t[3]}}, d;
    uint64_t br = sub4(&d, &s, &M->m);
    sel(r, t[4] | (1 - br), &d, &s);
}

static void mod_setup(Mod *M, const uint8_t be[32]) {
    M->m = from_be(be);
    uint64_t inv = 1;                                      // Newton: inverse of m mod 2^64
    for (int i = 0; i < 6; i++) inv *= 2 - M->m.v[0] * inv;
    M->n0 = 0 - inv;
    U x = {{1, 0, 0, 0}};
    for (int i = 0; i < 512; i++) {
        mod_add(&x, &x, &x, M);
        if (i == 255) M->one = x;
    }
    M->rr = x;
}

static pthread_once_t gOnce = PTHREAD_ONCE_INIT;
static void setup(void) {
    mod_setup(&gP, kP256P);
    mod_setup(&gN, kP256N);
    U b = from_be(kB), three = {{3, 0, 0, 0}};
    mmul(&gBm, &b, &gP.rr, &gP);
    mmul(&gThree, &three, &gP.rr, &gP);
}
static void init(void) { pthread_once(&gOnce, setup); }

static void to_mont(U *r, const U *a, const Mod *M) { mmul(r, a, &M->rr, M); }
static void from_mont(U *r, const U *a, const Mod *M) {
    U one = {{1, 0, 0, 0}};
    mmul(r, a, &one, M);
}

// r = a^e mod m, a and r in Montgomery form; the same 256 squarings and multiplications whatever e is.
static void mpow(U *r, const U *a, const U *e, const Mod *M) {
    U acc = M->one, t;
    for (int i = 255; i >= 0; i--) {
        mmul(&acc, &acc, &acc, M);
        mmul(&t, &acc, a, M);
        sel(&acc, (e->v[i / 64] >> (i % 64)) & 1, &t, &acc);
    }
    *r = acc;
}

// ---------------------------------------------------------------- field helpers (mod p, Montgomery form)
#define FM(r, a, b) mmul(r, a, b, &gP)
#define FA(r, a, b) mod_add(r, a, b, &gP)
#define FS(r, a, b) mod_sub(r, a, b, &gP)

static void finv(U *r, const U *a) {      // a^(p-2)
    U e = gP.m, two = {{2, 0, 0, 0}};
    sub4(&e, &e, &two);
    mpow(r, a, &e, &gP);
}

// t = x^3 - 3x + b (Montgomery form in and out).
static void curve_rhs(U *t, const U *x) {
    U x2, x3, tx;
    FM(&x2, x, x);
    FM(&x3, &x2, x);
    FM(&tx, &gThree, x);
    FS(t, &x3, &tx);
    FA(t, t, &gBm);
}

typedef struct { U X, Y, Z; } J;          // Jacobian, Montgomery form; Z = 0 is infinity

static void j_double(J *r, const J *p) {
    U delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
    FM(&delta, &p->Z, &p->Z);
    FM(&gamma, &p->Y, &p->Y);
    FM(&beta, &p->X, &gamma);
    FS(&t1, &p->X, &delta);
    FA(&t2, &p->X, &delta);
    FM(&alpha, &t1, &t2);
    FA(&t1, &alpha, &alpha);
    FA(&alpha, &t1, &alpha);                // alpha = 3 (X - delta)(X + delta)
    FM(&x3, &alpha, &alpha);
    FA(&t1, &beta, &beta);
    FA(&t1, &t1, &t1);                      // 4 beta
    FA(&t2, &t1, &t1);                      // 8 beta
    FS(&x3, &x3, &t2);
    FA(&z3, &p->Y, &p->Z);
    FM(&z3, &z3, &z3);
    FS(&z3, &z3, &gamma);
    FS(&z3, &z3, &delta);
    FS(&t1, &t1, &x3);
    FM(&y3, &alpha, &t1);
    FM(&t2, &gamma, &gamma);
    FA(&t2, &t2, &t2);
    FA(&t2, &t2, &t2);
    FA(&t2, &t2, &t2);                      // 8 gamma^2
    FS(&y3, &y3, &t2);
    r->X = x3; r->Y = y3; r->Z = z3;
}

static void j_add(J *r, const J *a, const J *b) {
    if (is_zero(&a->Z)) { *r = *b; return; }
    if (is_zero(&b->Z)) { *r = *a; return; }
    U z1z1, z2z2, u1, u2, s1, s2, h, rr, hh, hhh, v, t, x3, y3, z3;
    FM(&z1z1, &a->Z, &a->Z);
    FM(&z2z2, &b->Z, &b->Z);
    FM(&u1, &a->X, &z2z2);
    FM(&u2, &b->X, &z1z1);
    FM(&s1, &a->Y, &b->Z); FM(&s1, &s1, &z2z2);
    FM(&s2, &b->Y, &a->Z); FM(&s2, &s2, &z1z1);
    FS(&h, &u2, &u1);
    FS(&rr, &s2, &s1);
    if (is_zero(&h)) {
        if (is_zero(&rr)) { j_double(r, a); return; }
        memset(r, 0, sizeof *r);            // a = -b
        return;
    }
    FM(&hh, &h, &h);
    FM(&hhh, &hh, &h);
    FM(&v, &u1, &hh);
    FM(&x3, &rr, &rr);
    FS(&x3, &x3, &hhh);
    FS(&x3, &x3, &v);
    FS(&x3, &x3, &v);
    FS(&t, &v, &x3);
    FM(&y3, &rr, &t);
    FM(&t, &s1, &hhh);
    FS(&y3, &y3, &t);
    FM(&z3, &a->Z, &b->Z);
    FM(&z3, &z3, &h);
    r->X = x3; r->Y = y3; r->Z = z3;
}

static void to_j(J *j, const P256Point *p) {
    if (p->inf) { memset(j, 0, sizeof *j); return; }
    U x = from_be(p->x), y = from_be(p->y);
    to_mont(&j->X, &x, &gP);
    to_mont(&j->Y, &y, &gP);
    j->Z = gP.one;
}

static void from_j(P256Point *p, const J *j) {
    memset(p, 0, sizeof *p);
    if (is_zero(&j->Z)) { p->inf = 1; return; }
    U zi, zi2, zi3, x, y;
    finv(&zi, &j->Z);
    FM(&zi2, &zi, &zi);
    FM(&zi3, &zi2, &zi);
    FM(&x, &j->X, &zi2);
    FM(&y, &j->Y, &zi3);
    from_mont(&x, &x, &gP);
    from_mont(&y, &y, &gP);
    to_be(p->x, &x);
    to_be(p->y, &y);
}

static int lt(const U *a, const U *b) {
    U d;
    return (int)sub4(&d, a, b);
}

// ---------------------------------------------------------------- public API

int p256_lt_p(const uint8_t v[32]) {
    init();
    U a = from_be(v);
    return lt(&a, &gP.m);
}

int p256_on_curve(const P256Point *p) {
    init();
    if (p->inf) return 0;
    U x = from_be(p->x), y = from_be(p->y), xm, ym, rhs, y2;
    if (!lt(&x, &gP.m) || !lt(&y, &gP.m)) return 0;
    to_mont(&xm, &x, &gP);
    to_mont(&ym, &y, &gP);
    curve_rhs(&rhs, &xm);
    FM(&y2, &ym, &ym);
    return equal(&rhs, &y2);
}

int p256_mul(P256Point *r, const uint8_t k[32], const P256Point *p) {
    init();
    U e = from_be(k);
    J base, acc, t;
    to_j(&base, p);
    memset(&acc, 0, sizeof acc);
    for (int i = 255; i >= 0; i--) {
        j_double(&acc, &acc);
        j_add(&t, &acc, &base);
        uint64_t bit = (e.v[i / 64] >> (i % 64)) & 1;
        sel(&acc.X, bit, &t.X, &acc.X);
        sel(&acc.Y, bit, &t.Y, &acc.Y);
        sel(&acc.Z, bit, &t.Z, &acc.Z);
    }
    from_j(r, &acc);
    memset(&e, 0, sizeof e);
    memset(&acc, 0, sizeof acc);
    memset(&t, 0, sizeof t);
    return 0;
}

int p256_add(P256Point *r, const P256Point *a, const P256Point *b) {
    init();
    J ja, jb, jr;
    to_j(&ja, a);
    to_j(&jb, b);
    j_add(&jr, &ja, &jb);
    from_j(r, &jr);
    return 0;
}

void p256_field_neg(uint8_t r[32], const uint8_t a[32]) {
    init();
    U x = from_be(a), zero = {{0, 0, 0, 0}}, d;
    mod_sub(&d, &zero, &x, &gP);
    to_be(r, &d);
}

void p256_neg(P256Point *r, const P256Point *a) {
    *r = *a;
    if (!a->inf) p256_field_neg(r->y, a->y);
}

int p256_y_from_x(const uint8_t x[32], uint8_t y[32]) {
    init();
    U xv = from_be(x), xm, t, s, s2, e = gP.m, one = {{1, 0, 0, 0}};
    if (!lt(&xv, &gP.m)) return -1;
    to_mont(&xm, &xv, &gP);
    curve_rhs(&t, &xm);
    add4(&e, &e, &one);                      // (p + 1) / 4: p + 1 doesn't overflow 256 bits for this prime
    for (int i = 0; i < 3; i++) e.v[i] = (e.v[i] >> 2) | (e.v[i + 1] << 62);
    e.v[3] >>= 2;
    mpow(&s, &t, &e, &gP);
    FM(&s2, &s, &s);
    int ok = equal(&s2, &t);
    from_mont(&s, &s, &gP);
    to_be(y, &s);
    return ok;
}

int p256_scalar_ok(const uint8_t s[32]) {
    init();
    U a = from_be(s), one = {{1, 0, 0, 0}};
    return lt(&one, &a) && lt(&a, &gN.m);
}

void p256_scalar_add(uint8_t r[32], const uint8_t a[32], const uint8_t b[32]) {
    init();
    U x = from_be(a), y = from_be(b), t;
    // reduce each input below n first (inputs are < 2^256 < 2n)
    uint64_t br = sub4(&t, &x, &gN.m);
    sel(&x, 1 - br, &t, &x);
    br = sub4(&t, &y, &gN.m);
    sel(&y, 1 - br, &t, &y);
    mod_add(&t, &x, &y, &gN);
    to_be(r, &t);
}

// ---------------------------------------------------------------- hash-to-element helpers

void p256_reduce48(const uint8_t in[48], uint8_t out[32]) {
    init();
    uint8_t hb[32] = {0};
    memcpy(hb + 16, in, 16);                      // high 128 bits
    U hi = from_be(hb), lo = from_be(in + 16), hm, t;
    to_mont(&hm, &hi, &gP);                       // hi * 2^256 mod p (hi < p)
    uint64_t br = sub4(&t, &lo, &gP.m);           // lo < 2^256 < 2p: one subtraction reduces it
    sel(&lo, 1 - br, &t, &lo);
    mod_add(&t, &hm, &lo, &gP);
    to_be(out, &t);
}

void p256_scalar_mod_nm1_plus1(const uint8_t v[32], uint8_t out[32]) {
    init();
    U x = from_be(v), m = gN.m, one = {{1, 0, 0, 0}}, t;
    sub4(&m, &m, &one);                           // n - 1
    uint64_t br = sub4(&t, &x, &m);               // v < 2^256 < 2(n - 1): one subtraction reduces it
    sel(&x, 1 - br, &t, &x);
    add4(&x, &x, &one);
    to_be(out, &x);
}

static U mont_small_neg(uint64_t k) {             // (p - k) in Montgomery form
    U v = gP.m, kk = {{k, 0, 0, 0}}, r;
    sub4(&v, &v, &kk);
    to_mont(&r, &v, &gP);
    return r;
}

int p256_sswu(const uint8_t ub[32], P256Point *out) {
    init();
    U u = from_be(ub);
    if (!lt(&u, &gP.m)) return -1;
    U um, u2, z = mont_small_neg(10), a = mont_small_neg(3), t1, t2, m, t, e, two = {{2, 0, 0, 0}}, one = {{1, 0, 0, 0}};
    to_mont(&um, &u, &gP);
    FM(&u2, &um, &um);
    FM(&t1, &z, &u2);                             // z u^2
    FM(&t2, &t1, &t1);
    FA(&m, &t1, &t2);                             // m = z^2 u^4 + z u^2
    uint64_t m_zero = (uint64_t)is_zero(&m);
    e = gP.m;
    sub4(&e, &e, &two);
    mpow(&t, &m, &e, &gP);                        // t = m^(p-2): 1/m, or 0 when m = 0
    U za, inv, x1a, x1b, nb, x1, x2, gx1, gx2, chk;
    FM(&za, &z, &a);
    finv(&inv, &za);
    FM(&x1a, &gBm, &inv);                         // b / (z a)
    U zero = {{0, 0, 0, 0}}, ainv, tp1;
    FS(&nb, &zero, &gBm);
    finv(&ainv, &a);
    FM(&x1b, &nb, &ainv);
    FA(&tp1, &gP.one, &t);
    FM(&x1b, &x1b, &tp1);                         // (-b/a)(1 + t)
    sel(&x1, m_zero, &x1a, &x1b);
    curve_rhs(&gx1, &x1);                         // x^3 + a x + b with a = -3
    FM(&x2, &t1, &x1);                            // z u^2 x1
    curve_rhs(&gx2, &x2);
    e = gP.m;                                     // (p - 1) / 2
    sub4(&e, &e, &one);
    for (int i = 0; i < 3; i++) e.v[i] = (e.v[i] >> 1) | (e.v[i + 1] << 63);
    e.v[3] >>= 1;
    mpow(&chk, &gx1, &e, &gP);
    uint64_t qr = (uint64_t)(is_zero(&chk) | equal(&chk, &gP.one));
    U v, x, y, yn, ny;
    sel(&v, qr, &gx1, &gx2);
    sel(&x, qr, &x1, &x2);
    e = gP.m;                                     // (p + 1) / 4
    add4(&e, &e, &one);
    for (int i = 0; i < 3; i++) e.v[i] = (e.v[i] >> 2) | (e.v[i + 1] << 62);
    e.v[3] >>= 2;
    mpow(&y, &v, &e, &gP);
    from_mont(&yn, &y, &gP);
    from_mont(&x, &x, &gP);
    mod_sub(&ny, &zero, &yn, &gP);
    sel(&yn, (uint64_t)((u.v[0] & 1) == (yn.v[0] & 1)), &yn, &ny);
    memset(out, 0, sizeof *out);
    to_be(out->x, &x);
    to_be(out->y, &yn);
    return p256_on_curve(out) ? 0 : -1;
}
