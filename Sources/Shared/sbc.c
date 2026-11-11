// sbc.c — see sbc.h. Original code (GPL-2.0).

#include "sbc.h"

#include <math.h>
#include <string.h>

// A2DP specification, Appendix B: prototype filter for 8 subbands (Proto_8_80).
const float sbc_proto_8_80[80] = {
    0.00000000E+00f, 1.56575398E-04f, 3.43256425E-04f, 5.54620202E-04f,
    8.23919506E-04f, 1.13992507E-03f, 1.47640169E-03f, 1.78371725E-03f,
    2.01182542E-03f, 2.10371989E-03f, 1.99454554E-03f, 1.61656283E-03f,
    9.02154502E-04f, -1.78805361E-04f, -1.64973098E-03f, -3.49717454E-03f,
    5.65949473E-03f, 8.02941163E-03f, 1.04584443E-02f, 1.27472335E-02f,
    1.46525263E-02f, 1.59045603E-02f, 1.62208471E-02f, 1.53184106E-02f,
    1.29371806E-02f, 8.85757540E-03f, 2.92408442E-03f, -4.91578024E-03f,
    -1.46404076E-02f, -2.61098752E-02f, -3.90751381E-02f, -5.31873032E-02f,
    6.79989431E-02f, 8.29847578E-02f, 9.75753918E-02f, 1.11196689E-01f,
    1.23264548E-01f, 1.33264415E-01f, 1.40753505E-01f, 1.45389847E-01f,
    1.46955068E-01f, 1.45389847E-01f, 1.40753505E-01f, 1.33264415E-01f,
    1.23264548E-01f, 1.11196689E-01f, 9.75753918E-02f, 8.29847578E-02f,
    -6.79989431E-02f, -5.31873032E-02f, -3.90751381E-02f, -2.61098752E-02f,
    -1.46404076E-02f, -4.91578024E-03f, 2.92408442E-03f, 8.85757540E-03f,
    1.29371806E-02f, 1.53184106E-02f, 1.62208471E-02f, 1.59045603E-02f,
    1.46525263E-02f, 1.27472335E-02f, 1.04584443E-02f, 8.02941163E-03f,
    -5.65949473E-03f, -3.49717454E-03f, -1.64973098E-03f, -1.78805361E-04f,
    9.02154502E-04f, 1.61656283E-03f, 1.99454554E-03f, 2.10371989E-03f,
    2.01182542E-03f, 1.78371725E-03f, 1.47640169E-03f, 1.13992507E-03f,
    8.23919506E-04f, 5.54620202E-04f, 3.43256425E-04f, 1.56575398E-04f,
};

static const int kOffset8[4][8] = {   // loudness offsets for 8 subbands, by sampling frequency (A2DP 12.6.3)
    {-2, 0, 0, 0, 0, 0, 0, 1}, {-3, 0, 0, 0, 0, 0, 1, 2}, {-4, 0, 0, 0, 0, 0, 1, 2}, {-4, 0, 0, 0, 0, 0, 1, 2},
};

static float gAna[8][16];             // analysis matrix cos((k + 0.5)(i - 4) pi / 8)
static int gAnaReady;

static void ana_init(void) {
    if (gAnaReady) return;
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < 16; i++) gAna[k][i] = (float)cos((k + 0.5) * (i - 4) * M_PI / 8.0);
    gAnaReady = 1;
}

static int channels_of(int mode) { return mode == SBC_MODE_MONO ? 1 : 2; }

int sbc_init(SbcEncoder *e, const SbcConfig *cfg) {
    if (cfg->fs < 0 || cfg->fs > 3 || cfg->mode < 0 || cfg->mode > 3 || (cfg->alloc != 0 && cfg->alloc != 1) ||
        (cfg->blocks != 4 && cfg->blocks != 8 && cfg->blocks != 12 && cfg->blocks != 16) || cfg->bitpool < 2)
        return -1;
    int ch = channels_of(cfg->mode);
    // Bitpool limits (A2DP 12.5.2): 16 * subbands for mono/dual (per channel), 32 * subbands for stereo/joint.
    if (cfg->bitpool > (ch == 2 && cfg->mode != SBC_MODE_DUAL ? 32 * 8 : 16 * 8)) return -1;
    memset(e, 0, sizeof *e);
    e->cfg = *cfg;
    e->channels = ch;
    ana_init();
    return 0;
}

int sbc_frame_samples(const SbcConfig *cfg) { return cfg->blocks * 8; }

size_t sbc_frame_length(const SbcConfig *cfg) {
    int ch = channels_of(cfg->mode);
    size_t n = 4 + (4 * 8 * (size_t)ch) / 8;                     // header + scale factors
    size_t audio_bits;
    if (cfg->mode == SBC_MODE_MONO || cfg->mode == SBC_MODE_DUAL)
        audio_bits = (size_t)cfg->blocks * (size_t)ch * (size_t)cfg->bitpool;
    else
        audio_bits = (cfg->mode == SBC_MODE_JOINT ? 8 : 0) + (size_t)cfg->blocks * (size_t)cfg->bitpool;
    return n + (audio_bits + 7) / 8;
}

uint8_t sbc_crc8(const uint8_t *p, size_t bits) {
    uint8_t crc = 0x0F;
    for (size_t i = 0; i < bits; i++) {
        int bit = (p[i / 8] >> (7 - (i % 8))) & 1;
        int top = (crc >> 7) & 1;
        crc = (uint8_t)(crc << 1);
        if (top ^ bit) crc ^= 0x1D;
    }
    return crc;
}

void sbc_bit_alloc(const SbcConfig *cfg, int channels, const uint8_t sf[2][8], int bits[2][8]) {
    int bitneed[2][8];
    const int nsb = 8;
    for (int ch = 0; ch < channels; ch++)
        for (int sb = 0; sb < nsb; sb++) {
            if (cfg->alloc == SBC_ALLOC_SNR) {
                bitneed[ch][sb] = sf[ch][sb];
            } else if (sf[ch][sb] == 0) {
                bitneed[ch][sb] = -5;
            } else {
                int loud = sf[ch][sb] - kOffset8[cfg->fs][sb];
                bitneed[ch][sb] = loud > 0 ? loud / 2 : loud;
            }
        }
    int joint = cfg->mode == SBC_MODE_STEREO || cfg->mode == SBC_MODE_JOINT;
    int groups = joint ? 1 : channels;       // the stereo modes share one bitpool across both channels
    for (int g = 0; g < groups; g++) {
        int c0 = joint ? 0 : g, c1 = joint ? channels : g + 1;
        int max_need = 0;
        for (int ch = c0; ch < c1; ch++)
            for (int sb = 0; sb < nsb; sb++) if (bitneed[ch][sb] > max_need) max_need = bitneed[ch][sb];
        int bitcount = 0, slicecount = 0, bitslice = max_need + 1;
        do {
            bitslice--;
            bitcount += slicecount;
            slicecount = 0;
            for (int ch = c0; ch < c1; ch++)
                for (int sb = 0; sb < nsb; sb++) {
                    if (bitneed[ch][sb] > bitslice + 1 && bitneed[ch][sb] < bitslice + 16) slicecount++;
                    else if (bitneed[ch][sb] == bitslice + 1) slicecount += 2;
                }
        } while (bitcount + slicecount < cfg->bitpool);
        if (bitcount + slicecount == cfg->bitpool) {
            bitcount += slicecount;
            bitslice--;
        }
        for (int ch = c0; ch < c1; ch++)
            for (int sb = 0; sb < nsb; sb++) {
                if (bitneed[ch][sb] < bitslice + 2) bits[ch][sb] = 0;
                else {
                    int b = bitneed[ch][sb] - bitslice;
                    bits[ch][sb] = b > 16 ? 16 : b;
                }
            }
        // What's left goes first to subbands that already have 2..15 bits (or can take 2), then one bit at a time;
        // in the stereo modes the two channels alternate within each subband.
        int ch = c0, sb = 0;
        while (bitcount < cfg->bitpool && sb < nsb) {
            if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
                bits[ch][sb]++;
                bitcount++;
            } else if (bitneed[ch][sb] == bitslice + 1 && cfg->bitpool > bitcount + 1) {
                bits[ch][sb] = 2;
                bitcount += 2;
            }
            if (ch + 1 < c1) ch++;
            else { ch = c0; sb++; }
        }
        ch = c0;
        sb = 0;
        while (bitcount < cfg->bitpool && sb < nsb) {
            if (bits[ch][sb] < 16) {
                bits[ch][sb]++;
                bitcount++;
            }
            if (ch + 1 < c1) ch++;
            else { ch = c0; sb++; }
        }
    }
}

typedef struct { uint8_t *p; size_t cap, bit; int over; } BitW;
static void put_bits(BitW *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        size_t byte = w->bit / 8;
        if (byte >= w->cap) { w->over = 1; return; }
        if ((v >> i) & 1) w->p[byte] |= (uint8_t)(0x80 >> (w->bit % 8));
        w->bit++;
    }
}

size_t sbc_encode(SbcEncoder *e, const int16_t *pcm, uint8_t *out, size_t cap) {
    const SbcConfig *c = &e->cfg;
    const int nch = e->channels, nblk = c->blocks;
    size_t flen = sbc_frame_length(c);
    if (cap < flen) return 0;
    float sb[16][2][8];
    // Analysis filter (A2DP 12.8: shift in 8 samples, window, partial sums, matrixing), per block and channel.
    for (int blk = 0; blk < nblk; blk++)
        for (int ch = 0; ch < nch; ch++) {
            float *X = e->X[ch];
            memmove(X + 8, X, 72 * sizeof(float));
            for (int i = 7; i >= 0; i--) X[i] = pcm[(blk * 8 + (7 - i)) * nch + ch];
            float Y[16];
            for (int i = 0; i < 16; i++) {
                float y = 0;
                for (int k = 0; k < 5; k++) y += sbc_proto_8_80[i + k * 16] * X[i + k * 16];
                Y[i] = y;
            }
            for (int k = 0; k < 8; k++) {
                float s = 0;
                for (int i = 0; i < 16; i++) s += gAna[k][i] * Y[i];
                sb[blk][ch][k] = s;
            }
        }
    // Scale factors: the smallest x with |sample| < 2^(x+1) over the frame.
    uint8_t sf[2][8] = {{0}};
    for (int ch = 0; ch < nch; ch++)
        for (int k = 0; k < 8; k++) {
            float m = 0;
            for (int blk = 0; blk < nblk; blk++) m = fmaxf(m, fabsf(sb[blk][ch][k]));
            int x = 0;
            while (x < 15 && m >= (float)(2 << x)) x++;
            sf[ch][k] = (uint8_t)x;
        }
    int bits[2][8] = {{0}};
    sbc_bit_alloc(c, nch, (const uint8_t (*)[8])sf, bits);

    memset(out, 0, flen);
    BitW w = {out, flen, 0, 0};
    put_bits(&w, SBC_SYNCWORD, 8);
    put_bits(&w, (uint32_t)c->fs, 2);
    put_bits(&w, (uint32_t)(nblk / 4 - 1), 2);
    put_bits(&w, (uint32_t)c->mode, 2);
    put_bits(&w, (uint32_t)c->alloc, 1);
    put_bits(&w, 1, 1);                                      // 8 subbands
    put_bits(&w, (uint32_t)c->bitpool, 8);
    put_bits(&w, 0, 8);                                      // CRC, filled in below
    if (c->mode == SBC_MODE_JOINT) put_bits(&w, 0, 8);      // join flags: none
    for (int ch = 0; ch < nch; ch++)
        for (int k = 0; k < 8; k++) put_bits(&w, sf[ch][k], 4);
    // CRC over bytes 1-2, then everything after the CRC byte up to the end of the scale factors.
    {
        uint8_t crcbuf[2 + 1 + 8];
        size_t extra_bits = (c->mode == SBC_MODE_JOINT ? 8 : 0) + (size_t)nch * 8 * 4;
        crcbuf[0] = out[1];
        crcbuf[1] = out[2];
        memcpy(crcbuf + 2, out + 4, (extra_bits + 7) / 8);
        out[3] = sbc_crc8(crcbuf, 16 + extra_bits);
    }
    for (int blk = 0; blk < nblk; blk++)
        for (int ch = 0; ch < nch; ch++)
            for (int k = 0; k < 8; k++) {
                int b = bits[ch][k];
                if (!b) continue;
                uint32_t levels = (1u << b) - 1;
                float scale = (float)(2 << sf[ch][k]);
                float v = (sb[blk][ch][k] / scale + 1.0f) * (float)levels / 2.0f;
                long q = (long)floorf(v);
                if (q < 0) q = 0;
                if (q > (long)levels) q = (long)levels;
                put_bits(&w, (uint32_t)q, b);
            }
    return w.over ? 0 : flen;
}
