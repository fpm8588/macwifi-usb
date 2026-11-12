// bthost.c — see bthost.h. Original code (GPL-2.0).

#include "bthost.h"

#include <stdio.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void bthost_init(BtHost *h, const BtHostOps *ops) {
    memset(h, 0, sizeof *h);
    h->ops = *ops;
    h->next_cid = L2CAP_FIRST_DYN_CID;
    h->credits = 1;
    h->acl_mtu = 27;          // the smallest any controller has, until Read Buffer Size says otherwise
    h->acl_total = h->acl_credits = 1;
}

void bthost_set_acl(BtHost *h, uint16_t mtu, int count) {
    if (mtu >= 27) h->acl_mtu = mtu;
    if (count > 0) h->acl_total = h->acl_credits = count;
}

void bthost_reset(BtHost *h) {
    BtHostOps ops = h->ops;
    bthost_init(h, &ops);
}

// ---------------------------------------------------------------- commands (flow-controlled)

static void try_send(BtHost *h) {
    while (h->credits > 0 && h->qn > 0) {
        int i = h->qh;
        h->qh = (h->qh + 1) % BTH_CMDQ;
        h->qn--;
        h->credits--;
        h->last_send = -1;   // stamped by the next poll
        if (h->ops.send_cmd) h->ops.send_cmd(h->ops.ctx, h->q[i].op, h->q[i].p, h->q[i].len);
    }
}

int bthost_cmd(BtHost *h, uint16_t opcode, const void *params, size_t plen) {
    if (plen > 255 || h->qn >= BTH_CMDQ) { h->dropped_cmds++; return -1; }
    int i = (h->qh + h->qn) % BTH_CMDQ;
    h->q[i].op = opcode;
    h->q[i].len = (uint8_t)plen;
    if (plen) memcpy(h->q[i].p, params, plen);
    h->qn++;
    try_send(h);
    return 0;
}

void bthost_poll(BtHost *h, double now) {
    h->now = now;
    if (h->last_nocp <= 0) h->last_nocp = now;
    // No buffer-free report for a second while we think every buffer is in use: the count is stale (a lost or
    // never-sent event). Start over rather than stall audio forever.
    if (h->acl_credits <= 0 && now - h->last_nocp > 1.0) {
        h->acl_credits = h->acl_total;
        h->last_nocp = now;
        h->credit_resets++;
    }
    if (h->last_send < 0) h->last_send = now;
    // No Command Complete/Status for our last command seen in 0.5 s (it may have been consumed by a synchronous
    // caller, or lost): allow one more.
    if (h->qn > 0 && h->credits <= 0 && now - h->last_send > 0.5) {
        h->credits = 1;
        try_send(h);
    }
}

// ---------------------------------------------------------------- connections

static BtConn *by_addr(BtHost *h, const uint8_t a[6]) {
    for (int i = 0; i < BTH_MAX_CONN; i++)
        if (h->c[i].state != BTC_FREE && !memcmp(h->c[i].addr, a, 6)) return &h->c[i];
    return NULL;
}

static BtConn *by_handle(BtHost *h, uint16_t handle) {
    for (int i = 0; i < BTH_MAX_CONN; i++)
        if (h->c[i].state != BTC_FREE && h->c[i].state != BTC_PAGING && h->c[i].handle == handle) return &h->c[i];
    return NULL;
}

static BtConn *alloc_conn(BtHost *h, const uint8_t a[6]) {
    for (int i = 0; i < BTH_MAX_CONN; i++)
        if (h->c[i].state == BTC_FREE) {
            memset(&h->c[i], 0, sizeof h->c[i]);
            memcpy(h->c[i].addr, a, 6);
            h->c[i].state = BTC_PAGING;
            return &h->c[i];
        }
    return NULL;
}

static void report(BtHost *h, const uint8_t a[6], BtHostEvent ev, uint32_t v) {
    if (h->ops.event) h->ops.event(h->ops.ctx, a, ev, v);
}

static void free_conn(BtConn *c) { memset(c, 0, sizeof *c); }

static void hci_disconnect(BtHost *h, BtConn *c, uint8_t reason) {
    uint8_t p[3];
    put16(p, c->handle);
    p[2] = reason;
    bthost_cmd(h, HCI_OP_DISCONNECT, p, 3);
    c->state = BTC_CLOSING;
}

BtConnState bthost_state(const BtHost *h, const uint8_t addr[6]) {
    for (int i = 0; i < BTH_MAX_CONN; i++)
        if (h->c[i].state != BTC_FREE && !memcmp(h->c[i].addr, addr, 6)) return (BtConnState)h->c[i].state;
    return BTC_FREE;
}

int bthost_connect(BtHost *h, const uint8_t addr[6], uint8_t psrm, uint16_t clock_offset) {
    return bthost_connect_profile(h, addr, psrm, clock_offset, L2CAP_PSM_HID_CTRL);
}

int bthost_connect_profile(BtHost *h, const uint8_t addr[6], uint8_t psrm, uint16_t clock_offset, uint16_t first_psm) {
    if (by_addr(h, addr)) return -1;
    BtConn *c = alloc_conn(h, addr);
    if (!c) return -1;
    c->initiator = 1;
    c->first_psm = first_psm;
    uint8_t p[13];
    memcpy(p, addr, 6);
    put16(p + 6, 0xCC18);                 // DM1, DH1, DM3, DH3, DM5, DH5 (the 2/3-EDR "may not use" bits clear)
    p[8] = psrm;
    p[9] = 0;
    put16(p + 10, clock_offset ? (uint16_t)(clock_offset | 0x8000) : 0);
    p[12] = 0x01;                          // allow role switch
    if (bthost_cmd(h, HCI_OP_CREATE_CONN, p, 13)) { free_conn(c); return -1; }
    report(h, addr, BTH_EV_CONNECTING, 0);
    return 0;
}

int bthost_disconnect(BtHost *h, const uint8_t addr[6]) {
    BtConn *c = by_addr(h, addr);
    if (!c) return -1;
    if (c->state == BTC_PAGING) {
        if (c->initiator) bthost_cmd(h, HCI_OP_CREATE_CONN_CANCEL, addr, 6);
        c->state = BTC_CLOSING;   // Connection Complete (status "unknown connection" / cancelled) frees it
        return 0;
    }
    if (c->state != BTC_CLOSING) hci_disconnect(h, c, 0x13);   // remote user terminated connection
    return 0;
}

void bthost_disconnect_all(BtHost *h) {
    for (int i = 0; i < BTH_MAX_CONN; i++)
        if (h->c[i].state != BTC_FREE) bthost_disconnect(h, h->c[i].addr);
}

// ---------------------------------------------------------------- L2CAP

// One L2CAP PDU, split into ACL packets of at most acl_mtu bytes (first: packet boundary 0b10, the rest 0b01).
// Each ACL packet uses one controller buffer; signaling and HID go out regardless (they're tiny and must not be
// lost), callers that can drop data check the credits first (bthost_l2cap_send).
static int send_l2cap(BtHost *h, BtConn *c, uint16_t cid, const uint8_t *data, size_t n) {
    uint8_t pdu[4 + 2048];
    uint8_t pkt[4 + 2048];
    if (n > 2048 || !h->ops.send_acl) return -1;
    put16(pdu, (uint16_t)n);
    put16(pdu + 2, cid);
    memcpy(pdu + 4, data, n);
    size_t total = 4 + n, off = 0;
    while (off < total) {
        size_t len = total - off < h->acl_mtu ? total - off : h->acl_mtu;
        put16(pkt, (uint16_t)((c->handle & 0x0FFF) | ((off == 0 ? 0x2 : 0x1) << 12)));
        put16(pkt + 2, (uint16_t)len);
        memcpy(pkt + 4, pdu + off, len);
        if (h->ops.send_acl(h->ops.ctx, pkt, 4 + len) != 0) return -1;
        h->acl_credits--;
        off += len;
    }
    return 0;
}

static void send_sig(BtHost *h, BtConn *c, uint8_t code, uint8_t id, const uint8_t *data, size_t n) {
    uint8_t b[4 + 64];
    if (n > 64) return;
    b[0] = code;
    b[1] = id;
    put16(b + 2, (uint16_t)n);
    if (n) memcpy(b + 4, data, n);
    send_l2cap(h, c, L2CAP_CID_SIGNALING, b, 4 + n);
}

static uint8_t next_id(BtHost *h) {
    if (++h->sig_id == 0) h->sig_id = 1;
    return h->sig_id;
}

static uint16_t new_cid(BtHost *h) {
    uint16_t cid = h->next_cid++;
    if (h->next_cid < L2CAP_FIRST_DYN_CID) h->next_cid = L2CAP_FIRST_DYN_CID;
    return cid;
}

static void conf_req(BtHost *h, BtConn *c, BtChan *ch) {
    uint8_t d[4];
    put16(d, ch->rcid);
    put16(d + 2, 0);            // flags; no options: MTU 672 and basic mode, the defaults
    send_sig(h, c, L2CAP_CONF_REQ, next_id(h), d, 4);
}

static void conn_rsp(BtHost *h, BtConn *c, uint8_t id, uint16_t dcid, uint16_t scid, uint16_t result, uint16_t status) {
    uint8_t d[8];
    put16(d, dcid);
    put16(d + 2, scid);
    put16(d + 4, result);
    put16(d + 6, status);
    send_sig(h, c, L2CAP_CONN_RSP, id, d, 8);
}

static void open_chan(BtHost *h, BtConn *c, BtChan *ch, uint16_t psm) {
    memset(ch, 0, sizeof *ch);
    ch->psm = psm;
    ch->rmtu = L2CAP_DEFAULT_MTU;
    ch->lcid = new_cid(h);
    ch->state = CH_WAIT_CONN_RSP;
    uint8_t d[4];
    put16(d, psm);
    put16(d + 2, ch->lcid);
    send_sig(h, c, L2CAP_CONN_REQ, next_id(h), d, 4);
}

static BtChan *chan_by_lcid(BtConn *c, uint16_t lcid) {
    if (c->ctrl.state != CH_CLOSED && c->ctrl.lcid == lcid) return &c->ctrl;
    if (c->intr.state != CH_CLOSED && c->intr.lcid == lcid) return &c->intr;
    for (int i = 0; i < BTH_EXT_CHANS; i++)
        if (c->ext[i].state != CH_CLOSED && c->ext[i].lcid == lcid) return &c->ext[i];
    return NULL;
}

static BtChan *free_ext(BtConn *c) {
    for (int i = 0; i < BTH_EXT_CHANS; i++) if (c->ext[i].state == CH_CLOSED) return &c->ext[i];
    return NULL;
}

static int is_ext(BtConn *c, BtChan *ch) { return ch >= c->ext && ch < c->ext + BTH_EXT_CHANS; }

static void check_open(BtHost *h, BtConn *c, BtChan *ch) {
    if (ch->state != CH_CONFIG || !ch->cfg_in_ok || !ch->cfg_out_ok) return;
    ch->state = CH_OPEN;
    if (is_ext(c, ch)) {
        if (c->state != BTC_OPEN) {                // the profile's first channel: the device is "connected"
            c->state = BTC_OPEN;
            report(h, c->addr, BTH_EV_CONNECTED, 0);
        }
        if (h->ops.l2cap_open) h->ops.l2cap_open(h->ops.ctx, c->addr, ch->psm, ch->lcid);
        return;
    }
    if (ch == &c->ctrl && c->initiator && c->intr.state == CH_CLOSED) open_chan(h, c, &c->intr, L2CAP_PSM_HID_INTR);
    if (c->ctrl.state == CH_OPEN && c->intr.state == CH_OPEN && c->state != BTC_OPEN) {
        c->state = BTC_OPEN;
        report(h, c->addr, BTH_EV_CONNECTED, 0);
        if (!c->protocol_sent) {
            // Boot protocol: fixed 8-byte keyboard / 3-4 byte mouse reports, no report descriptor needed.
            uint8_t sp = HID_SET_PROTOCOL_BOOT;
            send_l2cap(h, c, c->ctrl.rcid, &sp, 1);
            c->protocol_sent = 1;
        }
    }
}

static void accept_pending(BtHost *h, BtConn *c, BtChan *ch) {
    if (ch->state != CH_WAIT_SECURITY) return;
    conn_rsp(h, c, ch->rsp_id, ch->lcid, ch->rcid, 0, 0);
    ch->state = CH_CONFIG;
    conf_req(h, c, ch);
}

static void on_sig(BtHost *h, BtConn *c, uint8_t code, uint8_t id, const uint8_t *d, size_t n) {
    switch (code) {
    case L2CAP_CONN_REQ: {
        if (n < 4) return;
        uint16_t psm = le16(d), scid = le16(d + 2);
        BtChan *ch = psm == L2CAP_PSM_HID_CTRL ? &c->ctrl : psm == L2CAP_PSM_HID_INTR ? &c->intr : NULL;
        if (!ch && h->ops.l2cap_accept && h->ops.l2cap_accept(h->ops.ctx, c->addr, psm)) {
            ch = free_ext(c);
            if (!ch) { conn_rsp(h, c, id, 0, scid, 0x0004, 0); return; }        // no resources
        }
        if (!ch) { conn_rsp(h, c, id, 0, scid, 0x0002, 0); return; }            // PSM not supported
        if (ch->state != CH_CLOSED) { conn_rsp(h, c, id, 0, scid, 0x0004, 0); return; }   // no resources
        memset(ch, 0, sizeof *ch);
        ch->psm = psm;
        ch->rmtu = L2CAP_DEFAULT_MTU;
        ch->lcid = new_cid(h);
        ch->rcid = scid;
        if (!c->encrypted && psm != L2CAP_PSM_SDP) {   // profiles need an encrypted link (SDP doesn't)
            ch->state = CH_WAIT_SECURITY;
            ch->rsp_id = id;
            conn_rsp(h, c, id, ch->lcid, scid, 0x0001, 0x0001);   // pending, authentication pending
            return;
        }
        conn_rsp(h, c, id, ch->lcid, scid, 0, 0);
        ch->state = CH_CONFIG;
        conf_req(h, c, ch);
        return;
    }
    case L2CAP_CONN_RSP: {
        if (n < 8) return;
        uint16_t dcid = le16(d), scid = le16(d + 2), result = le16(d + 4);
        BtChan *ch = chan_by_lcid(c, scid);
        if (!ch || ch->state != CH_WAIT_CONN_RSP) return;
        if (result == 0) {
            ch->rcid = dcid;
            ch->state = CH_CONFIG;
            conf_req(h, c, ch);
        } else if (result != 0x0001) {           // refused
            ch->state = CH_CLOSED;
            report(h, c->addr, BTH_EV_FAILED, 0x100 | result);
            hci_disconnect(h, c, 0x13);
        }
        return;
    }
    case L2CAP_CONF_REQ: {
        if (n < 4) return;
        uint16_t dcid = le16(d);
        BtChan *ch = chan_by_lcid(c, dcid);
        if (!ch || ch->state != CH_CONFIG) {
            uint8_t r[6];
            put16(r, 0x0002);                     // invalid CID in request
            put16(r + 2, dcid);
            put16(r + 4, 0);
            send_sig(h, c, L2CAP_CMD_REJECT, id, r, 6);
            return;
        }
        // Options [Vol 3 Part A 5]: MTU (1), flush timeout (2), QoS (3), retransmission and flow control mode (4),
        // FCS (5), ... We take any MTU / flush timeout / QoS, but only basic mode: a request for ERTM or streaming
        // mode (which frame every packet differently) is refused with basic mode as the counter-proposal.
        int bad_mode = 0;
        char opts[160];
        size_t ol = 0;
        opts[0] = 0;
        for (size_t o = 4; o + 2 <= n && o + 2 + d[o + 1] <= n; o += 2 + d[o + 1]) {
            uint8_t type = d[o] & 0x7F, len = d[o + 1];
            if (type == 0x01 && len == 2) ch->rmtu = le16(d + o + 2);
            if (type == 0x04 && len >= 1 && d[o + 2] != 0x00) bad_mode = d[o + 2];
            if (ol < sizeof opts - 24) {
                ol += (size_t)snprintf(opts + ol, sizeof opts - ol, " %u:", type);
                for (uint8_t k = 0; k < len && k < 4 && ol < sizeof opts - 4; k++)
                    ol += (size_t)snprintf(opts + ol, sizeof opts - ol, "%02x", d[o + 2 + k]);
            }
        }
        if (h->ops.log) {
            char line[220];
            snprintf(line, sizeof line, "L2CAP config request for PSM 0x%04x, flags 0x%04x, options:%s", ch->psm,
                     le16(d + 2), opts[0] ? opts : " none");
            h->ops.log(h->ops.ctx, line);
        }
        if (bad_mode) {
            uint8_t r[6 + 11];
            put16(r, ch->rcid);
            put16(r + 2, 0);
            put16(r + 4, 0x0001);                 // unacceptable parameters
            r[6] = 0x04;                          // retransmission and flow control option, 9 bytes: basic mode
            r[7] = 9;
            memset(r + 8, 0, 9);
            send_sig(h, c, L2CAP_CONF_RSP, id, r, 17);
            if (h->ops.log) h->ops.log(h->ops.ctx, "L2CAP: refused a non-basic mode, proposed basic mode");
            return;
        }
        uint8_t r[6];
        put16(r, ch->rcid);
        put16(r + 2, 0);
        put16(r + 4, 0);                          // success: we take whatever MTU / flush timeout they ask for
        send_sig(h, c, L2CAP_CONF_RSP, id, r, 6);
        ch->cfg_in_ok = 1;
        check_open(h, c, ch);
        return;
    }
    case L2CAP_CONF_RSP: {
        if (n < 6) return;
        BtChan *ch = chan_by_lcid(c, le16(d));
        if (!ch || ch->state != CH_CONFIG) return;
        uint16_t result = le16(d + 4);
        if (h->ops.log && result != 0) {
            char line[96];
            snprintf(line, sizeof line, "L2CAP config response for PSM 0x%04x: result 0x%04x", ch->psm, result);
            h->ops.log(h->ops.ctx, line);
        }
        if (result == 0) {
            ch->cfg_out_ok = 1;
            check_open(h, c, ch);
        } else if (result != 0x0004) {            // not "pending": give up on this connection
            report(h, c->addr, BTH_EV_FAILED, 0x200 | result);
            hci_disconnect(h, c, 0x13);
        }
        return;
    }
    case L2CAP_DISCONN_REQ: {
        if (n < 4) return;
        uint16_t dcid = le16(d), scid = le16(d + 2);
        BtChan *ch = chan_by_lcid(c, dcid);
        uint8_t r[4];
        put16(r, dcid);
        put16(r + 2, scid);
        send_sig(h, c, L2CAP_DISCONN_RSP, id, r, 4);
        if (ch) {
            int ext = is_ext(c, ch);
            uint16_t psm = ch->psm, cid = ch->lcid;
            ch->state = CH_CLOSED;
            if (ext) {
                if (h->ops.l2cap_closed) h->ops.l2cap_closed(h->ops.ctx, c->addr, psm, cid);
                return;
            }
        }
        if (c->state == BTC_OPEN) c->state = BTC_CHANNELS;
        return;
    }
    case L2CAP_DISCONN_RSP: {
        if (n < 4) return;
        BtChan *ch = chan_by_lcid(c, le16(d + 2));
        if (ch) {
            uint16_t psm = ch->psm, cid = ch->lcid;
            int ext = is_ext(c, ch);
            ch->state = CH_CLOSED;
            if (ext && h->ops.l2cap_closed) h->ops.l2cap_closed(h->ops.ctx, c->addr, psm, cid);
        }
        return;
    }
    case L2CAP_ECHO_REQ:
        send_sig(h, c, L2CAP_ECHO_RSP, id, d, n > 64 ? 64 : n);
        return;
    case L2CAP_INFO_REQ: {
        if (n < 2) return;
        uint16_t type = le16(d);
        uint8_t r[12];
        put16(r, type);
        if (type == 0x0002) {                    // extended features: none
            put16(r + 2, 0);
            memset(r + 4, 0, 4);
            send_sig(h, c, L2CAP_INFO_RSP, id, r, 8);
        } else if (type == 0x0003) {             // fixed channels: signaling only
            put16(r + 2, 0);
            memset(r + 4, 0, 8);
            r[4] = 0x02;
            send_sig(h, c, L2CAP_INFO_RSP, id, r, 12);
        } else if (type == 0x0001) {             // connectionless MTU
            put16(r + 2, 0);
            put16(r + 4, 672);
            send_sig(h, c, L2CAP_INFO_RSP, id, r, 6);
        } else {
            put16(r + 2, 1);                     // not supported
            send_sig(h, c, L2CAP_INFO_RSP, id, r, 4);
        }
        return;
    }
    case L2CAP_CMD_REJECT:
    case L2CAP_ECHO_RSP:
    case L2CAP_INFO_RSP:
        return;
    default: {
        uint8_t r[2] = {0, 0};                   // command not understood
        send_sig(h, c, L2CAP_CMD_REJECT, id, r, 2);
        return;
    }
    }
}

static void l2cap_rx(BtHost *h, BtConn *c, uint16_t cid, const uint8_t *d, size_t n) {
    if (cid == L2CAP_CID_SIGNALING) {
        size_t o = 0;
        while (o + 4 <= n) {
            size_t len = le16(d + o + 2);
            if (o + 4 + len > n) break;
            on_sig(h, c, d[o], d[o + 1], d + o + 4, len);
            if (c->state == BTC_FREE) return;
            o += 4 + len;
        }
        return;
    }
    BtChan *ch = chan_by_lcid(c, cid);
    if (!ch || ch->state != CH_OPEN || n < 1) return;
    if (is_ext(c, ch)) {
        if (h->ops.l2cap_data) h->ops.l2cap_data(h->ops.ctx, c->addr, ch->psm, ch->lcid, d, n);
        return;
    }
    if (ch == &c->intr) {
        if (d[0] == HID_DATA_INPUT && n >= 2 && h->ops.hid_input) h->ops.hid_input(h->ops.ctx, c->addr, d + 1, n - 1);
    } else if (d[0] == HID_CONTROL_UNPLUG) {
        if (h->ops.key_forget) h->ops.key_forget(h->ops.ctx, c->addr);
        report(h, c->addr, BTH_EV_UNPLUGGED, 0);
        hci_disconnect(h, c, 0x13);
    }
}

void bthost_acl(BtHost *h, const uint8_t *pkt, size_t n) {
    if (n < 4) return;
    uint16_t hf = le16(pkt);
    uint16_t handle = hf & 0x0FFF;
    uint8_t pb = (hf >> 12) & 3;
    size_t dlen = le16(pkt + 2);
    if (4 + dlen > n) return;
    BtConn *c = by_handle(h, handle);
    if (!c) return;
    const uint8_t *d = pkt + 4;
    if (pb == 0x2 || pb == 0x0) {                // start of an L2CAP PDU
        if (dlen < 4) return;
        size_t need = 4 + (size_t)le16(d);
        if (need > sizeof c->rx) { c->rx_need = 0; c->rx_have = 0; return; }
        c->rx_need = need;
        c->rx_have = 0;
    } else if (pb != 0x1 || !c->rx_need) {      // continuation without a start
        return;
    }
    if (c->rx_have + dlen > c->rx_need) { c->rx_need = 0; c->rx_have = 0; return; }
    memcpy(c->rx + c->rx_have, d, dlen);
    c->rx_have += dlen;
    if (c->rx_have == c->rx_need) {
        size_t l = c->rx_need - 4;
        c->rx_need = 0;
        c->rx_have = 0;
        l2cap_rx(h, c, le16(c->rx + 2), c->rx + 4, l);
    }
}

// ---------------------------------------------------------------- HCI events

void bthost_event(BtHost *h, const HciEvent *ev) {
    const uint8_t *p = ev->p;
    size_t n = ev->len;
    BtConn *c;
    switch (ev->code) {
    case HCI_EV_CMD_COMPLETE:
        if (n >= 1) { h->credits = p[0]; try_send(h); }
        return;
    case HCI_EV_CMD_STATUS:
        if (n < 4) return;
        h->credits = p[1];
        if (p[0] != 0 && le16(p + 2) == HCI_OP_CREATE_CONN) {   // paging couldn't even start
            for (int i = 0; i < BTH_MAX_CONN; i++)
                if (h->c[i].state == BTC_PAGING && h->c[i].initiator) {
                    report(h, h->c[i].addr, BTH_EV_FAILED, p[0]);
                    free_conn(&h->c[i]);
                    break;
                }
        }
        try_send(h);
        return;
    case HCI_EV_CONN_REQUEST: {
        if (n < 10) return;
        uint32_t cod = (uint32_t)p[6] | ((uint32_t)p[7] << 8) | ((uint32_t)p[8] << 16);
        int ok = p[9] == 0x01 && !by_addr(h, p) && h->ops.accept && h->ops.accept(h->ops.ctx, p, cod);
        if (ok && (c = alloc_conn(h, p)) != NULL) {
            c->cod = cod;
            uint8_t r[7];
            memcpy(r, p, 6);
            r[6] = 0x00;                       // become central: we're the host
            bthost_cmd(h, HCI_OP_ACCEPT_CONN_REQ, r, 7);
            report(h, p, BTH_EV_CONNECTING, 0);
        } else {
            uint8_t r[7];
            memcpy(r, p, 6);
            r[6] = 0x0F;                       // connection rejected due to unacceptable BD_ADDR
            bthost_cmd(h, HCI_OP_REJECT_CONN_REQ, r, 7);
        }
        return;
    }
    case HCI_EV_CONN_COMPLETE: {
        if (n < 11 || p[9] != 0x01) return;    // ACL only
        c = by_addr(h, p + 3);
        if (!c) {
            if (p[0] == 0) {                   // a link we didn't ask for: close it
                uint8_t r[3];
                put16(r, le16(p + 1));
                r[2] = 0x13;
                bthost_cmd(h, HCI_OP_DISCONNECT, r, 3);
            }
            return;
        }
        if (p[0] != 0 || c->state == BTC_CLOSING) {
            if (p[0] != 0) report(h, c->addr, BTH_EV_FAILED, p[0]);
            if (p[0] == 0) {                   // cancelled too late: the link came up anyway
                c->handle = le16(p + 1) & 0x0FFF;
                hci_disconnect(h, c, 0x13);
                return;
            }
            free_conn(c);
            return;
        }
        c->handle = le16(p + 1) & 0x0FFF;
        c->state = BTC_SECURING;
        uint8_t r[2];
        put16(r, c->handle);
        bthost_cmd(h, HCI_OP_AUTH_REQUESTED, r, 2);
        return;
    }
    case HCI_EV_DISCONN_COMPLETE:
        if (n < 4 || p[0] != 0) return;
        c = by_handle(h, le16(p + 1) & 0x0FFF);
        if (!c) return;
        report(h, c->addr, c->state == BTC_SECURING ? BTH_EV_FAILED : BTH_EV_DISCONNECTED, p[3]);
        free_conn(c);
        return;
    case HCI_EV_AUTH_COMPLETE:
        if (n < 3) return;
        c = by_handle(h, le16(p + 1) & 0x0FFF);
        if (!c) return;
        if (p[0] == 0) {
            uint8_t r[3];
            put16(r, c->handle);
            r[2] = 0x01;
            bthost_cmd(h, HCI_OP_SET_CONN_ENCRYPT, r, 3);
        } else {
            // 0x06 "PIN or key missing" after we offered a stored key: the device forgot us; pair afresh next time
            if (p[0] == 0x06 && c->have_key_rq && h->ops.key_forget) h->ops.key_forget(h->ops.ctx, c->addr);
            report(h, c->addr, BTH_EV_FAILED, p[0]);
            hci_disconnect(h, c, 0x05);        // authentication failure
        }
        return;
    case HCI_EV_ENCRYPT_CHANGE:
        if (n < 4) return;
        c = by_handle(h, le16(p + 1) & 0x0FFF);
        if (!c) return;
        if (p[0] != 0 || p[3] == 0) {
            report(h, c->addr, BTH_EV_FAILED, p[0] ? p[0] : 0x3F);
            hci_disconnect(h, c, 0x05);
            return;
        }
        c->encrypted = 1;
        if (c->state == BTC_SECURING) c->state = BTC_CHANNELS;
        {
            // Keep the link active: in sniff mode the device may only talk every few tens of milliseconds, and a
            // mouse then moves in jumps (the classic "laggy Bluetooth mouse"). Allow role switch only.
            uint8_t r[4];
            put16(r, c->handle);
            put16(r + 2, 0x0001);
            bthost_cmd(h, HCI_OP_WRITE_LINK_POLICY, r, 4);
        }
        accept_pending(h, c, &c->ctrl);
        accept_pending(h, c, &c->intr);
        if (c->initiator && c->first_psm == L2CAP_PSM_HID_CTRL && c->ctrl.state == CH_CLOSED)
            open_chan(h, c, &c->ctrl, L2CAP_PSM_HID_CTRL);
        else if (c->initiator && c->first_psm && c->first_psm != L2CAP_PSM_HID_CTRL) {
            BtChan *ch = free_ext(c);
            int have = 0;
            for (int i = 0; i < BTH_EXT_CHANS; i++) if (c->ext[i].state != CH_CLOSED && c->ext[i].psm == c->first_psm) have = 1;
            if (ch && !have) open_chan(h, c, ch, c->first_psm);
        }
        return;
    case HCI_EV_LINK_KEY_REQUEST: {
        if (n < 6) return;
        uint8_t r[22];
        memcpy(r, p, 6);
        c = by_addr(h, p);
        if (h->ops.key_load && h->ops.key_load(h->ops.ctx, p, r + 6) == 0) {
            if (c) c->have_key_rq = 1;
            bthost_cmd(h, HCI_OP_LINK_KEY_REPLY, r, 22);
        } else {
            bthost_cmd(h, HCI_OP_LINK_KEY_NEG_REPLY, r, 6);
            report(h, p, BTH_EV_PAIRING, 0);
        }
        memset(r, 0, sizeof r);
        return;
    }
    case HCI_EV_LINK_KEY_NOTIFY:
        if (n < 23) return;
        if (h->ops.key_store) h->ops.key_store(h->ops.ctx, p, p + 6, p[22]);
        return;
    case HCI_EV_PIN_CODE_REQUEST: {             // legacy pairing (older devices): the common "0000"
        if (n < 6) return;
        uint8_t r[23];
        memset(r, 0, sizeof r);
        memcpy(r, p, 6);
        r[6] = 4;
        memcpy(r + 7, "0000", 4);
        bthost_cmd(h, HCI_OP_PIN_CODE_REPLY, r, 23);
        report(h, p, BTH_EV_PAIRING, 0);
        return;
    }
    case HCI_EV_IO_CAPABILITY_REQUEST: {
        if (n < 6) return;
        uint8_t r[9];
        memcpy(r, p, 6);
        r[6] = 0x01;                           // DisplayYesNo: keyboards get a passkey to type, mice Just Works
        r[7] = 0x00;                           // no OOB data
        r[8] = 0x05;                           // MITM protection required, general bonding
        bthost_cmd(h, HCI_OP_IO_CAPABILITY_REPLY, r, 9);
        report(h, p, BTH_EV_PAIRING, 0);
        return;
    }
    case HCI_EV_USER_CONFIRM_REQUEST:           // Just Works / numeric comparison: the user started this pairing
        if (n < 6) return;
        bthost_cmd(h, HCI_OP_USER_CONFIRM_REPLY, p, 6);
        return;
    case HCI_EV_USER_PASSKEY_NOTIFY:
        if (n < 10) return;
        report(h, p, BTH_EV_PASSKEY, (uint32_t)p[6] | ((uint32_t)p[7] << 8) | ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24));
        return;
    case HCI_EV_NUM_COMPLETED_PKTS:
        if (n < 1) return;
        for (size_t i = 0; i < p[0] && 1 + i * 4 + 4 <= n; i++) {
            h->acl_credits += le16(p + 1 + i * 4 + 2);
            h->completed += le16(p + 1 + i * 4 + 2);
        }
        if (h->acl_credits > h->acl_total) h->acl_credits = h->acl_total;
        h->last_nocp = h->now;
        return;
    case HCI_EV_FLUSH_OCCURRED:
        h->flushed++;
        return;
    case HCI_EV_DATA_BUFFER_OVERFLOW:
        h->overflows++;
        return;
    case HCI_EV_MODE_CHANGE:
        if (n < 6 || p[0] != 0) return;
        c = by_handle(h, le16(p + 1) & 0x0FFF);
        if (!c) return;
        report(h, c->addr, BTH_EV_MODE, (uint32_t)p[3] | ((uint32_t)le16(p + 4) << 8));
        if (p[3] == 0x02) {                    // sniff anyway (the device insisted): ask to leave it
            uint8_t r[2];
            put16(r, c->handle);
            bthost_cmd(h, HCI_OP_EXIT_SNIFF_MODE, r, 2);
        }
        return;
    case HCI_EV_SIMPLE_PAIRING_COMPLETE:
        if (n < 7 || p[0] == 0) return;
        report(h, p + 1, BTH_EV_FAILED, p[0]);
        return;
    default:
        return;
    }
}

// ---------------------------------------------------------------- other-profile channels

int bthost_l2cap_connect(BtHost *h, const uint8_t addr[6], uint16_t psm) {
    BtConn *c = by_addr(h, addr);
    if (!c || !c->handle || (!c->encrypted && psm != L2CAP_PSM_SDP)) return -1;
    BtChan *ch = free_ext(c);
    if (!ch) return -1;
    open_chan(h, c, ch, psm);
    return ch->lcid;
}

int bthost_l2cap_send(BtHost *h, const uint8_t addr[6], uint16_t cid, const uint8_t *d, size_t n) {
    BtConn *c = by_addr(h, addr);
    if (!c) return -1;
    BtChan *ch = chan_by_lcid(c, cid);
    if (!ch || ch->state != CH_OPEN || n > ch->rmtu) return -1;
    int frags = (int)((4 + n + h->acl_mtu - 1) / h->acl_mtu);
    if (h->acl_credits < frags) return -2;
    return send_l2cap(h, c, ch->rcid, d, n);
}

int bthost_l2cap_send_ctrl(BtHost *h, const uint8_t addr[6], uint16_t cid, const uint8_t *d, size_t n) {
    BtConn *c = by_addr(h, addr);
    if (!c) return -1;
    BtChan *ch = chan_by_lcid(c, cid);
    if (!ch || ch->state != CH_OPEN || n > ch->rmtu) return -1;
    return send_l2cap(h, c, ch->rcid, d, n);
}

int bthost_l2cap_close(BtHost *h, const uint8_t addr[6], uint16_t cid) {
    BtConn *c = by_addr(h, addr);
    if (!c) return -1;
    BtChan *ch = chan_by_lcid(c, cid);
    if (!ch) return -1;
    uint8_t d[4];
    put16(d, ch->rcid);
    put16(d + 2, ch->lcid);
    send_sig(h, c, L2CAP_DISCONN_REQ, next_id(h), d, 4);
    uint16_t psm = ch->psm;
    ch->state = CH_CLOSED;
    if (is_ext(c, ch) && h->ops.l2cap_closed) h->ops.l2cap_closed(h->ops.ctx, c->addr, psm, cid);
    return 0;
}

uint16_t bthost_l2cap_mtu(const BtHost *h, const uint8_t addr[6], uint16_t cid) {
    for (int i = 0; i < BTH_MAX_CONN; i++) {
        const BtConn *c = &h->c[i];
        if (c->state == BTC_FREE || memcmp(c->addr, addr, 6)) continue;
        for (int k = 0; k < BTH_EXT_CHANS; k++)
            if (c->ext[k].state == CH_OPEN && c->ext[k].lcid == cid) return c->ext[k].rmtu;
    }
    return 0;
}
