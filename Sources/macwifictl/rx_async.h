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
// Aborts the outstanding reads and joins the thread.
void rx_async_stop(RxAsync *r);

#endif
