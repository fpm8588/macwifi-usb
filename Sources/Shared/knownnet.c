// knownnet.c — see knownnet.h. Original code (MIT).

#include "knownnet.h"

#include <string.h>

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes exactly `n` hex digit pairs from s[0..slen); returns bytes written or -1.
static int unhex(const char *s, size_t slen, uint8_t *out, size_t cap) {
    if (slen % 2 || slen / 2 > cap) return -1;
    for (size_t i = 0; i < slen / 2; i++) {
        int h = hexv(s[2 * i]), l = hexv(s[2 * i + 1]);
        if (h < 0 || l < 0) return -1;
        out[i] = (uint8_t)(h << 4 | l);
    }
    return (int)(slen / 2);
}

int kn_parse(KnownList *k, const char *t, size_t len) {
    memset(k, 0, sizeof *k);
    size_t i = 0;
    while (i < len && k->count < KN_MAX) {
        size_t e = i;
        while (e < len && t[e] != '\n') e++;
        // fields
        const char *f[4];
        size_t fl[4];
        int nf = 0;
        size_t p = i;
        while (p < e && nf < 4) {
            while (p < e && t[p] == ' ') p++;
            if (p >= e) break;
            size_t q = p;
            while (q < e && t[q] != ' ') q++;
            f[nf] = t + p;
            fl[nf] = q - p;
            nf++;
            p = q;
        }
        KnownNet n;
        memset(&n, 0, sizeof n);
        int ok = nf == 3 || nf == 4;
        if (ok) {
            int sl = unhex(f[0], fl[0], n.ssid, sizeof n.ssid);
            ok = sl >= 1;
            n.ssid_len = (uint8_t)(sl > 0 ? sl : 0);
        }
        if (ok) {
            if (fl[1] == 1 && f[1][0] == '-') n.has_pmk = 0;
            else ok = (n.has_pmk = unhex(f[1], fl[1], n.pmk, sizeof n.pmk) == 32);
        }
        if (ok) ok = fl[2] == 1 && (f[2][0] == '0' || f[2][0] == '1');
        if (ok && nf == 4) {
            int pl = unhex(f[3], fl[3], (uint8_t *)n.pass, sizeof n.pass - 1);
            ok = pl >= 8 && pl <= 63 && !memchr(n.pass, 0, (size_t)pl);
            n.pass_len = (uint8_t)(ok ? pl : 0);
        }
        if (ok) {
            n.autojoin = f[2][0] == '1';
            if (!kn_find(k, n.ssid, n.ssid_len)) k->n[k->count++] = n;
        }
        memset(&n, 0, sizeof n);
        i = e + 1;
    }
    return k->count;
}

size_t kn_format(const KnownList *k, char *out, size_t cap) {
    static const char hx[] = "0123456789abcdef";
    size_t o = 0;
    for (int i = 0; i < k->count; i++) {
        const KnownNet *n = &k->n[i];
        size_t need = (size_t)n->ssid_len * 2 + 1 + (n->has_pmk ? 64 : 1) + 3 + (n->pass_len ? 1 + 2 * (size_t)n->pass_len : 0);
        if (o + need + 1 > cap) return 0;
        for (int b = 0; b < n->ssid_len; b++) { out[o++] = hx[n->ssid[b] >> 4]; out[o++] = hx[n->ssid[b] & 15]; }
        out[o++] = ' ';
        if (n->has_pmk) for (int b = 0; b < 32; b++) { out[o++] = hx[n->pmk[b] >> 4]; out[o++] = hx[n->pmk[b] & 15]; }
        else out[o++] = '-';
        out[o++] = ' ';
        out[o++] = n->autojoin ? '1' : '0';
        if (n->pass_len) {
            out[o++] = ' ';
            for (int b = 0; b < n->pass_len; b++) { out[o++] = hx[(uint8_t)n->pass[b] >> 4]; out[o++] = hx[n->pass[b] & 15]; }
        }
        out[o++] = '\n';
    }
    if (o < cap) out[o] = 0;
    return o;
}

KnownNet *kn_find(KnownList *k, const uint8_t *ssid, size_t ssid_len) {
    for (int i = 0; i < k->count; i++)
        if (k->n[i].ssid_len == ssid_len && !memcmp(k->n[i].ssid, ssid, ssid_len)) return &k->n[i];
    return NULL;
}

int kn_put(KnownList *k, const uint8_t *ssid, size_t ssid_len, const uint8_t *pmk) {
    if (!ssid_len || ssid_len > 32) return -1;
    kn_remove(k, ssid, ssid_len);
    if (k->count == KN_MAX) k->count--;                     // drop the oldest
    memmove(&k->n[1], &k->n[0], sizeof k->n[0] * (size_t)k->count);
    KnownNet *n = &k->n[0];
    memset(n, 0, sizeof *n);
    memcpy(n->ssid, ssid, ssid_len);
    n->ssid_len = (uint8_t)ssid_len;
    if (pmk) { n->has_pmk = 1; memcpy(n->pmk, pmk, 32); }
    n->autojoin = 1;
    k->count++;
    return 0;
}

int kn_set_pass(KnownNet *n, const char *pass, size_t len) {
    memset(n->pass, 0, sizeof n->pass);
    n->pass_len = 0;
    if (!len) return 0;
    if (len < 8 || len > 63 || memchr(pass, 0, len)) return -1;
    memcpy(n->pass, pass, len);
    n->pass_len = (uint8_t)len;
    return 0;
}

int kn_remove(KnownList *k, const uint8_t *ssid, size_t ssid_len) {
    KnownNet *n = kn_find(k, ssid, ssid_len);
    if (!n) return 0;
    int i = (int)(n - k->n);
    memset(n, 0, sizeof *n);
    memmove(&k->n[i], &k->n[i + 1], sizeof k->n[0] * (size_t)(k->count - i - 1));
    k->count--;
    memset(&k->n[k->count], 0, sizeof k->n[0]);
    return 1;
}

void kn_wipe(KnownList *k) {
    volatile uint8_t *p = (volatile uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) p[i] = 0;
}

int kn_choose(const KnownList *k, const KnSeen *seen, int n) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        const KnSeen *s = &seen[i];
        if (!s->usable || !s->ssid_len) continue;
        const KnownNet *kn = kn_find((KnownList *)k, s->ssid, s->ssid_len);
        if (!kn || !kn->autojoin || kn->has_pmk != (s->secured != 0)) continue;
        if (s->sae_only && !kn->pass_len) continue;         // WPA3-only: a PMK alone can't join
        if (best < 0 || s->rssi > seen[best].rssi) best = i;
    }
    return best;
}
