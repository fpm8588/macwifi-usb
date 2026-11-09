// feth.c — see feth.h. Original code (MIT).

#include "feth.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/ndrv.h>
#include <netinet/in.h>
#include <spawn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <netdb.h>
#include <poll.h>

#include "bridge.h"
#include "captive.h"

// bridge_bpf_next (Sources/Shared/bridge.c) walks BPF buffers with these offsets; make sure they hold.
_Static_assert(offsetof(struct bpf_hdr, bh_caplen) == 8, "bpf_hdr.bh_caplen offset");
_Static_assert(offsetof(struct bpf_hdr, bh_hdrlen) == 16, "bpf_hdr.bh_hdrlen offset");
_Static_assert(BPF_WORDALIGN(1) == BRIDGE_BPF_ALIGN, "BPF record alignment");

extern char **environ;

int feth_run(const char *const argv[]) {
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0) return -1;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int feth_spawn_sh(const char *script) {
    pid_t pid;
    const char *argv[] = {"/bin/sh", "-c", script, NULL};
    return posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) == 0 ? (int)pid : 0;
}

static int ifc(const char *a, const char *b, const char *c, const char *d) {
    const char *argv[] = {"/sbin/ifconfig", a, b, c, d, NULL};
    int rc = feth_run(argv);
    if (rc != 0) fprintf(stderr, "ifconfig %s %s%s%s%s%s failed (%d)\n", a, b, c ? " " : "", c ? c : "", d ? " " : "",
                         d ? d : "", rc);
    usleep(10000);
    return rc;
}

static void destroy_pair(const char *name, const char *peer) {
    const char *a[] = {"/sbin/ifconfig", peer, "destroy", NULL};
    const char *b[] = {"/sbin/ifconfig", name, "destroy", NULL};
    if (if_nametoindex(peer)) feth_run(a);
    if (if_nametoindex(name)) feth_run(b);
}

int feth_open(Feth *f, const uint8_t mac[6]) {
    memset(f, 0, sizeof *f);
    f->bpf = f->ndrv = -1;
    snprintf(f->name, sizeof f->name, "%s", FETH_NAME);
    snprintf(f->peer, sizeof f->peer, "%s", FETH_PEER);
    int have_name = if_nametoindex(f->name) != 0, have_peer = if_nametoindex(f->peer) != 0;
    if (have_name && have_peer) {
        printf("Removing %s/%s left over from an earlier run.\n", f->name, f->peer);
        destroy_pair(f->name, f->peer);
    } else if (have_name || have_peer) {
        fprintf(stderr, "%s already exists and isn't ours; not touching it.\n", have_name ? f->name : f->peer);
        return -1;
    }

    char lladdr[18];
    snprintf(lladdr, sizeof lladdr, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (ifc(f->peer, "create", NULL, NULL) || ifc(f->name, "create", NULL, NULL)) goto fail;
    f->created = 1;
    if (ifc(f->name, "lladdr", lladdr, NULL) || ifc(f->peer, "peer", f->name, NULL) ||
        ifc(f->peer, "mtu", "1500", "up") || ifc(f->name, "mtu", "1500", "up"))
        goto fail;

    // BPF on the peer: what macOS sends on `name` arrives here.
    for (int i = 1; i < 256 && f->bpf < 0; i++) {
        char dev[16];
        snprintf(dev, sizeof dev, "/dev/bpf%d", i);
        f->bpf = open(dev, O_RDWR);
        if (f->bpf < 0 && errno != EBUSY) break;
    }
    if (f->bpf < 0) { perror("open /dev/bpf*"); goto fail; }
    u_int blen = 131072, one = 1, zero = 0;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", f->peer);
    struct timeval tv = {0, 250000};
    if (ioctl(f->bpf, BIOCSBLEN, &blen) < 0 || ioctl(f->bpf, BIOCSETIF, &ifr) < 0 ||
        ioctl(f->bpf, BIOCIMMEDIATE, &one) < 0 || ioctl(f->bpf, BIOCSSEESENT, &zero) < 0 ||
        ioctl(f->bpf, BIOCSHDRCMPLT, &one) < 0 || ioctl(f->bpf, BIOCPROMISC, NULL) < 0 ||
        ioctl(f->bpf, BIOCSRTIMEOUT, &tv) < 0 || ioctl(f->bpf, BIOCGBLEN, &blen) < 0) {
        perror("BPF setup");
        goto fail;
    }
    f->blen = blen;
    f->rbuf = malloc(f->blen);
    if (!f->rbuf) goto fail;

    // AF_NDRV on the peer: what we write comes out of `name` as received traffic.
    f->ndrv = socket(AF_NDRV, SOCK_RAW, 0);
    if (f->ndrv < 0) { perror("socket(AF_NDRV)"); goto fail; }
    struct sockaddr_ndrv sa;
    memset(&sa, 0, sizeof sa);
    sa.snd_len = sizeof sa;
    sa.snd_family = AF_NDRV;
    snprintf((char *)sa.snd_name, sizeof sa.snd_name, "%s", f->peer);
    if (bind(f->ndrv, (struct sockaddr *)&sa, sizeof sa) < 0 || connect(f->ndrv, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("AF_NDRV bind/connect");
        goto fail;
    }
    return 0;

fail:
    feth_close(f);
    return -1;
}

long feth_read(Feth *f) {
    ssize_t n = read(f->bpf, f->rbuf, f->blen);
    if (n < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    return (long)n;
}

int feth_write(Feth *f, const uint8_t *eth, size_t len) {
    return write(f->ndrv, eth, len) == (ssize_t)len ? 0 : -1;
}

void feth_close(Feth *f) {
    if (f->ndrv >= 0) close(f->ndrv);
    if (f->bpf >= 0) close(f->bpf);
    free(f->rbuf);
    f->rbuf = NULL;
    f->ndrv = f->bpf = -1;
    if (f->created) destroy_pair(f->name, f->peer);
    f->created = 0;
}

void feth_ipv4(const Feth *f, char *out, size_t cap) {
    out[0] = 0;
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || strcmp(i->ifa_name, f->name) != 0) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)cap);
        break;
    }
    freeifaddrs(ifa);
}

// ---------------------------------------------------------------- the network service (SystemConfiguration)

static CFStringRef cfstr(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

static void sc_error(const char *what) { fprintf(stderr, "%s: %s\n", what, SCErrorString(SCError())); }

// Opens and locks the network preferences; returns the current set's path ("/Sets/<id>") in *set.
static SCPreferencesRef prefs_open(CFStringRef *set) {
    SCPreferencesRef p = SCPreferencesCreate(NULL, CFSTR("macwifictl"), NULL);
    if (!p) { sc_error("SCPreferencesCreate"); return NULL; }
    if (!SCPreferencesLock(p, 1)) { sc_error("SCPreferencesLock"); CFRelease(p); return NULL; }
    CFTypeRef cur = SCPreferencesGetValue(p, kSCPrefCurrentSet);
    if (!cur || CFGetTypeID(cur) != CFStringGetTypeID()) {
        fprintf(stderr, "network preferences have no current set\n");
        SCPreferencesUnlock(p);
        CFRelease(p);
        return NULL;
    }
    *set = (CFStringRef)cur;
    return p;
}

static int prefs_close(SCPreferencesRef p, int changed) {
    int rc = 0;
    if (changed) {
        if (!SCPreferencesCommitChanges(p)) { sc_error("SCPreferencesCommitChanges"); rc = -1; }
        else if (!SCPreferencesApplyChanges(p)) { sc_error("SCPreferencesApplyChanges"); rc = -1; }
    }
    SCPreferencesUnlock(p);
    CFRelease(p);
    return rc;
}

// Adds or removes our ID in <set>/Network/Global/IPv4 ServiceOrder. Returns 1 if it changed something.
static int service_order(SCPreferencesRef p, CFStringRef set, CFStringRef id, int add) {
    CFStringRef path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@/%@/%@"), set, kSCCompNetwork, kSCCompGlobal,
                                                kSCEntNetIPv4);
    CFDictionaryRef g = SCPreferencesPathGetValue(p, path);
    CFMutableDictionaryRef ng = g ? CFDictionaryCreateMutableCopy(NULL, 0, g)
                                  : CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks);
    CFArrayRef order = CFDictionaryGetValue(ng, kSCPropNetServiceOrder);
    CFMutableArrayRef no = (order && CFGetTypeID(order) == CFArrayGetTypeID())
                               ? CFArrayCreateMutableCopy(NULL, 0, order)
                               : CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFIndex i = CFArrayGetFirstIndexOfValue(no, CFRangeMake(0, CFArrayGetCount(no)), id);
    int changed = 0;
    if (add && i == kCFNotFound) { CFArrayAppendValue(no, id); changed = 1; }
    if (!add && i != kCFNotFound) { CFArrayRemoveValueAtIndex(no, i); changed = 1; }
    if (changed) {
        CFDictionarySetValue(ng, kSCPropNetServiceOrder, no);
        if (!SCPreferencesPathSetValue(p, path, ng)) { sc_error("set ServiceOrder"); changed = 0; }
    }
    CFRelease(no);
    CFRelease(ng);
    CFRelease(path);
    return changed;
}

static CFMutableDictionaryRef dict(void) {
    return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

int feth_service_add(const char *ifname) {
    CFStringRef set = NULL;
    SCPreferencesRef p = prefs_open(&set);
    if (!p) return -1;
    CFStringRef id = CFSTR(FETH_SERVICE_ID);
    CFStringRef svc = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"), kSCPrefNetworkServices, id);
    CFStringRef link = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@/%@/%@"), set, kSCCompNetwork, kSCCompService, id);
    CFStringRef dev = cfstr(ifname);

    CFMutableDictionaryRef intf = dict(), v4 = dict(), v6 = dict(), dns = dict(), top = dict();
    CFDictionarySetValue(intf, kSCPropNetInterfaceDeviceName, dev);
    CFDictionarySetValue(intf, kSCPropNetInterfaceHardware, kSCEntNetEthernet);
    CFDictionarySetValue(intf, kSCPropNetInterfaceType, kSCValNetInterfaceTypeEthernet);
    CFDictionarySetValue(intf, kSCPropUserDefinedName, CFSTR(FETH_SERVICE_NAME));
    CFDictionarySetValue(v4, kSCPropNetIPv4ConfigMethod, kSCValNetIPv4ConfigMethodDHCP);
    CFDictionarySetValue(v6, kSCPropNetIPv6ConfigMethod, kSCValNetIPv6ConfigMethodAutomatic);
    CFDictionarySetValue(top, kSCEntNetInterface, intf);
    CFDictionarySetValue(top, kSCEntNetIPv4, v4);
    CFDictionarySetValue(top, kSCEntNetIPv6, v6);
    CFDictionarySetValue(top, kSCEntNetDNS, dns);
    CFDictionarySetValue(top, kSCPropUserDefinedName, CFSTR(FETH_SERVICE_NAME));

    int ok = SCPreferencesPathSetValue(p, svc, top) && SCPreferencesPathSetLink(p, link, svc);
    if (!ok) sc_error("adding the network service");
    else service_order(p, set, id, 1);
    int rc = prefs_close(p, ok);

    CFRelease(top); CFRelease(dns); CFRelease(v6); CFRelease(v4); CFRelease(intf);
    CFRelease(dev); CFRelease(link); CFRelease(svc);
    return ok ? rc : -1;
}

int feth_service_remove(void) {
    CFStringRef set = NULL;
    SCPreferencesRef p = prefs_open(&set);
    if (!p) return -1;
    CFStringRef id = CFSTR(FETH_SERVICE_ID);
    CFStringRef svc = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"), kSCPrefNetworkServices, id);
    int changed = 0;
    // the link may exist in every set (location) if someone switched locations meanwhile
    CFDictionaryRef sets = SCPreferencesGetValue(p, kSCPrefSets);
    if (sets && CFGetTypeID(sets) == CFDictionaryGetTypeID()) {
        CFIndex n = CFDictionaryGetCount(sets);
        const void **keys = calloc((size_t)(n ? n : 1), sizeof *keys);
        CFDictionaryGetKeysAndValues(sets, keys, NULL);
        for (CFIndex i = 0; i < n; i++) {
            CFStringRef sp = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"), kSCPrefSets, keys[i]);
            CFStringRef link = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@/%@/%@"), sp, kSCCompNetwork,
                                                        kSCCompService, id);
            if (SCPreferencesPathGetValue(p, link) && SCPreferencesPathRemoveValue(p, link)) changed = 1;
            if (service_order(p, sp, id, 0)) changed = 1;
            CFRelease(link);
            CFRelease(sp);
        }
        free(keys);
    }
    if (SCPreferencesPathGetValue(p, svc) && SCPreferencesPathRemoveValue(p, svc)) changed = 1;
    CFRelease(svc);
    int rc = prefs_close(p, changed);
    return rc < 0 ? -1 : changed;
}

int feth_cleanup(void) {
    int n = 0, r = feth_service_remove();
    if (r < 0) return -1;
    n += r;
    const char *a[] = {"/sbin/ifconfig", FETH_PEER, "destroy", NULL};
    const char *b[] = {"/sbin/ifconfig", FETH_NAME, "destroy", NULL};
    if (if_nametoindex(FETH_PEER) && feth_run(a) == 0) n++;
    if (if_nametoindex(FETH_NAME) && feth_run(b) == 0) n++;
    return n;
}

// ---------------------------------------------------------------- captive portal check

int feth_captive_probe(const char *ifname, char *url, size_t cap) {
    snprintf(url, cap, "%s", CAPTIVE_URL);
    unsigned idx = if_nametoindex(ifname);
    if (!idx) return CAPTIVE_UNKNOWN;
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(CAPTIVE_HOST, "80", &hints, &ai) != 0 || !ai) return CAPTIVE_UNKNOWN;
    int rc = CAPTIVE_UNKNOWN;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof idx) == 0) {
        struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int c = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (c != 0 && errno == EINPROGRESS) {
            struct pollfd pf = {fd, POLLOUT, 0};
            int err = 0;
            socklen_t el = sizeof err;
            c = (poll(&pf, 1, 5000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && !err) ? 0 : -1;
        }
        fcntl(fd, F_SETFL, fl);
        char req[256], resp[8192];
        size_t rl = captive_request(req, sizeof req), got = 0;
        if (c == 0 && rl && write(fd, req, rl) == (ssize_t)rl) {
            ssize_t n;
            while (got < sizeof resp - 1 && (n = read(fd, resp + got, sizeof resp - 1 - got)) > 0) got += (size_t)n;
            resp[got] = 0;
            rc = captive_classify(resp, got, url, cap);
        }
    }
    if (fd >= 0) close(fd);
    freeaddrinfo(ai);
    return rc;
}
