// reorder.c — see reorder.h. Original code (MIT).

#include "reorder.h"

#include <stdlib.h>
#include <string.h>

#define SN_MASK 0xFFF
static uint16_t sn_add(uint16_t a, int d) { return (uint16_t)((a + d) & SN_MASK); }
static int sn_dist(uint16_t from, uint16_t to) { return (to - from) & SN_MASK; }   // 0..4095

void ro_init(Reorder *r, double timeout_s) {
    memset(r, 0, sizeof *r);
    r->timeout_s = timeout_s;
}

void ro_reset(Reorder *r) {
    for (int t = 0; t < RO_TIDS; t++) {
        for (int i = 0; i < RO_WIN; i++) free(r->tid[t].slot[i].data);
        memset(&r->tid[t], 0, sizeof r->tid[t]);
    }
}

static void pass_slot(RoTid *t, RoSlot *s, RoDeliver cb, void *ctx) {
    uint8_t *d = s->data;
    size_t n = s->len;
    s->data = NULL;
    s->len = 0;
    t->count--;
    cb(ctx, d, n);
    free(d);
}

// Passes on the buffered units that continue from head, advancing head.
static void release_in_order(RoTid *t, RoDeliver cb, void *ctx) {
    for (;;) {
        RoSlot *s = &t->slot[t->head % RO_WIN];
        if (!s->data || s->sn != t->head) break;
        pass_slot(t, s, cb, ctx);
        t->head = sn_add(t->head, 1);
    }
}

// Moves head forward to `new_head`, passing on (in order) everything buffered before it.
static void move_head(RoTid *t, uint16_t new_head, RoDeliver cb, void *ctx) {
    int d = sn_dist(t->head, new_head);
    if (d >= RO_WIN) {   // every slot holds something older than new_head: pass all on in sequence order
        for (int k = 0; k < RO_WIN && t->count; k++) {
            uint16_t sn = sn_add(t->head, k);
            for (int i = 0; i < RO_WIN; i++) {
                RoSlot *s = &t->slot[i];
                if (s->data && sn_dist(t->head, s->sn) < RO_WIN && s->sn == sn) { pass_slot(t, s, cb, ctx); break; }
            }
        }
        for (int i = 0; i < RO_WIN && t->count; i++)          // anything left is stale: pass it on too
            if (t->slot[i].data) pass_slot(t, &t->slot[i], cb, ctx);
    } else {
        for (int k = 0; k < d; k++) {
            RoSlot *s = &t->slot[(t->head + k) % RO_WIN];
            if (s->data && s->sn == sn_add(t->head, k)) pass_slot(t, s, cb, ctx);
        }
    }
    t->head = new_head;
}

int ro_rx(Reorder *r, uint8_t tid, uint16_t sn, const uint8_t *unit, size_t len, double now, RoDeliver cb, void *ctx) {
    if (tid >= RO_TIDS) { cb(ctx, unit, len); return 0; }
    sn &= SN_MASK;
    RoTid *t = &r->tid[tid];
    if (!t->active) { t->active = 1; t->head = sn; }
    int d = sn_dist(t->head, sn);
    if (d >= 2048) {                                       // older than the window: pass on, don't hold back
        r->late++;
        cb(ctx, unit, len);
        return 0;
    }
    if (d >= RO_WIN) {                                     // beyond the window: make it the last slot
        r->moved++;
        move_head(t, sn_add(sn, -(RO_WIN - 1)), cb, ctx);
        d = RO_WIN - 1;
    }
    if (d == 0) {                                          // the one we were waiting for
        r->in_order++;
        cb(ctx, unit, len);
        t->head = sn_add(t->head, 1);
        release_in_order(t, cb, ctx);
        return 0;
    }
    RoSlot *s = &t->slot[sn % RO_WIN];
    if (s->data) {                                         // already have this one
        if (s->sn == sn) { r->dups++; return 0; }
        pass_slot(t, s, cb, ctx);                          // stale leftover (can't happen with a sane head)
    }
    s->data = malloc(len ? len : 1);
    if (!s->data) { cb(ctx, unit, len); return -1; }
    memcpy(s->data, unit, len);
    s->len = len;
    s->sn = sn;
    s->t = now;
    t->count++;
    r->buffered++;
    return 0;
}

void ro_tick(Reorder *r, double now, RoDeliver cb, void *ctx) {
    for (int k = 0; k < RO_TIDS; k++) {
        RoTid *t = &r->tid[k];
        while (t->count) {
            // the nearest buffered unit after head, and the oldest arrival among them
            int best = -1, bd = RO_WIN;
            double oldest = now;
            for (int i = 0; i < RO_WIN; i++) {
                RoSlot *s = &t->slot[i];
                if (!s->data) continue;
                int d = sn_dist(t->head, s->sn);
                if (d < bd) { bd = d; best = i; }
                if (s->t < oldest) oldest = s->t;
            }
            if (best < 0 || now - oldest < r->timeout_s) break;
            r->timeouts++;
            t->head = t->slot[best].sn;                    // give up on the gap
            release_in_order(t, cb, ctx);
        }
    }
}

int ro_pending(const Reorder *r) {
    int n = 0;
    for (int t = 0; t < RO_TIDS; t++) n += r->tid[t].count;
    return n;
}
