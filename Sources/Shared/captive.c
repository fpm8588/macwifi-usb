// captive.c — see captive.h. Original code (MIT).

#include "captive.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

size_t captive_request(char *out, size_t cap) {
    int n = snprintf(out, cap,
                     "GET " CAPTIVE_PATH " HTTP/1.0\r\nHost: " CAPTIVE_HOST "\r\n"
                     "User-Agent: CaptiveNetworkSupport (MacWifi)\r\nConnection: close\r\n\r\n");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static const char *find_ci(const char *h, size_t hl, const char *needle) {
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k = 0;
        while (k < nl && tolower((unsigned char)h[i + k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nl) return h + i;
    }
    return NULL;
}

CaptiveState captive_classify(const char *r, size_t len, char *url, size_t cap) {
    if (cap) snprintf(url, cap, "%s", CAPTIVE_URL);
    int status = 0;
    if (len < 12 || memcmp(r, "HTTP/1.", 7) != 0 || sscanf(r + 9, "%3d", &status) != 1) return CAPTIVE_UNKNOWN;
    const char *hdr_end = find_ci(r, len, "\r\n\r\n");
    size_t hlen = hdr_end ? (size_t)(hdr_end - r) : len;
    if (status >= 300 && status < 400) {
        const char *loc = find_ci(r, hlen, "\r\nlocation:");
        if (loc && cap) {
            loc += 11;
            while (loc < r + hlen && (*loc == ' ' || *loc == '\t')) loc++;
            size_t n = 0;
            while (loc + n < r + hlen && loc[n] != '\r' && loc[n] != '\n') n++;
            if ((n > 7 && !strncmp(loc, "http://", 7)) || (n > 8 && !strncmp(loc, "https://", 8))) {
                if (n >= cap) n = cap - 1;
                memcpy(url, loc, n);
                url[n] = 0;
            }
        }
        return CAPTIVE_PORTAL;
    }
    if (status == 200 && hdr_end) {
        const char *body = hdr_end + 4;
        size_t bl = len - (size_t)(body - r);
        if (find_ci(body, bl, "<BODY>Success</BODY>")) return CAPTIVE_NONE;
    }
    return CAPTIVE_PORTAL;   // anything else in place of Apple's page: something intercepted it
}
