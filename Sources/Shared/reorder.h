// reorder.h — receive reordering for A-MPDU block-ack sessions. Original code (MIT).
//
// The chip marks frames from a block-ack session with "need reorder" (hw_rxhdr flags bit 5) and leaves putting
// them back in sequence-number order to the host, as the Linux driver does (CONFIG_RX_REORDER: a 64-frame window
// per traffic ID, 50 ms before a gap is given up on). Out-of-order delivery isn't wrong, but TCP reads it as
// loss and slows down. The procedure is the IEEE 802.11 receive reordering buffer:
//   - a frame older than the window start is passed on at once (late or duplicate MPDUs aren't held back);
//   - the frame at the window start is passed on, and so is every buffered frame that now follows in order;
//   - a frame beyond the window end moves the window so that it is the last slot, releasing what falls out;
//   - anything else waits; a gap older than the timeout is skipped.
// The unit stored per sequence number is opaque (main.c keeps all Ethernet frames of one MPDU in it).

#ifndef REORDER_H
#define REORDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RO_WIN 64
#define RO_TIDS 8

typedef void (*RoDeliver)(void *ctx, const uint8_t *unit, size_t len);

typedef struct {
    uint8_t *data;
    size_t len;
    uint16_t sn;
    double t;          // arrival
} RoSlot;

typedef struct {
    int active;
    uint16_t head;     // next sequence number to pass on
    int count;         // buffered units
    RoSlot slot[RO_WIN];
} RoTid;

typedef struct {
    RoTid tid[RO_TIDS];
    double timeout_s;
    unsigned long in_order, buffered, late, dups, timeouts, moved;
} Reorder;

void ro_init(Reorder *r, double timeout_s);
// Frees every buffered unit without passing it on (a new association starts over).
void ro_reset(Reorder *r);
// One MPDU (tid 0..7, 12-bit sequence number). `unit` is copied if it has to wait. Returns 0, or -1 if a copy
// couldn't be made (then the unit is passed on at once).
int ro_rx(Reorder *r, uint8_t tid, uint16_t sn, const uint8_t *unit, size_t len, double now, RoDeliver cb, void *ctx);
// Skips gaps whose oldest waiting unit is older than the timeout. Call regularly.
void ro_tick(Reorder *r, double now, RoDeliver cb, void *ctx);
// Units waiting in all TIDs.
int ro_pending(const Reorder *r);

#ifdef __cplusplus
}
#endif
#endif
