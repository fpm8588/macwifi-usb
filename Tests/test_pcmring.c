// test_pcmring.c — jitter buffer, rate conversion and the plug-in datagram format. Original code (GPL-2.0).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcmring.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int crossings(const int16_t *s, size_t n, int stride) {
    int c = 0;
    for (size_t i = 1; i < n; i++) if ((s[(i - 1) * stride] < 0) != (s[i * stride] < 0)) c++;
    return c;
}

int main(void) {
    PcmRing r;
    CHECK(pcmring_init(&r, 1000, 1000) == -1);
    CHECK(pcmring_init(&r, 4096, 256) == 0);
    int16_t in[2048 * 2], out[4096 * 2];
    for (int i = 0; i < 2048; i++) { in[i * 2] = (int16_t)i; in[i * 2 + 1] = (int16_t)-i; }
    // Not primed yet: silence
    pcmring_write(&r, in, 100, 44100);
    CHECK(pcmring_read(&r, out, 50, 2, 44100) == 0 && out[0] == 0 && out[99] == 0);
    pcmring_write(&r, in + 200, 200, 44100);                  // frames 100..299
    CHECK(pcmring_level(&r) == 300);
    // Same rate: the samples come out unchanged and in order
    CHECK(pcmring_read(&r, out, 250, 2, 44100) == 250);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 1 && out[3] == -1 && out[2 * 249] == 249);
    // Underrun: 50 left, ask for 80 -> 50 real then silence, and it re-primes
    size_t got = pcmring_read(&r, out, 80, 2, 44100);
    CHECK(got == 49 || got == 50);
    CHECK(r.primed == 0 && r.underruns == 1 && out[2 * 79] == 0);
    // Mono output is the average of the channels (here 0)
    pcmring_clear(&r);
    pcmring_write(&r, in, 1000, 44100);
    CHECK(pcmring_read(&r, out, 10, 1, 44100) == 10 && out[5] == 0);
    // Overrun: the oldest frames go, the level stays at the capacity
    pcmring_clear(&r);
    for (int k = 0; k < 3; k++) pcmring_write(&r, in, 2048, 44100);
    CHECK(pcmring_level(&r) == 4096 && r.overruns == 3 * 2048 - 4096);
    // Rate conversion 44.1k -> 48k: a 1 kHz sine keeps its frequency
    PcmRing q;
    pcmring_init(&q, 48000, 1024);
    int16_t *sine = malloc(44100 * 4);
    for (int i = 0; i < 44100; i++) sine[i * 2] = sine[i * 2 + 1] = (int16_t)(10000 * sin(2 * M_PI * 1000 * i / 44100.0));
    pcmring_write(&q, sine, 44100, 44100);
    int16_t *o48 = malloc(48000 * 4);
    size_t real = pcmring_read(&q, o48, 47000, 2, 48000);
    CHECK(real > 46000);
    int zc = crossings(o48, 47000, 2);                        // 47000 / 48000 s of 1 kHz -> ~1958 crossings
    CHECK(zc > 1950 && zc < 1966);
    // and 48k -> 44.1k
    pcmring_clear(&q);
    for (int i = 0; i < 44100; i++) sine[i * 2] = sine[i * 2 + 1] = (int16_t)(10000 * sin(2 * M_PI * 1000 * i / 48000.0));
    pcmring_write(&q, sine, 44100, 48000);
    real = pcmring_read(&q, o48, 40000, 2, 44100);
    zc = crossings(o48, 40000, 2);                             // 40000 / 44100 s -> ~1814
    CHECK(real > 39000 && zc > 1806 && zc < 1822);
    // A rate change resets
    pcmring_write(&q, sine, 10, 32000);
    CHECK(pcmring_level(&q) == 10 && q.in_rate == 32000);
    free(sine);
    free(o48);
    pcmring_free(&q);
    // Datagrams
    uint8_t dg[sizeof(MwPcmHeader) + 4 * 8];
    MwPcmHeader h = {MW_PCM_MAGIC, 44100, 7, 8, 2};
    memcpy(dg, &h, sizeof h);
    memset(dg + sizeof h, 0x11, 32);
    MwPcmHeader g;
    const int16_t *smp = NULL;
    CHECK(pcm_datagram(dg, sizeof dg, &g, &smp) == 0 && g.frames == 8 && g.seq == 7 && smp == (const int16_t *)(const void *)(dg + sizeof h));
    CHECK(pcm_datagram(dg, sizeof dg - 1, &g, &smp) == -1);
    h.magic = 0x12345678;
    memcpy(dg, &h, sizeof h);
    CHECK(pcm_datagram(dg, sizeof dg, &g, &smp) == -1);
    h.magic = MW_PCM_MAGIC;
    h.channels = 1;
    memcpy(dg, &h, sizeof h);
    CHECK(pcm_datagram(dg, sizeof dg, &g, &smp) == -1);
    pcmring_free(&r);
    printf("pcmring: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
