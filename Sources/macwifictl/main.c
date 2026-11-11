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

#include <ctype.h>
#include <pthread.h>
#include <readpassphrase.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aic8800.h"
#include "aic_fmac.h"
#include "aic_fwload.h"
#include "aic_data.h"
#include "aic_msg.h"
#include "bt_usb.h"
#include "hci.h"
#include "bridge.h"
#include "captive.h"
#include "dhcp.h"
#include "eapol.h"
#include "ieee80211.h"
#include "wpa.h"
#include "descdump.h"
#include "feth.h"
#include "json.h"
#include "power.h"
#include "reorder.h"
#include "rx_async.h"
#include "rxq.h"
#include "sae.h"
#include "knownnet.h"

extern char **environ;

typedef IOUSBDeviceInterface320 **Dev;
typedef IOUSBInterfaceInterface300 **Intf;

#ifndef MW_VERSION
#define MW_VERSION "dev"
#endif

static int gDebug = 0;
static int gNoDump = 0;           // set while key material is on the wire, so MACWIFI_DEBUG=1 never prints keys
static size_t gDumpLimit = 256;   // bytes shown per transfer with MACWIFI_DEBUG=1 (lowered during uploads)

static void hexdump(const char *tag, const uint8_t *p, size_t n) {
    if (gNoDump) {
        if (gDebug) fprintf(stderr, "%s %zu bytes (contents hidden: key material)\n", tag, n);
        return;
    }
    if (!gDebug) return;
    fprintf(stderr, "%s (%zu bytes):", tag, n);
    for (size_t i = 0; i < n && i < gDumpLimit; i++) fprintf(stderr, "%s%02x", (i % 16) ? " " : "\n  ", p[i]);
    fprintf(stderr, "%s\n", n > gDumpLimit ? "\n  ..." : "");
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
    UInt8 out_ep;                         // endpoint number behind out_pipe (data must go to bulk OUT 0x01)
    RxQ *rxq;                             // when set, bulk IN is read asynchronously into this queue
    RxAsync *rx;
    const AicId *id;
    int dev_open;                         // 0 when macOS's composite-device driver holds the device
} Link;

// Starts the asynchronous bulk-IN reader (several reads in flight; rx_async.c). Used once the radio is up and
// everything goes through pump(); before that (firmware load, bring-up) reads are synchronous.
static void link_rx_start(Link *l) {
    if (l->rx) return;
    l->rxq = rxq_new(8192, 64u << 20);
    l->rx = l->rxq ? rx_async_start(l->intf, l->in_pipe, l->rxq) : NULL;
    if (!l->rx) { rxq_free(l->rxq); l->rxq = NULL; }
}

static void link_rx_stop(Link *l) {
    if (!l->rx) return;
    rx_async_stop(l->rx);
    RxQStats s;
    rxq_stats(l->rxq, &s);
    printf("RX: %lu transfers, deepest queue %zu, dropped %lu\n", s.pushed, s.max_depth, s.dropped);
    rxq_free(l->rxq);
    l->rx = NULL;
    l->rxq = NULL;
}

static void link_close(Link *l) {
    link_rx_stop(l);
    if (l->intf) { (*l->intf)->USBInterfaceClose(l->intf); (*l->intf)->Release(l->intf); }
    if (l->dev) {
        if (l->dev_open) (*l->dev)->USBDeviceClose(l->dev);
        (*l->dev)->Release(l->dev);
    }
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
    if (kr == kIOReturnSuccess) {
        l->dev_open = 1;
    } else if (kr == kIOReturnExclusiveAccess) {
        // Multi-interface devices (a69c:8d81 has Bluetooth + Wi-Fi) are held by macOS's composite driver, which
        // has already configured them; the Wi-Fi interface can still be opened on its own.
        if (gDebug) fprintf(stderr, "USBDeviceOpen: exclusive access (composite device); opening the interface only\n");
    } else {
        fprintf(stderr, "USBDeviceOpen failed: 0x%08x\n", kr);
        (*l->dev)->Release(l->dev);
        l->dev = NULL;
        return 1;
    }
    UInt8 cfg = 0;
    (*l->dev)->GetConfiguration(l->dev, &cfg);
    if (cfg == 0 && l->dev_open) {
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
        else if (dir == kUSBOut && !l->out_pipe) { l->out_pipe = p; l->out_ep = num; }
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
            if ((pk.type & 0x7f) == AIC_USB_TYPE_CFG_PRINT) {   // firmware log line
                size_t tl = pk.packet[0] | (pk.packet[1] << 8);
                const char *t = (const char *)pk.packet + 4;
                while (tl && (t[tl - 1] == '\n' || t[tl - 1] == '\r' || t[tl - 1] == 0)) tl--;
                fprintf(stderr, "  [firmware] %.*s\n", (int)tl, t);
                continue;
            }
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

static int mem_mask_write(Link *l, uint32_t addr, uint32_t mask, uint32_t val) {
    uint8_t cmd[64];
    size_t n = aic_build_mem_mask_write(cmd, sizeof cmd, addr, mask, val);
    if (!n || link_send(l, cmd, n)) return -1;
    return link_wait_msg(l, AIC_DBG_MEM_MASK_WRITE_CFM, NULL, 0, NULL, 2000);
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


// ---------------------------------------------------------------- Phase 2: firmware upload

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static uint8_t *read_file(const char *dir, const char *name, size_t *len) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Missing firmware file %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (n > 0 && n < 16 * 1024 * 1024) ? malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "Could not read %s\n", path);
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

// rwnx_plat_bin_fw_upload_android: DBG_MEM_BLOCK_WRITE in 1 KiB chunks, each confirmed by the ROM.
static int upload(Link *l, const char *dir, const char *name, uint32_t addr) {
    size_t len = 0;
    uint8_t *data = read_file(dir, name, &len);
    if (!data) return -1;
    printf("  %-32s %7zu bytes -> 0x%08x ", name, len, addr);
    fflush(stdout);
    double t0 = now_s();
    size_t saved = gDumpLimit;
    gDumpLimit = 32;
    uint8_t cmd[16 + 8 + AIC_BLOCK_WRITE_MAX];
    int rc = 0;
    uint32_t chunks = 0;
    for (uint32_t off = 0, c; (c = aic_upload_chunk((uint32_t)len, off)) != 0; off += c, chunks++) {
        size_t n = aic_build_block_write(cmd, sizeof cmd, addr + off, data + off, c);
        if (!n || link_send(l, cmd, n) || link_wait_msg(l, AIC_DBG_MEM_BLOCK_WRITE_CFM, NULL, 0, NULL, 2000)) {
            fprintf(stderr, "\nUpload of %s failed at offset %u (address 0x%08x).\n", name, off, addr + off);
            uint32_t v = 0;
            if (mem_read(l, AIC_ADDR_CHIP_ID, &v) == 0)
                fprintf(stderr, "The ROM still answers ([0x%08x] = 0x%08x), so it refused or lost that chunk.\n",
                        AIC_ADDR_CHIP_ID, v);
            else
                fprintf(stderr, "The ROM no longer answers at all.\n");
            rc = -1;
            break;
        }
        if (chunks % 32 == 31) { putchar('.'); fflush(stdout); }
    }
    gDumpLimit = saved;
    if (!rc) printf(" ok (%u chunks, %.1f s)\n", chunks, now_s() - t0);
    free(data);
    return rc;
}

static int write_ops(Link *l, const AicMemOp *ops, int n) {
    for (int i = 0; i < n; i++) {
        int bad = ops[i].mask ? mem_mask_write(l, ops[i].addr, ops[i].mask, ops[i].value)
                              : mem_write(l, ops[i].addr, ops[i].value);
        if (bad) {
            fprintf(stderr, "Write [0x%08x] <- 0x%08x (mask 0x%08x) failed.\n", ops[i].addr, ops[i].value, ops[i].mask);
            return -1;
        }
        if (ops[i].delay_ms_after) usleep(ops[i].delay_ms_after * 1000);
    }
    return 0;
}

// Reads a word of the uploaded image and checks it against the file, so a bad upload stops before START_APP.
static int read_check(Link *l, const uint8_t *img, size_t img_len, uint32_t ofs, uint32_t *val) {
    if (mem_read(l, AIC_D80_RAM_FMAC_FW_ADDR + ofs, val)) return -1;
    if (ofs + 4 <= img_len && aic_le32(img + ofs) != *val) {
        fprintf(stderr, "Read-back mismatch at 0x%08x: chip has 0x%08x, file has 0x%08x. Not starting firmware.\n",
                AIC_D80_RAM_FMAC_FW_ADDR + ofs, *val, aic_le32(img + ofs));
        return -1;
    }
    return 0;
}

static const char *opt(int argc, char **argv, const char *name) {
    for (int i = 2; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static int have_fw(const char *dir) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, AIC_D80_FW_PATCH_TABLE);
    return access(p, R_OK) == 0;
}

typedef struct {
    const char *dir;        // --fw DIR or MACWIFI_FW; NULL = pick by profile
    int profile;            // --profile v5|legacy; -1 = pick from the chip
} FwOpts;

static int fwload_steps(Link *l, const FwOpts *o) {
    // 1. Chip revision, loader profile and system config (system_config_8800d80).
    uint32_t chip = 0;
    if (mem_read(l, AIC_ADDR_CHIP_ID, &chip)) return 1;
    uint8_t rev = (uint8_t)(chip >> 16);
    int h = (rev & AIC_D80_CHIP_ID_H_MASK) == AIC_D80_CHIP_ID_H_MASK;
    int mcu = aic_d80_is_mcu1(chip);
    printf("Chip: [0x%08x] = 0x%08x -> revision 0x%02x (%s), product 0x%04x, chip_mcu_id %d%s\n", AIC_ADDR_CHIP_ID,
           chip, rev, aic_chip_rev_name(rev), chip & AIC_D80_PRODUCT_MASK, mcu, h ? ", H variant" : "");
    if (rev != AIC_CHIP_REV_U02 && rev != AIC_CHIP_REV_U03) {
        fprintf(stderr, "Only U02/U03 AIC8800D80 chips are supported so far (the _u02 firmware set). Record this in\n"
                        "docs/HARDWARE_LOG.md.\n");
        return 1;
    }
    AicD80Profile prof = o->profile >= 0 ? (AicD80Profile)o->profile : aic_d80_choose_profile(chip);
    const char *dir = o->dir ? o->dir : (prof == AIC_D80_PROFILE_LEGACY_MCU1 ? AIC_D80_FW_DIR_LEGACY : AIC_D80_FW_DIR_V5);
    printf("Loader profile: %s%s, firmware from %s\n", aic_d80_profile_name(prof), o->profile >= 0 ? " (forced)" : "",
           dir);
    if (!have_fw(dir)) {
        fprintf(stderr, "Firmware not found in %s. Run scripts/fetch-firmware.sh (it downloads both sets).\n", dir);
        return 4;
    }
    if (h && prof == AIC_D80_PROFILE_LEGACY_MCU1) {
        fprintf(stderr, "The legacy-mcu1 firmware set has no H-variant image. Record this in docs/HARDWARE_LOG.md.\n");
        return 1;
    }
    if (mcu) {
        uint32_t v = 0;
        if (mem_read(l, AIC_D80_CACHE_CTRL_ADDR, &v) || mem_write(l, AIC_D80_CACHE_CTRL_ADDR, v | 1u)) return 1;
        printf("System config: [0x%08x] 0x%08x -> 0x%08x (MCU1 cache fix)\n", AIC_D80_CACHE_CTRL_ADDR, v, v | 1u);
    }
    {
        AicMemOp sc[8];
        int nsc = aic_d80_syscfg_writes(prof, chip, sc, 8);
        if (nsc < 0 || write_ops(l, sc, nsc)) return 1;
        if (nsc) printf("System config: %d product-0x%04x writes\n", nsc, chip & AIC_D80_PRODUCT_MASK);
    }

    // 2. Patch table.
    size_t tlen = 0;
    uint8_t *table = read_file(dir, AIC_D80_FW_PATCH_TABLE, &tlen);
    if (!table) return 4;
    AicPatchTable pt;
    if (aic_pt_parse(table, tlen, AIC_D80_RAM_ADID_ADDR, AIC_D80_RAM_PATCH_ADDR, &pt)) {
        fprintf(stderr, "%s is not a valid patch table.\n", AIC_D80_FW_PATCH_TABLE);
        free(table);
        return 4;
    }
    printf("Patch table: %d records, ADID -> 0x%08x, patch -> 0x%08x, %u ext patch(es), version \"%s\"\n", pt.n,
           pt.addr_adid, pt.addr_patch, pt.ext_patch_nb, pt.version);

    // 3. ROM patches.
    printf("Uploading:\n");
    int rc = 1;
    AicMemOp ops[512];
    int nops = 0;
    if (upload(l, dir, AIC_D80_FW_ADID, pt.addr_adid) || upload(l, dir, AIC_D80_FW_PATCH, pt.addr_patch)) goto out;
    for (uint32_t i = 0; i < pt.ext_patch_nb; i++) {
        uint32_t id = 0, addr = 0;
        char name[64];
        if (aic_pt_ext_patch(&pt, i, &id, &addr)) goto out;
        snprintf(name, sizeof name, AIC_D80_FW_PATCH_EXT_FMT, id);
        if (upload(l, dir, name, addr)) goto out;
    }
    nops = aic_pt_writes(&pt, ops, sizeof ops / sizeof ops[0]);
    if (nops < 0) { fprintf(stderr, "Patch table has an unexpected BT-mode record.\n"); goto out; }
    printf("  patch table: %d register writes ", nops);
    fflush(stdout);
    if (write_ops(l, ops, nops)) goto out;
    printf("ok\n");

    // 4. Wi-Fi firmware.
    const char *fmac = h ? AIC_D80_FMACFW_H : AIC_D80_FMACFW;
    if (upload(l, dir, fmac, AIC_D80_RAM_FMAC_FW_ADDR)) goto out;

    // 5. Patch config (aicwf_patch_config_8800d80), reading pointers back from the uploaded image.
    {
        size_t ilen = 0;
        uint8_t *img = read_file(dir, fmac, &ilen);
        if (!img) goto out;
        uint32_t version = 0, config_base = 0, patch_str = 0, patch_buff = 0;
        int bad = read_check(l, img, ilen, AIC_D80_FW_OFS_VERSION, &version) ||
                  read_check(l, img, ilen, AIC_D80_FW_OFS_CONFIG_BASE, &config_base) ||
                  read_check(l, img, ilen, AIC_D80_FW_OFS_PATCH_STR, &patch_str) ||
                  (aic_d80_needs_patch_buff(prof, version) &&
                   read_check(l, img, ilen, AIC_D80_FW_OFS_PATCH_BUFF, &patch_buff));
        free(img);
        if (bad) goto out;
        if (version)
            printf("Firmware version 0x%08x (%u.%u.%u.%u); read-back matches the file.\n", version, version >> 24,
                   (version >> 16) & 0xff, (version >> 8) & 0xff, version & 0xff);
        else
            printf("Firmware read-back matches the file (this image has no version word).\n");
        nops = aic_d80_patch_config_writes(prof, config_base, patch_str, version, patch_buff, ops,
                                           sizeof ops / sizeof ops[0]);
        if (nops < 0 || write_ops(l, ops, nops)) goto out;
        printf("Patch config: %d writes (config base 0x%08x, patch struct 0x%08x, pairs at 0x%08x)\n", nops,
               config_base, patch_str, ops[2].value);
    }

    // 6. Start the firmware. The ROM does not confirm this; the chip reboots into the firmware.
    {
        uint8_t cmd[64];
        size_t n = aic_build_start_app(cmd, sizeof cmd, AIC_D80_RAM_FMAC_FW_ADDR, AIC_HOST_START_APP_AUTO);
        if (!n) goto out;
        // The chip may reset before the transfer completes, so a send error here is not a failure by itself;
        // the re-enumeration check below decides.
        if (link_send(l, cmd, n)) printf("START_APP sent; the adapter dropped the transfer (it may be restarting).\n");
        else printf("START_APP sent (boot 0x%08x).\n", AIC_D80_RAM_FMAC_FW_ADDR);
    }
    rc = 0;
out:
    free(table);
    return rc;
}

static int fwload_impl(int argc, char **argv, int show_info) {
    FwOpts o = {opt(argc, argv, "--fw"), -1};
    if (!o.dir) {
        const char *e = getenv("MACWIFI_FW");
        if (e && *e) o.dir = e;
    }
    const char *pf = opt(argc, argv, "--profile");
    if (pf) {
        if (!strcmp(pf, "v5")) o.profile = AIC_D80_PROFILE_V5;
        else if (!strcmp(pf, "legacy")) o.profile = AIC_D80_PROFILE_LEGACY_MCU1;
        else { fprintf(stderr, "--profile must be v5 or legacy\n"); return 2; }
    }
    if (o.dir ? !have_fw(o.dir) : (!have_fw(AIC_D80_FW_DIR_V5) && !have_fw(AIC_D80_FW_DIR_LEGACY))) {
        fprintf(stderr, "Firmware not found in %s. Run scripts/fetch-firmware.sh first.\n",
                o.dir ? o.dir : "firmware/");
        return 4;
    }
    const AicId *id = NULL;
    io_service_t svc = find_adapter(AIC_MODE_WIFI, &id);
    if (svc) {
        IOObjectRelease(svc);
        if (id->pid == AIC_PID_D80_FW) {
            printf("The adapter is already running firmware (a69c:8d81). To start over, unplug it, plug it back in and\n"
                   "run `macwifictl modeswitch` again.\n");
            return 0;
        }
        if (id->pid != AIC_PID_D80_ROM) {
            fprintf(stderr, "fwload supports the AIC8800D80 (a69c:8d80) only; found %s.\n", id->name);
            return 1;
        }
    }
    Link l;
    int rc = link_open(&l);
    if (rc) return rc;
    double t0 = now_s();
    rc = fwload_steps(&l, &o);
    link_close(&l);
    if (rc) {
        fprintf(stderr, "Firmware load stopped. Unplug and replug the adapter to reset it (then `modeswitch`).\n");
        return rc;
    }

    // 7. Watch the adapter re-enumerate. Linux expects it back as a69c:8d81 (see aic8800.h).
    printf("Waiting for the adapter to restart with its firmware");
    fflush(stdout);
    int gone = 0;
    for (int i = 0; i < 40; i++) {
        usleep(250 * 1000);
        const AicId *now = NULL;
        io_service_t s = find_adapter(AIC_MODE_NONE, &now);
        if (s) IOObjectRelease(s);
        if (!s || now->pid != AIC_PID_D80_ROM) gone = 1;
        if (s && now->pid != AIC_PID_D80_ROM) {
            printf("\nThe adapter came back as %04x:%04x (%s) %.1f s after the upload started.\n", now->vid, now->pid,
                   now->name, now_s() - t0);
            if (show_info) {
                printf("\n");
                cmd_info();
                printf("\nPhase 2a hardware check %s.\n",
                       now->pid == AIC_PID_D80_FW ? "passed" : "finished (unexpected ID: record it)");
            }
            return now->pid == AIC_PID_D80_FW ? 0 : 1;
        }
        if (i % 4 == 3) { putchar('.'); fflush(stdout); }
    }
    printf("\n%s\n", gone ? "The adapter disconnected and has not come back within 10 s. Run `macwifictl list --all`."
                          : "The adapter is still a69c:8d80 after 10 s (no restart seen). Run `macwifictl list --all`.");
    return 1;
}

static int cmd_fwload(int argc, char **argv) { return fwload_impl(argc, argv, 1); }

// ---------------------------------------------------------------- Phase 2b: FullMAC bring-up

// Sends one firmware message and waits for its confirmation. `out` receives up to `cap` parameter bytes.
static int fmac_call(Link *l, uint16_t id, const void *params, uint16_t plen, uint16_t cfm, uint8_t *out, size_t cap,
                     uint16_t *olen, int timeout_ms) {
    uint8_t cmd[16 + 256];
    size_t n = aic_build_cmd(cmd, sizeof cmd, id, (uint16_t)(id >> 10), params, plen);
    if (!n || link_send(l, cmd, n)) return -1;
    return link_wait_msg(l, cfm, out, cap, olen, timeout_ms);
}

static int fmac_step(const char *what, int rc) {
    if (rc) fprintf(stderr, "%s: no confirmation from the firmware.\n", what);
    return rc;
}

static int read_text(const char *dir, const char *name, char **buf, size_t *len) {
    *buf = (char *)read_file(dir, name, len);
    return *buf ? 0 : -1;
}

static int bringup(Link *l, const FwOpts *o, uint8_t mac_out[6]) {
    uint8_t p[128], r[128];
    uint16_t rl = 0;

    // system_config_8800d80 (fdrv): the running firmware still answers debug reads.
    uint32_t chip = 0;
    if (mem_read(l, AIC_ADDR_CHIP_ID, &chip)) {
        fprintf(stderr, "The firmware does not answer. Unplug and replug the adapter, then run `macwifictl up` again.\n");
        return 1;
    }
    AicD80Profile prof = o->profile >= 0 ? (AicD80Profile)o->profile : aic_d80_choose_profile(chip);
    const char *dir = o->dir ? o->dir : (prof == AIC_D80_PROFILE_LEGACY_MCU1 ? AIC_D80_FW_DIR_LEGACY : AIC_D80_FW_DIR_V5);
    printf("Firmware answers: [0x%08x] = 0x%08x (profile %s)\n", AIC_ADDR_CHIP_ID, chip, aic_d80_profile_name(prof));

    // rwnx_plat_userconfig_load_8800d80: transmit-power tables and crystal trim from the user-config file.
    AicUserConfig uc;
    aic_userconfig_defaults(&uc);
    char *txt = NULL;
    size_t tlen = 0;
    if (read_text(dir, "aic_userconfig_8800d80.txt", &txt, &tlen) == 0) {
        int nkeys = aic_userconfig_parse(&uc, txt, tlen);
        printf("User config: %d settings from %s/aic_userconfig_8800d80.txt\n", nkeys, dir);
        free(txt);
    } else {
        printf("User config: file not found, using the driver defaults\n");
    }

    // MM_SET_STACK_START(on, efuse_valid 0, set_vendor_info BIT(5), fw trace off).
    AicStackStartCfm ss = {0, 0};
    aic_fmac_stack_start_params(p, 1, 0, AIC_D80_STACK_SET_VENDOR_INFO, 0);
    if (fmac_step("MM_SET_STACK_START",
                  fmac_call(l, AIC_MM_SET_STACK_START_REQ, p, AIC_SZ_STACK_START_REQ, AIC_MM_SET_STACK_START_CFM, r,
                            sizeof r, &rl, 5000)) ||
        aic_fmac_parse_stack_start_cfm(r, rl, &ss))
        return 1;
    printf("Stack started: 5 GHz %s, vendor info 0x%02x\n", ss.is_5g_support ? "supported" : "NOT supported",
           ss.vendor_info);

    char ver[64] = "";
    memset(p, 0, AIC_SZ_GET_FW_VER_REQ);
    if (fmac_step("MM_GET_FW_VERSION", fmac_call(l, AIC_MM_GET_FW_VERSION_REQ, p, AIC_SZ_GET_FW_VER_REQ,
                                                 AIC_MM_GET_FW_VERSION_CFM, r, sizeof r, &rl, 2000)))
        return 1;
    aic_fmac_parse_fw_version_cfm(r, rl, ver, sizeof ver);
    printf("Firmware version: %s\n", ver[0] ? ver : "(empty)");

    // aicwf_set_rf_config_8800d80: each power message is sent only if the user config enables it.
    size_t n = aic_fmac_txpwr_lvl_v3_params(&uc, p);
    if (n && fmac_step("MM_SET_TXPWR_IDX_LVL", fmac_call(l, AIC_MM_SET_TXPWR_IDX_LVL_REQ, p, (uint16_t)n,
                                                         AIC_MM_SET_TXPWR_IDX_LVL_CFM, NULL, 0, NULL, 2000)))
        return 1;
    printf("TX power levels: %s\n", n ? "sent" : "disabled in the user config");
    n = aic_fmac_txpwr_lvl_adj_params(&uc, p);
    if (n && fmac_step("MM_SET_TXPWR_LVL_ADJ", fmac_call(l, AIC_MM_SET_TXPWR_LVL_ADJ_REQ, p, (uint16_t)n,
                                                         AIC_MM_SET_TXPWR_LVL_ADJ_CFM, NULL, 0, NULL, 2000)))
        return 1;
    if (n) printf("TX power adjust: sent\n");
    n = aic_fmac_txpwr_ofst2x_params(&uc, p);
    if (n && fmac_step("MM_SET_TXPWR_OFST", fmac_call(l, AIC_MM_SET_TXPWR_OFST_REQ, p, (uint16_t)n,
                                                      AIC_MM_SET_TXPWR_OFST_CFM, NULL, 0, NULL, 2000)))
        return 1;
    if (n) printf("TX power offsets: sent\n");
    AicRfCalibCfm cal;
    n = aic_fmac_rf_calib_params(&uc, p);
    if (fmac_step("MM_SET_RF_CALIB", fmac_call(l, AIC_MM_SET_RF_CALIB_REQ, p, (uint16_t)n, AIC_MM_SET_RF_CALIB_CFM, r,
                                               sizeof r, &rl, 5000)) ||
        aic_fmac_parse_rf_calib_cfm(r, rl, &cal))
        return 1;
    printf("RF calibration done (rx gain tables 0x%08x / 0x%08x, tx 0x%08x / 0x%08x)\n", cal.rxgain_24g_addr,
           cal.rxgain_5g_addr, cal.txgain_24g_addr, cal.txgain_5g_addr);

    uint8_t mac[6];
    aic_fmac_get_mac_params(p);
    if (fmac_step("MM_GET_MAC_ADDR", fmac_call(l, AIC_MM_GET_MAC_ADDR_REQ, p, AIC_SZ_GET_MAC_ADDR_REQ,
                                               AIC_MM_GET_MAC_ADDR_CFM, r, sizeof r, &rl, 2000)) ||
        aic_fmac_parse_mac_cfm(r, rl, mac))
        return 1;
    printf("MAC address: %02x:%02x:%02x:%02x:%02x:%02x%s\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           aic_fmac_mac_valid(mac) ? "" : "  (none in efuse; Linux would use a random one)");
    if (mac_out) memcpy(mac_out, mac, 6);

    if (fmac_step("MM_RESET", fmac_call(l, AIC_MM_RESET_REQ, NULL, 0, AIC_MM_RESET_CFM, NULL, 0, NULL, 2000))) return 1;
    AicVersionCfm vc;
    if (fmac_step("MM_VERSION", fmac_call(l, AIC_MM_VERSION_REQ, NULL, 0, AIC_MM_VERSION_CFM, r, sizeof r, &rl, 2000)) ||
        aic_fmac_parse_version_cfm(r, rl, &vc))
        return 1;
    printf("MAC reset; LMAC 0x%08x, MAC HW 0x%08x/0x%08x, PHY 0x%08x/0x%08x, features 0x%08x, %u stations, %u "
           "interfaces\n", vc.version_lmac, vc.version_machw_1, vc.version_machw_2, vc.version_phy_1,
           vc.version_phy_2, vc.features, vc.max_sta_nb, vc.max_vif_nb);
    return 0;
}

// modeswitch + fwload as needed, then open the Wi-Fi interface and run the bring-up. On success `l` stays open.
static int up_link(int argc, char **argv, Link *l, uint8_t mac[6]) {
    const AicId *id = NULL;
    io_service_t svc = find_adapter(AIC_MODE_NONE, &id);
    if (!svc) { fprintf(stderr, "No AIC8800 adapter found.\n"); return 3; }
    IOObjectRelease(svc);
    if (id->mode == AIC_MODE_DRIVER_CD) {
        int rc = cmd_modeswitch();
        if (rc) return rc;
        io_service_t s2 = find_adapter(AIC_MODE_WIFI, &id);
        if (s2) IOObjectRelease(s2);
    }
    if (id && id->pid != AIC_PID_D80_FW) {
        int rc = fwload_impl(argc, argv, 0);
        if (rc) return rc;
        usleep(300 * 1000);   // let IOKit finish publishing the new device's interfaces
    } else {
        printf("Firmware already running (a69c:8d81). If a step fails, unplug and replug the adapter for a clean "
               "start.\n");
    }
    FwOpts o = {opt(argc, argv, "--fw"), -1};
    const char *pf = opt(argc, argv, "--profile");
    if (pf) o.profile = !strcmp(pf, "legacy") ? AIC_D80_PROFILE_LEGACY_MCU1 : AIC_D80_PROFILE_V5;
    int rc = 1;
    for (int i = 0; i < 10 && rc; i++) {   // the interface can take a moment to appear after re-enumeration
        rc = link_open(l);
        if (rc) usleep(300 * 1000);
    }
    if (rc) return rc;
    printf("Wi-Fi interface: pipes in %u, out %u, msg %u\n", l->in_pipe, l->out_pipe, l->msg_pipe);
    rc = bringup(l, &o, mac);
    if (rc) link_close(l);
    return rc;
}

static int cmd_up(int argc, char **argv) {
    Link l;
    uint8_t mac[6];
    int rc = up_link(argc, argv, &l, mac);
    if (!rc) link_close(&l);
    printf("%s\n", rc ? "Bring-up stopped. Unplug and replug the adapter to reset it." : "Phase 2b hardware check passed.");
    return rc;
}

// ---------------------------------------------------------------- Phase 3a: scan

typedef struct {
    WifiBss bss;
    int rssi, freq;
    int seen;
} ScanEntry;

typedef struct {
    ScanEntry e[256];
    int n, frames, unparsed;
} ScanList;

static int mhz_to_channel(int mhz) {
    if (mhz == 2484) return 14;
    if (mhz >= 2412 && mhz <= 2472) return (mhz - 2407) / 5;
    if (mhz >= 5000 && mhz <= 5900) return (mhz - 5000) / 5;
    return 0;
}

static void scan_add(ScanList *sl, const AicScanResult *r) {
    sl->frames++;
    WifiBss b;
    if (wifi_parse_bss(r->frame, r->frame_len, &b) != 0) { sl->unparsed++; return; }
    for (int i = 0; i < sl->n; i++) {
        if (memcmp(sl->e[i].bss.bssid, b.bssid, 6) != 0) continue;
        sl->e[i].seen++;
        if (r->rssi > sl->e[i].rssi) sl->e[i].rssi = r->rssi;
        if (!sl->e[i].bss.ssid_len && b.ssid_len) sl->e[i].bss = b;   // probe response revealing a hidden SSID
        return;
    }
    if (sl->n >= (int)(sizeof sl->e / sizeof sl->e[0])) return;
    ScanEntry *e = &sl->e[sl->n++];
    e->bss = b;
    e->rssi = r->rssi;
    e->freq = r->center_freq;
    e->seen = 1;
}

static int by_rssi(const void *a, const void *b) {
    return ((const ScanEntry *)b)->rssi - ((const ScanEntry *)a)->rssi;
}

// Reads messages until `end_id` arrives, handing SCANU_RESULT_IND to the list. Returns 0 and the end message's
// parameters, or -1 on timeout / USB error.
static int collect_scan(Link *l, ScanList *sl, uint16_t end_id, uint8_t *params, size_t cap, uint16_t *plen,
                        int timeout_ms, int *ack_seen) {
    static uint8_t buf[16384];
    double t0 = now_s();
    while ((now_s() - t0) * 1000 < timeout_ms) {
        UInt32 size = sizeof buf;
        IOReturn kr = (*l->intf)->ReadPipeTO(l->intf, l->in_pipe, buf, &size, 500, 500);
        if (kr == kIOUSBTransactionTimeout || kr == kIOReturnTimeout) continue;
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "bulk IN failed: 0x%08x\n", kr);
            return -1;
        }
        hexdump("IN", buf, size);
        size_t off = 0;
        AicRxPacket pk;
        while (aic_rx_next(buf, size, &off, &pk) == 1) {
            if ((pk.type & 0x7f) == AIC_USB_TYPE_CFG_PRINT) {
                size_t tl = pk.packet[0] | (pk.packet[1] << 8);
                const char *t = (const char *)pk.packet + 4;
                while (tl && (t[tl - 1] == '\n' || t[tl - 1] == '\r' || t[tl - 1] == 0)) tl--;
                fprintf(stderr, "  [firmware] %.*s\n", (int)tl, t);
                continue;
            }
            if (!pk.is_msg) continue;
            if (pk.id == AIC_SCANU_RESULT_IND) {
                AicScanResult r;
                if (aic_fmac_parse_scan_result(pk.params, pk.param_len, &r) == 0) scan_add(sl, &r);
                else sl->unparsed++;
                continue;
            }
            if (pk.id == AIC_SCANU_START_CFM_ADDTIONAL) {   // the request's acknowledgement
                if (ack_seen) *ack_seen = 1;
                continue;
            }
            if (pk.id == end_id) {
                size_t n = pk.param_len < cap ? pk.param_len : cap;
                if (params) memcpy(params, pk.params, n);
                if (plen) *plen = pk.param_len;
                return 0;
            }
            if (gDebug) fprintf(stderr, "(skipping message 0x%04x)\n", pk.id);
        }
    }
    fprintf(stderr, "no end-of-scan message (0x%04x) within %d ms\n", end_id, timeout_ms);
    return -1;
}

static int radio_start(Link *l, const uint8_t mac[6], uint8_t *vif) {
    uint8_t p[512], r[64];
    uint16_t rl = 0;
    size_t n = aic_fmac_me_config_params(p);
    if (fmac_step("ME_CONFIG", fmac_call(l, AIC_ME_CONFIG_REQ, p, (uint16_t)n, AIC_ME_CONFIG_CFM, NULL, 0, NULL, 2000)))
        return 1;
    n = aic_fmac_me_chan_config_params(p);
    if (fmac_step("ME_CHAN_CONFIG", fmac_call(l, AIC_ME_CHAN_CONFIG_REQ, p, (uint16_t)n, AIC_ME_CHAN_CONFIG_CFM, NULL,
                                              0, NULL, 2000)))
        return 1;
    printf("Capabilities and channels set (%d at 2.4 GHz, %d at 5 GHz, world regulatory domain)\n",
           aic_fmac_channel_count_2g4(), aic_fmac_channel_count_5g());
    n = aic_fmac_start_params(p);
    if (fmac_step("MM_START", fmac_call(l, AIC_MM_START_REQ, p, (uint16_t)n, AIC_MM_START_CFM, NULL, 0, NULL, 5000)))
        return 1;
    n = aic_fmac_coex_params(p);
    if (fmac_step("MM_SET_COEX", fmac_call(l, AIC_MM_SET_COEX_REQ, p, (uint16_t)n, AIC_MM_SET_COEX_CFM, NULL, 0,
                                           NULL, 2000)))
        return 1;
    n = aic_fmac_add_if_params(p, mac);
    uint8_t status = 0xff;
    if (fmac_step("MM_ADD_IF", fmac_call(l, AIC_MM_ADD_IF_REQ, p, (uint16_t)n, AIC_MM_ADD_IF_CFM, r, sizeof r, &rl,
                                         2000)) ||
        aic_fmac_parse_add_if_cfm(r, rl, &status, vif))
        return 1;
    if (status != 0) {
        fprintf(stderr, "MM_ADD_IF refused (status %u).\n", status);
        return 1;
    }
    printf("Radio started; station interface %u\n", *vif);
    return 0;
}

static int cmd_scan(int argc, char **argv) {
    const char *ssid = (argc > 2 && argv[2][0] != '-') ? argv[2] : NULL;
    Link l;
    uint8_t mac[6], vif = 0;
    int rc = up_link(argc, argv, &l, mac);
    if (rc) return rc;
    rc = radio_start(&l, mac, &vif);
    if (!rc) {
        static ScanList sl;
        memset(&sl, 0, sizeof sl);
        uint8_t p[AIC_SZ_SCANU_START_REQ], r[16];
        uint16_t rl = 0;
        size_t n = aic_fmac_scan_params(p, vif, ssid);
        uint8_t cmd[16 + AIC_SZ_SCANU_START_REQ];
        size_t cl = aic_build_cmd(cmd, sizeof cmd, AIC_SCANU_START_REQ, AIC_SCANU_START_REQ >> 10, p, (uint16_t)n);
        printf("Scanning %d channels%s%s%s...\n", aic_fmac_channel_count_2g4() + aic_fmac_channel_count_5g(),
               ssid ? " for \"" : "", ssid ? ssid : "", ssid ? "\"" : "");
        fflush(stdout);
        double t0 = now_s();
        AicScanCfm sc = {0, 0xff, 0};
        // SCANU_START_CFM_ADDTIONAL acknowledges the request; results follow; SCANU_START_CFM ends the scan.
        int ack = 0;
        if (!cl || link_send(&l, cmd, cl) ||
            collect_scan(&l, &sl, AIC_SCANU_START_CFM, r, sizeof r, &rl, 20000, &ack) ||
            aic_fmac_parse_scan_cfm(r, rl, &sc)) {
            if (sl.frames) printf("(%d scan frames arrived before the stop)\n", sl.frames);
            rc = 1;
        } else {
            if (!ack) printf("(note: no SCANU_START_CFM_ADDTIONAL acknowledgement was seen)\n");
            qsort(sl.e, (size_t)sl.n, sizeof sl.e[0], by_rssi);
            printf("Scan done in %.1f s: status %u, firmware counted %u results; %d frames, %d networks%s\n\n",
                   now_s() - t0, sc.status, sc.result_cnt, sl.frames, sl.n, sl.unparsed ? " (some frames unparsed)" : "");
            printf("  %-32s  %-17s  %4s  %5s  %4s  %-10s %s\n", "SSID", "BSSID", "CH", "BAND", "RSSI", "SECURITY", "PHY");
            for (int i = 0; i < sl.n; i++) {
                const ScanEntry *e = &sl.e[i];
                int ch = e->bss.channel ? e->bss.channel : mhz_to_channel(e->freq);
                char name[40];
                int blank = 1;   // hidden networks send an empty SSID or one made of NUL bytes
                for (int k = 0; k < e->bss.ssid_len; k++) if (e->bss.ssid[k]) blank = 0;
                if (!blank) snprintf(name, sizeof name, "%s", e->bss.ssid);
                else snprintf(name, sizeof name, "(hidden)");
                printf("  %-32s  %02x:%02x:%02x:%02x:%02x:%02x  %4d  %5s  %4d  %-10s %s%s\n", name, e->bss.bssid[0],
                       e->bss.bssid[1], e->bss.bssid[2], e->bss.bssid[3], e->bss.bssid[4], e->bss.bssid[5], ch,
                       e->freq >= 5000 ? "5G" : "2.4G", e->rssi, wifi_security_name(e->bss.security),
                       e->bss.he ? "ax" : e->bss.vht ? "ac" : e->bss.ht ? "n" : "a/b/g",
                       e->bss.rsn_pmf_required ? ", PMF required" : "");
            }
            if (sl.n == 0) printf("  (nothing found)\n");
        }
    }
    link_close(&l);
    printf("\n%s\n", rc ? "Scan stopped. Unplug and replug the adapter to reset it, then try again with "
                          "MACWIFI_DEBUG=1."
                        : "Phase 3a hardware check: compare this list with your Mac's Wi-Fi menu.");
    return rc;
}

// ---------------------------------------------------------------- Phase 3b: connect (WPA2-PSK) and a DHCP probe
//
// Order as the Linux driver does it with wpa_supplicant (rwnx_cfg80211_connect / add_key / change_station):
// SM_CONNECT_REQ (the firmware authenticates and associates, holding the control port closed), SM_CONNECT_IND
// (gives the AP's station index), the EAPOL 4-way handshake over the data path, MM_KEY_ADD_REQ for the pairwise
// and group keys, then ME_SET_CONTROL_PORT_REQ(open). The supplicant (eapol.c) is our own code.

static atomic_int gStop = 0;   // set by signal handlers (lock-free), read by every thread
static void on_sigint(int sig) { (void)sig; gStop = 1; }

static void secure_zero(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static int link_send_data(Link *l, const uint8_t *buf, size_t len) {
    hexdump("OUT data", buf, len);
    IOReturn kr = (*l->intf)->WritePipeTO(l->intf, l->out_pipe, (void *)buf, (UInt32)len, 1000, 2000);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "bulk OUT (data) failed: 0x%08x\n", kr);
        if (kr == kIOUSBPipeStalled) (*l->intf)->ClearPipeStallBothEnds(l->intf, l->out_pipe);
        return -1;
    }
    return 0;
}

static int send_msg(Link *l, uint16_t id, const void *params, uint16_t plen) {
    uint8_t cmd[16 + 1024];
    size_t n = aic_build_cmd(cmd, sizeof cmd, id, (uint16_t)(id >> 10), params, plen);
    return (!n || link_send(l, cmd, n)) ? -1 : 0;
}

typedef struct {
    pthread_t th;
    int th_running, use_ipconfig, had_ip, losses, self_test;
    double t0, last_status, last_ip_check, last_reset;
    char ip[64];
    pid_t test_pid;
} Bridge;

typedef struct {
    Link *l;
    uint8_t mac[6], bssid[6], vif;
    int open_net;                     // no RSN: no handshake, the port opens at association
    uint8_t ap_rsn[2 + 255];          // the AP's RSN element, owned here (scans reuse their lists)
    // WPA3 (SAE): the firmware hands authentication to us (SM_EXTERNAL_AUTH_REQUIRED_IND); Authentication frames
    // go out through the management TX path and come back on the data path flagged as 802.11 MPDUs
    Sae sae;
    int sae_active, ext_auth, sae_done, sae_failed, sae_wrong, used_sae, sae_h2e;
    char sae_err[120];
    uint8_t sae_tx[SAE_MAX_FRAME];
    size_t sae_tx_len;
    unsigned rx_mgmt;
    ScanList *scan;                   // set while a scan runs through pump()
    int usb_error;                    // the IN pipe failed (adapter unplugged or reset)
    int last_rssi;                    // from the RX header of the latest data frame
    Bridge br;
    // association
    int connect_cfm, associated;
    atomic_int disconnected;          // also read by the macOS->air thread
    uint8_t connect_status;
    uint16_t disc_reason;
    AicConnectInd ind;
    uint8_t ind_buf[900];
    // handshake
    WpaSupplicant *ws;
    uint8_t snonce[32];
    int pending, keys_installed, eapol_rejects;
    atomic_int port_open;
    uint8_t next_cfm_idx, last_eapol_idx;
    int last_eapol_cfm;
    uint32_t last_eapol_status;
    // message wait
    uint16_t want;
    int got;
    uint8_t params[1024];
    uint16_t plen;
    // traffic
    uint8_t cur_decr;
    uint32_t xid;
    int offer;
    uint8_t offer_decr;
    DhcpReply reply;
    unsigned rx, rx_group, rx_ipv4, rx_arp, rx_ipv6, rx_eapol, rx_other, rx_plain, rx_not_eth, tx;
    // Phase 4 bridge (frames to/from macOS through a feth pair)
    Feth *feth;
    atomic_int bridging;
    pthread_mutex_t tx_lock;          // the data pipe is written by the RX loop (EAPOL) and the macOS->air thread
    unsigned long to_host, to_host_bytes, host_write_err, reord, rx_malformed;
    atomic_ulong from_host, from_host_bytes;   // written by the macOS->air thread
    unsigned long drop_to[BRIDGE_DROP_COUNT], drop_from[BRIDGE_DROP_COUNT];
    // A-MPDU receive reordering (frames the chip flags "need reorder"): one unit per MPDU =
    // [decryption status][u16 length][Ethernet frame]..., released in sequence order by reorder.c
    Reorder ro;
    uint8_t unit[8192];
    size_t unit_len;
    int unit_over;
} Conn;

static void conn_send_eth(Conn *c, const uint8_t *eth, size_t len, int eapol) {
    uint8_t out[2048];
    uint8_t idx = 0;
    if (eapol) {
        idx = c->next_cfm_idx++ & 0x1f;
        c->last_eapol_idx = idx;
        c->last_eapol_cfm = 0;
    }
    size_t n = aic_data_build_tx(out, sizeof out, eth, len, c->vif, c->ind.ap_idx, 0, idx);
    if (!n) return;
    pthread_mutex_lock(&c->tx_lock);
    if (link_send_data(c->l, out, n) == 0) c->tx++;
    pthread_mutex_unlock(&c->tx_lock);
}

static void on_eth(void *ctx, const uint8_t *eth, size_t len) {
    Conn *c = (Conn *)ctx;
    if (len < 14) return;
    c->rx++;
    if (eth[0] & 1) c->rx_group++;
    if (!c->cur_decr) c->rx_plain++;
    uint16_t type = (uint16_t)((eth[12] << 8) | eth[13]);
    if (c->bridging && c->feth && type != 0x888E) {
        BridgeVerdict v = bridge_to_host(eth, len, c->mac, c->cur_decr != 0, c->keys_installed);
        if (v != BRIDGE_PASS) c->drop_to[v]++;
        else if (feth_write(c->feth, eth, len) == 0) { c->to_host++; c->to_host_bytes += len; }
        else c->host_write_err++;
    }
    if (type == 0x888E) {
        c->rx_eapol++;
        if (!c->associated || !c->ws || memcmp(eth + 6, c->bssid, 6) != 0) return;
        uint8_t reply[512], frame[14 + 512];
        size_t rl = 0;
        const char *err = NULL;
        int ev = wpa_supp_rx(c->ws, eth + 14, len - 14, c->snonce, reply, sizeof reply, &rl, &err);
        if (ev < 0) {
            c->eapol_rejects++;
            printf("  EAPOL frame rejected: %s\n", err ? err : "?");
            return;
        }
        if (ev & WPA_EV_SEND) {
            memcpy(frame, c->bssid, 6);
            memcpy(frame + 6, c->mac, 6);
            frame[12] = 0x88;
            frame[13] = 0x8E;
            memcpy(frame + 14, reply, rl);
            conn_send_eth(c, frame, 14 + rl, 1);
        }
        if (ev & ~WPA_EV_SEND) printf("  EAPOL: %s\n", (ev & WPA_EV_DONE) ? "message 3 accepted, message 4 sent"
                                                    : (ev & WPA_EV_INSTALL_GTK) ? "group key update answered"
                                                                                : "message accepted");
        else if (ev & WPA_EV_SEND) printf("  EAPOL: message 1 received, message 2 sent\n");
        c->pending |= ev & ~WPA_EV_SEND;
        return;
    }
    if (type == 0x0800) {
        c->rx_ipv4++;
        DhcpReply r;
        if (!c->offer && dhcp_parse_reply(eth, len, c->mac, c->xid, &r) == 0 && r.type == 2) {
            c->offer = 1;
            c->reply = r;
            c->offer_decr = c->cur_decr;
        }
    } else if (type == 0x0806) c->rx_arp++;
    else if (type == 0x86DD) c->rx_ipv6++;
    else c->rx_other++;
}

// Sends an Authentication frame (body from sae.c) to the AP through the management TX path.
static void conn_send_auth(Conn *c, const uint8_t *body, size_t n) {
    uint8_t frame[24 + SAE_MAX_FRAME], out[64 + 24 + SAE_MAX_FRAME];
    if (n > SAE_MAX_FRAME) return;
    memset(frame, 0, 24);
    frame[0] = 0xB0;                              // management, subtype authentication
    memcpy(frame + 4, c->bssid, 6);
    memcpy(frame + 10, c->mac, 6);
    memcpy(frame + 16, c->bssid, 6);
    memcpy(frame + 24, body, n);
    size_t len = aic_data_build_mgmt_tx(out, sizeof out, frame, 24 + n, c->vif);
    if (!len) return;
    pthread_mutex_lock(&c->tx_lock);
    if (link_send_data(c->l, out, len) == 0) c->tx++;
    pthread_mutex_unlock(&c->tx_lock);
}

// A management frame from the air. Only SAE Authentication frames from our AP matter here.
static void on_mgmt(Conn *c, const uint8_t *f, size_t len) {
    c->rx_mgmt++;
    if (len < 30 || f[0] != 0xB0 || !c->sae_active || c->sae_done || c->sae_failed) return;
    if (memcmp(f + 10, c->bssid, 6) != 0 || memcmp(f + 4, c->mac, 6) != 0) return;
    uint8_t reply[SAE_MAX_FRAME];
    size_t rl = 0;
    const char *err = NULL;
    int wrong = 0;
    SaeState before = c->sae.state;
    int ev = sae_rx(&c->sae, f + 24, len - 24, reply, sizeof reply, &rl, &err, &wrong);
    if (ev < 0) {
        c->sae_failed = 1;
        c->sae_wrong = wrong;
        snprintf(c->sae_err, sizeof c->sae_err, "%s", err ? err : "SAE failed");
        return;
    }
    if (ev & SAE_EV_SEND) {
        memcpy(c->sae_tx, reply, rl);
        c->sae_tx_len = rl;
        conn_send_auth(c, reply, rl);
        printf("  SAE: %s\n", c->sae.state == SAE_CONFIRMED && before == SAE_COMMITTED ? "commits exchanged, confirm sent"
                                                                                   : "anti-clogging token requested, commit sent again");
    }
    if (ev & SAE_EV_DONE) {
        c->sae_done = 1;
        printf("  SAE: the access point's confirm verifies\n");
    }
}

static void collect_unit(void *ctx, const uint8_t *eth, size_t len) {
    Conn *c = (Conn *)ctx;
    if (c->unit_len + 2 + len > sizeof c->unit) { c->unit_over = 1; return; }
    c->unit[c->unit_len] = (uint8_t)len;
    c->unit[c->unit_len + 1] = (uint8_t)(len >> 8);
    memcpy(c->unit + c->unit_len + 2, eth, len);
    c->unit_len += 2 + len;
}

static void deliver_unit(void *ctx, const uint8_t *u, size_t n) {
    Conn *c = (Conn *)ctx;
    if (n < 1) return;
    uint8_t saved = c->cur_decr;
    c->cur_decr = u[0];
    for (size_t o = 1; o + 2 <= n;) {
        size_t l = (size_t)(u[o] | (u[o + 1] << 8));
        if (o + 2 + l > n) break;
        on_eth(c, u + o + 2, l);
        o += 2 + l;
    }
    c->cur_decr = saved;
}

// Reads the bulk IN pipe for up to `timeout_ms`, handling messages, data and TX confirmations, until `done` says
// so. Returns 0 when done, 1 on timeout, -1 on a USB error or Ctrl-C.
static int pump(Conn *c, int timeout_ms, int (*done)(const Conn *)) {
    static uint8_t buf[32768], scratch[4096];
    double t0 = now_s();
    while (!(done && done(c))) {
        if (gStop) return -1;
        if ((now_s() - t0) * 1000 >= timeout_ms) return 1;
        UInt32 size = sizeof buf;
        if (ro_pending(&c->ro)) ro_tick(&c->ro, now_s(), deliver_unit, c);
        if (c->l->rxq) {                       // asynchronous reader: take the next completed transfer
            int err = 0;
            long got = rxq_pop(c->l->rxq, buf, sizeof buf, ro_pending(&c->ro) ? 20 : 200, &err);
            if (got == 0) continue;
            if (got < 0) {
                fprintf(stderr, "bulk IN failed: 0x%08x\n", (unsigned)err);
                c->usb_error = 1;
                return -1;
            }
            size = (UInt32)got;
        } else {
            IOReturn kr = (*c->l->intf)->ReadPipeTO(c->l->intf, c->l->in_pipe, buf, &size, 200, 200);
            if (kr == kIOUSBTransactionTimeout || kr == kIOReturnTimeout) continue;
            if (kr != kIOReturnSuccess) {
                fprintf(stderr, "bulk IN failed: 0x%08x\n", kr);
                if (kr == kIOUSBPipeStalled) (*c->l->intf)->ClearPipeStallBothEnds(c->l->intf, c->l->in_pipe);
                c->usb_error = 1;
                return -1;
            }
        }
        hexdump("IN", buf, size);
        size_t off = 0;
        AicRxPacket pk;
        int r;
        while ((r = aic_rx_next(buf, size, &off, &pk)) == 1) {
            uint8_t t = pk.type & 0x7f;
            if (t == AIC_USB_TYPE_CFG_PRINT) {
                size_t tl = pk.packet[0] | (pk.packet[1] << 8);
                const char *s = (const char *)pk.packet + 4;
                if (tl > pk.packet_len - 4) tl = pk.packet_len - 4;
                while (tl && (s[tl - 1] == '\n' || s[tl - 1] == '\r' || s[tl - 1] == 0)) tl--;
                fprintf(stderr, "  [firmware] %.*s\n", (int)tl, s);
                continue;
            }
            if (t == AIC_USB_TYPE_CFG_DATA_CFM) {
                uint32_t st = 0, idx = 0;
                if (aic_data_parse_tx_cfm(pk.packet, pk.packet_len, &st, &idx) == 0 && idx == c->last_eapol_idx) {
                    c->last_eapol_cfm = 1;
                    c->last_eapol_status = st;
                }
                continue;
            }
            if (pk.is_msg) {
                if (pk.id == c->want && !c->got) {
                    size_t n = pk.param_len < sizeof c->params ? pk.param_len : sizeof c->params;
                    memcpy(c->params, pk.params, n);
                    c->plen = (uint16_t)n;
                    c->got = 1;
                }
                if (pk.id == AIC_SCANU_RESULT_IND && c->scan) {
                    AicScanResult sr;
                    if (aic_fmac_parse_scan_result(pk.params, pk.param_len, &sr) == 0) scan_add(c->scan, &sr);
                    else c->scan->unparsed++;
                } else if (pk.id == AIC_SM_CONNECT_CFM) {
                    c->connect_cfm = 1;
                    c->connect_status = pk.param_len ? pk.params[0] : 0xff;
                } else if (pk.id == AIC_SM_CONNECT_IND) {
                    size_t n = pk.param_len < sizeof c->ind_buf ? pk.param_len : sizeof c->ind_buf;
                    memcpy(c->ind_buf, pk.params, n);
                    if (aic_fmac_parse_connect_ind(c->ind_buf, n, &c->ind) != 0) {
                        fprintf(stderr, "SM_CONNECT_IND too short (%zu bytes)\n", n);
                        c->disconnected = 1;
                    } else if (c->ind.status_code != 0) {
                        c->disconnected = 1;
                    } else {
                        c->associated = 1;
                    }
                } else if (pk.id == AIC_SM_EXTERNAL_AUTH_REQUIRED_IND) {
                    c->ext_auth = 1;
                } else if (pk.id == AIC_SM_DISCONNECT_IND) {
                    uint8_t v = 0;
                    aic_fmac_parse_disconnect_ind(pk.params, pk.param_len, &c->disc_reason, &v);
                    c->disconnected = 1;
                    c->associated = 0;
                } else if (pk.id == AIC_DBG_ERROR_IND) {
                    fprintf(stderr, "firmware reported DBG_ERROR_IND\n");
                } else if (gDebug && pk.id != c->want) {
                    fprintf(stderr, "(message 0x%04x, %u bytes)\n", pk.id, pk.param_len);
                }
                continue;
            }
            if (pk.type & AIC_USB_TYPE_CFG) continue;   // other config packets
            AicRxInfo ri;
            if (aic_data_parse_rx(pk.packet, pk.packet_len, &ri) != 0) { c->rx_not_eth++; continue; }
            c->cur_decr = ri.decr_status;
            if (ri.need_reord) c->reord++;
            if (ri.rssi) c->last_rssi = ri.rssi;
            const uint8_t *f = ri.frame;
            if (ri.frame_len >= 24 && ((f[0] >> 2) & 3) == 0) {   // management frame (rwnx_rx_mgmt_any)
                on_mgmt(c, f, ri.frame_len);
                continue;
            }
            // Block-ack session frames: QoS data, TID from the QoS control field, sequence number from the
            // sequence control field; put back in order before they go anywhere (as CONFIG_RX_REORDER does).
            if (ri.need_reord && ri.frame_len >= 26 && ((f[0] >> 2) & 3) == 2 && (f[0] & 0x80) && !(f[0] & 0x40)) {
                uint16_t sn = (uint16_t)((f[22] | (f[23] << 8)) >> 4);
                uint8_t tid = f[24] & 0x0f;
                c->unit[0] = ri.decr_status;
                c->unit_len = 1;
                c->unit_over = 0;
                int r = aic_data_rx_to_eth(&ri, scratch, sizeof scratch, collect_unit, c);
                if (r < 0) c->rx_not_eth++;
                else if (c->unit_over) aic_data_rx_to_eth(&ri, scratch, sizeof scratch, on_eth, c);   // too big to hold
                else if (c->unit_len > 1) ro_rx(&c->ro, tid, sn, c->unit, c->unit_len, now_s(), deliver_unit, c);
            } else if (aic_data_rx_to_eth(&ri, scratch, sizeof scratch, on_eth, c) < 0) {
                c->rx_not_eth++;
            }
        }
        if (r < 0 && ++c->rx_malformed <= 5) {
            const uint8_t *h = buf + off;
            fprintf(stderr, "malformed IN transfer: %u bytes, stopped at offset %zu (header %02x %02x %02x %02x)%s\n",
                    size, off, off < size ? h[0] : 0, off + 1 < size ? h[1] : 0, off + 2 < size ? h[2] : 0,
                    off + 3 < size ? h[3] : 0, c->rx_malformed == 5 ? "; not reporting further ones" : "");
        }
    }
    return 0;
}

static int w_got(const Conn *c) { return c->got || c->disconnected; }
static int w_connect_cfm(const Conn *c) { return c->connect_cfm; }
static int w_assoc(const Conn *c) { return c->associated || c->disconnected; }
static int w_ext_auth(const Conn *c) { return c->ext_auth || c->associated || c->disconnected; }
static int w_sae(const Conn *c) { return c->sae_done || c->sae_failed || c->disconnected; }
static int w_pending(const Conn *c) { return c->pending || c->disconnected; }
static int w_eapol_cfm(const Conn *c) { return c->last_eapol_cfm; }

// Sends a message and pumps until its confirmation. Returns 0 with c->params / c->plen, else -1.
static int conn_call(Conn *c, uint16_t id, const void *params, uint16_t plen, uint16_t cfm, int timeout_ms) {
    c->want = cfm;
    c->got = 0;
    if (send_msg(c->l, id, params, plen)) return -1;
    int r = pump(c, timeout_ms, w_got);
    if (c->got) return 0;
    if (r == 1) fprintf(stderr, "no reply (message 0x%04x) within %d ms\n", cfm, timeout_ms);
    return -1;
}

static int install_key(Conn *c, int pairwise, const uint8_t *key, uint8_t len, uint8_t idx) {
    uint8_t p[AIC_SZ_KEY_ADD_REQ];
    aic_fmac_key_add_params(p, c->vif, pairwise ? c->ind.ap_idx : 0xFF, pairwise, key, len, idx, AIC_MAC_CIPHER_CCMP);
    gNoDump = 1;
    int r = conn_call(c, AIC_MM_KEY_ADD_REQ, p, sizeof p, AIC_MM_KEY_ADD_CFM, 2000);
    gNoDump = 0;
    secure_zero(p, sizeof p);
    if (r) return -1;
    if (c->plen < 1 || c->params[0] != 0) {
        fprintf(stderr, "MM_KEY_ADD refused (status %u)\n", c->plen ? c->params[0] : 0xff);
        return -1;
    }
    printf("  %s key installed (hardware slot %u)\n", pairwise ? "Pairwise" : "Group", c->plen > 1 ? c->params[1] : 0);
    return 0;
}

// The integrity group key (IGTK, key index 4 or 5): the chip checks broadcast management frames (deauthentication,
// disassociation) with it (BIP-CMAC-128), so they can't be forged. As rwnx_cfg80211_add_key with
// WLAN_CIPHER_SUITE_AES_CMAC: a default key (sta 0xFF) with cipher MAC_CIPHER_BIP_CMAC_128.
static int install_igtk(Conn *c, const uint8_t key[16], uint8_t idx) {
    uint8_t p[AIC_SZ_KEY_ADD_REQ];
    aic_fmac_key_add_params(p, c->vif, 0xFF, 0, key, 16, idx, AIC_MAC_CIPHER_BIP_CMAC_128);
    gNoDump = 1;
    int r = conn_call(c, AIC_MM_KEY_ADD_REQ, p, sizeof p, AIC_MM_KEY_ADD_CFM, 2000);
    gNoDump = 0;
    secure_zero(p, sizeof p);
    if (r || c->plen < 1 || c->params[0] != 0) {
        fprintf(stderr, "  (the firmware didn't take the management-frame integrity key, status %u; continuing without "
                        "broadcast management frame checks)\n", r ? 0xffu : c->params[0]);
        return -1;
    }
    printf("  Management integrity key installed (index %u, hardware slot %u)\n", idx, c->plen > 1 ? c->params[1] : 0);
    return 0;
}

// Acts on what the supplicant reported (keys to install, control port to open). Returns 0, or -1 if the firmware
// refused a step.
static int handle_wpa_events(Conn *c, WpaSupplicant *ws) {
    int ev = c->pending;
    c->pending = 0;
    if (ev & WPA_EV_INSTALL_PTK) {
        // Let message 4 leave unencrypted before the pairwise key exists (wpa_supplicant's order).
        pump(c, 500, w_eapol_cfm);
        if (!c->last_eapol_cfm) printf("  (no TX confirmation for message 4 within 500 ms; continuing)\n");
        else if (!(c->last_eapol_status & AIC_TXST_ACKED))
            printf("  (message 4 TX status 0x%x: not acknowledged by the AP)\n", c->last_eapol_status);
        if (install_key(c, 1, WPA_TK(ws->ptk), 16, 0)) return -1;
        c->keys_installed = 1;
    }
    if ((ev & WPA_EV_INSTALL_GTK) && ws->gtk_len) {
        if (install_key(c, 0, ws->gtk, (uint8_t)ws->gtk_len, (uint8_t)ws->gtk_idx)) return -1;
    }
    if ((ev & WPA_EV_INSTALL_IGTK) && ws->have_igtk) install_igtk(c, ws->igtk, (uint8_t)ws->igtk_idx);
    if ((ev & WPA_EV_DONE) && !c->port_open) {
        uint8_t cp[AIC_SZ_CTRL_PORT_REQ];
        aic_fmac_ctrl_port_params(cp, c->ind.ap_idx, 1);
        if (conn_call(c, AIC_ME_SET_CONTROL_PORT_REQ, cp, sizeof cp, AIC_ME_SET_CONTROL_PORT_CFM, 2000)) return -1;
        c->port_open = 1;
    }
    return 0;
}

static const char *status_text(uint16_t s) {
    switch (s) {
        case 1: return "unspecified failure";
        case 10: return "capabilities not supported";
        case 12: return "denied for another reason";
        case 13: return "authentication algorithm not supported";
        case 15: return "authentication challenge failure";
        case 17: return "the AP is full";
        case 18: return "basic rates not supported";
        case 30: return "temporarily rejected, try later";
        case 31: return "robust management frame policy violation";
        case 43: return "invalid AKM";
        case 45: return "RSN capabilities not supported";
        default: return "see IEEE 802.11 status codes";
    }
}

static const char *reason_text(uint16_t r) {
    switch (r) {
        case 1: return "unspecified";
        case 2: return "previous authentication no longer valid";
        case 3: return "leaving";
        case 4: return "inactivity";
        case 6: case 7: return "frame from a station that isn't associated";
        case 14: return "message integrity failure";
        case 15: return "4-way handshake timeout (usually a wrong password)";
        case 16: return "group key handshake timeout";
        case 17: return "RSN element in the 4-way handshake differs";
        case 23: return "802.1X authentication failed (usually a wrong password)";
        default: return "see IEEE 802.11 reason codes";
    }
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// Asks for the passphrase on the terminal (never echoed, logged or stored) and derives the PMK.
// `pass_out` (128 bytes) gets the passphrase for WPA3, or "" when a raw 64-digit key was typed (WPA2 only);
// the caller wipes it.
static int ask_pmk(const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32], char *pass_out) {
    char pass[128];
    char prompt[96];
    pass_out[0] = 0;
    snprintf(prompt, sizeof prompt, "Wi-Fi password for \"%.*s\" (not shown, not saved): ", (int)ssid_len,
             (const char *)ssid);
    if (!readpassphrase(prompt, pass, sizeof pass, RPP_REQUIRE_TTY)) {
        fprintf(stderr, "Couldn't read the password from the terminal.\n");
        return -1;
    }
    size_t n = strlen(pass);
    int rc = 0;
    if (n == 64) {   // a raw 256-bit PSK in hex
        for (size_t i = 0; i < 32 && rc == 0; i++) {
            int h = hexval(pass[2 * i]), lo = hexval(pass[2 * i + 1]);
            if (h < 0 || lo < 0) rc = -1;
            else pmk[i] = (uint8_t)(h << 4 | lo);
        }
        if (rc) fprintf(stderr, "A 64-character key must be hexadecimal.\n");
    } else if (n < 8 || n > 63) {
        fprintf(stderr, "A WPA2 passphrase has 8 to 63 characters (this one has %zu).\n", n);
        rc = -1;
    } else if (wpa_pmk_from_passphrase(pass, ssid, ssid_len, pmk) != 0) {
        fprintf(stderr, "Key derivation failed.\n");
        rc = -1;
    } else {
        memcpy(pass_out, pass, n + 1);
    }
    secure_zero(pass, sizeof pass);
    return rc;
}

static void print_ip(const char *label, const uint8_t a[4]) {
    if (a[0] | a[1] | a[2] | a[3]) printf("    %-10s %u.%u.%u.%u\n", label, a[0], a[1], a[2], a[3]);
}

// ---------------------------------------------------------------- reusable steps (CLI connect/join and macwifid)

// What we can join: open networks; WPA2-PSK with CCMP for both keys (and no required PMF); WPA3-Personal (SAE,
// group 19, hunting-and-pecking or hash-to-element) with CCMP and management frame protection (BIP-CMAC-128).
static int bss_can_psk(const WifiBss *b) {
    return b->security != WIFI_SEC_OPEN && b->rsn_akm_psk && !b->rsn_pmf_required && b->rsn_ccmp && b->rsn_group_ccmp &&
           b->rsn_ie_len >= 2;
}
static int bss_can_sae(const WifiBss *b) {
    return b->security != WIFI_SEC_OPEN && b->rsn_akm_sae && b->rsn_pmf_capable && b->rsn_mgmt_bip_cmac && b->rsn_ccmp &&
           b->rsn_group_ccmp && b->rsn_ie_len >= 2;
}
// SAE with hash-to-element whenever the access point advertises it (as wpa_supplicant's sae_pwe=2 does).
static int bss_sae_h2e(const WifiBss *b) { return b->sae_h2e || b->sae_h2e_only; }
// `why` gets a short reason when not.
static int bss_usable(const WifiBss *b, int *secured, const char **why) {
    *secured = b->security != WIFI_SEC_OPEN;
    if (b->security == WIFI_SEC_OPEN) { *why = "open"; return 1; }
    if (bss_can_sae(b)) { *why = bss_can_psk(b) ? "usable (WPA3, or WPA2)" : "usable (WPA3)"; return 1; }
    if (bss_can_psk(b)) { *why = "usable"; return 1; }
    if (b->rsn_akm_sae && !b->rsn_akm_psk) { *why = "WPA3 with settings that aren't supported yet"; return 0; }
    if (!b->rsn_akm_psk) { *why = "needs enterprise sign-in or WEP (not supported)"; return 0; }
    if (b->rsn_pmf_required) { *why = "WPA2 with required PMF (not supported yet)"; return 0; }
    *why = "needs CCMP for both keys";
    return 0;
}

static int ssid_is(const WifiBss *b, const uint8_t *ssid, size_t len) {
    return b->ssid_len == len && memcmp(b->ssid, ssid, len) == 0;
}

// Band preference: 5 GHz is much faster and usually less crowded, so it wins unless it is clearly weaker (by
// more than this many dB), much like macOS's own choice between the bands of one network.
#define BAND_5G_BONUS_DB 15
static int bss_score(const ScanEntry *e) { return e->rssi + (e->freq >= 5000 ? BAND_5G_BONUS_DB : 0); }

// The best usable BSS of `ssid` in `sl` (signal plus the 5 GHz preference; NULL if none); prints each candidate
// when `verbose`.
static const ScanEntry *scan_pick(const ScanList *sl, const uint8_t *ssid, size_t ssid_len, int verbose, int *found) {
    const ScanEntry *best = NULL;
    *found = 0;
    for (int i = 0; i < sl->n; i++) {
        const ScanEntry *e = &sl->e[i];
        if (!ssid_is(&e->bss, ssid, ssid_len)) continue;
        (*found)++;
        int sec;
        const char *why;
        int ok = bss_usable(&e->bss, &sec, &why);
        if (verbose)
            printf("  %02x:%02x:%02x:%02x:%02x:%02x  %4d MHz  RSSI %4d  %-10s %s\n", e->bss.bssid[0], e->bss.bssid[1],
                   e->bss.bssid[2], e->bss.bssid[3], e->bss.bssid[4], e->bss.bssid[5], e->freq, e->rssi,
                   wifi_security_name(e->bss.security), why);
        if (ok && (!best || bss_score(e) > bss_score(best))) best = e;
    }
    return best;
}

// One scan through pump(), so data keeps flowing while it runs. `ssid` NULL = all networks.
static int conn_scan(Conn *c, const char *ssid, ScanList *sl) {
    uint8_t p[AIC_SZ_SCANU_START_REQ];
    size_t n = aic_fmac_scan_params(p, c->vif, ssid);
    memset(sl, 0, sizeof *sl);
    c->scan = sl;
    // Not associated: a stale disconnect flag must not end the wait (conn_call stops on it); associated: a real
    // disconnect during the scan must stay visible.
    int saved_disc = c->disconnected;
    if (!c->associated) c->disconnected = 0;
    int r = conn_call(c, AIC_SCANU_START_REQ, p, (uint16_t)n, AIC_SCANU_START_CFM, 20000);
    if (!c->associated) c->disconnected = saved_disc;
    c->scan = NULL;
    return r;
}

static void conn_reset_assoc(Conn *c) {
    c->connect_cfm = c->associated = 0;
    c->disconnected = 0;
    c->connect_status = 0;
    c->disc_reason = 0;
    memset(&c->ind, 0, sizeof c->ind);
    memset(c->ind_buf, 0, sizeof c->ind_buf);
    c->pending = c->keys_installed = c->eapol_rejects = 0;
    c->port_open = 0;
    c->last_eapol_cfm = 0;
    c->open_net = 0;
    c->sae_active = c->ext_auth = c->sae_done = c->sae_failed = c->sae_wrong = c->used_sae = 0;
    c->sae_err[0] = 0;
    sae_wipe(&c->sae);
    ro_reset(&c->ro);
    c->ro.timeout_s = 0.05;                 // REORDER_UPDATE_TIME: 50 ms
}

typedef enum { JOIN_OK = 0, JOIN_FW, JOIN_ASSOC, JOIN_PASSWORD, JOIN_TIMEOUT, JOIN_STOPPED } JoinResult;

static void sae_rng(uint8_t *out, size_t n) { arc4random_buf(out, n); }

// The SAE exchange, once the firmware has asked for it. On success the PMK is in ws->pmk and the firmware has been
// told to go on (it associates next).
static JoinResult run_sae(Conn *c, WpaSupplicant *ws, const char *pass, size_t pass_len, const uint8_t *ssid,
                          size_t ssid_len, char *err, size_t errcap) {
    double t0 = now_s();
    pump(c, 8000, w_ext_auth);
    if (!c->ext_auth) {
        if (c->disconnected || (c->ind_buf[0] | c->ind_buf[1]))
            snprintf(err, errcap, "the connection ended before WPA3 sign-in started (status %u, reason %u)",
                     c->ind.status_code, c->disc_reason);
        else
            snprintf(err, errcap, "the firmware didn't ask for WPA3 (SAE) sign-in");
        return gStop ? JOIN_STOPPED : JOIN_FW;
    }
    if (sae_start(&c->sae, c->mac, c->bssid, (const uint8_t *)pass, pass_len, ssid, ssid_len, c->sae_h2e, sae_rng) != 0) {
        snprintf(err, errcap, "couldn't prepare the WPA3 (SAE) exchange");
        return JOIN_FW;
    }
    c->sae_active = 1;
    c->sae_tx_len = sae_build_commit(&c->sae, c->sae_tx, sizeof c->sae_tx);
    conn_send_auth(c, c->sae_tx, c->sae_tx_len);
    printf("  SAE: commit sent (%s)\n", c->sae_h2e ? "hash-to-element" : "hunting-and-pecking");
    fflush(stdout);
    double last = now_s();
    for (int tries = 1; !c->sae_done && !c->sae_failed && !c->disconnected && !gStop && now_s() - t0 < 12;) {
        pump(c, 500, w_sae);
        if (!c->sae_done && !c->sae_failed && now_s() - last >= 1.5 && tries < 4) {   // no answer: say it again
            conn_send_auth(c, c->sae_tx, c->sae_tx_len);
            last = now_s();
            tries++;
        }
    }
    int confirmed = c->sae.state == SAE_CONFIRMED;
    uint8_t rsp[AIC_SZ_EXT_AUTH_RSP];
    if (c->sae_done) {
        memcpy(ws->pmk, c->sae.pmk, 32);
        sae_wipe(&c->sae);
        c->sae_active = 0;
        aic_fmac_ext_auth_rsp_params(rsp, c->vif, 0);
        if (send_msg(c->l, AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP, rsp, sizeof rsp)) {
            snprintf(err, errcap, "couldn't report the WPA3 sign-in to the firmware");
            return JOIN_FW;
        }
        printf("  SAE finished in %.2f s\n", now_s() - t0);
        return JOIN_OK;
    }
    sae_wipe(&c->sae);
    c->sae_active = 0;
    if (!c->disconnected && !c->usb_error) {       // tell the firmware it failed, and let it wind the attempt down
        aic_fmac_ext_auth_rsp_params(rsp, c->vif, 1);   // WLAN_STATUS_UNSPECIFIED_FAILURE
        send_msg(c->l, AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP, rsp, sizeof rsp);
        pump(c, 2000, w_assoc);
    }
    if (gStop) { snprintf(err, errcap, "stopped"); return JOIN_STOPPED; }
    if (c->sae_failed) {
        snprintf(err, errcap, "%s", c->sae_err);
        return c->sae_wrong ? JOIN_PASSWORD : JOIN_ASSOC;
    }
    if (confirmed) {    // the AP took our commit but never confirmed: what a wrong password looks like from here
        snprintf(err, errcap, "the password seems wrong: the access point didn't confirm the WPA3 sign-in "
                              "(%u management frames received)", c->rx_mgmt);
        return JOIN_PASSWORD;
    }
    snprintf(err, errcap, "the access point didn't answer the WPA3 (SAE) sign-in (%u management frames received)",
             c->rx_mgmt);
    return JOIN_TIMEOUT;
}

// Associates with `best` and runs the sign-in until the port is open. `ws` is NULL for an open network; for WPA2
// it holds the PMK; for WPA3 `sae_pass` is the password (the PMK then comes out of SAE). Writes a one-line reason
// into `err` on failure.
static JoinResult conn_associate(Conn *c, const ScanEntry *best, const uint8_t *ssid, size_t ssid_len,
                                 WpaSupplicant *ws, const char *sae_pass, size_t sae_pass_len, char *err, size_t errcap) {
    conn_reset_assoc(c);
    memcpy(c->bssid, best->bss.bssid, 6);
    c->ws = ws;
    c->open_net = ws == NULL;
    int sae = ws && sae_pass;
    c->used_sae = sae;
    c->sae_h2e = sae && bss_sae_h2e(&best->bss);
    if (ws) {
        memcpy(ws->aa, best->bss.bssid, 6);
        memcpy(ws->spa, c->mac, 6);
        ws->akm_sae = sae;
        ws->rsn_ie_len = sae ? wpa_build_rsn_ie_sae(ws->rsn_ie, sizeof ws->rsn_ie) : wpa_build_rsn_ie(ws->rsn_ie, sizeof ws->rsn_ie);
        if (c->sae_h2e) {
            // RSN Extension element saying we use H2E: in the association request and, as wpa_supplicant does,
            // next to the RSN element in message 2 of the 4-way handshake.
            static const uint8_t rsnxe[3] = {244, 1, 0x20};
            memcpy(ws->rsn_ie + ws->rsn_ie_len, rsnxe, 3);
            ws->rsn_ie_len += 3;
        }
        // Keep our own copy: `best` lives in a scan list that the next scan (e.g. the menu's) overwrites, and a
        // later 4-way handshake (PTK rekey) checks message 3 against this element.
        memcpy(c->ap_rsn, best->bss.rsn_ie, best->bss.rsn_ie_len);
        ws->ap_rsn_ie = c->ap_rsn;
        ws->ap_rsn_ie_len = best->bss.rsn_ie_len;
        arc4random_buf(c->snonce, sizeof c->snonce);
    }
    int ch = best->bss.channel;
    int freq = ch ? wifi_channel_to_mhz(ch) : 0;
    if (!freq || (freq >= 5000) != (best->freq >= 5000)) freq = best->freq;   // trust the scan's band
    uint8_t band = freq >= 5000 ? 1 : 0;

    printf("Connecting to %02x:%02x:%02x:%02x:%02x:%02x on %d MHz%s...\n", c->bssid[0], c->bssid[1], c->bssid[2],
           c->bssid[3], c->bssid[4], c->bssid[5], freq, sae ? " with WPA3" : ws ? " with WPA2" : "");
    fflush(stdout);
    uint8_t p[AIC_SZ_SM_CONNECT_REQ];
    if (sae)
        aic_fmac_connect_params_sae(p, ssid, ssid_len, c->bssid, (uint16_t)freq, band, c->vif, ws->rsn_ie, ws->rsn_ie_len);
    else
        aic_fmac_connect_params(p, ssid, ssid_len, c->bssid, (uint16_t)freq, band, c->vif, ws ? ws->rsn_ie : NULL,
                                ws ? ws->rsn_ie_len : 0);
    double t0 = now_s();
    if (send_msg(c->l, AIC_SM_CONNECT_REQ, p, AIC_SZ_SM_CONNECT_REQ) || pump(c, 3000, w_connect_cfm) || !c->connect_cfm) {
        snprintf(err, errcap, "the firmware didn't answer the connect request");
        return gStop ? JOIN_STOPPED : JOIN_FW;
    }
    if (c->connect_status != 0) {
        snprintf(err, errcap, "the firmware refused the connect request (status %u)", c->connect_status);
        return JOIN_FW;
    }
    if (sae) {
        JoinResult sr = run_sae(c, ws, sae_pass, sae_pass_len, ssid, ssid_len, err, errcap);
        if (sr != JOIN_OK) return sr;
    }
    pump(c, 10000, w_assoc);
    if (!c->associated) {
        if (c->ind_buf[0] | c->ind_buf[1])
            snprintf(err, errcap, "association failed: status %u (%s)", c->ind.status_code, status_text(c->ind.status_code));
        else if (c->disconnected)
            snprintf(err, errcap, "disconnected during association: reason %u (%s)", c->disc_reason, reason_text(c->disc_reason));
        else
            snprintf(err, errcap, "no association result within 10 s");
        return gStop ? JOIN_STOPPED : JOIN_ASSOC;
    }
    printf("Associated in %.2f s: AP station index %u, AID %u, %s, %u MHz, width code %u\n", now_s() - t0,
           c->ind.ap_idx, c->ind.aid, c->ind.qos ? "QoS" : "no QoS", c->ind.center_freq, c->ind.width);

    if (!ws) {
        c->port_open = 1;                       // open network: no control port in use (flags 0)
        printf("Connected (open network, no encryption).\n");
        return JOIN_OK;
    }
    // 4-way handshake (EAPOL frames are answered inside pump; keys are installed here).
    double th = now_s();
    while (!c->port_open && !c->disconnected && !gStop && now_s() - th < 15) {
        pump(c, 1000, w_pending);
        if (handle_wpa_events(c, ws)) break;
    }
    if (c->port_open) {
        printf("Connected: %s handshake done in %.2f s, keys installed, port open.\n", sae ? "WPA3 (SAE)" : "WPA2-PSK",
               now_s() - th);
        return JOIN_OK;
    }
    if (gStop) { snprintf(err, errcap, "stopped"); return JOIN_STOPPED; }
    if (c->disconnected) {
        uint16_t r = c->disc_reason;
        snprintf(err, errcap, "%s: reason %u (%s)", (r == 15 || r == 23 || r == 2) && ws->have_ptk ? "the password seems wrong"
                 : "disconnected during the handshake", r, reason_text(r));
        return (r == 15 || r == 23 || r == 2) && ws->have_ptk ? JOIN_PASSWORD : JOIN_ASSOC;
    }
    if (!ws->have_ptk) {
        snprintf(err, errcap, "no EAPOL message 1 from the AP (%u EAPOL frames seen)", c->rx_eapol);
        return JOIN_TIMEOUT;
    }
    snprintf(err, errcap, "the password seems wrong: the 4-way handshake didn't finish (%u EAPOL frames, %d rejected)",
             c->rx_eapol, c->eapol_rejects);
    return JOIN_PASSWORD;
}

static void conn_leave(Conn *c);

// What we know to sign in with: the password (needed for WPA3) and/or the WPA2 key derived from it.
typedef struct {
    const char *pass;
    size_t pass_len;
    const uint8_t *pmk;
} JoinCred;

// Joins a secured network: WPA3 when the access point offers it and we have the password, else WPA2. If WPA3
// fails for a reason other than the password and the access point also offers WPA2 (a "transition" network),
// WPA2 is tried: that's no weaker than what such a network accepts from any other client.
static JoinResult conn_join_secured(Conn *c, const ScanEntry *best, const uint8_t *ssid, size_t ssid_len, WpaSupplicant *ws,
                                    const JoinCred *cred, char *err, size_t errcap) {
    int can_sae = bss_can_sae(&best->bss) && cred->pass && cred->pass_len && !getenv("MACWIFI_NO_WPA3");
    int can_psk = bss_can_psk(&best->bss) && cred->pmk;
    if (!can_sae && !can_psk) {
        snprintf(err, errcap, "%s", bss_can_sae(&best->bss) && !bss_can_psk(&best->bss)
                                        ? "this network uses WPA3: enter its password again"
                                        : "a password is needed");
        return JOIN_PASSWORD;
    }
    if (can_sae) {
        memset(ws, 0, sizeof *ws);
        JoinResult jr = conn_associate(c, best, ssid, ssid_len, ws, cred->pass, cred->pass_len, err, errcap);
        if (jr == JOIN_OK || jr == JOIN_PASSWORD || jr == JOIN_STOPPED || !can_psk || c->usb_error) return jr;
        printf("WPA3 sign-in didn't work (%s); trying WPA2.\n", err);
        fflush(stdout);
        conn_leave(c);
    }
    memset(ws, 0, sizeof *ws);
    memcpy(ws->pmk, cred->pmk, 32);
    return conn_associate(c, best, ssid, ssid_len, ws, NULL, 0, err, errcap);
}

// Sends SM_DISCONNECT if we're still associated.
static void conn_leave(Conn *c) {
    if (c->associated && !c->disconnected && !c->usb_error) {
        uint8_t d[AIC_SZ_SM_DISCONNECT_REQ];
        aic_fmac_disconnect_params(d, 3, c->vif);   // WLAN_REASON_DEAUTH_LEAVING
        int saved = gStop;
        gStop = 0;
        if (conn_call(c, AIC_SM_DISCONNECT_REQ, d, sizeof d, AIC_SM_DISCONNECT_CFM, 3000) == 0) printf("Disconnected.\n");
        gStop = saved;
    }
    c->associated = 0;
    c->port_open = 0;
    ro_reset(&c->ro);
}

// ---------------------------------------------------------------- bridge to macOS (feth pair)

static void *host_to_air(void *arg) {
    Conn *c = (Conn *)arg;
    while (c->bridging && !gStop) {
        long n = feth_read(c->feth);
        if (n < 0) { perror("BPF read"); break; }
        size_t off = 0, cl = 0;
        const uint8_t *pk = NULL;
        while (bridge_bpf_next(c->feth->rbuf, (size_t)n, &off, &pk, &cl) == 1) {
            BridgeVerdict v = bridge_from_host(pk, cl, c->mac);
            if (v != BRIDGE_PASS) { c->drop_from[v]++; continue; }
            if (!c->port_open || c->disconnected) continue;
            conn_send_eth(c, pk, cl, 0);
            c->from_host++;
            c->from_host_bytes += cl;
        }
    }
    return NULL;
}

// Checks run once macOS has an address; output goes straight to the terminal. Bound to the feth interface with
// ping -b so they test this path even while the Mac's own Wi-Fi is on.
static const char kSelfTest[] =
    "N=" FETH_NAME "; sleep 3\n"
    "R=$(ipconfig getoption $N router); D=$(ipconfig getoption $N domain_name_server)\n"
    "echo \"  [self-test] router ${R:-none}, DNS server from DHCP ${D:-none}\"\n"
    "[ -n \"$R\" ] && echo \"  [self-test] ping router:  $(ping -b $N -c 3 -t 6 -q $R | grep transmitted)\"\n"
    "echo \"  [self-test] ping 1.1.1.1: $(ping -b $N -c 3 -t 6 -q 1.1.1.1 | grep transmitted)\"\n"
    "echo \"  [self-test] default route now uses: $(route -n get default 2>/dev/null | awk '/interface:/{print $2}')\"\n"
    "echo \"  [self-test] resolvers mentioning $N: $(scutil --dns | grep -c \"($N)\")\"\n"
    "echo \"  [self-test] macOS interface order (scutil --nwi):\"\n"
    "scutil --nwi | sed -n '/IPv4 network interface information/,/REACH/p' | sed 's/^/      /'\n";

// Starts passing frames between the air and macOS (c->feth must be open). A real network service, so macOS keeps
// DHCP running across configuration changes; the temporary `ipconfig set` service is the fallback (macOS drops
// it at the next change, and bridge_poll re-sets it).
static int bridge_start(Conn *c, int self_test) {
    Bridge *b = &c->br;
    memset(b, 0, sizeof *b);
    b->self_test = self_test;
    c->bridging = 1;
    if (pthread_create(&b->th, NULL, host_to_air, c) != 0) {
        c->bridging = 0;
        fprintf(stderr, "couldn't start the macOS->air thread\n");
        return -1;
    }
    b->th_running = 1;
    if (feth_service_add(FETH_NAME) == 0) {
        printf("Added the network service \"%s\" for %s (System Settings > Network shows it while this runs).\n",
               FETH_SERVICE_NAME, FETH_NAME);
    } else {
        b->use_ipconfig = 1;
        const char *dhcp[] = {"/usr/sbin/ipconfig", "set", FETH_NAME, "DHCP", NULL};
        printf("Couldn't add a network service; falling back to a temporary `ipconfig set` service.\n");
        if (feth_run(dhcp) != 0) fprintf(stderr, "ipconfig set %s DHCP failed\n", FETH_NAME);
    }
    b->t0 = b->last_status = now_s();
    fflush(stdout);
    return 0;
}

// Housekeeping between pumps: notices address changes, re-sets the fallback service, self-test, status lines
// (every `status_every` s; 0 = never).
static void bridge_poll(Conn *c, int status_every) {
    Bridge *b = &c->br;
    double t = now_s();
    if (t - b->last_ip_check >= 1) {
        b->last_ip_check = t;
        char now_ip[64];
        feth_ipv4(c->feth, now_ip, sizeof now_ip);
        if (now_ip[0] && strcmp(now_ip, b->ip) != 0) {
            printf("macOS configured %s: %s (after %.1f s)\n", FETH_NAME, now_ip, t - b->t0);
            fflush(stdout);
            if (b->self_test && !b->had_ip && !b->test_pid) b->test_pid = (pid_t)feth_spawn_sh(kSelfTest);
            b->had_ip = 1;
        } else if (!now_ip[0] && b->ip[0]) {
            b->losses++;
            printf("macOS removed the address from %s after %.1f s%s\n", FETH_NAME, t - b->t0,
                   b->use_ipconfig ? "; asking for DHCP again" : "; it should ask for a new one by itself");
            fflush(stdout);
        }
        snprintf(b->ip, sizeof b->ip, "%s", now_ip);
        if (b->use_ipconfig && !b->ip[0] && b->had_ip && t - b->last_reset >= 5) {
            const char *dhcp[] = {"/usr/sbin/ipconfig", "set", FETH_NAME, "DHCP", NULL};
            b->last_reset = t;
            feth_run(dhcp);
        }
    }
    if (b->test_pid && waitpid(b->test_pid, NULL, WNOHANG) == b->test_pid) {
        b->test_pid = 0;
        printf("  [self-test] done. Now try it with the Mac's own Wi-Fi turned off (see README).\n");
        fflush(stdout);
    }
    if (status_every > 0 && t - b->last_status >= status_every) {
        b->last_status = t;
        printf("  [%4.0f s] %s %s; air->Mac %lu frames (%.1f MB), Mac->air %lu frames (%.1f MB)\n", t - b->t0,
               FETH_NAME, b->ip[0] ? b->ip : "has no IPv4 address", c->to_host, c->to_host_bytes / 1e6,
               (unsigned long)c->from_host, c->from_host_bytes / 1e6);
        fflush(stdout);
    }
}

static void bridge_stop(Conn *c) {
    Bridge *b = &c->br;
    c->bridging = 0;
    if (b->th_running) pthread_join(b->th, NULL);
    b->th_running = 0;
    if (b->test_pid) { kill(b->test_pid, SIGTERM); waitpid(b->test_pid, NULL, 0); b->test_pid = 0; }
    if (b->use_ipconfig) {
        const char *none[] = {"/usr/sbin/ipconfig", "set", FETH_NAME, "NONE", NULL};
        feth_run(none);
    } else if (feth_service_remove() < 0) {
        fprintf(stderr, "Couldn't remove the network service; run `sudo build/macwifictl cleanup`.\n");
    }
    printf("Bridged for %.0f s: air->Mac %lu frames (%.1f MB), Mac->air %lu frames (%.1f MB)%s\n", now_s() - b->t0,
           c->to_host, c->to_host_bytes / 1e6, (unsigned long)c->from_host, c->from_host_bytes / 1e6,
           c->host_write_err ? " (some writes failed)" : "");
    if (b->losses) printf("  macOS removed the address %d time(s) while bridging\n", b->losses);
    for (int v = 1; v < BRIDGE_DROP_COUNT; v++) {
        if (c->drop_to[v]) printf("  dropped on the way to macOS (%s): %lu\n", bridge_verdict_name((BridgeVerdict)v), c->drop_to[v]);
        if (c->drop_from[v]) printf("  dropped on the way from macOS (%s): %lu\n", bridge_verdict_name((BridgeVerdict)v), c->drop_from[v]);
    }
    if (c->reord)
        printf("  reordering: %lu frames flagged; %lu in order, %lu held back, %lu late, %lu duplicates, %lu gaps skipped\n",
               c->reord, c->ro.in_order, c->ro.buffered, c->ro.late, c->ro.dups, c->ro.timeouts);
}

// `join`: bridge until Ctrl-C, `seconds`, or a disconnect. 0 if macOS got an address.
static int bridge_run(Conn *c, WpaSupplicant *ws, int seconds) {
    if (bridge_start(c, 1)) return 1;
    printf("Bridging: %s is up and macOS is asking for an address by DHCP. Press Ctrl-C to stop.\n", FETH_NAME);
    fflush(stdout);
    int rc = 0;
    while (!gStop && !c->disconnected && !c->usb_error && (seconds <= 0 || now_s() - c->br.t0 < seconds)) {
        pump(c, 500, w_pending);
        if (c->pending && ws && handle_wpa_events(c, ws)) { rc = 1; break; }
        bridge_poll(c, 30);
    }
    if (c->disconnected) {
        printf("The access point disconnected us: reason %u (%s).\n", c->disc_reason, reason_text(c->disc_reason));
        rc = 1;
    }
    if (!c->br.had_ip && rc == 0) rc = 1;
    bridge_stop(c);
    return rc;
}

// ---------------------------------------------------------------- CLI: connect / join

static int connect_run(Link *lp, const uint8_t mac[6], uint8_t vif, const char *ssid, int listen_s, Feth *feth);

static int cmd_connect(int argc, char **argv) {
    if (argc < 3 || argv[2][0] == '-' || !argv[2][0] || strlen(argv[2]) > 32) {
        fprintf(stderr, "usage: macwifictl connect \"SSID\" [--seconds N] [--profile v5|legacy] [--fw DIR]\n");
        return 2;
    }
    const char *ssid = argv[2];
    const char *secs_opt = opt(argc, argv, "--seconds");
    int listen_s = secs_opt ? atoi(secs_opt) : 15;
    if (listen_s < 1) listen_s = 1;
    if (listen_s > 600) listen_s = 600;

    Link l;
    uint8_t mac[6], vif = 0;
    int rc = up_link(argc, argv, &l, mac);
    if (rc) return rc;
    rc = radio_start(&l, mac, &vif);
    if (!rc) rc = connect_run(&l, mac, vif, ssid, listen_s, NULL);
    link_close(&l);
    return rc;
}

static int cmd_join(int argc, char **argv) {
    if (argc < 3 || argv[2][0] == '-' || !argv[2][0] || strlen(argv[2]) > 32) {
        fprintf(stderr, "usage: sudo macwifictl join \"SSID\" [--seconds N] [--profile v5|legacy] [--fw DIR]\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "join creates a network interface for macOS, which needs root: run it with sudo.\n");
        return 2;
    }
    const char *secs_opt = opt(argc, argv, "--seconds");
    int seconds = secs_opt ? atoi(secs_opt) : 0;
    Link l;
    uint8_t mac[6], vif = 0;
    int rc = up_link(argc, argv, &l, mac);
    if (rc) return rc;
    rc = radio_start(&l, mac, &vif);
    if (!rc) {
        static Feth f;
        if (feth_service_remove() == 1) printf("Removed a network service left over from an earlier run.\n");
        if (feth_open(&f, mac)) {
            rc = 1;
        } else {
            printf("Created %s for macOS (MAC %02x:%02x:%02x:%02x:%02x:%02x; peer %s for raw I/O)\n", f.name, mac[0],
                   mac[1], mac[2], mac[3], mac[4], mac[5], f.peer);
            rc = connect_run(&l, mac, vif, argv[2], seconds, &f);
            feth_close(&f);
            printf("Removed %s and %s.\n", FETH_NAME, FETH_PEER);
        }
    }
    link_close(&l);
    return rc;
}

// Everything after the radio is up: scan, password, associate, handshake, then probe (`connect`) or bridge
// (`join`, when `feth` is given), disconnect. Leaves `lp` open.
static int connect_run(Link *lp, const uint8_t mac[6], uint8_t vif, const char *ssid, int listen_s, Feth *feth) {
    Link l = *lp;
    size_t ssid_len = strlen(ssid);
    if (l.out_ep != 1) printf("(note: the first bulk OUT endpoint is 0x%02x, not 0x01; data may not flow)\n", l.out_ep);

    // 1. Directed scan for the network.
    static ScanList sl;
    memset(&sl, 0, sizeof sl);
    uint8_t p[AIC_SZ_SCANU_START_REQ], r[16];
    uint16_t rl = 0;
    size_t n = aic_fmac_scan_params(p, vif, ssid);
    uint8_t cmd[16 + AIC_SZ_SCANU_START_REQ];
    size_t cl = aic_build_cmd(cmd, sizeof cmd, AIC_SCANU_START_REQ, AIC_SCANU_START_REQ >> 10, p, (uint16_t)n);
    printf("Looking for \"%s\"...\n", ssid);
    fflush(stdout);
    AicScanCfm sc;
    if (!cl || link_send(&l, cmd, cl) || collect_scan(&l, &sl, AIC_SCANU_START_CFM, r, sizeof r, &rl, 20000, NULL) ||
        aic_fmac_parse_scan_cfm(r, rl, &sc)) {
        printf("Scan failed. Unplug and replug the adapter, then try again.\n");
        return 1;
    }
    int found = 0;
    const ScanEntry *best = scan_pick(&sl, (const uint8_t *)ssid, ssid_len, 1, &found);
    if (!best) {
        printf("%s\n", found ? "No access point for this network uses WPA2 or WPA3 Personal with CCMP (or is open), "
                               "which is what this version supports."
                             : "Network not found. Check the name (it is case-sensitive) and that it is in range.");
        return 1;
    }
    int secured;
    const char *why;
    bss_usable(&best->bss, &secured, &why);

    // 2. Password -> PMK. Nothing about it is printed or written anywhere.
    static WpaSupplicant ws;
    memset(&ws, 0, sizeof ws);
    uint8_t pmk[32];
    char pass[128] = "";
    if (secured && ask_pmk((const uint8_t *)ssid, ssid_len, pmk, pass)) return 2;

    link_rx_start(&l);                     // several bulk-IN reads in flight from here on
    static Conn c;
    memset(&c, 0, sizeof c);
    c.l = &l;
    memcpy(c.mac, mac, 6);
    c.vif = vif;
    c.feth = feth;
    pthread_mutex_init(&c.tx_lock, NULL);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGHUP, on_sigint);

    // 3-4. Associate and (for WPA2) the 4-way handshake.
    char err[200] = "";
    int stage = 0;   // 2 connected, 3 traffic seen
    JoinCred cred = {pass[0] ? pass : NULL, strlen(pass), pmk};
    JoinResult jr = secured ? conn_join_secured(&c, best, (const uint8_t *)ssid, ssid_len, &ws, &cred, err, sizeof err)
                            : conn_associate(&c, best, (const uint8_t *)ssid, ssid_len, NULL, NULL, 0, err, sizeof err);
    secure_zero(pass, sizeof pass);
    secure_zero(pmk, sizeof pmk);
    if (jr != JOIN_OK) {
        printf("%c%s.\n", toupper((unsigned char)err[0]), err + 1);
    } else if (feth) {
        stage = 2;
        if (bridge_run(&c, secured ? &ws : NULL, listen_s) == 0) stage = 3;
    } else {
        stage = 2;
        // 5. DHCP probe: DISCOVER only (no address is taken), and count what arrives.
        printf("Listening for %d s and asking the DHCP server for an offer...\n", listen_s);
        fflush(stdout);
        unsigned base = c.rx;
        c.xid = arc4random();
        uint8_t f[600];
        size_t fl = dhcp_build_discover(f, sizeof f, mac, c.xid);
        double tl = now_s();
        for (int tries = 0; !c.disconnected && !gStop && now_s() - tl < listen_s;) {
            if (!c.offer && tries < 3 && now_s() - tl >= tries * 4.0) {
                conn_send_eth(&c, f, fl, 0);
                tries++;
            }
            pump(&c, 500, NULL);
            if (c.pending && secured) handle_wpa_events(&c, &ws);
        }
        if (c.offer) {
            stage = 3;
            printf("  DHCP offer received (%s):\n", c.offer_decr ? "arrived encrypted" : "arrived unencrypted");
            print_ip("address", c.reply.yiaddr);
            print_ip("netmask", c.reply.mask);
            print_ip("router", c.reply.router);
            print_ip("server", c.reply.server);
            if (c.reply.lease_s) printf("    %-10s %u s\n", "lease", c.reply.lease_s);
            printf("  (only offered: nothing was requested, so no address was assigned)\n");
        } else {
            printf("  No DHCP offer arrived.\n");
        }
        if (c.rx > base) stage = 3;
        if (c.disconnected) printf("  Disconnected: reason %u (%s).\n", c.disc_reason, reason_text(c.disc_reason));
    }

    printf("Traffic: %u frames received (%u broadcast/multicast, %u unencrypted): IPv4 %u, ARP %u, IPv6 %u, EAPOL %u, "
           "other %u; %u not convertible; %u frames sent\n", c.rx, c.rx_group, c.rx_plain, c.rx_ipv4, c.rx_arp,
           c.rx_ipv6, c.rx_eapol, c.rx_other, c.rx_not_eth, c.tx);
    if (c.rx_malformed) printf("  %lu IN transfers couldn't be fully parsed (please report)\n", c.rx_malformed);

    // 6. Leave cleanly.
    conn_leave(&c);
    link_rx_stop(&l);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    pthread_mutex_destroy(&c.tx_lock);
    secure_zero(&ws, sizeof ws);
    secure_zero(c.snonce, sizeof c.snonce);
    if (feth) {
        printf("\n%s\n", stage == 3 ? "Phase 4: the bridge ran and macOS got an address. Report the self-test lines "
                                     "and the Wi-Fi-off checks."
                                   : "join stopped. Unplug and replug the adapter before the next try.");
        return stage == 3 ? 0 : 1;
    }
    printf("\n%s\n", stage == 3 ? "Phase 3b hardware check passed: connected and traffic flowed."
                     : stage == 2 ? "Connected, but no traffic arrived. Unplug and replug the adapter, then try "
                                    "again with MACWIFI_DEBUG=1."
                                  : "Connect stopped. Unplug and replug the adapter before the next try.");
    return stage == 3 ? 0 : 1;
}

#include "daemon.c"   // macwifid (shares this file's static helpers)

// ---------------------------------------------------------------- Bluetooth (phase B1)

typedef struct { HciSeen seen; int inquiry_done; } BtScan;

static void bt_on_device(void *ctx, const HciDevice *d) { hci_seen_add(&((BtScan *)ctx)->seen, d); }

static void bt_on_event(void *ctx, const HciEvent *ev) {
    BtScan *s = ctx;
    if (ev->code == HCI_EV_INQUIRY_COMPLETE) s->inquiry_done = 1;
    else if (ev->code == HCI_EV_HW_ERROR) printf("Bluetooth controller reported a hardware error (0x%02x).\n", ev->len ? ev->p[0] : 0);
    else hci_scan_event(ev, bt_on_device, s);
}

static int bt_by_rssi(const void *a, const void *b) {
    const HciDevice *x = a, *y = b;
    int rx = x->rssi == 127 ? -200 : x->rssi, ry = y->rssi == 127 ? -200 : y->rssi;
    return ry - rx;
}

static int bt_step(const char *what, int st) {
    if (st == 0) return 0;
    if (st == -2) fprintf(stderr, "%s: the controller didn't answer.\n", what);
    else if (st == -1) fprintf(stderr, "%s: USB error.\n", what);
    else fprintf(stderr, "%s: controller error 0x%02x.\n", what, st);
    return 1;
}

static int cmd_bt(int argc, char **argv) {
    const char *sec = opt(argc, argv, "--seconds");
    int seconds = sec ? atoi(sec) : 10;
    if (seconds < 1) seconds = 1;
    if (seconds > 120) seconds = 120;
    int show_addr = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--addresses")) show_addr = 1;

    const AicId *id = NULL;
    io_service_t svc = find_adapter(AIC_MODE_WIFI, &id);
    if (!svc) {
        io_service_t cd = find_adapter(AIC_MODE_DRIVER_CD, NULL);
        if (cd) IOObjectRelease(cd);
        fprintf(stderr, cd ? "The adapter is in driver-CD mode. Start the service (or run `macwifictl fwload`) first.\n"
                           : "No AIC8800 adapter found. Run `macwifictl list --all`.\n");
        return 3;
    }
    printf("Adapter %s (%s), %s.\n", id ? id->name : "?", daemon_running() ? "service running" : "service not running",
           "opening its Bluetooth interface");
    char err[160];
    BtLink *b = bt_open(svc, gDebug, err, sizeof err);
    IOObjectRelease(svc);
    if (!b) { fprintf(stderr, "Bluetooth: %s.\n", err); return 1; }

    BtScan scan;
    memset(&scan, 0, sizeof scan);
    uint8_t ret[260];
    size_t rl = 0;
    int rc = 1;
    if (bt_step("HCI Reset", bt_call(b, HCI_OP_RESET, NULL, 0, NULL, 0, NULL, 3000, bt_on_event, &scan))) {
        fprintf(stderr, "If the service isn't running, the chip has no firmware yet: start it, or run "
                        "`sudo build/macwifictl fwload` first.\n");
        goto out;
    }
    printf("HCI Reset: ok\n");
    if (bt_step("Read Local Version", bt_call(b, HCI_OP_READ_LOCAL_VERSION, NULL, 0, ret, sizeof ret, &rl, 2000, bt_on_event, &scan)))
        goto out;
    if (rl >= 8) {
        printf("Controller: Bluetooth %s (HCI rev 0x%04x), LMP %s subversion 0x%04x, manufacturer 0x%04x\n",
               hci_version_name(ret[0]), ret[1] | (ret[2] << 8), hci_version_name(ret[3]), ret[6] | (ret[7] << 8),
               ret[4] | (ret[5] << 8));
    }
    if (bt_step("Read BD_ADDR", bt_call(b, HCI_OP_READ_BD_ADDR, NULL, 0, ret, sizeof ret, &rl, 2000, bt_on_event, &scan)))
        goto out;
    if (rl >= 6) {
        char a[18];
        hci_addr_str(ret, a);
        printf("Controller address: %s\n", show_addr ? a : "read ok (add --addresses to show it)");
    }
    if (bt_call(b, HCI_OP_READ_LOCAL_NAME, NULL, 0, ret, sizeof ret, &rl, 2000, bt_on_event, &scan) == 0 && rl) {
        ret[rl < sizeof ret ? rl : sizeof ret - 1] = 0;
        printf("Controller name: \"%s\"\n", (char *)ret);
    }
    if (bt_call(b, HCI_OP_READ_BUFFER_SIZE, NULL, 0, ret, sizeof ret, &rl, 2000, bt_on_event, &scan) == 0 && rl >= 7)
        printf("ACL buffers: %u x %u bytes; SCO: %u x %u\n", ret[3] | (ret[4] << 8), ret[0] | (ret[1] << 8),
               ret[5] | (ret[6] << 8), ret[2]);

    // Scan: LE (active) and classic inquiry together.
    uint8_t mask[8];
    uint64_t m = HCI_EVENT_MASK_SCAN;
    for (int i = 0; i < 8; i++) mask[i] = (uint8_t)(m >> (8 * i));
    if (bt_step("Set Event Mask", bt_call(b, HCI_OP_SET_EVENT_MASK, mask, 8, NULL, 0, NULL, 2000, bt_on_event, &scan))) goto out;
    uint8_t mode = 2;                                                      // inquiry results with RSSI or EIR
    bt_call(b, HCI_OP_WRITE_INQUIRY_MODE, &mode, 1, NULL, 0, NULL, 2000, bt_on_event, &scan);
    const uint8_t sp[7] = {1, 0x60, 0x00, 0x30, 0x00, 0, 0};              // active, 60 ms interval, 30 ms window
    int le = bt_call(b, HCI_OP_LE_SET_SCAN_PARAMS, sp, 7, NULL, 0, NULL, 2000, bt_on_event, &scan) == 0;
    const uint8_t on[2] = {1, 1};
    if (le) le = bt_call(b, HCI_OP_LE_SET_SCAN_ENABLE, on, 2, NULL, 0, NULL, 2000, bt_on_event, &scan) == 0;
    if (!le) printf("LE scan: the controller refused it; classic inquiry only.\n");
    uint8_t inq[5] = {(uint8_t)HCI_GIAC_LAP, (uint8_t)(HCI_GIAC_LAP >> 8), (uint8_t)(HCI_GIAC_LAP >> 16), 0, 0};
    int len128 = (int)(seconds / 1.28);
    inq[3] = (uint8_t)(len128 < 1 ? 1 : len128 > 0x30 ? 0x30 : len128);
    int classic = bt_call(b, HCI_OP_INQUIRY, inq, 5, NULL, 0, NULL, 2000, bt_on_event, &scan) == 0;
    if (!classic) printf("Classic inquiry: the controller refused it; LE only.\n");
    if (!le && !classic) goto out;
    printf("Scanning for %d s (%s%s%s)...\n", seconds, le ? "Bluetooth LE" : "", le && classic ? " + " : "",
           classic ? "classic" : "");
    fflush(stdout);
    double end = now_s() + seconds;
    uint8_t ev[HCI_MAX_EVENT];
    size_t n = 0;
    while (now_s() < end) {
        int r = bt_next_event(b, ev, &n, 250);
        if (r < 0) { fprintf(stderr, "Bluetooth: lost the event stream (adapter unplugged?).\n"); goto out; }
        HciEvent e;
        if (r == 1 && hci_event(ev, n, &e) == 0) bt_on_event(&scan, &e);
        if (scan.inquiry_done && classic && end - now_s() > 2) {              // keep inquiring for the whole scan
            scan.inquiry_done = 0;
            bt_call(b, HCI_OP_INQUIRY, inq, 5, NULL, 0, NULL, 2000, bt_on_event, &scan);
        }
    }
    const uint8_t off[2] = {0, 0};
    if (le) bt_call(b, HCI_OP_LE_SET_SCAN_ENABLE, off, 2, NULL, 0, NULL, 2000, bt_on_event, &scan);
    if (classic && !scan.inquiry_done) bt_call(b, HCI_OP_INQUIRY_CANCEL, NULL, 0, NULL, 0, NULL, 2000, bt_on_event, &scan);

    qsort(scan.seen.dev, (size_t)scan.seen.n, sizeof scan.seen.dev[0], bt_by_rssi);
    int nle = 0, ncl = 0, named = 0;
    for (int i = 0; i < scan.seen.n; i++) {
        const HciDevice *d = &scan.seen.dev[i];
        if (d->kind == HCI_DEV_LE) nle++; else ncl++;
        if (d->name[0]) named++;
    }
    printf("Found %d device(s): %d Bluetooth LE, %d classic, %d with a name%s.\n", scan.seen.n, nle, ncl, named,
           scan.seen.overflow ? " (list full)" : "");
    for (int i = 0; i < scan.seen.n; i++) {
        const HciDevice *d = &scan.seen.dev[i];
        const char *kind = d->kind == HCI_DEV_CLASSIC ? hci_cod_kind(d->cod) : hci_appearance_kind(d->appearance);
        char a[18] = "";
        if (show_addr) hci_addr_str(d->addr, a);
        char rs[8];
        if (d->rssi == 127) snprintf(rs, sizeof rs, "  ?"); else snprintf(rs, sizeof rs, "%4d", d->rssi);
        printf("  %-7s %s dBm  %-14s %-32.32s %s\n", d->kind == HCI_DEV_LE ? "LE" : "Classic", rs, kind ? kind : "",
               d->name[0] ? d->name : "(no name)", a);
    }
    printf("Bluetooth phase B1: PASS.\n");
    rc = 0;
out:
    bt_close(b);
    return rc;
}

static void usage(void) {
    fprintf(stderr, "usage: macwifictl list [--all] | modeswitch | info | identify | memr <addr> | memw <addr> <val>\n"
                    "       fwload [--profile v5|legacy] [--fw DIR] | up [--profile v5|legacy] [--fw DIR] |\n"
                    "       scan [SSID] [--profile v5|legacy] [--fw DIR] |\n"
                    "       connect SSID [--seconds N] [--profile v5|legacy] [--fw DIR] |\n"
                    "       join SSID [--seconds N] [--profile v5|legacy] [--fw DIR] | cleanup   (both with sudo)\n"
                    "       status | networks | disconnect | ctl '<json>'   (talk to macwifid)\n"
                    "       bt [--seconds N] [--addresses]   (Bluetooth: identify the controller, scan nearby devices)\n"
                    "Supported: AICsemi AIC8800 family, e.g. UGREEN AX900 (CM762). MACWIFI_DEBUG=1 dumps USB traffic.\n");
}

int main(int argc, char **argv) {
    gDebug = getenv("MACWIFI_DEBUG") != NULL;
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return 2; }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version")) { printf("macwifictl %s\n", MW_VERSION); return 0; }
    const char *c = argv[1];
    if (!strcmp(c, "daemon")) return cmd_daemon();
    if (!strcmp(c, "status") || !strcmp(c, "networks") || !strcmp(c, "disconnect") || !strcmp(c, "ctl"))
        return cmd_ctl(argc, argv);
    if ((!strcmp(c, "fwload") || !strcmp(c, "up") || !strcmp(c, "scan") || !strcmp(c, "connect") ||
         !strcmp(c, "join") || !strcmp(c, "identify") || !strcmp(c, "modeswitch")) && daemon_running()) {
        fprintf(stderr, "macwifid (the background service) owns the adapter. Use the menu bar app or "
                        "`macwifictl status`, or uninstall the service first.\n");
        return 2;
    }
    if (!strcmp(c, "bt")) return cmd_bt(argc, argv);
    if (!strcmp(c, "list")) return cmd_list(argc > 2 && !strcmp(argv[2], "--all"));
    if (!strcmp(c, "info")) return cmd_info();
    if (!strcmp(c, "modeswitch")) return cmd_modeswitch();
    if (!strcmp(c, "identify")) return cmd_identify();
    if (!strcmp(c, "fwload")) return cmd_fwload(argc, argv);
    if (!strcmp(c, "up")) return cmd_up(argc, argv);
    if (!strcmp(c, "scan")) return cmd_scan(argc, argv);
    if (!strcmp(c, "connect")) return cmd_connect(argc, argv);
    if (!strcmp(c, "join")) return cmd_join(argc, argv);
    if (!strcmp(c, "cleanup")) {
        if (geteuid() != 0) { fprintf(stderr, "cleanup changes network settings: run it with sudo.\n"); return 2; }
        int n = feth_cleanup();
        printf("%s\n", n < 0 ? "Couldn't change the network settings." : n ? "Removed what join had left behind."
                                                                           : "Nothing to clean up.");
        return n < 0 ? 1 : 0;
    }
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
