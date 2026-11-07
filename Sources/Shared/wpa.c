// wpa.c — see wpa.h. Original code (MIT).
#include "wpa.h"

#include <string.h>

int wpa_pmk_from_passphrase(const char *pass, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[WPA_PMK_LEN]) {
    if (!pass || !ssid || !pmk) return -1;
    size_t n = strlen(pass);
    if (n < 8 || n > 63 || ssid_len < 1 || ssid_len > 32) return -1;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)pass[i] < 32 || (unsigned char)pass[i] > 126) return -1;
    return mw_pbkdf2_sha1(pass, n, ssid, ssid_len, 4096, pmk, WPA_PMK_LEN);
}

int wpa_prf_sha1(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen,
                 uint8_t *out, size_t outlen) {
    uint8_t msg[256];
    size_t llen = strlen(label);
    if (llen + 1 + dlen + 1 > sizeof msg) return -1;
    memcpy(msg, label, llen);
    msg[llen] = 0;
    memcpy(msg + llen + 1, data, dlen);
    size_t mlen = llen + 1 + dlen + 1;
    uint8_t digest[20];
    size_t pos = 0;
    for (uint8_t counter = 0; pos < outlen; counter++) {
        msg[mlen - 1] = counter;
        if (mw_hmac_sha1(key, klen, msg, mlen, digest)) return -1;
        size_t take = outlen - pos < 20 ? outlen - pos : 20;
        memcpy(out + pos, digest, take);
        pos += take;
    }
    memset(digest, 0, sizeof digest);
    memset(msg, 0, sizeof msg);
    return 0;
}

static const uint8_t *min_ptr(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) <= 0 ? a : b; }
static const uint8_t *max_ptr(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) <= 0 ? b : a; }

int wpa_derive_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
                   const uint8_t anonce[WPA_NONCE_LEN], const uint8_t snonce[WPA_NONCE_LEN],
                   uint8_t ptk[WPA_PTK_LEN]) {
    uint8_t data[6 + 6 + 32 + 32];
    memcpy(data, min_ptr(aa, spa, 6), 6);
    memcpy(data + 6, max_ptr(aa, spa, 6), 6);
    memcpy(data + 12, min_ptr(anonce, snonce, 32), 32);
    memcpy(data + 44, max_ptr(anonce, snonce, 32), 32);
    int r = wpa_prf_sha1(pmk, WPA_PMK_LEN, "Pairwise key expansion", data, sizeof data, ptk, WPA_PTK_LEN);
    memset(data, 0, sizeof data);
    return r;
}

int wpa_eapol_mic(const uint8_t kck[16], int key_desc_version, const uint8_t *eapol, size_t len,
                  uint8_t mic[WPA_MIC_LEN]) {
    if (key_desc_version != 2) return -1;
    uint8_t d[20];
    if (mw_hmac_sha1(kck, 16, eapol, len, d)) return -1;
    memcpy(mic, d, WPA_MIC_LEN);
    return 0;
}

static const uint8_t kIV[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};

int wpa_aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t in_len, uint8_t *out) {
    if (in_len < 24 || in_len % 8) return -1;
    size_t n = in_len / 8 - 1;
    uint8_t a[8], b[16];
    memcpy(a, in, 8);
    memmove(out, in + 8, n * 8);
    for (int j = 5; j >= 0; j--) {
        for (size_t i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * (uint64_t)j + i;
            memcpy(b, a, 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(b + 8, out + (i - 1) * 8, 8);
            if (mw_aes128_block(0, kek, b, b)) return -1;
            memcpy(a, b, 8);
            memcpy(out + (i - 1) * 8, b + 8, 8);
        }
    }
    return memcmp(a, kIV, 8) == 0 ? 0 : -1;
}

int wpa_aes_wrap(const uint8_t kek[16], const uint8_t *in, size_t in_len, uint8_t *out) {
    if (in_len < 16 || in_len % 8) return -1;
    size_t n = in_len / 8;
    uint8_t a[8], b[16];
    memcpy(a, kIV, 8);
    memmove(out + 8, in, in_len);
    for (int j = 0; j <= 5; j++) {
        for (size_t i = 1; i <= n; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + i * 8, 8);
            if (mw_aes128_block(1, kek, b, b)) return -1;
            uint64_t t = (uint64_t)n * (uint64_t)j + i;
            memcpy(a, b, 8);
            for (int k = 0; k < 8; k++) a[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(out + i * 8, b + 8, 8);
        }
    }
    memcpy(out, a, 8);
    return 0;
}
