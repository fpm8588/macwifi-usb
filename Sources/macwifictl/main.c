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
#include "bridge.h"
#include "dhcp.h"
#include "eapol.h"
#include "ieee80211.h"
#include "wpa.h"
#include "descdump.h"
#include "feth.h"

extern char **environ;

typedef IOUSBDeviceInterface320 **Dev;
typedef IOUSBInterfaceInterface300 **Intf;

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
    const AicId *id;
    int dev_open;                         // 0 when macOS's composite-device driver holds the device
} Link;

static void link_close(Link *l) {
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

static volatile sig_atomic_t gStop = 0;
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
    Link *l;
    uint8_t mac[6], bssid[6], vif;
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
    unsigned long to_host, to_host_bytes, host_write_err, reord;
    atomic_ulong from_host, from_host_bytes;   // written by the macOS->air thread
    unsigned long drop_to[BRIDGE_DROP_COUNT], drop_from[BRIDGE_DROP_COUNT];
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

// Reads the bulk IN pipe for up to `timeout_ms`, handling messages, data and TX confirmations, until `done` says
// so. Returns 0 when done, 1 on timeout, -1 on a USB error or Ctrl-C.
static int pump(Conn *c, int timeout_ms, int (*done)(const Conn *)) {
    static uint8_t buf[32768], scratch[4096];
    double t0 = now_s();
    while (!(done && done(c))) {
        if (gStop) return -1;
        if ((now_s() - t0) * 1000 >= timeout_ms) return 1;
        UInt32 size = sizeof buf;
        IOReturn kr = (*c->l->intf)->ReadPipeTO(c->l->intf, c->l->in_pipe, buf, &size, 200, 200);
        if (kr == kIOUSBTransactionTimeout || kr == kIOReturnTimeout) continue;
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "bulk IN failed: 0x%08x\n", kr);
            if (kr == kIOUSBPipeStalled) (*c->l->intf)->ClearPipeStallBothEnds(c->l->intf, c->l->in_pipe);
            return -1;
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
                if (pk.id == AIC_SM_CONNECT_CFM) {
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
            if (aic_data_rx_to_eth(&ri, scratch, sizeof scratch, on_eth, c) < 0) c->rx_not_eth++;
        }
        if (r < 0) fprintf(stderr, "malformed IN transfer (%u bytes); set MACWIFI_DEBUG=1 to see it\n", size);
    }
    return 0;
}

static int w_got(const Conn *c) { return c->got || c->disconnected; }
static int w_connect_cfm(const Conn *c) { return c->connect_cfm; }
static int w_assoc(const Conn *c) { return c->associated || c->disconnected; }
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
static int ask_pmk(const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]) {
    char pass[128];
    char prompt[96];
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
    }
    secure_zero(pass, sizeof pass);
    return rc;
}

static void print_ip(const char *label, const uint8_t a[4]) {
    if (a[0] | a[1] | a[2] | a[3]) printf("    %-10s %u.%u.%u.%u\n", label, a[0], a[1], a[2], a[3]);
}

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

// ---------------------------------------------------------------- Phase 4: join (bridge to macOS via feth)

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

static int bridge_run(Conn *c, WpaSupplicant *ws, int seconds) {
    c->bridging = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, host_to_air, c) != 0) {
        c->bridging = 0;
        fprintf(stderr, "couldn't start the macOS->air thread\n");
        return 1;
    }
    // A real network service, so macOS keeps DHCP running across configuration changes; the temporary
    // `ipconfig set` service is the fallback (macOS drops it at the next change; the watchdog below re-sets it).
    const char *dhcp[] = {"/usr/sbin/ipconfig", "set", FETH_NAME, "DHCP", NULL};
    int use_ipconfig = 0;
    if (feth_service_add(FETH_NAME) == 0) {
        printf("Added the network service \"%s\" for %s (System Settings > Network shows it while this runs).\n",
               FETH_SERVICE_NAME, FETH_NAME);
    } else {
        use_ipconfig = 1;
        printf("Couldn't add a network service; falling back to a temporary `ipconfig set` service.\n");
        if (feth_run(dhcp) != 0) fprintf(stderr, "ipconfig set %s DHCP failed\n", FETH_NAME);
    }
    printf("Bridging: %s is up and macOS is asking for an address by DHCP. Press Ctrl-C to stop.\n", FETH_NAME);
    fflush(stdout);

    double t0 = now_s(), last_status = t0, last_ip_check = 0, last_reset = 0;
    char ip[64] = "";
    int had_ip = 0, losses = 0;
    pid_t test_pid = 0;
    int rc = 0;
    while (!gStop && !c->disconnected && (seconds <= 0 || now_s() - t0 < seconds)) {
        pump(c, 500, w_pending);
        if (c->pending && handle_wpa_events(c, ws)) { rc = 1; break; }
        double t = now_s();
        if (t - last_ip_check >= 1) {
            last_ip_check = t;
            char now_ip[64];
            feth_ipv4(c->feth, now_ip, sizeof now_ip);
            if (now_ip[0] && strcmp(now_ip, ip) != 0) {
                printf("macOS configured %s: %s (after %.1f s)\n", FETH_NAME, now_ip, t - t0);
                fflush(stdout);
                if (!had_ip && !test_pid) test_pid = (pid_t)feth_spawn_sh(kSelfTest);
                had_ip = 1;
            } else if (!now_ip[0] && ip[0]) {
                losses++;
                printf("macOS removed the address from %s after %.1f s%s\n", FETH_NAME, t - t0,
                       use_ipconfig ? "; asking for DHCP again" : "; it should ask for a new one by itself");
                fflush(stdout);
            }
            snprintf(ip, sizeof ip, "%s", now_ip);
            if (use_ipconfig && !ip[0] && had_ip && t - last_reset >= 5) {   // temporary service dropped: re-set
                last_reset = t;
                feth_run(dhcp);
            }
        }
        if (test_pid && waitpid(test_pid, NULL, WNOHANG) == test_pid) {
            test_pid = 0;
            printf("  [self-test] done. Now try it with the Mac's own Wi-Fi turned off (see README).\n");
            fflush(stdout);
        }
        if (t - last_status >= 30) {
            last_status = t;
            printf("  [%4.0f s] %s %s; air->Mac %lu frames (%.1f MB), Mac->air %lu frames (%.1f MB)\n", t - t0,
                   FETH_NAME, ip[0] ? ip : "has no IPv4 address", c->to_host, c->to_host_bytes / 1e6,
                   (unsigned long)c->from_host, c->from_host_bytes / 1e6);
            fflush(stdout);
        }
    }
    if (c->disconnected) {
        printf("The access point disconnected us: reason %u (%s).\n", c->disc_reason, reason_text(c->disc_reason));
        rc = 1;
    }
    if (!had_ip && rc == 0) rc = 1;
    c->bridging = 0;
    pthread_join(th, NULL);
    if (test_pid) { kill(test_pid, SIGTERM); waitpid(test_pid, NULL, 0); }
    if (use_ipconfig) {
        const char *none[] = {"/usr/sbin/ipconfig", "set", FETH_NAME, "NONE", NULL};
        feth_run(none);
    } else if (feth_service_remove() < 0) {
        fprintf(stderr, "Couldn't remove the network service; run `sudo build/macwifictl cleanup`.\n");
    }
    if (losses) printf("  macOS removed the address %d time(s) while bridging\n", losses);
    double el = now_s() - t0;
    printf("Bridged for %.0f s: air->Mac %lu frames (%.1f MB), Mac->air %lu frames (%.1f MB)%s\n", el, c->to_host,
           c->to_host_bytes / 1e6, (unsigned long)c->from_host, c->from_host_bytes / 1e6,
           c->host_write_err ? " (some writes failed)" : "");
    for (int v = 1; v < BRIDGE_DROP_COUNT; v++) {
        if (c->drop_to[v]) printf("  dropped on the way to macOS (%s): %lu\n", bridge_verdict_name((BridgeVerdict)v), c->drop_to[v]);
        if (c->drop_from[v]) printf("  dropped on the way from macOS (%s): %lu\n", bridge_verdict_name((BridgeVerdict)v), c->drop_from[v]);
    }
    if (c->reord) printf("  %lu frames were flagged for reordering (Linux reorders them; not done here yet)\n", c->reord);
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

// Everything after the radio is up: scan, password, associate, handshake, probe, disconnect. Leaves `l` open.
static int connect_run(Link *lp, const uint8_t mac[6], uint8_t vif, const char *ssid, int listen_s, Feth *feth) {
    Link l = *lp;
    size_t ssid_len = strlen(ssid);

    // 1. Directed scan for the network.
    static ScanList sl;
    memset(&sl, 0, sizeof sl);
    uint8_t p[AIC_SZ_SCANU_START_REQ > AIC_SZ_SM_CONNECT_REQ ? AIC_SZ_SCANU_START_REQ : AIC_SZ_SM_CONNECT_REQ], r[16];
    uint16_t rl = 0;
    if (l.out_ep != 1) printf("(note: the first bulk OUT endpoint is 0x%02x, not 0x01; data may not flow)\n", l.out_ep);
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
    const ScanEntry *best = NULL;
    int found = 0;
    for (int i = 0; i < sl.n; i++) {
        const ScanEntry *e = &sl.e[i];
        if (e->bss.ssid_len != ssid_len || memcmp(e->bss.ssid, ssid, ssid_len) != 0) continue;
        found++;
        int ok = e->bss.rsn_akm_psk && e->bss.rsn_ccmp && e->bss.rsn_group_ccmp && !e->bss.rsn_pmf_required &&
                 e->bss.rsn_ie_len >= 2;
        printf("  %02x:%02x:%02x:%02x:%02x:%02x  %4d MHz  RSSI %4d  %-10s %s\n", e->bss.bssid[0], e->bss.bssid[1],
               e->bss.bssid[2], e->bss.bssid[3], e->bss.bssid[4], e->bss.bssid[5], e->freq, e->rssi,
               wifi_security_name(e->bss.security),
               ok ? "usable" : !e->bss.rsn_akm_psk ? "no WPA2-PSK (WPA3-only, open or enterprise)"
                             : e->bss.rsn_pmf_required ? "requires PMF (not supported yet)"
                                                       : "needs CCMP for pairwise and group keys");
        if (ok && (!best || e->rssi > best->rssi)) best = e;
    }
    if (!best) {
        printf("%s\n", found ? "No access point for this network uses WPA2-PSK with CCMP, which is what this "
                               "version supports."
                             : "Network not found. Check the name (it is case-sensitive) and that it is in range.");
        return 1;
    }
    int ch = best->bss.channel;
    int freq = ch ? wifi_channel_to_mhz(ch) : 0;
    if (!freq || (freq >= 5000) != (best->freq >= 5000)) freq = best->freq;   // trust the scan's band
    uint8_t band = freq >= 5000 ? 1 : 0;

    // 2. Password -> PMK. Nothing about it is printed or written anywhere.
    static WpaSupplicant ws;
    memset(&ws, 0, sizeof ws);
    if (ask_pmk((const uint8_t *)ssid, ssid_len, ws.pmk)) {
        return 2;
    }
    memcpy(ws.aa, best->bss.bssid, 6);
    memcpy(ws.spa, mac, 6);
    ws.rsn_ie_len = wpa_build_rsn_ie(ws.rsn_ie, sizeof ws.rsn_ie);
    ws.ap_rsn_ie = best->bss.rsn_ie;
    ws.ap_rsn_ie_len = best->bss.rsn_ie_len;

    static Conn c;
    memset(&c, 0, sizeof c);
    c.l = &l;
    memcpy(c.mac, mac, 6);
    memcpy(c.bssid, best->bss.bssid, 6);
    c.vif = vif;
    c.ws = &ws;
    c.feth = feth;
    pthread_mutex_init(&c.tx_lock, NULL);
    arc4random_buf(c.snonce, sizeof c.snonce);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGHUP, on_sigint);

    // 3. Associate.
    printf("Connecting to %02x:%02x:%02x:%02x:%02x:%02x on %d MHz...\n", c.bssid[0], c.bssid[1], c.bssid[2],
           c.bssid[3], c.bssid[4], c.bssid[5], freq);
    fflush(stdout);
    aic_fmac_connect_params(p, (const uint8_t *)ssid, ssid_len, c.bssid, (uint16_t)freq, band, vif, ws.rsn_ie,
                            ws.rsn_ie_len);
    double t0 = now_s();
    int stage = 0;   // 1 associated, 2 handshake done, 3 traffic seen
    if (send_msg(&l, AIC_SM_CONNECT_REQ, p, AIC_SZ_SM_CONNECT_REQ) || pump(&c, 3000, w_connect_cfm) || !c.connect_cfm) {
        fprintf(stderr, "No SM_CONNECT_CFM from the firmware.\n");
    } else if (c.connect_status != 0) {
        fprintf(stderr, "The firmware refused the connect request (status %u).\n", c.connect_status);
    } else if (pump(&c, 10000, w_assoc), !c.associated) {
        if (c.ind_buf[0] | c.ind_buf[1])
            printf("Association failed: status %u (%s).\n", c.ind.status_code, status_text(c.ind.status_code));
        else if (c.disconnected)
            printf("Disconnected during association: reason %u (%s).\n", c.disc_reason, reason_text(c.disc_reason));
        else
            printf("No association result within 10 s.\n");
    } else {
        stage = 1;
        printf("Associated in %.2f s: AP station index %u, AID %u, %s, %u MHz, width code %u\n", now_s() - t0,
               c.ind.ap_idx, c.ind.aid, c.ind.qos ? "QoS" : "no QoS", c.ind.center_freq, c.ind.width);

        // 4. 4-way handshake (EAPOL frames are answered inside pump; keys are installed here).
        double th = now_s();
        while (!c.port_open && !c.disconnected && !gStop && now_s() - th < 15) {
            pump(&c, 1000, w_pending);
            if (handle_wpa_events(&c, &ws)) break;
        }
        if (!c.port_open) {
            if (c.disconnected)
                printf("Disconnected during the handshake: reason %u (%s).\n", c.disc_reason, reason_text(c.disc_reason));
            else if (!ws.have_ptk)
                printf("No EAPOL message 1 from the AP (%u EAPOL frames seen).\n", c.rx_eapol);
            else
                printf("The 4-way handshake did not finish (%u EAPOL frames seen, %d rejected). A wrong password "
                       "makes the AP stop after message 2.\n", c.rx_eapol, c.eapol_rejects);
        } else {
            stage = 2;
            printf("Connected: WPA2-PSK handshake done in %.2f s, keys installed, port open.\n", now_s() - th);
          if (feth) {
            if (bridge_run(&c, &ws, listen_s) == 0) stage = 3;
          } else {
            // 5. DHCP probe: DISCOVER only (no address is taken), and count what arrives.
            printf("Listening for %d s and asking the DHCP server for an offer...\n", listen_s);
            fflush(stdout);
            unsigned base = c.rx;
            c.xid = arc4random();
            uint8_t f[600];
            size_t fl = dhcp_build_discover(f, sizeof f, mac, c.xid);
            double tl = now_s();
            for (int tries = 0; !c.disconnected && !gStop && now_s() - tl < listen_s; ) {
                if (!c.offer && tries < 3 && now_s() - tl >= tries * 4.0) {
                    conn_send_eth(&c, f, fl, 0);
                    tries++;
                }
                pump(&c, 500, NULL);
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
        }
    }

    printf("Traffic: %u frames received (%u broadcast/multicast, %u unencrypted): IPv4 %u, ARP %u, IPv6 %u, EAPOL %u, "
           "other %u; %u not convertible; %u frames sent\n", c.rx, c.rx_group, c.rx_plain, c.rx_ipv4, c.rx_arp,
           c.rx_ipv6, c.rx_eapol, c.rx_other, c.rx_not_eth, c.tx);

    // 6. Leave cleanly.
    if (c.associated && !c.disconnected) {
        uint8_t d[AIC_SZ_SM_DISCONNECT_REQ];
        aic_fmac_disconnect_params(d, 3, vif);   // WLAN_REASON_DEAUTH_LEAVING
        gStop = 0;
        if (conn_call(&c, AIC_SM_DISCONNECT_REQ, d, sizeof d, AIC_SM_DISCONNECT_CFM, 3000) == 0) printf("Disconnected.\n");
    }
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

static void usage(void) {
    fprintf(stderr, "usage: macwifictl list [--all] | modeswitch | info | identify | memr <addr> | memw <addr> <val>\n"
                    "       fwload [--profile v5|legacy] [--fw DIR] | up [--profile v5|legacy] [--fw DIR] |\n"
                    "       scan [SSID] [--profile v5|legacy] [--fw DIR] |\n"
                    "       connect SSID [--seconds N] [--profile v5|legacy] [--fw DIR] |\n"
                    "       join SSID [--seconds N] [--profile v5|legacy] [--fw DIR] | cleanup   (both with sudo)\n"
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
