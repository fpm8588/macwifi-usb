// avdtp.h — AVDTP 1.3 signaling for an A2DP source (the Mac) with SBC: as initiator (discover the sink's stream
// end points, read their capabilities, configure, open, start) and as acceptor (answer a sink that drives it).
// Original code (GPL-2.0); facts from the AVDTP 1.3 and A2DP 1.3 specifications. No I/O: packets go out through
// callbacks and come in through avdtp_rx.

#ifndef AVDTP_H
#define AVDTP_H

#include <stddef.h>
#include <stdint.h>

#include "sbc.h"

// Signal identifiers (AVDTP 8.5)
enum {
    AVDTP_DISCOVER = 0x01, AVDTP_GET_CAPABILITIES = 0x02, AVDTP_SET_CONFIGURATION = 0x03,
    AVDTP_GET_CONFIGURATION = 0x04, AVDTP_RECONFIGURE = 0x05, AVDTP_OPEN = 0x06, AVDTP_START = 0x07,
    AVDTP_CLOSE = 0x08, AVDTP_SUSPEND = 0x09, AVDTP_ABORT = 0x0A, AVDTP_SECURITY_CONTROL = 0x0B,
    AVDTP_GET_ALL_CAPABILITIES = 0x0C, AVDTP_DELAYREPORT = 0x0D
};
// Message types (low 2 bits of the first byte)
enum { AVDTP_MSG_COMMAND = 0, AVDTP_MSG_GENERAL_REJECT = 1, AVDTP_MSG_ACCEPT = 2, AVDTP_MSG_REJECT = 3 };
// Service categories
enum { AVDTP_CAT_MEDIA_TRANSPORT = 0x01, AVDTP_CAT_MEDIA_CODEC = 0x07, AVDTP_CAT_DELAY_REPORTING = 0x08 };
#define AVDTP_LOCAL_SEID 1
#define AVDTP_CODEC_SBC  0x00

typedef enum {
    AV_IDLE = 0,
    AV_DISCOVERING,     // DISCOVER sent
    AV_GETTING_CAPS,    // GET_CAPABILITIES sent to sep[sep_i]
    AV_CONFIGURING,     // SET_CONFIGURATION sent
    AV_CONFIGURED,      // configured (either side), not open
    AV_OPENING,         // OPEN sent
    AV_OPEN,            // open; the media channel may still be coming
    AV_STARTING,        // START sent
    AV_STREAMING,
    AV_FAILED
} AvdtpState;

typedef struct {
    void *ctx;
    int (*send)(void *ctx, const uint8_t *d, size_t n);   // on the signaling channel
    void (*open_media)(void *ctx);                        // as initiator: open the media (transport) channel now
    void (*streaming)(void *ctx, int on, const SbcConfig *cfg);
    void (*failed)(void *ctx, const char *why);
} AvdtpOps;

typedef struct {
    AvdtpOps ops;
    AvdtpState state;
    uint8_t label;            // our next transaction label
    uint8_t sent_signal, sent_label;
    uint8_t seps[8];          // the sink's stream end points we may use (SEIDs)
    int nseps, sep_i;
    uint8_t acp_seid;         // the sink's SEID in use
    SbcConfig cfg;
    int configured, media_open, want_start;
} Avdtp;

void avdtp_init(Avdtp *a, const AvdtpOps *ops);
void avdtp_start(Avdtp *a);                   // as initiator: discover and set up a stream
void avdtp_rx(Avdtp *a, const uint8_t *d, size_t n);
void avdtp_media_open(Avdtp *a, int open);    // the media channel came up / went away
int avdtp_suspend(Avdtp *a);                  // pause streaming (0, or -1 when not streaming)
int avdtp_resume(Avdtp *a);                   // start again after a suspend

// Chooses an SBC configuration from a sink's SBC capability bytes (4: frequencies|modes, blocks|subbands|alloc,
// min bitpool, max bitpool). 0, or -1 if nothing usable (e.g. 4 subbands only).
int avdtp_pick_sbc(const uint8_t caps[4], SbcConfig *out, uint8_t *min_bitpool);
// The 4 codec-information bytes describing one chosen configuration.
void avdtp_sbc_info(const SbcConfig *cfg, uint8_t min_bitpool, uint8_t out[4]);
// Parses SBC codec-information bytes describing a single configuration. 0 or -1.
int avdtp_parse_sbc_config(const uint8_t info[4], SbcConfig *out);

#endif
