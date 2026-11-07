// Descriptor printer test: a vendor-class interface with bulk endpoints, like the AIC8800's Wi-Fi interface.
#include "descdump.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    unsigned char d[] = {
        9, 0x02, 0, 0, 1, 1, 0, 0x80, 250,
        9, 0x04, 0, 0, 3, 0xFF, 0xFF, 0xFF, 0,
        7, 0x05, 0x84, 0x02, 0x00, 0x02, 0,     // EP 0x84 IN bulk 512
        7, 0x05, 0x04, 0x02, 0x00, 0x02, 0,     // EP 0x04 OUT bulk 512
        7, 0x05, 0x05, 0x02, 0x00, 0x02, 0,     // EP 0x05 OUT bulk 512
    };
    d[2] = (unsigned char)sizeof d;
    UsbConfigSummary s;
    int ok = usb_dump_config(d, sizeof d, stdout, &s) == 0 && s.numEndpoints == 3 && !s.malformed;
    d[9] = 0;
    UsbConfigSummary s2;
    ok = ok && usb_dump_config(d, sizeof d, stdout, &s2) == -1 && s2.malformed;
    printf("descdump: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
