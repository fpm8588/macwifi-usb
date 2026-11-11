// avdtp.c — see avdtp.h. Original code (GPL-2.0).

#include "avdtp.h"

#include <string.h>

// SBC codec-information bits (A2DP 4.3.2)
#define SBC_F16 0x80
#define SBC_F32 0x40
#define SBC_F44 0x20
#define SBC_F48 0x10
#define SBC_CM_MONO 0x08
#define SBC_CM_DUAL 0x04
#define SBC_CM_STEREO 0x02
#define SBC_CM_JOINT 0x01
#define SBC_BL4 0x80
#define SBC_BL8 0x40
#define SBC_BL12 0x20
#define SBC_BL16 0x10
#define SBC_SB4 0x08
#define SBC_SB8 0x04
#define SBC_AL_SNR 0x02
#define SBC_AL_LOUD 0x01
#define OUR_MAX_BITPOOL 53   // the "high quality" setting A2DP recommends for 44.1/48 kHz joint stereo

void avdtp_init(Avdtp *a, const AvdtpOps *ops) {
    memset(a, 0, sizeof *a);
    a->ops = *ops;
}

static void fail(Avdtp *a, const char *why) {
    int was = a->state == AV_STREAMING;
    a->state = AV_FAILED;
    if (was && a->ops.streaming) a->ops.streaming(a->ops.ctx, 0, &a->cfg);
    if (a->ops.failed) a->ops.failed(a->ops.ctx, why);
}

static void send_cmd(Avdtp *a, uint8_t signal, const uint8_t *p, size_t n) {
    uint8_t b[64];
    if (n > sizeof b - 2) return;
    a->sent_label = a->label;
    a->sent_signal = signal;
    b[0] = (uint8_t)((a->label << 4) | AVDTP_MSG_COMMAND);   // single packet
    b[1] = signal & 0x3F;
    if (n) memcpy(b + 2, p, n);
    a->label = (a->label + 1) & 0x0F;
    a->ops.send(a->ops.ctx, b, 2 + n);
}

static void send_rsp(Avdtp *a, uint8_t label, uint8_t type, uint8_t signal, const uint8_t *p, size_t n) {
    uint8_t b[64];
    if (n > sizeof b - 2) return;
    b[0] = (uint8_t)((label << 4) | type);
    b[1] = signal & 0x3F;
    if (n) memcpy(b + 2, p, n);
    a->ops.send(a->ops.ctx, b, 2 + n);
}

int avdtp_pick_sbc(const uint8_t caps[4], SbcConfig *out, uint8_t *min_bitpool) {
    uint8_t f = caps[0], b = caps[1];
    if (!(b & SBC_SB8)) return -1;                       // our encoder does 8 subbands
    out->fs = (f & SBC_F44) ? SBC_FS_44 : (f & SBC_F48) ? SBC_FS_48 : (f & SBC_F32) ? SBC_FS_32 : (f & SBC_F16) ? SBC_FS_16 : -1;
    out->mode = (f & SBC_CM_STEREO) ? SBC_MODE_STEREO : (f & SBC_CM_JOINT) ? SBC_MODE_JOINT
              : (f & SBC_CM_DUAL) ? SBC_MODE_DUAL : (f & SBC_CM_MONO) ? SBC_MODE_MONO : -1;
    out->blocks = (b & SBC_BL16) ? 16 : (b & SBC_BL12) ? 12 : (b & SBC_BL8) ? 8 : (b & SBC_BL4) ? 4 : 0;
    out->alloc = (b & SBC_AL_LOUD) ? SBC_ALLOC_LOUDNESS : (b & SBC_AL_SNR) ? SBC_ALLOC_SNR : -1;
    if (out->fs < 0 || out->mode < 0 || !out->blocks || out->alloc < 0) return -1;
    uint8_t lo = caps[2] < 2 ? 2 : caps[2], hi = caps[3];
    int bp = OUR_MAX_BITPOOL;
    if (out->mode == SBC_MODE_MONO || out->mode == SBC_MODE_DUAL) bp = 31;   // per channel
    if (bp > hi) bp = hi;
    if (bp < lo) return -1;
    out->bitpool = bp;
    if (min_bitpool) *min_bitpool = lo;
    return 0;
}

void avdtp_sbc_info(const SbcConfig *c, uint8_t min_bitpool, uint8_t out[4]) {
    static const uint8_t fs[4] = {SBC_F16, SBC_F32, SBC_F44, SBC_F48};
    static const uint8_t cm[4] = {SBC_CM_MONO, SBC_CM_DUAL, SBC_CM_STEREO, SBC_CM_JOINT};
    out[0] = (uint8_t)(fs[c->fs & 3] | cm[c->mode & 3]);
    out[1] = (uint8_t)((c->blocks == 4 ? SBC_BL4 : c->blocks == 8 ? SBC_BL8 : c->blocks == 12 ? SBC_BL12 : SBC_BL16) |
                       SBC_SB8 | (c->alloc == SBC_ALLOC_SNR ? SBC_AL_SNR : SBC_AL_LOUD));
    out[2] = min_bitpool;
    out[3] = (uint8_t)c->bitpool;
}

static int one_bit(uint8_t v) { return v && !(v & (v - 1)); }

int avdtp_parse_sbc_config(const uint8_t info[4], SbcConfig *out) {
    uint8_t f = info[0] & 0xF0, m = info[0] & 0x0F, bl = info[1] & 0xF0, sb = info[1] & 0x0C, al = info[1] & 0x03;
    if (!one_bit(f) || !one_bit(m) || !one_bit(bl) || sb != SBC_SB8 || !one_bit(al)) return -1;
    out->fs = f == SBC_F16 ? SBC_FS_16 : f == SBC_F32 ? SBC_FS_32 : f == SBC_F44 ? SBC_FS_44 : SBC_FS_48;
    out->mode = m == SBC_CM_MONO ? SBC_MODE_MONO : m == SBC_CM_DUAL ? SBC_MODE_DUAL : m == SBC_CM_STEREO ? SBC_MODE_STEREO
                                                                                                     : SBC_MODE_JOINT;
    out->blocks = bl == SBC_BL4 ? 4 : bl == SBC_BL8 ? 8 : bl == SBC_BL12 ? 12 : 16;
    out->alloc = al == SBC_AL_SNR ? SBC_ALLOC_SNR : SBC_ALLOC_LOUDNESS;
    out->bitpool = info[3];
    if (info[3] < 2 || info[3] < info[2]) return -1;
    return 0;
}

// Finds the SBC media-codec capability in a capability list; 0 and the 4 info bytes, or -1.
static int find_sbc(const uint8_t *p, size_t n, uint8_t info[4]) {
    size_t o = 0;
    while (o + 2 <= n) {
        uint8_t cat = p[o], len = p[o + 1];
        if (o + 2 + len > n) return -1;
        if (cat == AVDTP_CAT_MEDIA_CODEC && len >= 6 && (p[o + 2] >> 4) == 0 && p[o + 3] == AVDTP_CODEC_SBC) {
            memcpy(info, p + o + 4, 4);
            return 0;
        }
        o += 2 + (size_t)len;
    }
    return -1;
}

static void get_caps_next(Avdtp *a) {
    if (a->sep_i >= a->nseps) { fail(a, "the device offers no SBC audio stream"); return; }
    uint8_t p = (uint8_t)(a->seps[a->sep_i] << 2);
    a->state = AV_GETTING_CAPS;
    send_cmd(a, AVDTP_GET_CAPABILITIES, &p, 1);
}

void avdtp_start(Avdtp *a) {
    a->state = AV_DISCOVERING;
    a->nseps = a->sep_i = 0;
    a->want_start = 1;
    send_cmd(a, AVDTP_DISCOVER, NULL, 0);
}

static void start_stream(Avdtp *a) {
    uint8_t p = (uint8_t)(a->acp_seid << 2);
    a->state = AV_STARTING;
    send_cmd(a, AVDTP_START, &p, 1);
}

void avdtp_media_open(Avdtp *a, int open) {
    a->media_open = open;
    if (!open) {
        if (a->state == AV_STREAMING && a->ops.streaming) a->ops.streaming(a->ops.ctx, 0, &a->cfg);
        if (a->state == AV_STREAMING || a->state == AV_OPEN || a->state == AV_STARTING) a->state = AV_CONFIGURED;
        return;
    }
    if (a->state == AV_OPEN && a->want_start) start_stream(a);
    else if (a->state == AV_STREAMING && a->ops.streaming) a->ops.streaming(a->ops.ctx, 1, &a->cfg);
}

int avdtp_suspend(Avdtp *a) {
    if (a->state != AV_STREAMING) return -1;
    uint8_t p = (uint8_t)(a->acp_seid << 2);
    send_cmd(a, AVDTP_SUSPEND, &p, 1);
    a->state = AV_OPEN;
    a->want_start = 0;
    if (a->ops.streaming) a->ops.streaming(a->ops.ctx, 0, &a->cfg);
    return 0;
}

int avdtp_resume(Avdtp *a) {
    a->want_start = 1;
    if (a->state == AV_OPEN && a->media_open) { start_stream(a); return 0; }
    return a->state == AV_STREAMING ? 0 : -1;
}

// Our stream end point's capabilities: media transport, SBC with everything our encoder does.
static size_t our_caps(uint8_t *out) {
    static const uint8_t caps[] = {AVDTP_CAT_MEDIA_TRANSPORT, 0, AVDTP_CAT_MEDIA_CODEC, 6, 0x00, AVDTP_CODEC_SBC,
                                   SBC_F44 | SBC_F48 | SBC_CM_MONO | SBC_CM_DUAL | SBC_CM_STEREO | SBC_CM_JOINT,
                                   SBC_BL4 | SBC_BL8 | SBC_BL12 | SBC_BL16 | SBC_SB8 | SBC_AL_SNR | SBC_AL_LOUD, 2,
                                   OUR_MAX_BITPOOL};
    memcpy(out, caps, sizeof caps);
    return sizeof caps;
}

static void on_command(Avdtp *a, uint8_t label, uint8_t sig, const uint8_t *p, size_t n) {
    uint8_t r[16];
    switch (sig) {
    case AVDTP_DISCOVER:
        r[0] = (uint8_t)((AVDTP_LOCAL_SEID << 2) | (a->configured ? 0x02 : 0));   // in use once configured
        r[1] = 0x00;                                                               // audio, source
        send_rsp(a, label, AVDTP_MSG_ACCEPT, sig, r, 2);
        return;
    case AVDTP_GET_CAPABILITIES:
    case AVDTP_GET_ALL_CAPABILITIES:
        if (n < 1 || (p[0] >> 2) != AVDTP_LOCAL_SEID) { r[0] = 0x12; send_rsp(a, label, AVDTP_MSG_REJECT, sig, r, 1); return; }
        send_rsp(a, label, AVDTP_MSG_ACCEPT, sig, r, our_caps(r));
        return;
    case AVDTP_SET_CONFIGURATION:
    case AVDTP_RECONFIGURE: {
        size_t caps_at = sig == AVDTP_SET_CONFIGURATION ? 2 : 1;
        uint8_t info[4];
        SbcConfig c;
        if (n < caps_at || (p[0] >> 2) != AVDTP_LOCAL_SEID || find_sbc(p + caps_at, n - caps_at, info) != 0 ||
            avdtp_parse_sbc_config(info, &c) != 0) {
            r[0] = AVDTP_CAT_MEDIA_CODEC;
            r[1] = 0x29;                                        // unsupported configuration
            send_rsp(a, label, AVDTP_MSG_REJECT, sig, r, 2);
            return;
        }
        if (sig == AVDTP_SET_CONFIGURATION && a->configured && a->state != AV_IDLE && a->state != AV_FAILED) {
            r[0] = 0;
            r[1] = 0x13;                                        // SEP in use
            send_rsp(a, label, AVDTP_MSG_REJECT, sig, r, 2);
            return;
        }
        a->cfg = c;
        a->configured = 1;
        if (sig == AVDTP_SET_CONFIGURATION) a->acp_seid = p[1] >> 2;   // the sink's SEID (its INT SEID)
        a->state = AV_CONFIGURED;
        send_rsp(a, label, AVDTP_MSG_ACCEPT, sig, NULL, 0);
        return;
    }
    case AVDTP_GET_CONFIGURATION: {
        if (!a->configured) { r[0] = 0x31; send_rsp(a, label, AVDTP_MSG_REJECT, sig, r, 1); return; }
        uint8_t c[10] = {AVDTP_CAT_MEDIA_TRANSPORT, 0, AVDTP_CAT_MEDIA_CODEC, 6, 0x00, AVDTP_CODEC_SBC};
        avdtp_sbc_info(&a->cfg, 2, c + 6);
        send_rsp(a, label, AVDTP_MSG_ACCEPT, sig, c, 10);
        return;
    }
    case AVDTP_OPEN:
        if (!a->configured) { r[0] = 0x31; send_rsp(a, label, AVDTP_MSG_REJECT, sig, r, 1); return; }   // bad state
        a->state = AV_OPEN;
        send_rsp(a, label, AVDTP_MSG_ACCEPT, sig, NULL, 0);
        return;
    case AVDTP_START:
        if (a->state != AV_OPEN && a->state != AV_STREAMING) {
            r[0] = n ? p[0] : 0;
            r[1] = 0x31;
            send_rsp(a, label, AVDTP_MSG_REJECT, sig, r, 2);
            return;
        }
        send_rsp(a, label, AVDTP_MSG_ACCEPT, sig, NULL, 0);
        a->state = AV_STREAMING;
        a->want_start = 1;
        if (a->media_open && a->ops.streaming) a->ops.streaming(a->ops.ctx, 1, &a->cfg);
        return;
    case AVDTP_SUSPEND:
        send_rsp(a, label, AVDTP_MSG_ACCEPT, sig, NULL, 0);
        if (a->state == AV_STREAMING) {
            a->state = AV_OPEN;
            if (a->ops.streaming) a->ops.streaming(a->ops.ctx, 0, &a->cfg);
        }
        return;
    case AVDTP_CLOSE:
    case AVDTP_ABORT:
        send_rsp(a, label, AVDTP_MSG_ACCEPT, sig, NULL, 0);
        if (a->state == AV_STREAMING && a->ops.streaming) a->ops.streaming(a->ops.ctx, 0, &a->cfg);
        a->state = AV_IDLE;
        a->configured = 0;
        return;
    case AVDTP_DELAYREPORT:
        send_rsp(a, label, AVDTP_MSG_ACCEPT, sig, NULL, 0);
        return;
    default:
        send_rsp(a, label, AVDTP_MSG_GENERAL_REJECT, sig, NULL, 0);
        return;
    }
}

static void on_response(Avdtp *a, uint8_t label, uint8_t type, uint8_t sig, const uint8_t *p, size_t n) {
    if (label != a->sent_label || sig != a->sent_signal) return;    // not the answer to what we asked
    if (type != AVDTP_MSG_ACCEPT) {
        if (sig == AVDTP_GET_CAPABILITIES) { a->sep_i++; get_caps_next(a); return; }   // try the next SEP
        if (sig == AVDTP_SUSPEND || sig == AVDTP_DELAYREPORT) return;
        fail(a, sig == AVDTP_SET_CONFIGURATION ? "the device refused the audio settings"
                : sig == AVDTP_START ? "the device refused to start audio" : "the device refused the audio stream");
        return;
    }
    switch (sig) {
    case AVDTP_DISCOVER:
        if (a->state != AV_DISCOVERING) return;
        a->nseps = 0;
        for (size_t o = 0; o + 2 <= n && a->nseps < 8; o += 2) {
            uint8_t seid = p[o] >> 2, in_use = (p[o] >> 1) & 1, media = p[o + 1] >> 4, tsep = (p[o + 1] >> 3) & 1;
            if (media == 0 && tsep == 1 && !in_use) a->seps[a->nseps++] = seid;   // audio sinks
        }
        a->sep_i = 0;
        get_caps_next(a);
        return;
    case AVDTP_GET_CAPABILITIES: {
        if (a->state != AV_GETTING_CAPS) return;
        uint8_t info[4];
        uint8_t minbp = 2;
        if (find_sbc(p, n, info) != 0 || avdtp_pick_sbc(info, &a->cfg, &minbp) != 0) {
            a->sep_i++;
            get_caps_next(a);
            return;
        }
        a->acp_seid = a->seps[a->sep_i];
        uint8_t c[12] = {(uint8_t)(a->acp_seid << 2), (uint8_t)(AVDTP_LOCAL_SEID << 2), AVDTP_CAT_MEDIA_TRANSPORT, 0,
                         AVDTP_CAT_MEDIA_CODEC, 6, 0x00, AVDTP_CODEC_SBC};
        avdtp_sbc_info(&a->cfg, minbp, c + 8);
        a->state = AV_CONFIGURING;
        send_cmd(a, AVDTP_SET_CONFIGURATION, c, 12);
        return;
    }
    case AVDTP_SET_CONFIGURATION: {
        if (a->state != AV_CONFIGURING) return;
        a->configured = 1;
        uint8_t s = (uint8_t)(a->acp_seid << 2);
        a->state = AV_OPENING;
        send_cmd(a, AVDTP_OPEN, &s, 1);
        return;
    }
    case AVDTP_OPEN:
        if (a->state != AV_OPENING) return;
        a->state = AV_OPEN;
        if (a->media_open) { if (a->want_start) start_stream(a); }
        else if (a->ops.open_media) a->ops.open_media(a->ops.ctx);
        return;
    case AVDTP_START:
        if (a->state != AV_STARTING) return;
        a->state = AV_STREAMING;
        if (a->ops.streaming) a->ops.streaming(a->ops.ctx, 1, &a->cfg);
        return;
    default:
        return;
    }
}

void avdtp_rx(Avdtp *a, const uint8_t *d, size_t n) {
    if (n < 2) return;
    uint8_t label = d[0] >> 4, ptype = (d[0] >> 2) & 3, mtype = d[0] & 3, sig = d[1] & 0x3F;
    if (ptype != 0) return;                     // fragmented signaling isn't used by these short messages
    if (mtype == AVDTP_MSG_COMMAND) on_command(a, label, sig, d + 2, n - 2);
    else on_response(a, label, mtype, sig, d + 2, n - 2);
}
