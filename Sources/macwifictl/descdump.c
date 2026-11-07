// descdump.c — portable USB configuration-descriptor printer (no OS headers; unit tested on Linux too).
#include "descdump.h"

#include <stdio.h>

static const char *xfer_name(uint8_t attr) {
    static const char *n[] = {"control", "isoch", "bulk", "interrupt"};
    return n[attr & 3];
}
static const char *sync_name(uint8_t attr) {
    static const char *n[] = {"none", "async", "adaptive", "sync"};
    return n[(attr >> 2) & 3];
}
static const char *usage_name(uint8_t attr) {
    static const char *n[] = {"data", "feedback", "implicit-fb", "reserved"};
    return n[(attr >> 4) & 3];
}

int usb_dump_config(const uint8_t *d, size_t len, FILE *out, UsbConfigSummary *sum) {
    if (sum) { sum->totalLength = 0; sum->numInterfaces = 0; sum->numEndpoints = 0; sum->malformed = 0; }
    if (len < 9 || d[1] != 0x02) { fprintf(out, "  not a configuration descriptor\n"); if (sum) sum->malformed = 1; return -1; }
    uint16_t total = (uint16_t)(d[2] | (d[3] << 8));
    if (total > len) total = (uint16_t)len;
    if (sum) { sum->totalLength = (uint16_t)(d[2] | (d[3] << 8)); sum->numInterfaces = d[4]; }
    fprintf(out, "  Configuration: wTotalLength=%u bNumInterfaces=%u bConfigurationValue=%u bMaxPower=%u mA\n",
            (unsigned)(d[2] | (d[3] << 8)), d[4], d[5], d[8] * 2u);

    size_t off = d[0];
    while (off + 2 <= total) {
        uint8_t bl = d[off], type = d[off + 1];
        if (bl < 2 || off + bl > total) {
            fprintf(out, "  !! malformed descriptor at offset %zu (bLength=%u)\n", off, bl);
            if (sum) sum->malformed = 1;
            return -1;
        }
        const uint8_t *p = d + off;
        if (type == 0x04 && bl >= 9) {                       // INTERFACE
            fprintf(out, "  Interface %u alt %u: class=0x%02x sub=0x%02x proto=0x%02x endpoints=%u%s\n",
                    p[2], p[3], p[5], p[6], p[7], p[4], p[5] == 0xFF ? "  (vendor-specific: no macOS driver claims it)" : "");
        } else if (type == 0x05 && bl >= 7) {                // ENDPOINT
            uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
            unsigned base = mps & 0x7FF, extra = (mps >> 11) & 3;
            unsigned interval = p[6] ? (1u << (p[6] - 1)) : 0;
            fprintf(out, "    Endpoint 0x%02x %s %s, sync=%s usage=%s, wMaxPacketSize=%u (x%u), bInterval=%u -> every %u microframe(s)\n",
                    p[2], (p[2] & 0x80) ? "IN " : "OUT", xfer_name(p[3]), sync_name(p[3]), usage_name(p[3]),
                    base, extra + 1, p[6], interval);
            if (sum) sum->numEndpoints++;
        } else if (type == 0x24) {                           // CS_INTERFACE (audio class-specific)
            fprintf(out, "    class-specific interface descriptor, subtype 0x%02x, %u bytes\n", bl > 2 ? p[2] : 0, bl);
        } else if (type == 0x25) {                           // CS_ENDPOINT
            fprintf(out, "    class-specific endpoint descriptor, %u bytes\n", bl);
        } else if (type == 0x0B) {                           // INTERFACE ASSOCIATION
            fprintf(out, "  Interface association: first=%u count=%u class=0x%02x\n", p[2], p[3], p[4]);
        } else {
            fprintf(out, "    descriptor type 0x%02x, %u bytes\n", type, bl);
        }
        off += bl;
    }
    return 0;
}
