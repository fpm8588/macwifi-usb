// wpa.h — WPA2-Personal (PSK, CCMP) key derivation for a station. Original code (MIT).
//
// Implements IEEE 802.11-2020 §12.7: PMK from passphrase (PBKDF2-HMAC-SHA1, 4096 rounds),
// PTK via PRF-384 ("Pairwise key expansion"), EAPOL-Key MIC (key descriptor version 2: HMAC-SHA1-128),
// and GTK unwrapping (AES Key Wrap, RFC 3394). CCMP encryption itself is done by the chip.
// Crypto primitives come from the OS: CommonCrypto on macOS, OpenSSL on Linux (tests only).

#ifndef MACWIFI_WPA_H
#define MACWIFI_WPA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WPA_PMK_LEN    32
#define WPA_NONCE_LEN  32
#define WPA_PTK_LEN    48   // KCK(16) | KEK(16) | TK(16) for CCMP
#define WPA_KCK(ptk)   (ptk)
#define WPA_KEK(ptk)   ((ptk) + 16)
#define WPA_TK(ptk)    ((ptk) + 32)
#define WPA_MIC_LEN    16

// ---- primitives (crypto_backend.c)
int mw_hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[20]);
int mw_pbkdf2_sha1(const char *pass, size_t plen, const uint8_t *salt, size_t slen, unsigned rounds,
                   uint8_t *out, size_t outlen);
int mw_aes128_block(int encrypt, const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
int mw_hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]);

// ---- WPA3-Personal (AKM 8, SAE) key hierarchy: SHA-256 based (IEEE 802.11-2020 §12.7.1.6.2, §12.7.3)
// KDF-SHA256-<bits>: HMAC-SHA256(key, i (LE16) || label || context || bits (LE16)), i = 1, 2, ...
int wpa_kdf_sha256(const uint8_t *key, size_t klen, const char *label, const uint8_t *ctx, size_t clen,
                   uint8_t *out, size_t bits);
// PTK for SHA-256 AKMs: KDF-SHA256-384(PMK, "Pairwise key expansion", addresses and nonces as for wpa_derive_ptk).
int wpa_derive_ptk_sha256(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
                          const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[WPA_PTK_LEN]);
// AES-128-CMAC (RFC 4493): the EAPOL-Key MIC of key descriptor version 0 with AKM 8, and BIP.
int wpa_aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16]);

// ---- WPA2
// passphrase must be 8..63 printable ASCII characters; ssid 1..32 bytes. Returns 0 or -1.
int wpa_pmk_from_passphrase(const char *passphrase, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[WPA_PMK_LEN]);

// IEEE PRF-n built on HMAC-SHA1: HMAC(key, label || 0x00 || data || counter), counter = 0, 1, ...
int wpa_prf_sha1(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen,
                 uint8_t *out, size_t outlen);

// PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(...))
int wpa_derive_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
                   const uint8_t anonce[WPA_NONCE_LEN], const uint8_t snonce[WPA_NONCE_LEN],
                   uint8_t ptk[WPA_PTK_LEN]);

// EAPOL-Key MIC over the whole EAPOL frame (802.1X header onward) with the MIC field zeroed.
// key_desc_version 2 = HMAC-SHA1-128 (WPA2-PSK/CCMP); 0 = AKM-defined, here AES-128-CMAC (SAE). 1 (TKIP/MD5)
// returns -1.
int wpa_eapol_mic(const uint8_t kck[16], int key_desc_version, const uint8_t *eapol, size_t len,
                  uint8_t mic[WPA_MIC_LEN]);

// RFC 3394 AES Key Wrap with a 128-bit KEK. in_len = 8*(n+1), out gets 8*n bytes.
// unwrap returns 0 if the integrity check (A6A6A6A6A6A6A6A6) passes, -1 otherwise.
int wpa_aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t in_len, uint8_t *out);
int wpa_aes_wrap(const uint8_t kek[16], const uint8_t *in, size_t in_len, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
