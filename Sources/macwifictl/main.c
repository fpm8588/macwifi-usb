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
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aic8800.h"
#include "aic_fwload.h"
#include "aic_msg.h"
#include "descdump.h"

extern char **environ;

typedef IOUSBDeviceInterface320 **Dev;
typedef IOUSBInterfaceInterface300 **Intf;

static int gDebug = 0;
static size_t gDumpLimit = 256;   // bytes shown per transfer with MACWIFI_DEBUG=1 (lowered during uploads)

static void hexdump(const char *tag, const uint8_t *p, size_t n) {
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

static int cmd_fwload(int argc, char **argv) {
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
            printf("\n");
            cmd_info();
            printf("\nPhase 2a hardware check %s.\n", now->pid == AIC_PID_D80_FW ? "passed" : "finished (unexpected ID: record it)");
            return now->pid == AIC_PID_D80_FW ? 0 : 1;
        }
        if (i % 4 == 3) { putchar('.'); fflush(stdout); }
    }
    printf("\n%s\n", gone ? "The adapter disconnected and has not come back within 10 s. Run `macwifictl list --all`."
                          : "The adapter is still a69c:8d80 after 10 s (no restart seen). Run `macwifictl list --all`.");
    return 1;
}

static void usage(void) {
    fprintf(stderr, "usage: macwifictl list [--all] | modeswitch | info | identify | fwload [--profile v5|legacy]\n"
                    "       [--fw DIR] | memr <addr> | memw <addr> <val>\n"
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
