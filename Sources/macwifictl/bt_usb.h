// bt_usb.h — HCI over USB to the adapter's Bluetooth controller (macOS, user space). Original code (GPL-2.0).

#ifndef BT_USB_H
#define BT_USB_H

#include <IOKit/IOKitLib.h>
#include <stddef.h>
#include <stdint.h>

#include "hci.h"

typedef struct BtLink BtLink;

// Opens the Bluetooth HCI interface of the USB device `svc` and starts reading events. NULL + err on failure.
BtLink *bt_open(io_service_t svc, int debug, char *err, size_t errcap);
void bt_close(BtLink *b);

int bt_send_cmd(BtLink *b, uint16_t opcode, const void *params, size_t plen);
// Next whole event (2 + len bytes) into out (HCI_MAX_EVENT bytes). 1 = event, 0 = timeout, -1 = USB error.
int bt_next_event(BtLink *b, uint8_t *out, size_t *n, int timeout_ms);

// Sends a command and waits for its Command Complete / Command Status. Returns the HCI status (0 = success),
// -1 on a USB error, -2 on timeout. Return parameters after the status go to ret. Other events seen while
// waiting are passed to `other` (may be NULL).
typedef void (*BtEventFn)(void *ctx, const HciEvent *ev);
int bt_call(BtLink *b, uint16_t opcode, const void *params, size_t plen, uint8_t *ret, size_t retcap, size_t *retlen,
            int timeout_ms, BtEventFn other, void *ctx);

#endif
