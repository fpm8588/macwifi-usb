// rxq.h — a thread-safe FIFO of USB IN transfers. Original code (MIT).
//
// The USB reader (rx_async.c: several bulk-IN reads always in flight, the way the Linux driver keeps ~20 RX
// URBs queued) pushes each completed transfer; the main thread pops them in order. Keeping reads queued means the
// chip never has to wait for the host between transfers, which under load is what keeps its RX buffers from
// overflowing (and the AP from giving up on us with "too many unacknowledged frames").

#ifndef RXQ_H
#define RXQ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RxQ RxQ;

// Limits: at most `max_items` transfers and `max_bytes` bytes queued; beyond that new transfers are dropped
// (and counted) rather than blocking the reader.
RxQ *rxq_new(size_t max_items, size_t max_bytes);
void rxq_free(RxQ *q);

// Copies a completed transfer in. Returns 0, or -1 if it was dropped (queue full or out of memory).
int rxq_push(RxQ *q, const uint8_t *data, size_t n);
// Records a fatal reader error (e.g. the device went away). Pops return it once the data before it is consumed.
void rxq_push_error(RxQ *q, int err);

// Waits up to `timeout_ms` for the next transfer and copies it into `buf` (a transfer larger than `cap` is
// truncated and counted). Returns the byte count (> 0), 0 on timeout, or -1 with *err set after an error.
long rxq_pop(RxQ *q, uint8_t *buf, size_t cap, int timeout_ms, int *err);

typedef struct {
    unsigned long pushed, popped, dropped, truncated;
    size_t depth, max_depth;     // transfers queued now / at most so far
} RxQStats;
void rxq_stats(RxQ *q, RxQStats *s);

#ifdef __cplusplus
}
#endif
#endif
