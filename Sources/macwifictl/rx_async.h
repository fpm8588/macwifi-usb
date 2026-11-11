// rx_async.h — keeps several bulk-IN reads in flight on a dedicated run-loop thread and feeds completed
// transfers into an RxQ (Shared/rxq.h). Original code (MIT). macOS only (IOKit async USB).

#ifndef RX_ASYNC_H
#define RX_ASYNC_H

#include <stddef.h>
#include <stdint.h>

#include "rxq.h"

typedef struct RxAsync RxAsync;

#define RX_ASYNC_READS   16
#define RX_ASYNC_BUFSIZE 32768

// `intf` is an IOUSBInterfaceInterface300 ** (already opened), `pipe` the bulk-IN pipe ref. Returns NULL if the
// async machinery couldn't be set up (callers then fall back to synchronous reads).
RxAsync *rx_async_start(void *intf, uint8_t pipe, RxQ *q);
// Aborts the outstanding reads and writes and joins the thread.
void rx_async_stop(RxAsync *r);

// Asynchronous bulk OUT on the same run loop (an interface has one async event source). The Linux driver keeps up
// to AICWF_USB_TX_URBS transfers queued, at most AICWF_USB_FC_PERSTA_HIGH_WATER (64) per station, instead of
// waiting for each one; waiting caps the packet rate, and with it TCP throughput (every download needs its ACKs
// sent). `buf` is copied, so the caller can reuse it at once. Blocks while TX_ASYNC_SLOTS transfers are in flight
// (that wait is the flow control). Transfers on one pipe complete in the order they were submitted.
#define TX_ASYNC_SLOTS   64
#define TX_ASYNC_BUFSIZE 2048
int rx_async_tx(RxAsync *r, uint8_t pipe, const void *buf, size_t len);   // 0, or -1 (too big / USB error)
// Waits until every queued write has completed (before a synchronous write that must come after them).
int rx_async_tx_drain(RxAsync *r, int timeout_ms);                        // 0, or -1 on timeout
typedef struct { unsigned long sent, errors, waits; int max_inflight; } TxAsyncStats;
void rx_async_tx_stats(RxAsync *r, TxAsyncStats *s);

#endif
