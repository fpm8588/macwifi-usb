// sdp.h — a minimal SDP server (Bluetooth Core v5.4 Vol 3 Part B): answers service searches for what this host
// offers, so headphones can see an A2DP audio source (and an AVRCP controller/target) here. Original code (GPL-2.0).

#ifndef SDP_H
#define SDP_H

#include <stddef.h>
#include <stdint.h>

// PDU IDs (Part B 4.2)
enum {
    SDP_ERROR_RSP = 0x01, SDP_SERVICE_SEARCH_REQ = 0x02, SDP_SERVICE_SEARCH_RSP = 0x03,
    SDP_SERVICE_ATTR_REQ = 0x04, SDP_SERVICE_ATTR_RSP = 0x05, SDP_SERVICE_SEARCH_ATTR_REQ = 0x06,
    SDP_SERVICE_SEARCH_ATTR_RSP = 0x07
};

// Answers one request PDU. Returns the response length (0 if the request can't be answered at all).
size_t sdp_server_handle(const uint8_t *req, size_t n, uint8_t *rsp, size_t cap);

// For tests: number of our records and the 16-bit UUIDs the i-th one carries.
int sdp_record_count(void);

#endif
