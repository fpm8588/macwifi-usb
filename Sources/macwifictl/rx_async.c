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

typedef IOUSBInterfaceInterface300 **Intf;

typedef struct {
    RxAsync *r;
    uint8_t *buf;
} Slot;

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
};

static void completed(void *refcon, IOReturn result, void *arg0);

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
    if ((r->stopping || r->failed) && r->outstanding == 0) CFRunLoopStop(r->rl);
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
        while (!((r->stopping || r->failed) && r->outstanding == 0))
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
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->mu);
    free(r);
    return NULL;
}

void rx_async_stop(RxAsync *r) {
    if (!r) return;
    r->stopping = 1;                                  // read by the reader thread's loop and callbacks
    (*r->intf)->AbortPipe(r->intf, r->pipe);           // outstanding reads complete with kIOReturnAborted
    if (r->rl) CFRunLoopWakeUp(r->rl);
    pthread_join(r->th, NULL);
    for (int i = 0; i < RX_ASYNC_READS; i++) free(r->slot[i].buf);
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->mu);
    free(r);
}
