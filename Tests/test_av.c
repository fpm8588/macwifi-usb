// test_av.c — AVDTP signaling (as initiator against a scripted sink, and as acceptor), SBC configuration choice,
// and the SDP server, plus fuzzing of both parsers. Original code (GPL-2.0).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avdtp.h"
#include "sdp.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t sent[32][64];
static size_t sentn[32];
static int nsent, media_requests, streaming_on, streaming_off, failed;
static SbcConfig stream_cfg;
static int f_send(void *c, const uint8_t *d, size_t n) {
    (void)c;
    if (nsent < 32 && n <= 64) { memcpy(sent[nsent], d, n); sentn[nsent] = n; nsent++; }
    return 0;
}
static void f_open(void *c) { (void)c; media_requests++; }
static void f_stream(void *c, int on, const SbcConfig *cfg) {
    (void)c;
    if (on) { streaming_on++; stream_cfg = *cfg; } else streaming_off++;
}
static void f_fail(void *c, const char *why) { (void)c; (void)why; failed++; }
static const AvdtpOps kOps = {NULL, f_send, f_open, f_stream, f_fail};

static const uint8_t *last(uint8_t *sig, uint8_t *label, uint8_t *type, size_t *n) {
    if (!nsent) return NULL;
    const uint8_t *d = sent[nsent - 1];
    *label = d[0] >> 4;
    *type = d[0] & 3;
    *sig = d[1] & 0x3F;
    *n = sentn[nsent - 1] - 2;
    return d + 2;
}

static void reply(Avdtp *a, uint8_t label, uint8_t type, uint8_t sig, const uint8_t *p, size_t n) {
    uint8_t b[64];
    b[0] = (uint8_t)((label << 4) | type);
    b[1] = sig;
    if (n) memcpy(b + 2, p, n);
    avdtp_rx(a, b, 2 + n);
}

static void test_initiator(void) {
    Avdtp a;
    avdtp_init(&a, &kOps);
    nsent = media_requests = streaming_on = streaming_off = failed = 0;
    avdtp_start(&a);
    uint8_t sig, label, type;
    size_t n;
    const uint8_t *p = last(&sig, &label, &type, &n);
    CHECK(p && sig == AVDTP_DISCOVER && type == AVDTP_MSG_COMMAND && n == 0);
    // Sink: SEID 1 is a source in use (skipped), SEID 2 audio sink, SEID 3 video sink (skipped), SEID 4 audio sink
    uint8_t disc[8] = {(1 << 2) | 2, 0x00, 2 << 2, 0x08, 3 << 2, 0x18, 4 << 2, 0x08};
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_DISCOVER, disc, 8);
    p = last(&sig, &label, &type, &n);
    CHECK(sig == AVDTP_GET_CAPABILITIES && n == 1 && p[0] == (2 << 2) && a.nseps == 2);
    // SEID 2: a vendor codec only -> next SEP
    uint8_t vendor[] = {0x01, 0x00, 0x07, 0x08, 0x00, 0xFF, 0x4C, 0x00, 0x00, 0x00, 0x01, 0x00};
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_GET_CAPABILITIES, vendor, sizeof vendor);
    p = last(&sig, &label, &type, &n);
    CHECK(sig == AVDTP_GET_CAPABILITIES && p[0] == (4 << 2));
    // SEID 4: SBC 44.1/48, joint+stereo, 16 blocks, 8 subbands, loudness, bitpool 2..53, plus delay reporting
    uint8_t caps[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x33, 0x15, 0x02, 0x35, 0x08, 0x00};
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_GET_CAPABILITIES, caps, sizeof caps);
    p = last(&sig, &label, &type, &n);
    CHECK(sig == AVDTP_SET_CONFIGURATION && n == 12 && p[0] == (4 << 2) && p[1] == (AVDTP_LOCAL_SEID << 2));
    CHECK(p[2] == 0x01 && p[3] == 0 && p[4] == 0x07 && p[5] == 6 && p[7] == 0x00);
    CHECK(p[8] == (0x20 | 0x02) && p[9] == (0x10 | 0x04 | 0x01) && p[10] == 2 && p[11] == 53);   // 44.1k stereo 16/8/loudness
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_SET_CONFIGURATION, NULL, 0);
    p = last(&sig, &label, &type, &n);
    CHECK(sig == AVDTP_OPEN && p[0] == (4 << 2));
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_OPEN, NULL, 0);
    CHECK(media_requests == 1 && a.state == AV_OPEN);
    int before = nsent;
    avdtp_media_open(&a, 1);
    p = last(&sig, &label, &type, &n);
    CHECK(nsent == before + 1 && sig == AVDTP_START && p[0] == (4 << 2));
    // A response with the wrong label is ignored
    reply(&a, (uint8_t)((label + 1) & 15), AVDTP_MSG_ACCEPT, AVDTP_START, NULL, 0);
    CHECK(streaming_on == 0);
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_START, NULL, 0);
    CHECK(streaming_on == 1 && a.state == AV_STREAMING && stream_cfg.fs == SBC_FS_44 && stream_cfg.mode == SBC_MODE_STEREO &&
          stream_cfg.blocks == 16 && stream_cfg.bitpool == 53);
    // The sink suspends, then starts again (it drives this after a pause)
    uint8_t s4 = 1 << 2;
    reply(&a, 7, AVDTP_MSG_COMMAND, AVDTP_SUSPEND, &s4, 1);
    p = last(&sig, &label, &type, &n);
    CHECK(sig == AVDTP_SUSPEND && type == AVDTP_MSG_ACCEPT && label == 7 && streaming_off == 1 && a.state == AV_OPEN);
    reply(&a, 8, AVDTP_MSG_COMMAND, AVDTP_START, &s4, 1);
    p = last(&sig, &label, &type, &n);
    CHECK(sig == AVDTP_START && type == AVDTP_MSG_ACCEPT && streaming_on == 2);
    // We suspend, then resume
    CHECK(avdtp_suspend(&a) == 0 && streaming_off == 2);
    p = last(&sig, &label, &type, &n);
    CHECK(sig == AVDTP_SUSPEND && type == AVDTP_MSG_COMMAND);
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_SUSPEND, NULL, 0);
    CHECK(avdtp_resume(&a) == 0);
    p = last(&sig, &label, &type, &n);
    CHECK(sig == AVDTP_START);
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_START, NULL, 0);
    CHECK(streaming_on == 3);
    // The media channel drops: streaming stops
    avdtp_media_open(&a, 0);
    CHECK(streaming_off == 3 && a.state == AV_CONFIGURED);
    // Unknown signal -> general reject; delay report -> accepted
    reply(&a, 9, AVDTP_MSG_COMMAND, 0x2A, NULL, 0);
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_GENERAL_REJECT && label == 9);
    uint8_t dr[3] = {1 << 2, 0x01, 0x40};
    reply(&a, 10, AVDTP_MSG_COMMAND, AVDTP_DELAYREPORT, dr, 3);
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_ACCEPT && sig == AVDTP_DELAYREPORT);
}

static void test_initiator_rejects(void) {
    Avdtp a;
    avdtp_init(&a, &kOps);
    nsent = failed = 0;
    avdtp_start(&a);
    uint8_t sig, label, type;
    size_t n;
    last(&sig, &label, &type, &n);
    uint8_t none[2] = {1 << 2, 0x00};                    // only a source
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_DISCOVER, none, 2);
    CHECK(failed == 1 && a.state == AV_FAILED);
    avdtp_init(&a, &kOps);
    nsent = failed = 0;
    avdtp_start(&a);
    last(&sig, &label, &type, &n);
    uint8_t disc[2] = {2 << 2, 0x08};
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_DISCOVER, disc, 2);
    last(&sig, &label, &type, &n);
    uint8_t caps4[] = {0x07, 0x06, 0x00, 0x00, 0x22, 0x18, 0x02, 0x35};   // 4 subbands only
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_GET_CAPABILITIES, caps4, sizeof caps4);
    CHECK(failed == 1);
    avdtp_init(&a, &kOps);
    nsent = failed = 0;
    avdtp_start(&a);
    last(&sig, &label, &type, &n);
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_DISCOVER, disc, 2);
    last(&sig, &label, &type, &n);
    uint8_t caps[] = {0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x23};    // joint only, max bitpool 35
    reply(&a, label, AVDTP_MSG_ACCEPT, AVDTP_GET_CAPABILITIES, caps, sizeof caps);
    const uint8_t *p = last(&sig, &label, &type, &n);
    CHECK(sig == AVDTP_SET_CONFIGURATION && p[8] == 0x21 && p[11] == 35);
    uint8_t err[2] = {0x07, 0x29};
    reply(&a, label, AVDTP_MSG_REJECT, AVDTP_SET_CONFIGURATION, err, 2);
    CHECK(failed == 1 && a.state == AV_FAILED);
}

static void test_acceptor(void) {
    Avdtp a;
    avdtp_init(&a, &kOps);
    nsent = streaming_on = streaming_off = 0;
    uint8_t sig, label, type;
    size_t n;
    reply(&a, 1, AVDTP_MSG_COMMAND, AVDTP_DISCOVER, NULL, 0);
    const uint8_t *p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_ACCEPT && n == 2 && (p[0] >> 2) == AVDTP_LOCAL_SEID && p[1] == 0x00);
    uint8_t s = AVDTP_LOCAL_SEID << 2;
    reply(&a, 2, AVDTP_MSG_COMMAND, AVDTP_GET_ALL_CAPABILITIES, &s, 1);
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_ACCEPT && n == 10 && p[2] == 0x07 && p[5] == AVDTP_CODEC_SBC && p[9] == 53);
    uint8_t bad = 5 << 2;
    reply(&a, 3, AVDTP_MSG_COMMAND, AVDTP_GET_CAPABILITIES, &bad, 1);
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_REJECT);
    // Sink configures us: 48 kHz joint stereo, 16 blocks, loudness, bitpool 2..51; its SEID is 6
    uint8_t conf[] = {AVDTP_LOCAL_SEID << 2, 6 << 2, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x11, 0x15, 0x02, 51};
    reply(&a, 4, AVDTP_MSG_COMMAND, AVDTP_SET_CONFIGURATION, conf, sizeof conf);
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_ACCEPT && a.cfg.fs == SBC_FS_48 && a.cfg.mode == SBC_MODE_JOINT && a.cfg.bitpool == 51 &&
          a.acp_seid == 6);
    reply(&a, 5, AVDTP_MSG_COMMAND, AVDTP_SET_CONFIGURATION, conf, sizeof conf);   // again: in use
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_REJECT && n == 2 && p[1] == 0x13);
    uint8_t badconf[] = {AVDTP_LOCAL_SEID << 2, 6 << 2, 0x07, 0x06, 0x00, 0x00, 0x13, 0x15, 0x02, 51};   // two modes
    Avdtp b;
    avdtp_init(&b, &kOps);
    reply(&b, 6, AVDTP_MSG_COMMAND, AVDTP_SET_CONFIGURATION, badconf, sizeof badconf);
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_REJECT && p[1] == 0x29);
    reply(&a, 7, AVDTP_MSG_COMMAND, AVDTP_GET_CONFIGURATION, &s, 1);
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_ACCEPT && n == 10 && p[6] == 0x11 && p[9] == 51);
    reply(&a, 8, AVDTP_MSG_COMMAND, AVDTP_OPEN, &s, 1);
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_ACCEPT && a.state == AV_OPEN);
    reply(&a, 9, AVDTP_MSG_COMMAND, AVDTP_START, &s, 1);         // before the media channel exists
    CHECK(a.state == AV_STREAMING && streaming_on == 0);
    avdtp_media_open(&a, 1);
    CHECK(streaming_on == 1 && stream_cfg.fs == SBC_FS_48);
    reply(&a, 10, AVDTP_MSG_COMMAND, AVDTP_CLOSE, &s, 1);
    CHECK(streaming_off == 1 && a.state == AV_IDLE && !a.configured);
    reply(&a, 11, AVDTP_MSG_COMMAND, AVDTP_START, &s, 1);         // not open any more
    p = last(&sig, &label, &type, &n);
    CHECK(type == AVDTP_MSG_REJECT);
}

static void test_pick(void) {
    SbcConfig c;
    uint8_t lo;
    uint8_t all[4] = {0xFF, 0xFF, 2, 250};
    CHECK(avdtp_pick_sbc(all, &c, &lo) == 0 && c.fs == SBC_FS_44 && c.mode == SBC_MODE_STEREO && c.blocks == 16 &&
          c.alloc == SBC_ALLOC_LOUDNESS && c.bitpool == 53 && lo == 2);
    uint8_t mono[4] = {0x18, 0x45, 10, 40};
    CHECK(avdtp_pick_sbc(mono, &c, &lo) == 0 && c.fs == SBC_FS_48 && c.mode == SBC_MODE_MONO && c.blocks == 8 && c.bitpool == 31 &&
          lo == 10);
    uint8_t lowmax[4] = {0x21, 0x15, 2, 1};
    CHECK(avdtp_pick_sbc(lowmax, &c, &lo) == -1);
    uint8_t info[4];
    SbcConfig x = {SBC_FS_48, SBC_MODE_JOINT, 12, SBC_ALLOC_SNR, 45}, y;
    avdtp_sbc_info(&x, 2, info);
    CHECK(info[0] == 0x11 && info[1] == (0x20 | 0x04 | 0x02) && info[3] == 45);
    CHECK(avdtp_parse_sbc_config(info, &y) == 0 && y.fs == x.fs && y.mode == x.mode && y.blocks == 12 && y.alloc == x.alloc &&
          y.bitpool == 45);
}

// ---- SDP

static size_t req(uint8_t *b, uint8_t pdu, uint16_t tid, const uint8_t *params, size_t n) {
    b[0] = pdu;
    b[1] = (uint8_t)(tid >> 8);
    b[2] = (uint8_t)tid;
    b[3] = (uint8_t)(n >> 8);
    b[4] = (uint8_t)n;
    memcpy(b + 5, params, n);
    return 5 + n;
}

static int contains(const uint8_t *h, size_t hn, const uint8_t *nd, size_t nn) {
    for (size_t i = 0; i + nn <= hn; i++) if (!memcmp(h + i, nd, nn)) return 1;
    return 0;
}

static void test_sdp(void) {
    uint8_t b[128], r[1024];
    // Service Search Attribute: A2DP Source (16-bit), all attributes (range 0x0000-0xFFFF)
    uint8_t p1[] = {0x35, 0x03, 0x19, 0x11, 0x0A, 0x01, 0x00, 0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, 0x00};
    size_t n = sdp_server_handle(b, req(b, SDP_SERVICE_SEARCH_ATTR_REQ, 0x1234, p1, sizeof p1), r, sizeof r);
    CHECK(n > 20 && r[0] == SDP_SERVICE_SEARCH_ATTR_RSP && r[1] == 0x12 && r[2] == 0x34);
    CHECK((size_t)((r[3] << 8) | r[4]) == n - 5 && r[n - 1] == 0x00);                     // param length, no continuation
    size_t blen = (size_t)((r[5] << 8) | r[6]);
    CHECK(7 + blen + 1 == n && r[7] == 0x36);
    const uint8_t avdtp[] = {0x19, 0x00, 0x19, 0x09, 0x01, 0x03};
    const uint8_t cls[] = {0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0A};
    CHECK(contains(r, n, avdtp, sizeof avdtp) && contains(r, n, cls, sizeof cls));
    const uint8_t avrcp[] = {0x19, 0x11, 0x0E};
    CHECK(!contains(r, n, avrcp, sizeof avrcp));                                   // only the matching record
    // 128-bit form of the same UUID, and only attribute 0x0004
    uint8_t p2[] = {0x35, 0x11, 0x1C, 0x00, 0x00, 0x11, 0x0A, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B,
                    0x34, 0xFB, 0x01, 0x00, 0x35, 0x03, 0x09, 0x00, 0x04, 0x00};
    n = sdp_server_handle(b, req(b, SDP_SERVICE_SEARCH_ATTR_REQ, 2, p2, sizeof p2), r, sizeof r);
    CHECK(n > 0 && r[0] == SDP_SERVICE_SEARCH_ATTR_RSP && contains(r, n, avdtp, sizeof avdtp) && !contains(r, n, cls, sizeof cls));
    // Service Search for L2CAP (both records), then attributes of the AVRCP record by handle
    uint8_t p3[] = {0x35, 0x03, 0x19, 0x01, 0x00, 0x00, 0x10, 0x00};
    n = sdp_server_handle(b, req(b, SDP_SERVICE_SEARCH_REQ, 3, p3, sizeof p3), r, sizeof r);
    CHECK(n == 9 + 8 + 1 && r[0] == SDP_SERVICE_SEARCH_RSP && r[6] == 2 && r[8] == 2);
    uint8_t p4[] = {0x00, 0x01, 0x00, 0x02, 0x01, 0x00, 0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, 0x00};
    n = sdp_server_handle(b, req(b, SDP_SERVICE_ATTR_REQ, 4, p4, sizeof p4), r, sizeof r);
    CHECK(n > 0 && r[0] == SDP_SERVICE_ATTR_RSP && contains(r, n, avrcp, sizeof avrcp));
    p4[3] = 0x09;                                                                  // no such record
    n = sdp_server_handle(b, req(b, SDP_SERVICE_ATTR_REQ, 5, p4, sizeof p4), r, sizeof r);
    CHECK(n == 7 && r[0] == SDP_ERROR_RSP && r[6] == 0x02);
    // Something we don't offer: empty list; garbage: error
    uint8_t p5[] = {0x35, 0x03, 0x19, 0x11, 0x24, 0x01, 0x00, 0x35, 0x03, 0x09, 0x00, 0x00, 0x00};
    n = sdp_server_handle(b, req(b, SDP_SERVICE_SEARCH_ATTR_REQ, 6, p5, sizeof p5), r, sizeof r);
    CHECK(n == 7 + 3 + 1 && r[7] == 0x36 && r[8] == 0 && r[9] == 0);
    uint8_t junk[] = {0x35, 0x40, 0x19};
    n = sdp_server_handle(b, req(b, SDP_SERVICE_SEARCH_ATTR_REQ, 7, junk, sizeof junk), r, sizeof r);
    CHECK(n == 7 && r[0] == SDP_ERROR_RSP);
    CHECK(sdp_record_count() == 2);
}

static void test_fuzz(void) {
    srand(99);
    uint8_t b[300], r[1024];
    for (int i = 0; i < 300000; i++) {
        size_t n = (size_t)(rand() % 64);
        for (size_t k = 0; k < n; k++) b[k] = (uint8_t)rand();
        if (n >= 5 && (rand() & 1)) { b[3] = 0; b[4] = (uint8_t)(n - 5); b[0] = (uint8_t)(2 + (rand() % 3) * 2); }
        if (n >= 8 && (rand() & 1)) { b[5] = 0x35; b[6] = (uint8_t)(rand() % 8); }
        sdp_server_handle(b, n, r, sizeof r);
    }
    Avdtp a;
    for (int round = 0; round < 3000; round++) {
        avdtp_init(&a, &kOps);
        nsent = 0;
        if (rand() & 1) avdtp_start(&a);
        for (int step = 0; step < 40; step++) {
            size_t n = (size_t)(rand() % 20);
            for (size_t k = 0; k < n; k++) b[k] = (uint8_t)rand();
            if (n >= 2) { b[0] &= 0xF3; if (rand() & 1) b[1] = (uint8_t)(1 + rand() % 13); }
            if (n >= 2 && (rand() & 1)) b[0] = (uint8_t)((a.sent_label << 4) | AVDTP_MSG_ACCEPT), b[1] = a.sent_signal;
            avdtp_rx(&a, b, n);
            if (rand() % 10 == 0) avdtp_media_open(&a, rand() & 1);
            nsent = 0;
        }
    }
    CHECK(1);
}

int main(void) {
    test_initiator();
    test_initiator_rejects();
    test_acceptor();
    test_pick();
    test_sdp();
    test_fuzz();
    printf("av: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
