// knownnet.h — the networks macwifid remembers, and which one to join. Original code (MIT).
//
// Stored per network: the SSID and the PMK (the 256-bit key PBKDF2 derives from the passphrase and SSID; the
// passphrase itself is never stored), or no key for an open network. File format, one network per line:
//   <ssid as hex> <pmk as 64 hex digits, or "-" for open> <autojoin 0|1>
// The daemon keeps the file root-only (0600).

#ifndef KNOWNNET_H
#define KNOWNNET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KN_MAX 32

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    int has_pmk;
    uint8_t pmk[32];
    int autojoin;
} KnownNet;

typedef struct {
    KnownNet n[KN_MAX];
    int count;
} KnownList;

// Parses the file contents (bad lines are skipped). Returns the number of networks.
int kn_parse(KnownList *k, const char *text, size_t len);
// Serialises; returns the length, or 0 if `cap` is too small.
size_t kn_format(const KnownList *k, char *out, size_t cap);
KnownNet *kn_find(KnownList *k, const uint8_t *ssid, size_t ssid_len);
// Adds or replaces (newest first). Returns 0, or -1 if the SSID is invalid.
int kn_put(KnownList *k, const uint8_t *ssid, size_t ssid_len, const uint8_t *pmk /* NULL = open */);
// Returns 1 if a network was removed.
int kn_remove(KnownList *k, const uint8_t *ssid, size_t ssid_len);
void kn_wipe(KnownList *k);

// A network seen in a scan, as far as auto-join cares.
typedef struct {
    const uint8_t *ssid;
    size_t ssid_len;
    int rssi;
    int usable;   // we can join it (WPA2-PSK/CCMP without required PMF, or open)
    int secured;  // RSN present
} KnSeen;

// Index into `seen` of the network to auto-join: known with autojoin, usable, and whose stored key type matches
// (PMK for secured, none for open); among those the strongest signal. -1 if none.
int kn_choose(const KnownList *k, const KnSeen *seen, int n);

#ifdef __cplusplus
}
#endif
#endif
