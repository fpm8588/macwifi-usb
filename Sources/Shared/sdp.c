// sdp.c — see sdp.h. Original code (GPL-2.0).
//
// Records are kept as ready-made attribute (ID, value) pairs in SDP data-element encoding (Part B 3): type in
// the top 5 bits of the first byte, size index in the low 3 (0x09 = uint16, 0x0A = uint32, 0x19 = UUID16,
// 0x35 = sequence with a one-byte length).

#include "sdp.h"

#include <string.h>

typedef struct { uint16_t id; const uint8_t *v; size_t n; } Attr;
typedef struct { uint32_t handle; const uint16_t *uuids; int nuuids; const Attr *attrs; int nattrs; } Record;

// --- A2DP Source (A2DP 1.3 section 5.3, Table 5.1)
static const uint8_t a2dp_handle[] = {0x0A, 0x00, 0x01, 0x00, 0x01};
static const uint8_t a2dp_class[] = {0x35, 0x03, 0x19, 0x11, 0x0A};                    // Audio Source
static const uint8_t a2dp_proto[] = {0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,   // L2CAP, PSM AVDTP
                                     0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03};              // AVDTP 1.3
static const uint8_t browse[] = {0x35, 0x03, 0x19, 0x10, 0x02};                        // public browse root
static const uint8_t a2dp_profile[] = {0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0D, 0x09, 0x01, 0x03};   // A2DP 1.3
static const uint8_t a2dp_features[] = {0x09, 0x00, 0x01};                              // player
static const uint8_t a2dp_name[] = {0x25, 0x0C, 'A', 'u', 'd', 'i', 'o', ' ', 'S', 'o', 'u', 'r', 'c', 'e'};
static const Attr a2dp_attrs[] = {
    {0x0000, a2dp_handle, sizeof a2dp_handle}, {0x0001, a2dp_class, sizeof a2dp_class},
    {0x0004, a2dp_proto, sizeof a2dp_proto},   {0x0005, browse, sizeof browse},
    {0x0009, a2dp_profile, sizeof a2dp_profile}, {0x0100, a2dp_name, sizeof a2dp_name},
    {0x0311, a2dp_features, sizeof a2dp_features},
};
static const uint16_t a2dp_uuids[] = {0x110A, 0x0100, 0x0019, 0x1002, 0x110D};

// --- AVRCP Controller + Target (AVRCP 1.6 section 8): headphones look for these before sending volume / play
static const uint8_t avrcp_handle[] = {0x0A, 0x00, 0x01, 0x00, 0x02};
static const uint8_t avrcp_class[] = {0x35, 0x06, 0x19, 0x11, 0x0E, 0x19, 0x11, 0x0F};   // A/V Remote Control (+ Controller)
static const uint8_t avrcp_proto[] = {0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x17,  // L2CAP, PSM AVCTP
                                      0x35, 0x06, 0x19, 0x00, 0x17, 0x09, 0x01, 0x04};             // AVCTP 1.4
static const uint8_t avrcp_profile[] = {0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0E, 0x09, 0x01, 0x06};  // AVRCP 1.6
static const uint8_t avrcp_features[] = {0x09, 0x00, 0x01};                              // category 1
static const Attr avrcp_attrs[] = {
    {0x0000, avrcp_handle, sizeof avrcp_handle}, {0x0001, avrcp_class, sizeof avrcp_class},
    {0x0004, avrcp_proto, sizeof avrcp_proto},   {0x0005, browse, sizeof browse},
    {0x0009, avrcp_profile, sizeof avrcp_profile}, {0x0311, avrcp_features, sizeof avrcp_features},
};
static const uint16_t avrcp_uuids[] = {0x110E, 0x110F, 0x0100, 0x0017, 0x1002};

static const Record kRecords[] = {
    {0x00010001, a2dp_uuids, 5, a2dp_attrs, 7},
    {0x00010002, avrcp_uuids, 5, avrcp_attrs, 6},
};
#define NREC ((int)(sizeof kRecords / sizeof kRecords[0]))

int sdp_record_count(void) { return NREC; }

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

// Reads a data-element header: returns the header length and sets *len (payload) and *type; 0 on error.
static size_t de_header(const uint8_t *p, size_t n, uint8_t *type, size_t *len) {
    if (n < 1) return 0;
    *type = p[0] >> 3;
    uint8_t sz = p[0] & 7;
    static const size_t fixed[5] = {1, 2, 4, 8, 16};
    if (sz < 5) { *len = (*type == 0) ? 0 : fixed[sz]; return 1; }
    if (sz == 5) { if (n < 2) return 0; *len = p[1]; return 2; }
    if (sz == 6) { if (n < 3) return 0; *len = be16(p + 1); return 3; }
    if (n < 5) return 0;
    *len = be32(p + 1);
    return 5;
}

static const uint8_t kBase[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};

// Parses a sequence of UUIDs (16, 32 or Bluetooth-base 128-bit) into 32-bit values. Count, or -1.
static int parse_uuids(const uint8_t *p, size_t n, uint32_t *out, int max, size_t *used) {
    uint8_t type;
    size_t len, h = de_header(p, n, &type, &len);
    if (!h || type != 6 || h + len > n) return -1;
    *used = h + len;
    int k = 0;
    size_t o = h;
    while (o < h + len) {
        uint8_t t;
        size_t l, hh = de_header(p + o, h + len - o, &t, &l);
        if (!hh || t != 3 || o + hh + l > h + len || k >= max) return -1;
        const uint8_t *v = p + o + hh;
        if (l == 2) out[k++] = be16(v);
        else if (l == 4) out[k++] = be32(v);
        else if (l == 16) out[k++] = memcmp(v + 4, kBase, 12) == 0 ? be32(v) : 0xFFFFFFFFu;   // non-base: never matches
        else return -1;
        o += hh + l;
    }
    return k;
}

// Attribute ID list: uint16 IDs or uint32 ranges (high 16 bits start, low 16 end). Fills [lo, hi] pairs.
static int parse_attr_ids(const uint8_t *p, size_t n, uint16_t lo[], uint16_t hi[], int max, size_t *used) {
    uint8_t type;
    size_t len, h = de_header(p, n, &type, &len);
    if (!h || type != 6 || h + len > n) return -1;
    *used = h + len;
    int k = 0;
    size_t o = h;
    while (o < h + len) {
        uint8_t t;
        size_t l, hh = de_header(p + o, h + len - o, &t, &l);
        if (!hh || t != 1 || o + hh + l > h + len || k >= max) return -1;
        if (l == 2) { lo[k] = hi[k] = be16(p + o + hh); k++; }
        else if (l == 4) { uint32_t r = be32(p + o + hh); lo[k] = (uint16_t)(r >> 16); hi[k] = (uint16_t)r; k++; }
        else return -1;
        o += hh + l;
    }
    return k;
}

static int matches(const Record *r, const uint32_t *u, int nu) {
    for (int i = 0; i < nu; i++) {
        int found = 0;
        for (int k = 0; k < r->nuuids; k++) if (r->uuids[k] == u[i]) found = 1;
        if (!found) return 0;
    }
    return nu > 0;
}

// Writes one record's requested attributes as a sequence (0x36, 16-bit length). Returns bytes written or 0.
static size_t write_attrs(const Record *r, const uint16_t *lo, const uint16_t *hi, int nr, uint8_t *out, size_t cap) {
    if (cap < 3) return 0;
    size_t o = 3;
    for (int a = 0; a < r->nattrs; a++) {
        int want = 0;
        for (int i = 0; i < nr; i++) if (r->attrs[a].id >= lo[i] && r->attrs[a].id <= hi[i]) want = 1;
        if (!want) continue;
        if (o + 3 + r->attrs[a].n > cap) return 0;
        out[o] = 0x09;
        put16(out + o + 1, r->attrs[a].id);
        memcpy(out + o + 3, r->attrs[a].v, r->attrs[a].n);
        o += 3 + r->attrs[a].n;
    }
    out[0] = 0x36;
    put16(out + 1, (uint16_t)(o - 3));
    return o;
}

static size_t error_rsp(uint16_t tid, uint16_t code, uint8_t *rsp, size_t cap) {
    if (cap < 7) return 0;
    rsp[0] = SDP_ERROR_RSP;
    put16(rsp + 1, tid);
    put16(rsp + 3, 2);
    put16(rsp + 5, code);
    return 7;
}

size_t sdp_server_handle(const uint8_t *req, size_t n, uint8_t *rsp, size_t cap) {
    if (n < 5 || cap < 16) return 0;
    uint8_t pdu = req[0];
    uint16_t tid = be16(req + 1), plen = be16(req + 3);
    if ((size_t)plen + 5 > n) return error_rsp(tid, 0x0004, rsp, cap);        // invalid PDU size
    const uint8_t *p = req + 5;
    size_t used = 0;
    uint32_t uu[12];
    uint16_t lo[16], hi[16];
    if (pdu == SDP_SERVICE_SEARCH_REQ) {
        int nu = parse_uuids(p, plen, uu, 12, &used);
        if (nu < 0 || used + 2 > plen) return error_rsp(tid, 0x0003, rsp, cap);   // invalid request syntax
        uint16_t maxc = be16(p + used);
        int cnt = 0;
        uint8_t *h = rsp + 9;
        for (int i = 0; i < NREC && cnt < maxc && 9 + 4 * (size_t)(cnt + 1) + 1 <= cap; i++)
            if (matches(&kRecords[i], uu, nu)) { put32(h + 4 * cnt, kRecords[i].handle); cnt++; }
        rsp[0] = SDP_SERVICE_SEARCH_RSP;
        put16(rsp + 1, tid);
        put16(rsp + 5, (uint16_t)cnt);
        put16(rsp + 7, (uint16_t)cnt);
        rsp[9 + 4 * cnt] = 0;                                                   // no continuation
        put16(rsp + 3, (uint16_t)(4 + 4 * cnt + 1));
        return 9 + 4 * (size_t)cnt + 1;
    }
    if (pdu == SDP_SERVICE_ATTR_REQ) {
        if (plen < 6) return error_rsp(tid, 0x0003, rsp, cap);
        uint32_t handle = be32(p);
        int nr = parse_attr_ids(p + 6, plen - 6, lo, hi, 16, &used);
        if (nr < 0) return error_rsp(tid, 0x0003, rsp, cap);
        const Record *r = NULL;
        for (int i = 0; i < NREC; i++) if (kRecords[i].handle == handle) r = &kRecords[i];
        if (!r) return error_rsp(tid, 0x0002, rsp, cap);                       // invalid record handle
        size_t w = write_attrs(r, lo, hi, nr, rsp + 7, cap - 8);
        if (!w) return error_rsp(tid, 0x0006, rsp, cap);                       // insufficient resources
        rsp[0] = SDP_SERVICE_ATTR_RSP;
        put16(rsp + 1, tid);
        put16(rsp + 5, (uint16_t)w);
        rsp[7 + w] = 0;
        put16(rsp + 3, (uint16_t)(2 + w + 1));
        return 7 + w + 1;
    }
    if (pdu == SDP_SERVICE_SEARCH_ATTR_REQ) {
        int nu = parse_uuids(p, plen, uu, 12, &used);
        if (nu < 0 || used + 2 > plen) return error_rsp(tid, 0x0003, rsp, cap);
        size_t u2 = 0;
        int nr = parse_attr_ids(p + used + 2, plen - used - 2, lo, hi, 16, &u2);
        if (nr < 0) return error_rsp(tid, 0x0003, rsp, cap);
        // AttributeLists: a sequence of per-record sequences
        uint8_t *lists = rsp + 7;
        size_t o = 3;
        for (int i = 0; i < NREC; i++) {
            if (!matches(&kRecords[i], uu, nu)) continue;
            size_t w = write_attrs(&kRecords[i], lo, hi, nr, lists + o, cap - 8 - 7 - o);
            if (!w) return error_rsp(tid, 0x0006, rsp, cap);
            o += w;
        }
        lists[0] = 0x36;
        put16(lists + 1, (uint16_t)(o - 3));
        rsp[0] = SDP_SERVICE_SEARCH_ATTR_RSP;
        put16(rsp + 1, tid);
        put16(rsp + 5, (uint16_t)o);
        rsp[7 + o] = 0;
        put16(rsp + 3, (uint16_t)(2 + o + 1));
        return 7 + o + 1;
    }
    return error_rsp(tid, 0x0003, rsp, cap);
}
