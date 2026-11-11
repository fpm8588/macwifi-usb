// rx_async.c — see rx_async.h. Original code (MIT).
//
// The Linux driver keeps AICWF_USB_RX_URBS (20) bulk-IN URBs queued so the host controller always has an IN
// transfer pending and the chip never waits; a single synchronous read leaves gaps in which, under load, the
// chip's RX buffers fill up. Here: RX_ASYNC_READS ReadPipeAsync calls in flight, completions (in submission order)
// copied into the RxQ and resubmitted at once.

#include "rx_async.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef IOUSBInterfaceInterface300 **Intf;

typedef struct {
    RxAsync *r;
    uint8_t *buf;
} Slot;

typedef struct {
    RxAsync *r;
    int busy;
    uint8_t buf[TX_ASYNC_BUFSIZE];
} TxSlot;

struct RxAsync {
    Intf intf;
    UInt8 pipe;
    RxQ *q;
    pthread_t th;
    CFRunLoopRef rl;
    Slot slot[RX_ASYNC_READS];
    int outstanding, failed;          // reader thread only
    _Atomic int stopping;             // set by rx_async_stop on another thread
    pthread_mutex_t mu;               // only for the start handshake
    pthread_cond_t cv;
    int ready, start_ok;
    // TX: slots guarded by txmu; completions arrive on the reader thread.
    TxSlot *tx;
    pthread_mutex_t txmu;
    pthread_cond_t txcv;
    UInt8 tx_pipe;
    _Atomic int tx_inflight;
    int tx_failed;
    TxAsyncStats txs;
};

static void completed(void *refcon, IOReturn result, void *arg0);
static void tx_completed(void *refcon, IOReturn result, void *arg0);

static int submit(Slot *s) {
    RxAsync *r = s->r;
    IOReturn kr = (*r->intf)->ReadPipeAsync(r->intf, r->pipe, s->buf, RX_ASYNC_BUFSIZE, completed, s);
    if (kr != kIOReturnSuccess) return -1;
    r->outstanding++;
    return 0;
}

// Runs on the reader thread (its run loop delivers the completions).
static void completed(void *refcon, IOReturn result, void *arg0) {
    Slot *s = (Slot *)refcon;
    RxAsync *r = s->r;
    UInt32 n = (UInt32)(uintptr_t)arg0;
    r->outstanding--;
    if (result == kIOReturnSuccess) {
        if (n) rxq_push(r->q, s->buf, n);
    } else if (!r->stopping) {
        if (result == kIOUSBPipeStalled) {
            (*r->intf)->ClearPipeStallBothEnds(r->intf, r->pipe);
        } else if (!r->failed) {      // device gone, reset, not responding: let the consumer find out
            r->failed = 1;
            rxq_push_error(r->q, result);
        }
    }
    if (!r->stopping && !r->failed && submit(s) != 0 && !r->failed) {
        r->failed = 1;
        rxq_push_error(r->q, kIOReturnError);
    }
    if ((r->stopping || r->failed) && r->outstanding == 0 && r->tx_inflight == 0) CFRunLoopStop(r->rl);
}

// Runs on the reader thread.
static void tx_completed(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    TxSlot *t = (TxSlot *)refcon;
    RxAsync *r = t->r;
    pthread_mutex_lock(&r->txmu);
    t->busy = 0;
    r->tx_inflight--;
    if (result == kIOReturnSuccess) {
        r->txs.sent++;
    } else if (!r->stopping) {
        r->txs.errors++;
        if (result == kIOUSBPipeStalled) (*r->intf)->ClearPipeStallBothEnds(r->intf, r->tx_pipe);
        else if (result != kIOReturnAborted) r->tx_failed = 1;
    }
    pthread_cond_broadcast(&r->txcv);
    pthread_mutex_unlock(&r->txmu);
    if ((r->stopping || r->failed) && r->outstanding == 0 && r->tx_inflight == 0) CFRunLoopStop(r->rl);
}

int rx_async_tx(RxAsync *r, uint8_t pipe, const void *buf, size_t len) {
    if (!r || len > TX_ASYNC_BUFSIZE) return -1;
    pthread_mutex_lock(&r->txmu);
    if (r->tx_inflight >= TX_ASYNC_SLOTS) r->txs.waits++;
    while (r->tx_inflight >= TX_ASYNC_SLOTS && !r->stopping && !r->failed && !r->tx_failed) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        if (pthread_cond_timedwait(&r->txcv, &r->txmu, &ts) != 0) break;   // the device stopped completing
    }
    if (r->stopping || r->failed || r->tx_failed || r->tx_inflight >= TX_ASYNC_SLOTS) {
        pthread_mutex_unlock(&r->txmu);
        return -1;
    }
    TxSlot *t = NULL;
    for (int i = 0; i < TX_ASYNC_SLOTS && !t; i++) if (!r->tx[i].busy) t = &r->tx[i];
    if (!t) { pthread_mutex_unlock(&r->txmu); return -1; }
    memcpy(t->buf, buf, len);
    t->busy = 1;
    r->tx_pipe = pipe;
    r->tx_inflight++;
    if (r->tx_inflight > r->txs.max_inflight) r->txs.max_inflight = r->tx_inflight;
    // Submitted under the lock so that writes reach the pipe in the order callers queued them.
    IOReturn kr = (*r->intf)->WritePipeAsync(r->intf, pipe, t->buf, (UInt32)len, tx_completed, t);
    if (kr != kIOReturnSuccess) {
        t->busy = 0;
        r->tx_inflight--;
        r->txs.errors++;
        pthread_mutex_unlock(&r->txmu);
        return -1;
    }
    pthread_mutex_unlock(&r->txmu);
    return 0;
}

int rx_async_tx_drain(RxAsync *r, int timeout_ms) {
    if (!r) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&r->txmu);
    while (r->tx_inflight > 0)
        if (pthread_cond_timedwait(&r->txcv, &r->txmu, &ts) != 0) break;
    int ok = r->tx_inflight == 0;
    pthread_mutex_unlock(&r->txmu);
    return ok ? 0 : -1;
}

void rx_async_tx_stats(RxAsync *r, TxAsyncStats *s) {
    memset(s, 0, sizeof *s);
    if (!r) return;
    pthread_mutex_lock(&r->txmu);
    *s = r->txs;
    pthread_mutex_unlock(&r->txmu);
}

static void *reader(void *arg) {
    RxAsync *r = (RxAsync *)arg;
    r->rl = CFRunLoopGetCurrent();
    CFRunLoopSourceRef src = NULL;
    IOReturn kr = (*r->intf)->CreateInterfaceAsyncEventSource(r->intf, &src);
    int ok = kr == kIOReturnSuccess && src;
    if (ok) {
        CFRunLoopAddSource(r->rl, src, kCFRunLoopDefaultMode);
        for (int i = 0; i < RX_ASYNC_READS; i++)
            if (submit(&r->slot[i]) != 0) { ok = i > 0; break; }
    }
    pthread_mutex_lock(&r->mu);
    r->start_ok = ok;
    r->ready = 1;
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->mu);
    if (ok) {
        while (!((r->stopping || r->failed) && r->outstanding == 0 && r->tx_inflight == 0))
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, 0);
    }
    if (src) {
        CFRunLoopRemoveSource(r->rl, src, kCFRunLoopDefaultMode);
        CFRelease(src);
    }
    return NULL;
}

RxAsync *rx_async_start(void *intf, uint8_t pipe, RxQ *q) {
    RxAsync *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->intf = (Intf)intf;
    r->pipe = pipe;
    r->q = q;
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->cv, NULL);
    pthread_mutex_init(&r->txmu, NULL);
    pthread_cond_init(&r->txcv, NULL);
    r->tx = calloc(TX_ASYNC_SLOTS, sizeof *r->tx);
    if (!r->tx) goto fail;
    for (int i = 0; i < TX_ASYNC_SLOTS; i++) r->tx[i].r = r;
    for (int i = 0; i < RX_ASYNC_READS; i++) {
        r->slot[i].r = r;
        r->slot[i].buf = malloc(RX_ASYNC_BUFSIZE);
        if (!r->slot[i].buf) goto fail;
    }
    if (pthread_create(&r->th, NULL, reader, r) != 0) goto fail;
    pthread_mutex_lock(&r->mu);
    while (!r->ready) pthread_cond_wait(&r->cv, &r->mu);
    pthread_mutex_unlock(&r->mu);
    if (!r->start_ok) {
        r->stopping = 1;
        (*r->intf)->AbortPipe(r->intf, r->pipe);
        pthread_join(r->th, NULL);
        fprintf(stderr, "async USB reads unavailable; using synchronous reads\n");
        goto fail;
    }
    return r;
fail:
    for (int i = 0; i < RX_ASYNC_READS; i++) free(r->slot[i].buf);
    free(r->tx);
    pthread_cond_destroy(&r->txcv);
    pthread_mutex_destroy(&r->txmu);
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->mu);
    free(r);
    return NULL;
}

void rx_async_stop(RxAsync *r) {
    if (!r) return;
    rx_async_tx_drain(r, 500);                         // let queued frames (a deauth, say) go out first
    pthread_mutex_lock(&r->txmu);
    r->stopping = 1;                                  // read by the reader thread's loop and callbacks
    pthread_cond_broadcast(&r->txcv);
    UInt8 txp = r->tx_inflight ? r->tx_pipe : 0;
    pthread_mutex_unlock(&r->txmu);
    (*r->intf)->AbortPipe(r->intf, r->pipe);           // outstanding reads complete with kIOReturnAborted
    if (txp) (*r->intf)->AbortPipe(r->intf, txp);
    if (r->rl) CFRunLoopWakeUp(r->rl);
    pthread_join(r->th, NULL);
    for (int i = 0; i < RX_ASYNC_READS; i++) free(r->slot[i].buf);
    free(r->tx);
    pthread_cond_destroy(&r->txcv);
    pthread_mutex_destroy(&r->txmu);
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->mu);
    free(r);
}
