// macwifictl — Phase 1 hardware tool for AICsemi AIC8800 USB Wi-Fi adapters (UGREEN AX900) on Apple Silicon.
//
// No driver, no kext, no DriverKit, no paid Apple account: plain IOKit from user space.
//
//   macwifictl list [--all]       AIC adapters (or every USB device) and which mode they are in
//   macwifictl modeswitch         driver-CD mode (a69c:57xx) -> Wi-Fi mode, by ejecting the virtual disc
//   macwifictl info               descriptors of the adapter (either mode)
//   macwifictl identify           Wi-Fi mode: read the chip revision through the ROM's debug messages
//   macwifictl memr <addr>        Wi-Fi mode: DBG_MEM_READ
//   macwifictl memw <addr> <val>  Wi-Fi mode: DBG_MEM_WRITE (careful)
//
// Set MACWIFI_DEBUG=1 to hex-dump every USB transfer.
// Build: ./build.sh cli   (needs only the free Xcode Command Line Tools)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aic8800.h"
#include "aic_msg.h"
#include "descdump.h"

extern char **environ;

typedef IOUSBDeviceInterface320 **Dev;
typedef IOUSBInterfaceInterface300 **Intf;

static int gDebug = 0;

static void hexdump(const char *tag, const uint8_t *p, size_t n) {
    if (!gDebug) return;
    fprintf(stderr, "%s (%zu bytes):", tag, n);
    for (size_t i = 0; i < n && i < 256; i++) fprintf(stderr, "%s%02x", (i % 16) ? " " : "\n  ", p[i]);
    fprintf(stderr, "%s\n", n > 256 ? "\n  ..." : "");
}

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

static void prop_str(io_service_t s, CFStringRef key, int recursive, char *buf, size_t cap) {
    buf[0] = '\0';
    CFTypeRef v = recursive ? IORegistryEntrySearchCFProperty(s, kIOServicePlane, key, kCFAllocatorDefault,
                                                              kIORegistryIterateRecursively)
                            : IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    if (!v) return;
    if (CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, buf, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(v);
}

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

typedef struct { int all, cd, wifi; } ListCtx;
static int list_one(io_service_t s, uint16_t vid, uint16_t pid, void *ctx) {
    ListCtx *c = ctx;
    const AicId *id = aic_lookup(vid, pid);
    if (!id && !c->all) return 0;
    char name[128];
    prop_str(s, CFSTR("USB Product Name"), 0, name, sizeof name);
    const char *mode = !id ? "" : id->mode == AIC_MODE_DRIVER_CD ? "  [driver-CD mode -> run `macwifictl modeswitch`]"
                                                                  : "  [Wi-Fi mode]";
    printf("%s %04x:%04x  %-28s %s%s\n", id ? "*" : " ", vid, pid, name[0] ? name : "(no name)",
           id ? id->name : "", mode);
    if (id && id->mode == AIC_MODE_DRIVER_CD) c->cd++;
    if (id && id->mode == AIC_MODE_WIFI) c->wifi++;
    return 0;
}

typedef struct { AicMode want; io_service_t svc; const AicId *id; } FindCtx;
static int find_one(io_service_t s, uint16_t vid, uint16_t pid, void *ctx) {
    FindCtx *c = ctx;
    const AicId *id = aic_lookup(vid, pid);
    if (!id || (c->want != AIC_MODE_NONE && id->mode != c->want)) return 0;
    IOObjectRetain(s);
    c->svc = s;
    c->id = id;
    return 1;
}

static io_service_t find_adapter(AicMode want, const AicId **which) {
    FindCtx c = {want, 0, NULL};
    for_each_usb_device(find_one, &c);
    if (which) *which = c.id;
    return c.svc;
}

static Dev device_from_service(io_service_t svc) {
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
                                                         &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) {
        fprintf(stderr, "IOCreatePlugInInterfaceForService(device) failed: 0x%08x\n", kr);
        return NULL;
    }
    Dev dev = NULL;
    HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320), (LPVOID *)&dev);
    IODestroyPlugInInterface(plugin);
    return (hr == S_OK) ? dev : NULL;
}

// ---------------------------------------------------------------- Wi-Fi mode transport

typedef struct {
    Dev dev;
    Intf intf;
    UInt8 in_pipe, out_pipe, msg_pipe;   // pipe refs (0 = none)
    const AicId *id;
} Link;

static void link_close(Link *l) {
    if (l->intf) { (*l->intf)->USBInterfaceClose(l->intf); (*l->intf)->Release(l->intf); }
    if (l->dev) { (*l->dev)->USBDeviceClose(l->dev); (*l->dev)->Release(l->dev); }
    memset(l, 0, sizeof *l);
}

static int link_open(Link *l) {
    memset(l, 0, sizeof *l);
    io_service_t svc = find_adapter(AIC_MODE_WIFI, &l->id);
    if (!svc) {
        const AicId *cd = NULL;
        io_service_t s2 = find_adapter(AIC_MODE_DRIVER_CD, &cd);
        if (s2) {
            IOObjectRelease(s2);
            fprintf(stderr, "The adapter is in driver-CD mode. Run `macwifictl modeswitch` first.\n");
        } else {
            fprintf(stderr, "No AIC8800 adapter in Wi-Fi mode found. Run `macwifictl list --all`.\n");
        }
        return 3;
    }
    l->dev = device_from_service(svc);
    IOObjectRelease(svc);
    if (!l->dev) return 1;

    IOReturn kr = (*l->dev)->USBDeviceOpen(l->dev);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "USBDeviceOpen failed: 0x%08x%s\n", kr,
                kr == kIOReturnExclusiveAccess ? " (another program or driver owns the adapter)" : "");
        (*l->dev)->Release(l->dev);
        l->dev = NULL;
        return 1;
    }
    UInt8 cfg = 0;
    (*l->dev)->GetConfiguration(l->dev, &cfg);
    if (cfg == 0) {
        IOUSBConfigurationDescriptorPtr d = NULL;
        if ((*l->dev)->GetConfigurationDescriptorPtr(l->dev, 0, &d) == kIOReturnSuccess && d) {
            kr = (*l->dev)->SetConfiguration(l->dev, d->bConfigurationValue);
            if (kr != kIOReturnSuccess) fprintf(stderr, "SetConfiguration failed: 0x%08x\n", kr);
        }
    }

    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    io_iterator_t it = 0;
    if ((*l->dev)->CreateInterfaceIterator(l->dev, &req, &it) != kIOReturnSuccess) {
        fprintf(stderr, "CreateInterfaceIterator failed\n");
        link_close(l);
        return 1;
    }
    io_service_t is;
    while (!l->intf && (is = IOIteratorNext(it))) {
        IOCFPlugInInterface **plugin = NULL;
        SInt32 score = 0;
        kr = IOCreatePlugInInterfaceForService(is, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
                                               &score);
        IOObjectRelease(is);
        if (kr != KERN_SUCCESS || !plugin) continue;
        Intf intf = NULL;
        (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&intf);
        IODestroyPlugInInterface(plugin);
        if (!intf) continue;
        UInt8 cls = 0;
        (*intf)->GetInterfaceClass(intf, &cls);
        if (cls != 0xFF) { (*intf)->Release(intf); continue; }   // the Wi-Fi function is vendor-specific
        kr = (*intf)->USBInterfaceOpen(intf);
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "USBInterfaceOpen failed: 0x%08x\n", kr);
            (*intf)->Release(intf);
            continue;
        }
        l->intf = intf;
    }
    IOObjectRelease(it);
    if (!l->intf) {
        fprintf(stderr, "No vendor-specific interface found on the adapter.\n");
        link_close(l);
        return 1;
    }

    UInt8 n = 0;
    (*l->intf)->GetNumEndpoints(l->intf, &n);
    for (UInt8 p = 1; p <= n; p++) {
        UInt8 dir = 0, num = 0, type = 0, interval = 0;
        UInt16 mps = 0;
        if ((*l->intf)->GetPipeProperties(l->intf, p, &dir, &num, &type, &mps, &interval) != kIOReturnSuccess) continue;
        if (gDebug) fprintf(stderr, "pipe %u: ep %u %s type %u mps %u\n", p, num, dir == kUSBIn ? "IN" : "OUT", type, mps);
        if (type != kUSBBulk) continue;
        if (dir == kUSBIn && !l->in_pipe) l->in_pipe = p;
        else if (dir == kUSBOut && !l->out_pipe) l->out_pipe = p;
        else if (dir == kUSBOut && !l->msg_pipe) l->msg_pipe = p;
    }
    if (!l->in_pipe || !l->out_pipe) {
        fprintf(stderr, "Bulk endpoints not found (in=%u out=%u).\n", l->in_pipe, l->out_pipe);
        link_close(l);
        return 1;
    }
    return 0;
}

static int link_send(Link *l, const uint8_t *buf, size_t len) {
    UInt8 pipe = (l->id->msg_ep && l->msg_pipe) ? l->msg_pipe : l->out_pipe;
    hexdump("OUT", buf, len);
    IOReturn kr = (*l->intf)->WritePipeTO(l->intf, pipe, (void *)buf, (UInt32)len, 1000, 2000);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "bulk OUT failed: 0x%08x\n", kr);
        if (kr == kIOUSBPipeStalled) (*l->intf)->ClearPipeStallBothEnds(l->intf, pipe);
        return -1;
    }
    return 0;
}

// Waits for a message with id `want`; returns 0 and copies the packet's params into `params` (up to cap).
static int link_wait_msg(Link *l, uint16_t want, uint8_t *params, size_t cap, uint16_t *plen, int timeout_ms) {
    static uint8_t buf[16384];
    for (int waited = 0; waited < timeout_ms; waited += 500) {
        UInt32 size = sizeof buf;
        IOReturn kr = (*l->intf)->ReadPipeTO(l->intf, l->in_pipe, buf, &size, 500, 500);
        if (kr == kIOUSBTransactionTimeout || kr == kIOReturnTimeout) continue;
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "bulk IN failed: 0x%08x\n", kr);
            if (kr == kIOUSBPipeStalled) (*l->intf)->ClearPipeStallBothEnds(l->intf, l->in_pipe);
            return -1;
        }
        hexdump("IN", buf, size);
        size_t off = 0;
        AicRxPacket pk;
        int r;
        while ((r = aic_rx_next(buf, size, &off, &pk)) == 1) {
            if (!pk.is_msg) continue;
            if (pk.id == AIC_DBG_ERROR_IND) fprintf(stderr, "firmware reported DBG_ERROR_IND\n");
            if (pk.id != want) {
                if (gDebug) fprintf(stderr, "(skipping message 0x%04x)\n", pk.id);
                continue;
            }
            size_t n = pk.param_len < cap ? pk.param_len : cap;
            if (params) memcpy(params, pk.params, n);
            if (plen) *plen = pk.param_len;
            return 0;
        }
        if (r < 0) fprintf(stderr, "malformed IN transfer (%u bytes); set MACWIFI_DEBUG=1 to see it\n", size);
    }
    fprintf(stderr, "no reply (message 0x%04x) within %d ms\n", want, timeout_ms);
    return -1;
}

static int mem_read(Link *l, uint32_t addr, uint32_t *val) {
    uint8_t cmd[64], p[8];
    uint16_t pl = 0;
    size_t n = aic_build_mem_read(cmd, sizeof cmd, addr);
    if (!n || link_send(l, cmd, n) || link_wait_msg(l, AIC_DBG_MEM_READ_CFM, p, sizeof p, &pl, 2000) || pl < 8)
        return -1;
    if (aic_le32(p) != addr) fprintf(stderr, "note: reply is for 0x%08x, asked 0x%08x\n", aic_le32(p), addr);
    *val = aic_le32(p + 4);
    return 0;
}

static int mem_write(Link *l, uint32_t addr, uint32_t val) {
    uint8_t cmd[64];
    size_t n = aic_build_mem_write(cmd, sizeof cmd, addr, val);
    if (!n || link_send(l, cmd, n)) return -1;
    return link_wait_msg(l, AIC_DBG_MEM_WRITE_CFM, NULL, 0, NULL, 2000);
}

// ---------------------------------------------------------------- commands

static int cmd_list(int all) {
    ListCtx c = {all, 0, 0};
    for_each_usb_device(list_one, &c);
    if (!c.cd && !c.wifi) {
        printf("No AIC8800 adapter found.%s\n", all ? "" : " Run `macwifictl list --all` to see every USB device.");
        return 3;
    }
    return 0;
}

static int cmd_info(void) {
    const AicId *id = NULL;
    io_service_t svc = find_adapter(AIC_MODE_NONE, &id);
    if (!svc) { fprintf(stderr, "No AIC8800 adapter found.\n"); return 3; }
    Dev dev = device_from_service(svc);
    IOObjectRelease(svc);
    if (!dev) return 1;
    UInt16 vid = 0, pid = 0, rel = 0;
    UInt8 speed = 0, cfg = 0;
    (*dev)->GetDeviceVendor(dev, &vid);
    (*dev)->GetDeviceProduct(dev, &pid);
    (*dev)->GetDeviceReleaseNumber(dev, &rel);
    (*dev)->GetDeviceSpeed(dev, &speed);
    (*dev)->GetConfiguration(dev, &cfg);
    printf("%s  %04x:%04x  bcdDevice 0x%04x  %s mode\n", id->name, vid, pid, rel,
           id->mode == AIC_MODE_WIFI ? "Wi-Fi" : "driver-CD");
    printf("  speed %s, configuration %u\n",
           speed == kUSBDeviceSpeedHigh ? "high (480 Mb/s)" : speed == kUSBDeviceSpeedFull ? "FULL (12 Mb/s): too slow" : "other",
           cfg);
    IOUSBConfigurationDescriptorPtr d = NULL;
    int rc = 1;
    if ((*dev)->GetConfigurationDescriptorPtr(dev, 0, &d) == kIOReturnSuccess && d) {
        UsbConfigSummary sum;
        usb_dump_config((const uint8_t *)d, USBToHostWord(d->wTotalLength), stdout, &sum);
        rc = sum.malformed ? 1 : 0;
    }
    (*dev)->Release(dev);
    return rc;
}

static int run(const char *path, char *const argv[]) {
    pid_t pid;
    if (posix_spawn(&pid, path, NULL, NULL, argv, environ) != 0) return -1;
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int cmd_modeswitch(void) {
    const AicId *id = NULL;
    io_service_t wifi = find_adapter(AIC_MODE_WIFI, &id);
    if (wifi) {
        IOObjectRelease(wifi);
        printf("Already in Wi-Fi mode (%s).\n", id->name);
        return 0;
    }
    io_service_t svc = find_adapter(AIC_MODE_DRIVER_CD, &id);
    if (!svc) { fprintf(stderr, "No AIC8800 adapter found.\n"); return 3; }
    char bsd[64];
    prop_str(svc, CFSTR("BSD Name"), 1, bsd, sizeof bsd);
    IOObjectRelease(svc);
    if (!bsd[0]) {
        fprintf(stderr, "macOS has not created a disk for the adapter's virtual CD yet. Wait a few seconds and retry,\n"
                        "or eject it from Finder. (If it never appears, record this in docs/HARDWARE_LOG.md.)\n");
        return 1;
    }
    // Keep the whole-disk name (disk4), not a partition (disk4s1).
    char *s = strchr(bsd + 4, 's');
    if (strncmp(bsd, "disk", 4) == 0 && s) *s = '\0';
    char dev[80];
    snprintf(dev, sizeof dev, "/dev/%s", bsd);
    printf("Ejecting the adapter's driver disc (%s)...\n", dev);
    char *args[] = {"diskutil", "eject", dev, NULL};
    if (run("/usr/sbin/diskutil", args) != 0) fprintf(stderr, "diskutil eject reported an error; checking anyway.\n");
    for (int i = 0; i < 20; i++) {
        usleep(500 * 1000);
        wifi = find_adapter(AIC_MODE_WIFI, &id);
        if (wifi) {
            IOObjectRelease(wifi);
            printf("Switched: %s is now in Wi-Fi mode.\n", id->name);
            return 0;
        }
    }
    fprintf(stderr, "The adapter did not come back in Wi-Fi mode within 10 s. Run `macwifictl list --all`\n"
                    "and record what you see in docs/HARDWARE_LOG.md.\n");
    return 1;
}

static int cmd_identify(void) {
    Link l;
    int rc = link_open(&l);
    if (rc) return rc;
    uint32_t chip = 0, sub = 0;
    if (mem_read(&l, AIC_ADDR_CHIP_ID, &chip) || mem_read(&l, AIC_ADDR_CHIP_SUB_ID, &sub)) {
        fprintf(stderr, "Could not talk to the chip. Replug the adapter and retry with MACWIFI_DEBUG=1.\n");
        link_close(&l);
        return 1;
    }
    uint8_t rev = (uint8_t)(chip >> 16), subrev = (uint8_t)(sub >> 4);
    printf("%s (pipes: in %u, out %u, msg %u)\n", l.id->name, l.in_pipe, l.out_pipe, l.msg_pipe);
    printf("  [0x%08x] = 0x%08x  -> chip revision 0x%02x (%s)\n", AIC_ADDR_CHIP_ID, chip, rev, aic_chip_rev_name(rev));
    printf("  [0x%08x] = 0x%08x  -> sub revision 0x%02x\n", AIC_ADDR_CHIP_SUB_ID, sub, subrev);
    printf("  -> The ROM answered our messages. Phase 1 hardware check passed.\n");
    link_close(&l);
    return 0;
}

static void usage(void) {
    fprintf(stderr, "usage: macwifictl list [--all] | modeswitch | info | identify | memr <addr> | memw <addr> <val>\n"
                    "Supported: AICsemi AIC8800 family, e.g. UGREEN AX900 (CM762). MACWIFI_DEBUG=1 dumps USB traffic.\n");
}

int main(int argc, char **argv) {
    gDebug = getenv("MACWIFI_DEBUG") != NULL;
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return 2; }
    const char *c = argv[1];
    if (!strcmp(c, "list")) return cmd_list(argc > 2 && !strcmp(argv[2], "--all"));
    if (!strcmp(c, "info")) return cmd_info();
    if (!strcmp(c, "modeswitch")) return cmd_modeswitch();
    if (!strcmp(c, "identify")) return cmd_identify();
    if ((!strcmp(c, "memr") && argc > 2) || (!strcmp(c, "memw") && argc > 3)) {
        Link l;
        int rc = link_open(&l);
        if (rc) return rc;
        uint32_t a = (uint32_t)strtoul(argv[2], NULL, 0), v = 0;
        if (!strcmp(c, "memr")) rc = mem_read(&l, a, &v) ? 1 : (printf("[0x%08x] = 0x%08x\n", a, v), 0);
        else {
            v = (uint32_t)strtoul(argv[3], NULL, 0);
            rc = mem_write(&l, a, v) ? 1 : (printf("[0x%08x] <- 0x%08x\n", a, v), 0);
        }
        link_close(&l);
        return rc;
    }
    usage();
    return 2;
}
