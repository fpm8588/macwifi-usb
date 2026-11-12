// pcmring.h — the audio path from the Core Audio plug-in to the Bluetooth stream: the datagram format the plug-in
// sends to macwifid, and a jitter buffer with sample-rate conversion on the reading side. Original code (GPL-2.0).

#ifndef PCMRING_H
#define PCMRING_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

// Plug-in -> macwifid: UDP datagrams to 127.0.0.1:MW_PCM_PORT, each a header then interleaved signed 16-bit stereo.
#define MW_PCM_PORT   47883
#define MW_PCM_MAGIC  0x4D574131u          // "MWA1"
#define MW_PCM_MAX_FRAMES 2048
typedef struct {
    uint32_t magic;
    uint32_t rate;                         // Hz
    uint32_t seq;                          // datagram counter (gaps = lost datagrams)
    uint16_t frames;
    uint16_t channels;                     // 2
} MwPcmHeader;

// A ring of stereo int16 frames. Writers and the reader may be different threads.
typedef struct {
    pthread_mutex_t mu;
    int16_t *buf;                          // cap * 2 samples
    size_t cap, rd, wr, level;
    uint32_t in_rate;
    int primed;                            // reading starts once `prime` frames are buffered
    size_t prime;
    double pos;                            // fractional read position for rate conversion
    int16_t last[2];                       // the frame before rd (for interpolation)
    unsigned long overruns, underruns;
} PcmRing;

int pcmring_init(PcmRing *r, size_t cap_frames, size_t prime_frames);
void pcmring_free(PcmRing *r);
void pcmring_clear(PcmRing *r);
// Adds frames at `rate`. If the ring is full, the oldest frames are dropped (latency stays bounded).
void pcmring_write(PcmRing *r, const int16_t *stereo, size_t frames, uint32_t rate);
// Reads `frames` frames converted to `out_rate` into stereo (or mono, the average, when channels == 1).
// Returns how many came from real audio (the rest is silence: not primed yet, or an underrun).
size_t pcmring_read(PcmRing *r, int16_t *out, size_t frames, int channels, uint32_t out_rate);
size_t pcmring_level(PcmRing *r);

// Parses one datagram: 0 and the header + sample pointer, or -1.
int pcm_datagram(const uint8_t *d, size_t n, MwPcmHeader *h, const int16_t **samples);

#endif
