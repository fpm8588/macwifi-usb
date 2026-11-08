// test_rxq.c — the RX transfer queue, including a producer and a consumer thread. Original code (MIT).

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "rxq.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static double now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void test_basic(void) {
    RxQ *q = rxq_new(3, 100);
    uint8_t b[64];
    int err = 0;
    double t0 = now();
    CHECK(rxq_pop(q, b, sizeof b, 120, &err) == 0);          // timeout
    double el = now() - t0;
    CHECK(el >= 0.10 && el < 1.0);
    CHECK(rxq_push(q, (const uint8_t *)"abc", 3) == 0);
    CHECK(rxq_push(q, (const uint8_t *)"defgh", 5) == 0);
    CHECK(rxq_push(q, (const uint8_t *)"ij", 2) == 0);
    CHECK(rxq_push(q, (const uint8_t *)"k", 1) == -1);       // item limit
    CHECK(rxq_pop(q, b, sizeof b, 0, &err) == 3 && !memcmp(b, "abc", 3));
    CHECK(rxq_pop(q, b, 2, 0, &err) == 2 && !memcmp(b, "de", 2));   // truncated
    uint8_t big[99];
    memset(big, 7, sizeof big);
    CHECK(rxq_push(q, big, sizeof big) == -1);               // byte limit: 2 queued + 99 > 100
    RxQStats s;
    rxq_stats(q, &s);
    CHECK(s.pushed == 3 && s.popped == 2 && s.dropped == 2 && s.truncated == 1 && s.depth == 1 && s.max_depth == 3);
    rxq_push_error(q, 0xe00002c0);
    CHECK(rxq_pop(q, b, sizeof b, 0, &err) == 2 && !memcmp(b, "ij", 2));   // data before the error first
    CHECK(rxq_pop(q, b, sizeof b, 0, &err) == -1 && err == (int)0xe00002c0);
    err = 0;
    CHECK(rxq_pop(q, b, sizeof b, 50, &err) == -1 && err == (int)0xe00002c0); // sticky
    rxq_free(q);
}

// Byte limit, both sides of it.
static void test_bytes(void) {
    RxQ *q = rxq_new(10, 100);
    uint8_t a[60] = {0}, b[41] = {0}, c[40] = {0}, out[128];
    int err;
    CHECK(rxq_push(q, a, sizeof a) == 0);
    CHECK(rxq_push(q, b, sizeof b) == -1);                   // 101 > 100
    CHECK(rxq_push(q, c, sizeof c) == 0);                    // exactly 100
    CHECK(rxq_pop(q, out, sizeof out, 0, &err) == 60 && rxq_pop(q, out, sizeof out, 0, &err) == 40);
    CHECK(rxq_push(q, b, sizeof b) == 0);                    // room again
    rxq_free(q);
}

typedef struct { RxQ *q; int n; } Prod;
static void *producer(void *arg) {
    Prod *p = (Prod *)arg;
    uint8_t b[300];
    for (int i = 0; i < p->n; i++) {
        size_t len = 4 + (size_t)(i % 290);
        memcpy(b, &i, 4);
        for (size_t k = 4; k < len; k++) b[k] = (uint8_t)(i + k);
        while (rxq_push(p->q, b, len) != 0) ;                // spin if full (the test wants every item)
    }
    rxq_push_error(p->q, 42);
    return NULL;
}

static void test_threads(void) {
    RxQ *q = rxq_new(64, 1 << 20);
    Prod p = {q, 200000};
    pthread_t t;
    pthread_create(&t, NULL, producer, &p);
    uint8_t b[512];
    int err = 0, next = 0, bad = 0;
    long n;
    while ((n = rxq_pop(q, b, sizeof b, 2000, &err)) > 0) {
        int i;
        memcpy(&i, b, 4);
        size_t len = 4 + (size_t)(i % 290);
        if (i != next || (size_t)n != len) bad++;
        for (size_t k = 4; k < len && !bad; k++) if (b[k] != (uint8_t)(i + k)) bad++;
        next++;
    }
    pthread_join(t, NULL);
    CHECK(n == -1 && err == 42 && next == p.n && bad == 0);   // all, in order, intact
    RxQStats s;
    rxq_stats(q, &s);
    CHECK(s.max_depth <= 64 && s.popped == (unsigned long)p.n);
    rxq_free(q);
}

int main(void) {
    test_basic();
    test_bytes();
    test_threads();
    printf("rxq: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
