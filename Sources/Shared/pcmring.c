// pcmring.c — see pcmring.h. Original code (GPL-2.0).

#include "pcmring.h"

#include <stdlib.h>
#include <string.h>

int pcmring_init(PcmRing *r, size_t cap, size_t prime) {
    memset(r, 0, sizeof *r);
    if (cap < 2 || prime >= cap) return -1;
    r->buf = calloc(cap * 2, sizeof(int16_t));
    if (!r->buf) return -1;
    r->cap = cap;
    r->prime = prime;
    r->pos = 1.0;                               // the first read takes the first frame
    pthread_mutex_init(&r->mu, NULL);
    return 0;
}

void pcmring_free(PcmRing *r) {
    if (!r->buf) return;
    free(r->buf);
    pthread_mutex_destroy(&r->mu);
    memset(r, 0, sizeof *r);
}

void pcmring_clear(PcmRing *r) {
    pthread_mutex_lock(&r->mu);
    r->rd = r->wr = r->level = 0;
    r->primed = 0;
    r->pos = 1.0;
    r->last[0] = r->last[1] = 0;
    pthread_mutex_unlock(&r->mu);
}

size_t pcmring_level(PcmRing *r) {
    pthread_mutex_lock(&r->mu);
    size_t l = r->level;
    pthread_mutex_unlock(&r->mu);
    return l;
}

void pcmring_write(PcmRing *r, const int16_t *s, size_t frames, uint32_t rate) {
    pthread_mutex_lock(&r->mu);
    if (rate && rate != r->in_rate) {           // a new rate: start over
        r->in_rate = rate;
        r->rd = r->wr = r->level = 0;
        r->primed = 0;
        r->pos = 1.0;
    }
    for (size_t i = 0; i < frames; i++) {
        if (r->level == r->cap) {               // full: drop the oldest frame
            r->rd = (r->rd + 1) % r->cap;
            r->level--;
            r->overruns++;
        }
        r->buf[r->wr * 2] = s[i * 2];
        r->buf[r->wr * 2 + 1] = s[i * 2 + 1];
        r->wr = (r->wr + 1) % r->cap;
        r->level++;
    }
    pthread_mutex_unlock(&r->mu);
}

size_t pcmring_read(PcmRing *r, int16_t *out, size_t frames, int channels, uint32_t out_rate) {
    pthread_mutex_lock(&r->mu);
    if (!r->primed && r->level >= r->prime) r->primed = 1;
    double step = (r->in_rate && out_rate) ? (double)r->in_rate / (double)out_rate : 1.0;
    size_t real = 0;
    for (size_t i = 0; i < frames; i++) {
        int16_t f[2] = {0, 0};
        if (r->primed) {
            // Linear interpolation between `last` (position -1) and the frame at rd (position 0): pos in [0, 1).
            while (r->pos >= 1.0 && r->level > 0) {
                r->last[0] = r->buf[r->rd * 2];
                r->last[1] = r->buf[r->rd * 2 + 1];
                r->rd = (r->rd + 1) % r->cap;
                r->level--;
                r->pos -= 1.0;
            }
            if (r->level == 0) {                 // underrun: silence until the buffer fills again
                r->primed = 0;
                r->underruns++;
            } else {
                double t = r->pos;
                for (int c = 0; c < 2; c++) {
                    double a = r->last[c], b = r->buf[r->rd * 2 + c];
                    f[c] = (int16_t)(a + (b - a) * t);
                }
                r->pos += step;
                real++;
            }
        }
        if (channels == 1) out[i] = (int16_t)(((int)f[0] + (int)f[1]) / 2);
        else { out[i * 2] = f[0]; out[i * 2 + 1] = f[1]; }
    }
    pthread_mutex_unlock(&r->mu);
    return real;
}

int pcm_datagram(const uint8_t *d, size_t n, MwPcmHeader *h, const int16_t **samples) {
    if (n < sizeof *h) return -1;
    memcpy(h, d, sizeof *h);
    if (h->magic != MW_PCM_MAGIC || h->channels != 2 || h->frames == 0 || h->frames > MW_PCM_MAX_FRAMES ||
        h->rate < 8000 || h->rate > 192000)
        return -1;
    if (n != sizeof *h + (size_t)h->frames * 4) return -1;
    *samples = (const int16_t *)(const void *)(d + sizeof *h);
    return 0;
}
