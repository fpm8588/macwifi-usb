// knownnet.h — the networks macwifid remembers, and which one to join. Original code (MIT).
//
// Stored per network: the SSID and the WPA2 PMK (the 256-bit key PBKDF2 derives from the passphrase and SSID), or
// no key for an open network. WPA3 (SAE) can't work from a stored key: every sign-in runs a fresh exchange that
// needs the password itself, so for networks that offer WPA3 the password is kept too (as every WPA3 client
// has to). File format, one network per line:
//   <ssid as hex> <pmk as 64 hex digits, or "-"> <autojoin 0|1> [<password as hex>]
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
    uint8_t pass_len;         // 0: no password stored (WPA2-only entry, or open)
    char pass[64];
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
// Attaches the password to an entry (8..63 bytes); 0 length clears it. Returns 0 or -1.
int kn_set_pass(KnownNet *n, const char *pass, size_t len);
// Returns 1 if a network was removed.
int kn_remove(KnownList *k, const uint8_t *ssid, size_t ssid_len);
void kn_wipe(KnownList *k);

// A network seen in a scan, as far as auto-join cares.
typedef struct {
    const uint8_t *ssid;
    size_t ssid_len;
    int rssi;
    int usable;   // we can join it (WPA2-PSK/CCMP without required PMF, WPA3-SAE, or open)
    int secured;  // RSN present
    int sae_only; // secured and joinable only with WPA3 (needs a stored password, not just a PMK)
} KnSeen;

// Index into `seen` of the network to auto-join: known with autojoin, usable, and whose stored key type matches
// (PMK for secured, none for open); among those the strongest signal. -1 if none.
int kn_choose(const KnownList *k, const KnSeen *seen, int n);

#ifdef __cplusplus
}
#endif
#endif
