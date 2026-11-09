// test_captive.c — captive portal response classification. Original code (MIT).

#include <stdio.h>
#include <string.h>

#include "captive.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static CaptiveState cls(const char *r, char *u) { return captive_classify(r, strlen(r), u, 256); }

int main(void) {
    char req[256], u[256];
    size_t n = captive_request(req, sizeof req);
    CHECK(n > 0 && !strncmp(req, "GET /hotspot-detect.html HTTP/1.0\r\nHost: captive.apple.com\r\n", 59) &&
          !strcmp(req + n - 4, "\r\n\r\n"));
    CHECK(captive_request(req, 20) == 0);

    CHECK(cls("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
              "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>", u) == CAPTIVE_NONE);
    CHECK(cls("HTTP/1.0 302 Found\r\nLocation: https://portal.example.net/login?x=1\r\n\r\n", u) == CAPTIVE_PORTAL &&
          !strcmp(u, "https://portal.example.net/login?x=1"));
    CHECK(cls("HTTP/1.1 307 Temporary Redirect\r\nlocation:http://10.0.0.1/\r\n\r\n", u) == CAPTIVE_PORTAL &&
          !strcmp(u, "http://10.0.0.1/"));
    CHECK(cls("HTTP/1.1 302 Found\r\nLocation: javascript:alert(1)\r\n\r\n", u) == CAPTIVE_PORTAL &&
          !strcmp(u, CAPTIVE_URL));                                        // only http(s) redirects are followed
    CHECK(cls("HTTP/1.1 200 OK\r\n\r\n<html><body>Please accept the terms</body></html>", u) == CAPTIVE_PORTAL &&
          !strcmp(u, CAPTIVE_URL));                                        // page rewritten in place
    CHECK(cls("HTTP/1.1 511 Network Authentication Required\r\n\r\n", u) == CAPTIVE_PORTAL);
    CHECK(cls("HTTP/1.1 200 OK\r\n", u) == CAPTIVE_PORTAL);                 // cut short: not Apple's page
    CHECK(cls("SSH-2.0-junk", u) == CAPTIVE_UNKNOWN && cls("", u) == CAPTIVE_UNKNOWN);
    char tiny[8];
    CHECK(captive_classify("HTTP/1.1 302 Found\r\nLocation: http://long.example/xyz\r\n\r\n", 55, tiny, sizeof tiny) ==
              CAPTIVE_PORTAL && strlen(tiny) == 7);                        // truncated, still terminated
    printf("captive: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
