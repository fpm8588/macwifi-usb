// test_reorder.c — the A-MPDU receive reordering buffer. Original code (MIT).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reorder.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// Units are the 2-byte sequence number; delivered ones are recorded in order.
static int out[20000], nout;
static void rec(void *ctx, const uint8_t *u, size_t n) {
    (void)ctx;
    if (n == 2) out[nout++] = u[0] | (u[1] << 8);
    else out[nout++] = -1;
}
static void rx(Reorder *r, int tid, int sn, double t) {
    uint8_t u[2] = {(uint8_t)sn, (uint8_t)(sn >> 8)};
    ro_rx(r, (uint8_t)tid, (uint16_t)sn, u, 2, t, rec, NULL);
}
static int seq_is(const int *want, int n) {
    if (nout != n) return 0;
    for (int i = 0; i < n; i++) if (out[i] != want[i]) return 0;
    return 1;
}

static void test_basic(void) {
    Reorder r;
    ro_init(&r, 0.05);
    nout = 0;
    rx(&r, 0, 100, 0); rx(&r, 0, 101, 0);                  // in order: straight through
    CHECK(seq_is((int[]){100, 101}, 2) && ro_pending(&r) == 0);
    rx(&r, 0, 103, 0); rx(&r, 0, 104, 0);                  // 102 missing: wait
    CHECK(nout == 2 && ro_pending(&r) == 2);
    rx(&r, 0, 102, 0);                                     // gap filled: 102 103 104
    CHECK(seq_is((int[]){100, 101, 102, 103, 104}, 5) && ro_pending(&r) == 0);
    rx(&r, 0, 104, 0);                                     // late duplicate: passed on (not held back)
    CHECK(nout == 6 && out[5] == 104 && r.late == 1);
    rx(&r, 0, 107, 0); rx(&r, 0, 107, 0);                  // duplicate of a waiting one: dropped
    CHECK(r.dups == 1 && ro_pending(&r) == 1);
    ro_reset(&r);
    CHECK(ro_pending(&r) == 0);
}

static void test_timeout(void) {
    Reorder r;
    ro_init(&r, 0.05);
    nout = 0;
    rx(&r, 3, 10, 1.000);
    rx(&r, 3, 12, 1.010); rx(&r, 3, 13, 1.020); rx(&r, 3, 15, 1.030);
    ro_tick(&r, 1.050, rec, NULL);                         // oldest waited 40 ms: not yet
    CHECK(nout == 1 && ro_pending(&r) == 3);
    ro_tick(&r, 1.061, rec, NULL);                         // 51 ms: skip 11 -> 12 13; then 15 waited only 31 ms
    CHECK(seq_is((int[]){10, 12, 13}, 3) && ro_pending(&r) == 1 && r.timeouts == 1);
    ro_tick(&r, 1.081, rec, NULL);                         // 15 has waited 51 ms: skip 14
    CHECK(seq_is((int[]){10, 12, 13, 15}, 4) && ro_pending(&r) == 0);
    rx(&r, 3, 16, 1.1);
    CHECK(nout == 5 && out[4] == 16);                      // head moved on correctly
    ro_reset(&r);
}

static void test_window(void) {
    Reorder r;
    ro_init(&r, 0.05);
    nout = 0;
    rx(&r, 1, 0, 0);
    rx(&r, 1, 2, 0); rx(&r, 1, 5, 0);                      // waiting: 2, 5 (head 1)
    rx(&r, 1, 100, 0);                                     // beyond: head -> 37, releases 2 and 5 first
    CHECK(seq_is((int[]){0, 2, 5}, 3) && ro_pending(&r) == 1 && r.moved == 1);
    for (int s = 37; s < 100; s++) rx(&r, 1, s, 0);         // fill the window: all in order, then 100
    CHECK(nout == 3 + 63 + 1 && out[nout - 1] == 100 && ro_pending(&r) == 0);
    // a far jump with a full window of waiting frames: everything released in sequence order
    nout = 0;
    for (int s = 102; s < 165; s++) rx(&r, 1, s, 0);        // 101 missing: 63 waiting
    CHECK(nout == 0 && ro_pending(&r) == 63);
    rx(&r, 1, 1000, 0);
    int ok = nout == 63;
    for (int i = 0; i < nout && ok; i++) ok = out[i] == 102 + i;
    CHECK(ok && ro_pending(&r) == 1);
    ro_reset(&r);
}

static void test_wrap(void) {
    Reorder r;
    ro_init(&r, 0.05);
    nout = 0;
    rx(&r, 0, 4094, 0);
    rx(&r, 0, 1, 0); rx(&r, 0, 0, 0);                      // across the 4095 -> 0 wrap
    CHECK(nout == 1 && ro_pending(&r) == 2);
    rx(&r, 0, 4095, 0);
    CHECK(seq_is((int[]){4094, 4095, 0, 1}, 4) && ro_pending(&r) == 0);
    rx(&r, 0, 4000, 0);                                    // far behind (late): passed on
    CHECK(nout == 5 && r.late == 1);
    // TIDs are independent; out-of-range TIDs pass straight through
    rx(&r, 5, 7, 0); rx(&r, 5, 9, 0);
    CHECK(nout == 6 && ro_pending(&r) == 1);
    rx(&r, 9, 1, 0);
    CHECK(nout == 7);
    ro_reset(&r);
}

// Random arrival orders within the window: every unit comes out exactly once, in order, once the gaps fill.
static void test_random(void) {
    srand(11);
    Reorder r;
    ro_init(&r, 0.05);
    int sn = 3001, total = 1;
    nout = 0;
    rx(&r, 2, 3000, 0);                                    // the window starts at the first frame seen
    for (int round = 0; round < 200; round++) {
        int n = 1 + rand() % 40, perm[40];
        for (int i = 0; i < n; i++) perm[i] = i;
        for (int i = n - 1; i > 0; i--) { int j = rand() % (i + 1); int t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
        for (int i = 0; i < n; i++) rx(&r, 2, (sn + perm[i]) & 0xFFF, 0);
        sn = (sn + n) & 0xFFF;
        total += n;
    }
    int ok = nout == total && ro_pending(&r) == 0;
    for (int i = 0; i < nout && ok; i++) ok = out[i] == ((3000 + i) & 0xFFF);
    CHECK(ok);
    CHECK(r.timeouts == 0 && r.late == 0 && r.dups == 0);
    ro_reset(&r);
}

int main(void) {
    test_basic();
    test_timeout();
    test_window();
    test_wrap();
    test_random();
    printf("reorder: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
