// crypto_backend.c — the three primitives wpa.c needs, from the OS's own crypto library.
// macOS: CommonCrypto (part of the system, free, no dependency). Linux (tests): OpenSSL libcrypto.
#include "wpa.h"

#if defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>

int mw_hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[20]) {
    CCHmac(kCCHmacAlgSHA1, key, klen, msg, mlen, out);
    return 0;
}

int mw_pbkdf2_sha1(const char *pass, size_t plen, const uint8_t *salt, size_t slen, unsigned rounds,
                   uint8_t *out, size_t outlen) {
    return CCKeyDerivationPBKDF(kCCPBKDF2, pass, plen, salt, slen, kCCPRFHmacAlgSHA1, rounds, out, outlen) ==
                   kCCSuccess ? 0 : -1;
}

int mw_aes128_block(int encrypt, const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    size_t moved = 0;
    CCCryptorStatus s = CCCrypt(encrypt ? kCCEncrypt : kCCDecrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 16,
                                NULL, in, 16, out, 16, &moved);
    return (s == kCCSuccess && moved == 16) ? 0 : -1;
}

int mw_hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]) {
    CCHmac(kCCHmacAlgSHA256, key, klen, msg, mlen, out);
    return 0;
}

#else  // OpenSSL (Linux CI and local tests)
#include <openssl/evp.h>
#include <openssl/hmac.h>

int mw_hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[20]) {
    unsigned int olen = 0;
    return HMAC(EVP_sha1(), key, (int)klen, msg, mlen, out, &olen) && olen == 20 ? 0 : -1;
}

int mw_pbkdf2_sha1(const char *pass, size_t plen, const uint8_t *salt, size_t slen, unsigned rounds,
                   uint8_t *out, size_t outlen) {
    return PKCS5_PBKDF2_HMAC_SHA1(pass, (int)plen, salt, (int)slen, (int)rounds, (int)outlen, out) == 1 ? 0 : -1;
}

int mw_aes128_block(int encrypt, const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (!c) return -1;
    int ok = EVP_CipherInit_ex(c, EVP_aes_128_ecb(), NULL, key, NULL, encrypt ? 1 : 0) == 1;
    int l1 = 0, l2 = 0;
    uint8_t tmp[32];
    if (ok) ok = EVP_CIPHER_CTX_set_padding(c, 0) == 1;
    if (ok) ok = EVP_CipherUpdate(c, tmp, &l1, in, 16) == 1;
    if (ok) ok = EVP_CipherFinal_ex(c, tmp + l1, &l2) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok || l1 + l2 != 16) return -1;
    for (int i = 0; i < 16; i++) out[i] = tmp[i];
    return 0;
}

int mw_hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]) {
    unsigned int olen = 0;
    return HMAC(EVP_sha256(), key, (int)klen, msg, mlen, out, &olen) && olen == 32 ? 0 : -1;
}
#endif
