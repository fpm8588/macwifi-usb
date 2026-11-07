// macwifictl — Phase 1 hardware tool for MT7612U-family USB Wi-Fi adapters on Apple Silicon macOS.
//
// Proves, with no driver installed, no kext, no DriverKit and no paid Apple account, that this Mac can
// reach the adapter from user space and read its registers. Later phases build on the same transport.
//
//   macwifictl list [--all]     supported adapters (or every USB device, to find out what you have)
//   macwifictl info             descriptors, speed, configuration of the first supported adapter
//   macwifictl identify         read MT_ASIC_VERSION / MT_MAC_CSR0 and check the chip
//   macwifictl rr <addr>        read a 32-bit register (prefix 0x40000000 for the CFG space)
//   macwifictl wr <addr> <val>  write a 32-bit register (careful)
//   macwifictl reset            USB port reset (the Linux driver does this before probing)
//
// Build: ./build.sh cli   (needs only the free Xcode Command Line Tools)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "descdump.h"
#include "mt76x2u.h"

typedef IOUSBDeviceInterface320 **Dev;

// ---------------------------------------------------------------- enumeration

static int prop_u16(io_service_t s, CFStringRef key, uint16_t *out) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    if (!v) return -1;
    int ok = 0;
    SInt32 n = 0;
    if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &n)) {
        *out = (uint16_t)n;
        ok = 1;
    }
    CFRelease(v);
    return ok ? 0 : -1;
}

static void prop_str(io_service_t s, CFStringRef key, char *buf, size_t cap) {
    buf[0] = '\0';
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    if (!v) return;
    if (CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, buf, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(v);
}

static const char *vendor_hint(uint16_t vid) {
    switch (vid) {
        case 0x0e8d: return "MediaTek";
        case 0x148f: return "Ralink/MediaTek";
        case 0x0bda: return "Realtek";
        case 0x2357: return "TP-Link";
        case 0x0846: return "Netgear";
        case 0x0b05: return "Asus";
        case 0x7392: return "Edimax";
        default: return "";
    }
}

// Calls `fn` for every USB device; stops early if fn returns nonzero (and returns that value).
typedef int (*DevFn)(io_service_t svc, uint16_t vid, uint16_t pid, void *ctx);
static int for_each_usb_device(DevFn fn, void *ctx) {
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(kIOUSBDeviceClassName), &it) != KERN_SUCCESS)
        return -1;
    io_service_t s;
    int r = 0;
    while (!r && (s = IOIteratorNext(it))) {
        uint16_t vid = 0, pid = 0;
        if (prop_u16(s, CFSTR(kUSBVendorID), &vid) == 0 && prop_u16(s, CFSTR(kUSBProductID), &pid) == 0)
            r = fn(s, vid, pid, ctx);
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return r;
}

typedef struct { int all; int found; } ListCtx;
static int list_one(io_service_t s, uint16_t vid, uint16_t pid, void *ctx) {
    ListCtx *c = ctx;
    const Mt76x2uId *id = mt76x2u_lookup(vid, pid);
    if (!id && !c->all) return 0;
    char name[128];
    prop_str(s, CFSTR("USB Product Name"), name, sizeof name);
    printf("%s %04x:%04x  %-32s %s%s%s\n", id ? "*" : " ", vid, pid, name[0] ? name : "(no name)",
           vendor_hint(vid), id ? "  -> supported: " : "", id ? id->name : "");
    if (id) c->found++;
    return 0;
}

typedef struct { Dev dev; const Mt76x2uId *id; } OpenCtx;
static int open_one(io_service_t s, uint16_t vid, uint16_t pid, void *ctx) {
    OpenCtx *c = ctx;
    const Mt76x2uId *id = mt76x2u_lookup(vid, pid);
    if (!id) return 0;
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(s, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
                                                         &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) {
        fprintf(stderr, "IOCreatePlugInInterfaceForService failed: 0x%08x\n", kr);
        return -1;
    }
    Dev dev = NULL;
    HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320), (LPVOID *)&dev);
    IODestroyPlugInInterface(plugin);
    if (hr != S_OK || !dev) return -1;
    c->dev = dev;
    c->id = id;
    return 1;
}

static Dev open_adapter(const Mt76x2uId **which) {
    OpenCtx c = {NULL, NULL};
    for_each_usb_device(open_one, &c);
    if (which) *which = c.id;
    return c.dev;
}

// ---------------------------------------------------------------- register access (Linux usb.c policy)

static IOReturn vendor_request(Dev dev, uint8_t rt, uint8_t req, uint16_t wValue, uint16_t wIndex, void *buf,
                               uint16_t len) {
    IOReturn kr = kIOReturnError;
    for (int i = 0; i < MT_VEND_REQ_MAX_RETRY; i++) {
        IOUSBDevRequestTO r;
        memset(&r, 0, sizeof r);
        r.bmRequestType = rt;
        r.bRequest = req;
        r.wValue = wValue;
        r.wIndex = wIndex;
        r.wLength = len;
        r.pData = buf;
        r.noDataTimeout = MT_VEND_REQ_TIMEOUT_MS;
        r.completionTimeout = MT_VEND_REQ_TIMEOUT_MS;
        kr = (*dev)->DeviceRequestTO(dev, &r);
        if (kr == kIOReturnSuccess) return kr;
        if (kr == kIOReturnNoDevice || kr == kIOReturnNotResponding) return kr;
        usleep(5000 + (useconds_t)(rand() % 5000));
    }
    return kr;
}

static int reg_read(Dev dev, uint32_t addr, uint32_t *val) {
    uint8_t b[4] = {0};
    IOReturn kr = vendor_request(dev, MT_RT_VENDOR_IN, mt76x2u_read_req(addr), mt76x2u_wvalue(addr),
                                 mt76x2u_windex(addr), b, 4);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "read 0x%08x failed: 0x%08x\n", addr, kr);
        return -1;
    }
    *val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int reg_write(Dev dev, uint32_t addr, uint32_t val) {
    uint8_t b[4] = {(uint8_t)val, (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24)};
    IOReturn kr = vendor_request(dev, MT_RT_VENDOR_OUT, mt76x2u_write_req(addr), mt76x2u_wvalue(addr),
                                 mt76x2u_windex(addr), b, 4);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "write 0x%08x failed: 0x%08x\n", addr, kr);
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------- commands

static const char *speed_name(UInt8 s) {
    switch (s) {
        case kUSBDeviceSpeedLow: return "low (1.5 Mb/s)";
        case kUSBDeviceSpeedFull: return "full (12 Mb/s)  <-- too slow for Wi-Fi; try another port/cable";
        case kUSBDeviceSpeedHigh: return "high (480 Mb/s)";
        default: return "super speed (5 Gb/s+)";
    }
}

static int cmd_info(Dev dev, const Mt76x2uId *id) {
    UInt16 vid = 0, pid = 0, rel = 0;
    UInt32 loc = 0;
    UInt8 speed = 0, cfg = 0;
    (*dev)->GetDeviceVendor(dev, &vid);
    (*dev)->GetDeviceProduct(dev, &pid);
    (*dev)->GetDeviceReleaseNumber(dev, &rel);
    (*dev)->GetLocationID(dev, &loc);
    (*dev)->GetDeviceSpeed(dev, &speed);
    IOReturn gc = (*dev)->GetConfiguration(dev, &cfg);
    printf("%s\n", id->name);
    printf("  USB ID         %04x:%04x\n", vid, pid);
    printf("  bcdDevice      0x%04x\n", rel);
    printf("  Location       0x%08x\n", loc);
    printf("  Speed          %s\n", speed_name(speed));
    if (gc == kIOReturnSuccess) printf("  Configuration  %u%s\n", cfg, cfg ? "" : " (unconfigured: the daemon selects config 1 in Phase 2)");
    IOUSBConfigurationDescriptorPtr desc = NULL;
    if ((*dev)->GetConfigurationDescriptorPtr(dev, 0, &desc) != kIOReturnSuccess || !desc) {
        fprintf(stderr, "GetConfigurationDescriptorPtr failed\n");
        return 1;
    }
    UsbConfigSummary sum;
    usb_dump_config((const uint8_t *)desc, USBToHostWord(desc->wTotalLength), stdout, &sum);
    return sum.malformed ? 1 : 0;
}

static int cmd_identify(Dev dev, const Mt76x2uId *id) {
    uint32_t asic = 0, mac = 0;
    if (reg_read(dev, MT_ASIC_VERSION, &asic) || reg_read(dev, MT_MAC_CSR0, &mac)) return 1;
    printf("%s\n", id->name);
    printf("  MT_ASIC_VERSION  0x%08x  (chip %04x, rev %04x)\n", asic, mt76x2u_chip(asic), asic & 0xffff);
    printf("  MT_MAC_CSR0      0x%08x\n", mac);
    if (asic == 0 || asic == 0xffffffff) {
        printf("  -> No sensible answer. Run `macwifictl reset`, replug the adapter, and try again.\n");
        return 1;
    }
    if (!mt76x2u_is_supported_chip(asic)) {
        printf("  -> Not an MT76x2 chip. This adapter is not supported by this driver.\n");
        return 1;
    }
    printf("  -> MT76x2 chip confirmed. Phase 1 hardware check passed.\n");
    return 0;
}

static int cmd_reset(Dev dev) {
    IOReturn open = (*dev)->USBDeviceOpen(dev);
    if (open != kIOReturnSuccess) {
        fprintf(stderr, "USBDeviceOpen failed: 0x%08x (is another program using the adapter?)\n", open);
        return 1;
    }
    IOReturn kr = (*dev)->USBDeviceReEnumerate(dev, 0);
    (*dev)->USBDeviceClose(dev);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "USBDeviceReEnumerate failed: 0x%08x\n", kr);
        return 1;
    }
    printf("Reset requested; the adapter re-enumerates in about a second.\n");
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: macwifictl list [--all] | info | identify | rr <addr> | wr <addr> <val> | reset\n"
            "Supported: MT7612U / MT7632U / MT7662U adapters (e.g. Alfa AWUS036ACM, Netgear A6210).\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return 2; }

    if (!strcmp(argv[1], "list")) {
        ListCtx c = {argc > 2 && !strcmp(argv[2], "--all"), 0};
        for_each_usb_device(list_one, &c);
        if (!c.found) {
            printf("No supported adapter found.%s\n", c.all ? "" : " Run `macwifictl list --all` to see every USB device.");
            return 3;
        }
        return 0;
    }

    const Mt76x2uId *id = NULL;
    Dev dev = open_adapter(&id);
    if (!dev) {
        fprintf(stderr, "No supported adapter found. Plug it in directly (no hub) and run `macwifictl list --all`.\n");
        return 3;
    }
    int rc = 2;
    const char *c = argv[1];
    if (!strcmp(c, "info")) rc = cmd_info(dev, id);
    else if (!strcmp(c, "identify")) rc = cmd_identify(dev, id);
    else if (!strcmp(c, "rr") && argc > 2) {
        uint32_t v = 0, a = (uint32_t)strtoul(argv[2], NULL, 0);
        rc = reg_read(dev, a, &v) ? 1 : (printf("0x%08x = 0x%08x\n", a, v), 0);
    } else if (!strcmp(c, "wr") && argc > 3) {
        uint32_t a = (uint32_t)strtoul(argv[2], NULL, 0), v = (uint32_t)strtoul(argv[3], NULL, 0);
        rc = reg_write(dev, a, v) ? 1 : (printf("0x%08x <- 0x%08x\n", a, v), 0);
    } else if (!strcmp(c, "reset")) rc = cmd_reset(dev);
    else usage();
    (*dev)->Release(dev);
    return rc;
}
