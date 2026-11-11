// sbc.h — SBC (Low Complexity Subband Coding) encoder for A2DP, 8 subbands, floating point. Original code
// (GPL-2.0) written from the A2DP specification (v1.3, Appendix B / section 12: analysis filter, bit allocation,
// quantization, frame format and CRC). The 80 prototype filter coefficients are the specification's table, as
// also published in BlueZ's sbc.c (LGPL-2.1-or-later, Marcel Holtmann, Henryk Ploetz); see NOTICE.md.

#ifndef SBC_H
#define SBC_H

#include <stddef.h>
#include <stdint.h>

// Header field values (A2DP 12.5)
enum { SBC_FS_16 = 0, SBC_FS_32 = 1, SBC_FS_44 = 2, SBC_FS_48 = 3 };
enum { SBC_MODE_MONO = 0, SBC_MODE_DUAL = 1, SBC_MODE_STEREO = 2, SBC_MODE_JOINT = 3 };
enum { SBC_ALLOC_LOUDNESS = 0, SBC_ALLOC_SNR = 1 };
#define SBC_SYNCWORD 0x9C

typedef struct {
    int fs;           // SBC_FS_*
    int mode;         // SBC_MODE_* (joint stereo is written with every join flag clear: valid, and simple)
    int blocks;       // 4, 8, 12 or 16
    int alloc;        // SBC_ALLOC_*
    int bitpool;      // 2..250 (A2DP sinks usually take up to 53)
} SbcConfig;

typedef struct {
    SbcConfig cfg;
    int channels;
    float X[2][80];   // analysis input history per channel
} SbcEncoder;

int sbc_init(SbcEncoder *e, const SbcConfig *cfg);            // 0, or -1 for an invalid configuration
// Bytes in one frame for this configuration, and PCM samples (per channel) it carries.
size_t sbc_frame_length(const SbcConfig *cfg);
int sbc_frame_samples(const SbcConfig *cfg);                    // blocks * 8
// Encodes one frame from interleaved 16-bit PCM (blocks*8 samples per channel). Returns the frame length, or 0
// if out is too small.
size_t sbc_encode(SbcEncoder *e, const int16_t *pcm, uint8_t *out, size_t cap);

// CRC-8 of the header and scale factors (polynomial x^8+x^4+x^3+x^2+1, initial 0x0F), over `bits` bits from p.
uint8_t sbc_crc8(const uint8_t *p, size_t bits);
// Bit allocation (exposed for tests and the test decoder): bits[ch][sb] from scale factors.
void sbc_bit_alloc(const SbcConfig *cfg, int channels, const uint8_t sf[2][8], int bits[2][8]);

extern const float sbc_proto_8_80[80];

#endif
