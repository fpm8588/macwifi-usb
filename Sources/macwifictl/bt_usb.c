// bt_usb.c — the adapter's Bluetooth controller over USB (HCI, interface 0), from user space. Original code
// (GPL-2.0).
//
// After our firmware load (the same steps as Linux aic_load_fw), the AIC8800D80's Bluetooth side is a standard
// HCI-over-USB controller (Bluetooth Core v5.4, Vol 4 Part B): commands are class control requests to the
// interface, events arrive on the interrupt IN endpoint, ACL data on the bulk pair. macOS's own Bluetooth stack
// doesn't drive third-party controllers, so we talk to it ourselves. Phase B1: identify it and scan.

#include "bt_usb.h"

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hci.h"
#include "rxq.h"

typedef IOUSBDeviceInterface320 **BtDev;
typedef IOUSBInterfaceInterface300 **BtIntf;

struct BtLink {
    BtDev dev;
    BtIntf intf;
    UInt8 ifnum;
    UInt8 evt_pipe, acl_in, acl_out;
    UInt16 evt_mps;
    RxQ *q;
    pthread_t reader;
    int reader_on;
    volatile int stop;
    uint8_t stream[4096];         // event reassembly (an event can span several interrupt packets)
    size_t have;
    int debug;
};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void dump(BtLink *b, const char *tag, const uint8_t *p, size_t n) {
    if (!b->debug) return;
    fprintf(stderr, "BT %s %zu:", tag, n);
    for (size_t i = 0; i < n && i < 64; i++) fprintf(stderr, " %02x", p[i]);
    fprintf(stderr, n > 64 ? " ...\n" : "\n");
}

// Blocking reads of one interrupt packet at a time into the queue; AbortPipe ends it.
static void *reader_main(void *arg) {
    BtLink *b = arg;
    uint8_t buf[1024];
    while (!b->stop) {
        UInt32 size = b->evt_mps ? b->evt_mps : 64;
        IOReturn kr = (*b->intf)->ReadPipe(b->intf, b->evt_pipe, buf, &size);
        if (kr != kIOReturnSuccess) {
            if (kr == kIOUSBPipeStalled) { (*b->intf)->ClearPipeStallBothEnds(b->intf, b->evt_pipe); continue; }
            rxq_push_error(b->q, (int)kr);
            break;
        }
        if (size) rxq_push(b->q, buf, size);
    }
    return NULL;
}

void bt_close(BtLink *b) {
    if (!b) return;
    if (b->reader_on) {
        b->stop = 1;
        (*b->intf)->AbortPipe(b->intf, b->evt_pipe);
        pthread_join(b->reader, NULL);
    }
    if (b->q) rxq_free(b->q);
    if (b->intf) { (*b->intf)->USBInterfaceClose(b->intf); (*b->intf)->Release(b->intf); }
    if (b->dev) (*b->dev)->Release(b->dev);
    free(b);
}

BtLink *bt_open(io_service_t svc, int debug, char *err, size_t errcap) {
    BtLink *b = calloc(1, sizeof *b);
    if (!b) { snprintf(err, errcap, "out of memory"); return NULL; }
    b->debug = debug;
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score) !=
            KERN_SUCCESS || !plugin) {
        snprintf(err, errcap, "can't reach the USB device");
        free(b);
        return NULL;
    }
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320), (LPVOID *)&b->dev);
    IODestroyPlugInInterface(plugin);
    if (!b->dev) { snprintf(err, errcap, "can't reach the USB device"); free(b); return NULL; }

    IOUSBFindInterfaceRequest req = {0xE0, 0x01, 0x01, kIOUSBFindInterfaceDontCare};   // Bluetooth HCI
    io_iterator_t it = 0;
    if ((*b->dev)->CreateInterfaceIterator(b->dev, &req, &it) != kIOReturnSuccess) {
        snprintf(err, errcap, "can't list the adapter's interfaces");
        bt_close(b);
        return NULL;
    }
    io_service_t is;
    IOReturn last = kIOReturnNotFound;
    while (!b->intf && (is = IOIteratorNext(it))) {
        plugin = NULL;
        kern_return_t kr = IOCreatePlugInInterfaceForService(is, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID,
                                                             &plugin, &score);
        IOObjectRelease(is);
        if (kr != KERN_SUCCESS || !plugin) continue;
        BtIntf intf = NULL;
        (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&intf);
        IODestroyPlugInInterface(plugin);
        if (!intf) continue;
        UInt8 num = 0xFF;
        (*intf)->GetInterfaceNumber(intf, &num);
        UInt8 nep = 0;
        (*intf)->GetNumEndpoints(intf, &nep);
        if (nep < 3) { (*intf)->Release(intf); continue; }        // interface 1 (SCO audio) has 2 at most
        last = (*intf)->USBInterfaceOpen(intf);
        if (last != kIOReturnSuccess) { (*intf)->Release(intf); continue; }
        b->intf = intf;
        b->ifnum = num;
    }
    IOObjectRelease(it);
    if (!b->intf) {
        if (last == kIOReturnExclusiveAccess)
            snprintf(err, errcap, "macOS has a driver on the Bluetooth interface (exclusive access)");
        else if (last == kIOReturnNotFound)
            snprintf(err, errcap, "the adapter shows no Bluetooth interface (is its firmware loaded?)");
        else
            snprintf(err, errcap, "can't open the Bluetooth interface (0x%08x)", last);
        bt_close(b);
        return NULL;
    }
    UInt8 n = 0;
    (*b->intf)->GetNumEndpoints(b->intf, &n);
    for (UInt8 p = 1; p <= n; p++) {
        UInt8 dir = 0, num = 0, type = 0, interval = 0;
        UInt16 mps = 0;
        if ((*b->intf)->GetPipeProperties(b->intf, p, &dir, &num, &type, &mps, &interval) != kIOReturnSuccess) continue;
        if (type == kUSBInterrupt && dir == kUSBIn && !b->evt_pipe) { b->evt_pipe = p; b->evt_mps = mps; }
        else if (type == kUSBBulk && dir == kUSBIn && !b->acl_in) b->acl_in = p;
        else if (type == kUSBBulk && dir == kUSBOut && !b->acl_out) b->acl_out = p;
    }
    if (!b->evt_pipe) {
        snprintf(err, errcap, "the Bluetooth interface has no event endpoint");
        bt_close(b);
        return NULL;
    }
    b->q = rxq_new(1024, 1u << 20);
    if (!b->q || pthread_create(&b->reader, NULL, reader_main, b)) {
        snprintf(err, errcap, "can't start the event reader");
        bt_close(b);
        return NULL;
    }
    b->reader_on = 1;
    return b;
}

int bt_send_cmd(BtLink *b, uint16_t opcode, const void *params, size_t plen) {
    uint8_t pkt[3 + 255];
    size_t n = hci_cmd(pkt, sizeof pkt, opcode, params, plen);
    if (!n) return -1;
    dump(b, "CMD", pkt, n);
    IOUSBDevRequestTO req;
    memset(&req, 0, sizeof req);
    req.bmRequestType = HCI_USB_CMD_REQTYPE;
    req.bRequest = HCI_USB_CMD_REQUEST;
    req.wValue = 0;
    req.wIndex = b->ifnum;
    req.wLength = (UInt16)n;
    req.pData = pkt;
    req.noDataTimeout = 1000;
    req.completionTimeout = 2000;
    IOReturn kr = (*b->intf)->ControlRequestTO(b->intf, 0, &req);
    if (kr != kIOReturnSuccess) {
        if (b->debug) fprintf(stderr, "BT command 0x%04x: control request failed 0x%08x\n", opcode, kr);
        return -1;
    }
    return 0;
}

// Takes the next complete event out of the reassembly buffer into out (2 + len bytes). 1 = got one.
static int take_event(BtLink *b, uint8_t *out, size_t *n) {
    if (b->have < 2) return 0;
    size_t need = 2 + (size_t)b->stream[1];
    if (b->have < need) return 0;
    memcpy(out, b->stream, need);
    memmove(b->stream, b->stream + need, b->have - need);
    b->have -= need;
    *n = need;
    return 1;
}

int bt_next_event(BtLink *b, uint8_t *out, size_t *n, int timeout_ms) {
    double end = now_s() + timeout_ms / 1000.0;
    for (;;) {
        if (take_event(b, out, n)) { dump(b, "EVT", out, *n); return 1; }
        int left = (int)((end - now_s()) * 1000);
        if (left <= 0) return 0;
        uint8_t chunk[1024];
        int err = 0;
        long got = rxq_pop(b->q, chunk, sizeof chunk, left, &err);
        if (got < 0) return -1;
        if (got == 0) return 0;
        if (b->have + (size_t)got > sizeof b->stream) b->have = 0;      // garbage: resynchronise
        memcpy(b->stream + b->have, chunk, (size_t)got);
        b->have += (size_t)got;
    }
}

int bt_call(BtLink *b, uint16_t opcode, const void *params, size_t plen, uint8_t *ret, size_t retcap, size_t *retlen,
            int timeout_ms, BtEventFn other, void *ctx) {
    if (bt_send_cmd(b, opcode, params, plen)) return -1;
    double end = now_s() + timeout_ms / 1000.0;
    uint8_t ev[HCI_MAX_EVENT];
    size_t n = 0;
    for (;;) {
        int left = (int)((end - now_s()) * 1000);
        if (left <= 0) return -2;
        int r = bt_next_event(b, ev, &n, left);
        if (r < 0) return -1;
        if (r == 0) return -2;
        HciEvent e;
        HciCmdDone d;
        if (hci_event(ev, n, &e)) continue;
        if (hci_cmd_complete(&e, &d) == 0 && d.opcode == opcode) {
            if (ret && retlen) {
                size_t c = d.ret_len < retcap ? d.ret_len : retcap;
                memcpy(ret, d.ret, c);
                *retlen = c;
            }
            return d.status;
        }
        if (hci_cmd_status(&e, &d) == 0 && d.opcode == opcode) return d.status;
        if (other) other(ctx, &e);
    }
}
