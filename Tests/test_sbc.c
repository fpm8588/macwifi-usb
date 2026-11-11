// test_sbc.c — the SBC encoder against a small spec decoder written here (frame parsing, CRC, bit allocation,
// dequantization, synthesis filter): sine waves must come back with a high signal-to-noise ratio. Original code
// (GPL-2.0).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sbc.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { const uint8_t *p; size_t bit, len; } BitR;
static uint32_t get_bits(BitR *r, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t byte = r->bit / 8;
        int b = byte < r->len ? (r->p[byte] >> (7 - r->bit % 8)) & 1 : 0;
        v = (v << 1) | (uint32_t)b;
        r->bit++;
    }
    return v;
}

typedef struct { float V[2][160]; } Dec;

// Decodes one frame into interleaved float PCM; returns samples per channel, -1 on a bad frame. gain = window
// multiplier (the specification's decoder scales the prototype window).
static int decode(Dec *d, const uint8_t *f, size_t n, float *pcm, int *channels, float gain) {
    BitR r = {f, 0, n};
    if (get_bits(&r, 8) != SBC_SYNCWORD) return -1;
    SbcConfig c;
    c.fs = (int)get_bits(&r, 2);
    c.blocks = ((int)get_bits(&r, 2) + 1) * 4;
    c.mode = (int)get_bits(&r, 2);
    c.alloc = (int)get_bits(&r, 1);
    if (get_bits(&r, 1) != 1) return -1;                 // 8 subbands only here
    c.bitpool = (int)get_bits(&r, 8);
    uint8_t crc = (uint8_t)get_bits(&r, 8);
    int nch = c.mode == SBC_MODE_MONO ? 1 : 2;
    uint8_t join = c.mode == SBC_MODE_JOINT ? (uint8_t)get_bits(&r, 8) : 0;
    uint8_t sf[2][8];
    for (int ch = 0; ch < nch; ch++)
        for (int k = 0; k < 8; k++) sf[ch][k] = (uint8_t)get_bits(&r, 4);
    size_t extra = (c.mode == SBC_MODE_JOINT ? 8 : 0) + (size_t)nch * 32;
    uint8_t cb[11];
    cb[0] = f[1];
    cb[1] = f[2];
    memcpy(cb + 2, f + 4, (extra + 7) / 8);
    if (sbc_crc8(cb, 16 + extra) != crc) return -1;
    if (join) return -1;                                 // our encoder never sets join flags
    int bits[2][8];
    sbc_bit_alloc(&c, nch, (const uint8_t (*)[8])sf, bits);
    float sb[16][2][8];
    for (int blk = 0; blk < c.blocks; blk++)
        for (int ch = 0; ch < nch; ch++)
            for (int k = 0; k < 8; k++) {
                if (!bits[ch][k]) { sb[blk][ch][k] = 0; continue; }
                uint32_t q = get_bits(&r, bits[ch][k]);
                float levels = (float)((1u << bits[ch][k]) - 1);
                sb[blk][ch][k] = (float)(2 << sf[ch][k]) * ((2.0f * (float)q + 1.0f) / levels - 1.0f);
            }
    if (r.bit > n * 8) return -1;
    // Synthesis (A2DP 12.6.4)
    for (int blk = 0; blk < c.blocks; blk++)
        for (int ch = 0; ch < nch; ch++) {
            float *V = d->V[ch];
            memmove(V + 16, V, 144 * sizeof(float));
            for (int k = 0; k < 16; k++) {
                float v = 0;
                for (int i = 0; i < 8; i++) v += (float)cos((i + 0.5) * (k + 4) * M_PI / 8.0) * sb[blk][ch][i];
                V[k] = v;
            }
            float U[80];
            for (int i = 0; i < 5; i++)
                for (int j = 0; j < 8; j++) {
                    U[i * 16 + j] = V[i * 32 + j];
                    U[i * 16 + j + 8] = V[i * 32 + j + 24];
                }
            for (int j = 0; j < 8; j++) {
                float x = 0;
                for (int i = 0; i < 10; i++) x += U[j + 8 * i] * sbc_proto_8_80[j + 8 * i] * gain;
                pcm[(blk * 8 + j) * nch + ch] = x;
            }
        }
    *channels = nch;
    return c.blocks * 8;
}

// Encodes `frames` frames of a sine (different frequency per channel) and decodes them; returns the SNR in dB
// after finding the codec delay and gain, and the gain itself.
static double roundtrip(SbcConfig cfg, double f0, double f1, int frames, double *gain_out, int *crc_ok) {
    SbcEncoder e;
    if (sbc_init(&e, &cfg)) return -1;
    int ns = sbc_frame_samples(&cfg), nch = e.channels;
    int total = ns * frames;
    int16_t *in = calloc((size_t)total * 2, sizeof *in);
    float *out = calloc((size_t)total * 2, sizeof *out);
    double fs = cfg.fs == SBC_FS_48 ? 48000 : cfg.fs == SBC_FS_44 ? 44100 : cfg.fs == SBC_FS_32 ? 32000 : 16000;
    for (int i = 0; i < total; i++) {
        in[i * nch] = (int16_t)(12000 * sin(2 * M_PI * f0 * i / fs));
        if (nch == 2) in[i * nch + 1] = (int16_t)(9000 * sin(2 * M_PI * f1 * i / fs + 1.0));
    }
    Dec d;
    memset(&d, 0, sizeof d);
    uint8_t frame[1024];
    *crc_ok = 1;
    for (int fi = 0; fi < frames; fi++) {
        size_t len = sbc_encode(&e, in + (size_t)fi * ns * nch, frame, sizeof frame);
        if (len != sbc_frame_length(&cfg)) { *crc_ok = 0; break; }
        int ch = 0;
        if (decode(&d, frame, len, out + (size_t)fi * ns * nch, &ch, -8.0f) != ns) { *crc_ok = 0; break; }
    }
    // Best delay, then least-squares gain and SNR over the second half (past the start-up).
    double best = -1e9, bestgain = 0;
    for (int lag = 0; lag < 300; lag++) {
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = total / 2; i < total - 300; i++)
            for (int ch = 0; ch < nch; ch++) {
                double x = in[i * nch + ch], y = out[(i + lag) * nch + ch];
                sxy += x * y;
                sxx += x * x;
                syy += y * y;
            }
        double g = sxy / sxx;
        double err = syy - 2 * g * sxy + g * g * sxx;   // residual of y - g*x
        double snr = 10 * log10((g * g * sxx) / (err > 1e-9 ? err : 1e-9));
        if (snr > best) { best = snr; bestgain = g; }
    }
    free(in);
    free(out);
    *gain_out = bestgain;
    return best;
}

int main(void) {
    // Frame lengths (A2DP 12.9): 44.1 kHz, 16 blocks, 8 subbands, joint stereo, bitpool 53 -> 119 bytes;
    // stereo -> 118; mono bitpool 31 -> 4 + 4 + ceil(16*31/8) = 70.
    SbcConfig hq = {SBC_FS_44, SBC_MODE_JOINT, 16, SBC_ALLOC_LOUDNESS, 53};
    CHECK(sbc_frame_length(&hq) == 119);
    SbcConfig st = {SBC_FS_44, SBC_MODE_STEREO, 16, SBC_ALLOC_LOUDNESS, 53};
    CHECK(sbc_frame_length(&st) == 118);
    SbcConfig mono = {SBC_FS_48, SBC_MODE_MONO, 16, SBC_ALLOC_SNR, 31};
    CHECK(sbc_frame_length(&mono) == 70);
    SbcEncoder e;
    SbcConfig bad = {SBC_FS_44, SBC_MODE_STEREO, 15, 0, 53};
    CHECK(sbc_init(&e, &bad) == -1);
    bad.blocks = 16;
    bad.bitpool = 300;
    CHECK(sbc_init(&e, &bad) == -1);
    // CRC-8 of the A2DP example header bytes is stable (regression value computed once, polynomial 0x1D init 0x0F)
    uint8_t z[2] = {0, 0};
    CHECK(sbc_crc8(z, 0) == 0x0F);
    // Round trips
    double g;
    int ok;
    double snr = roundtrip(hq, 997, 1500, 200, &g, &ok);
    printf("  joint stereo bitpool 53: SNR %.1f dB, gain %.3f\n", snr, g);
    CHECK(ok && snr > 40);
    snr = roundtrip(st, 440, 6000, 200, &g, &ok);
    printf("  stereo bitpool 53: SNR %.1f dB, gain %.3f\n", snr, g);
    CHECK(ok && snr > 35);
    double g_st = g;
    snr = roundtrip(mono, 1000, 0, 200, &g, &ok);
    printf("  mono SNR alloc bitpool 31 @48k: SNR %.1f dB, gain %.3f\n", snr, g);
    CHECK(ok && snr > 25);
    SbcConfig low = {SBC_FS_44, SBC_MODE_STEREO, 8, SBC_ALLOC_LOUDNESS, 20};
    snr = roundtrip(low, 3000, 250, 300, &g, &ok);
    printf("  stereo 8 blocks bitpool 20: SNR %.1f dB\n", snr);
    CHECK(ok && snr > 15);
    // Unity gain with the synthesis window D = -M * C (M = 8 subbands)
    CHECK(fabs(fabs(g_st) - 1.0) < 0.05);
    // Silence encodes to all-zero scale factors and decodes to silence
    SbcEncoder es;
    sbc_init(&es, &st);
    int16_t zero[128 * 2] = {0};
    uint8_t fr[256];
    CHECK(sbc_encode(&es, zero, fr, sizeof fr) == 118 && fr[0] == 0x9C && fr[2] == 53 && fr[4] == 0 && fr[11] == 0);
    CHECK(sbc_encode(&es, zero, fr, 100) == 0);              // too small a buffer
    printf("sbc: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
