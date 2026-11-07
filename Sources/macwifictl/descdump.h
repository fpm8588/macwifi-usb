#ifndef MACWIFI_DESCDUMP_H
#define MACWIFI_DESCDUMP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint16_t totalLength;
    uint8_t numInterfaces;
    unsigned numEndpoints;
    int malformed;
} UsbConfigSummary;

// Pretty-prints a raw configuration descriptor (and everything after it). Returns 0, or -1 if malformed.
int usb_dump_config(const uint8_t *desc, size_t len, FILE *out, UsbConfigSummary *summary);

#endif
