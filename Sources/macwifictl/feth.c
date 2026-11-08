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

#include "bridge.h"

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
