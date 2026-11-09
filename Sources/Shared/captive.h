// captive.h — captive portal detection ("sign in to this network" pages). Original code (MIT).
//
// Like macOS's own check: fetch http://captive.apple.com/hotspot-detect.html over the adapter's interface. The
// real page is a tiny HTML document whose body is "Success"; anything else (a redirect, a login page, a rewritten
// page) means a portal sits in between. macOS only runs its check for its own Wi-Fi interfaces, so MacWifi does
// it for the adapter.

#ifndef CAPTIVE_H
#define CAPTIVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPTIVE_HOST "captive.apple.com"
#define CAPTIVE_PATH "/hotspot-detect.html"
#define CAPTIVE_URL  "http://" CAPTIVE_HOST CAPTIVE_PATH

typedef enum { CAPTIVE_UNKNOWN = 0, CAPTIVE_NONE, CAPTIVE_PORTAL } CaptiveState;

// The GET request; returns its length (0 if `cap` is too small).
size_t captive_request(char *out, size_t cap);

// Classifies a complete or partial HTTP response. For a portal, `portal_url` gets the redirect target if there
// is one (http/https only), else CAPTIVE_URL. Returns CAPTIVE_UNKNOWN if the response isn't HTTP at all.
CaptiveState captive_classify(const char *resp, size_t len, char *portal_url, size_t url_cap);

#ifdef __cplusplus
}
#endif
#endif
