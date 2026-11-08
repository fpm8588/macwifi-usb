// feth.h — a macOS feth (fake Ethernet) pair as the adapter's network interface. Original code (MIT). Needs root.
//
// `name` (e.g. feth4100) is the interface macOS configures (DHCP, IPv6, DNS); `peer` (e.g. feth9100) is its
// other end, where we read what macOS sends (BPF) and inject what the adapter receives (AF_NDRV). Facts about
// this arrangement come from ZeroTier's osdep/MacEthernetTapAgent.c (MPL-2.0; no code taken).

#ifndef FETH_H
#define FETH_H

#include <stddef.h>
#include <stdint.h>

#define FETH_NAME "feth4100"
#define FETH_PEER "feth9100"

typedef struct {
    char name[16], peer[16];
    int bpf, ndrv;
    uint8_t *rbuf;
    size_t blen;
    int created;
} Feth;

// Creates the pair (MAC = `mac`, MTU 1500, both up) and opens BPF + AF_NDRV on the peer. A pair left behind
// by an earlier run (both names present) is destroyed and recreated. Returns 0, or -1 with a message printed.
int feth_open(Feth *f, const uint8_t mac[6]);
// One BPF read (waits up to 250 ms). Returns the number of bytes in f->rbuf (0 on timeout), or -1.
long feth_read(Feth *f);
// Hands one Ethernet frame to macOS. Returns 0 or -1.
int feth_write(Feth *f, const uint8_t *eth, size_t len);
// Closes the descriptors and destroys both interfaces.
void feth_close(Feth *f);
// The interface's IPv4 address as text, or "" if it has none yet.
void feth_ipv4(const Feth *f, char *out, size_t cap);
// A persistent network service for `name` in macOS's network preferences (like the one macOS creates for a USB
// Ethernet adapter): IPv4 by DHCP, IPv6 automatic, placed last in the service order. `ipconfig set` only makes
// a temporary service that macOS drops at the next network configuration change (ipconfig(8)), which is
// exactly what happens when the Mac's own Wi-Fi or Ethernet goes away. Fixed service ID so leftovers can be
// found and removed. Return 0, or -1 with a message printed.
#define FETH_SERVICE_ID "6D616377-6966-6900-0000-00000F004100"
#define FETH_SERVICE_NAME "UGREEN USB Wi-Fi (macwifi)"
int feth_service_add(const char *ifname);
int feth_service_remove(void);   // also succeeds when there is nothing to remove; returns 1 if it removed one

// Removes our network service and our two interfaces if they exist (after a crash). Returns the number of
// things removed, or -1 if the preferences couldn't be changed.
int feth_cleanup(void);

// Runs a program and waits; returns its exit status (or -1).
int feth_run(const char *const argv[]);
// Starts `/bin/sh -c script` without waiting; returns its pid, or 0 if it couldn't start.
int feth_spawn_sh(const char *script);

#endif
