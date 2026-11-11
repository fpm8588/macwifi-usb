// test_bthost.c — the Bluetooth Classic host (pairing, L2CAP, HID) against a scripted device, the HID boot report
// decoding, and fuzzing of both. Original code (GPL-2.0).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bthost.h"
#include "hidboot.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------- what the host sent
typedef struct { uint16_t op; uint8_t p[255]; size_t n; } Cmd;
static Cmd cmds[256];
static int ncmds;
typedef struct { uint8_t p[700]; size_t n; } Acl;
static Acl acls[256];
static int nacls;
static int evs[64], nev;
static uint32_t evval[64];
static uint8_t stored_key[16];
static int have_key, forgot, accept_ok = 1, hid_reports;
static uint8_t last_report[64];
static size_t last_report_n;

static int f_cmd(void *ctx, uint16_t op, const uint8_t *p, size_t n) {
    (void)ctx;
    if (ncmds < 256) { cmds[ncmds].op = op; memcpy(cmds[ncmds].p, p, n); cmds[ncmds].n = n; ncmds++; }
    return 0;
}
static int f_acl(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    if (nacls < 256 && n <= 700) { memcpy(acls[nacls].p, p, n); acls[nacls].n = n; nacls++; }
    return 0;
}
static int f_key_load(void *ctx, const uint8_t a[6], uint8_t k[16]) {
    (void)ctx; (void)a;
    if (!have_key) return -1;
    memcpy(k, stored_key, 16);
    return 0;
}
static void f_key_store(void *ctx, const uint8_t a[6], const uint8_t k[16], uint8_t t) {
    (void)ctx; (void)a; (void)t;
    memcpy(stored_key, k, 16);
    have_key = 1;
}
static void f_key_forget(void *ctx, const uint8_t a[6]) { (void)ctx; (void)a; have_key = 0; forgot++; }
static int f_accept(void *ctx, const uint8_t a[6], uint32_t cod) { (void)ctx; (void)a; (void)cod; return accept_ok; }
static void f_event(void *ctx, const uint8_t a[6], BtHostEvent ev, uint32_t v) {
    (void)ctx; (void)a;
    if (nev < 64) { evs[nev] = ev; evval[nev] = v; nev++; }
}
static void f_hid(void *ctx, const uint8_t a[6], const uint8_t *r, size_t n) {
    (void)ctx; (void)a;
    hid_reports++;
    last_report_n = n < sizeof last_report ? n : sizeof last_report;
    memcpy(last_report, r, last_report_n);
}

static const BtHostOps kOps = {NULL, f_cmd, f_acl, f_key_load, f_key_store, f_key_forget, f_accept, f_event, f_hid};
static const uint8_t ADDR[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

static void reset_capture(void) { ncmds = 0; nacls = 0; nev = 0; }

static void feed(BtHost *h, uint8_t code, const uint8_t *p, size_t n) {
    uint8_t buf[2 + 255];
    buf[0] = code;
    buf[1] = (uint8_t)n;
    memcpy(buf + 2, p, n);
    HciEvent ev;
    if (hci_event(buf, n + 2, &ev) == 0) bthost_event(h, &ev);
}

static void cc(BtHost *h, uint16_t op) {   // Command Complete, status 0, 1 slot
    uint8_t p[4] = {1, (uint8_t)op, (uint8_t)(op >> 8), 0};
    feed(h, HCI_EV_CMD_COMPLETE, p, 4);
}
static void cs(BtHost *h, uint16_t op, uint8_t status) {
    uint8_t p[4] = {status, 1, (uint8_t)op, (uint8_t)(op >> 8)};
    feed(h, HCI_EV_CMD_STATUS, p, 4);
}

static const Cmd *last_cmd(void) { return ncmds ? &cmds[ncmds - 1] : NULL; }
static int sent(uint16_t op) {
    for (int i = 0; i < ncmds; i++) if (cmds[i].op == op) return 1;
    return 0;
}
static int had_event(BtHostEvent e) {
    for (int i = 0; i < nev; i++) if (evs[i] == (int)e) return 1;
    return 0;
}

// The device side: an L2CAP PDU from the device on `handle`.
static void dev_l2cap(BtHost *h, uint16_t handle, uint16_t cid, const uint8_t *d, size_t n) {
    uint8_t pkt[8 + 600];
    pkt[0] = (uint8_t)handle;
    pkt[1] = (uint8_t)(((handle >> 8) & 0x0F) | 0x20);
    pkt[2] = (uint8_t)(n + 4);
    pkt[3] = (uint8_t)((n + 4) >> 8);
    pkt[4] = (uint8_t)n;
    pkt[5] = (uint8_t)(n >> 8);
    pkt[6] = (uint8_t)cid;
    pkt[7] = (uint8_t)(cid >> 8);
    memcpy(pkt + 8, d, n);
    bthost_acl(h, pkt, 8 + n);
}
static void dev_sig(BtHost *h, uint16_t handle, uint8_t code, uint8_t id, const uint8_t *d, size_t n) {
    uint8_t b[4 + 64];
    b[0] = code;
    b[1] = id;
    b[2] = (uint8_t)n;
    b[3] = 0;
    memcpy(b + 4, d, n);
    dev_l2cap(h, handle, L2CAP_CID_SIGNALING, b, 4 + n);
}

// Finds the most recent signaling command `code` the host sent; returns its data (after code/id/len) or NULL.
static const uint8_t *host_sig(uint8_t code, uint8_t *id, uint16_t *cid_out) {
    for (int i = nacls - 1; i >= 0; i--) {
        const uint8_t *p = acls[i].p;
        if (acls[i].n >= 12 && (p[6] | (p[7] << 8)) == L2CAP_CID_SIGNALING && p[8] == code) {
            if (id) *id = p[9];
            if (cid_out) *cid_out = (uint16_t)(p[6] | (p[7] << 8));
            return p + 12;
        }
    }
    return NULL;
}
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Drives the device side of opening one channel that the host initiated: answer CONN_REQ, then both configs.
static uint16_t dev_accept_chan(BtHost *h, uint16_t handle, uint16_t psm, uint16_t dev_cid) {
    uint8_t id = 0;
    const uint8_t *req = host_sig(L2CAP_CONN_REQ, &id, NULL);
    CHECK(req && u16(req) == psm);
    if (!req) return 0;
    uint16_t lcid = u16(req + 2);
    uint8_t rsp[8] = {(uint8_t)dev_cid, (uint8_t)(dev_cid >> 8), (uint8_t)lcid, (uint8_t)(lcid >> 8), 0, 0, 0, 0};
    dev_sig(h, handle, L2CAP_CONN_RSP, id, rsp, 8);
    const uint8_t *creq = host_sig(L2CAP_CONF_REQ, &id, NULL);
    CHECK(creq && u16(creq) == dev_cid);
    uint8_t crsp[6] = {(uint8_t)lcid, (uint8_t)(lcid >> 8), 0, 0, 0, 0};
    dev_sig(h, handle, L2CAP_CONF_RSP, id, crsp, 6);
    uint8_t dreq[8] = {(uint8_t)lcid, (uint8_t)(lcid >> 8), 0, 0, 0x01, 0x02, 0xA0, 0x02};   // + MTU option 672
    dev_sig(h, handle, L2CAP_CONF_REQ, 0x33, dreq, 8);
    const uint8_t *drsp = host_sig(L2CAP_CONF_RSP, &id, NULL);
    CHECK(drsp && id == 0x33 && u16(drsp) == dev_cid && u16(drsp + 4) == 0);
    return lcid;
}

static void test_pair_keyboard(void) {
    BtHost h;
    bthost_init(&h, &kOps);
    reset_capture();
    have_key = 0;
    CHECK(bthost_connect(&h, ADDR, 1, 0x1234) == 0);
    CHECK(bthost_connect(&h, ADDR, 1, 0x1234) == -1);                // already connecting
    CHECK(last_cmd() && last_cmd()->op == HCI_OP_CREATE_CONN && !memcmp(last_cmd()->p, ADDR, 6) &&
          last_cmd()->p[8] == 1 && u16(last_cmd()->p + 10) == (0x1234 | 0x8000));
    CHECK(had_event(BTH_EV_CONNECTING) && bthost_state(&h, ADDR) == BTC_PAGING);
    cs(&h, HCI_OP_CREATE_CONN, 0);
    uint8_t ccmp[11] = {0, 0x0B, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01, 0x00};
    feed(&h, HCI_EV_CONN_COMPLETE, ccmp, 11);
    CHECK(last_cmd()->op == HCI_OP_AUTH_REQUESTED && u16(last_cmd()->p) == 0x000B);
    CHECK(bthost_state(&h, ADDR) == BTC_SECURING);
    cs(&h, HCI_OP_AUTH_REQUESTED, 0);
    feed(&h, HCI_EV_LINK_KEY_REQUEST, ADDR, 6);
    CHECK(last_cmd()->op == HCI_OP_LINK_KEY_NEG_REPLY && had_event(BTH_EV_PAIRING));
    cc(&h, HCI_OP_LINK_KEY_NEG_REPLY);
    feed(&h, HCI_EV_IO_CAPABILITY_REQUEST, ADDR, 6);
    CHECK(last_cmd()->op == HCI_OP_IO_CAPABILITY_REPLY && last_cmd()->p[6] == 0x01 && last_cmd()->p[8] == 0x05);
    cc(&h, HCI_OP_IO_CAPABILITY_REPLY);
    uint8_t pk[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x40, 0xE2, 0x01, 0x00};   // 123456
    feed(&h, HCI_EV_USER_PASSKEY_NOTIFY, pk, 10);
    CHECK(nev > 0 && evs[nev - 1] == BTH_EV_PASSKEY && evval[nev - 1] == 123456);
    uint8_t lk[23];
    memcpy(lk, ADDR, 6);
    for (int i = 0; i < 16; i++) lk[6 + i] = (uint8_t)(0xA0 + i);
    lk[22] = 0x05;
    feed(&h, HCI_EV_LINK_KEY_NOTIFY, lk, 23);
    CHECK(have_key && stored_key[0] == 0xA0 && stored_key[15] == 0xAF);
    uint8_t spc[7] = {0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    feed(&h, HCI_EV_SIMPLE_PAIRING_COMPLETE, spc, 7);
    uint8_t ac[3] = {0, 0x0B, 0};
    feed(&h, HCI_EV_AUTH_COMPLETE, ac, 3);
    CHECK(last_cmd()->op == HCI_OP_SET_CONN_ENCRYPT && last_cmd()->p[2] == 1);
    cs(&h, HCI_OP_SET_CONN_ENCRYPT, 0);
    uint8_t ec[4] = {0, 0x0B, 0, 1};
    feed(&h, HCI_EV_ENCRYPT_CHANGE, ec, 4);
    CHECK(bthost_state(&h, ADDR) == BTC_CHANNELS);
    CHECK(last_cmd()->op == HCI_OP_WRITE_LINK_POLICY && u16(last_cmd()->p) == 0x0B && u16(last_cmd()->p + 2) == 0x0001);
    cc(&h, HCI_OP_WRITE_LINK_POLICY);
    // The device puts the link in sniff mode anyway: reported, and we ask to exit
    uint8_t mc[6] = {0, 0x0B, 0, 0x02, 0x12, 0x00};
    feed(&h, HCI_EV_MODE_CHANGE, mc, 6);
    CHECK(evs[nev - 1] == BTH_EV_MODE && evval[nev - 1] == (0x02 | (0x12 << 8)) && last_cmd()->op == HCI_OP_EXIT_SNIFF_MODE);
    cc(&h, HCI_OP_EXIT_SNIFF_MODE);
    uint16_t lctrl = dev_accept_chan(&h, 0x0B, L2CAP_PSM_HID_CTRL, 0x0050);
    CHECK(lctrl >= L2CAP_FIRST_DYN_CID);
    CHECK(bthost_state(&h, ADDR) == BTC_CHANNELS && !had_event(BTH_EV_CONNECTED));
    uint16_t lintr = dev_accept_chan(&h, 0x0B, L2CAP_PSM_HID_INTR, 0x0051);
    CHECK(lintr && lintr != lctrl);
    CHECK(bthost_state(&h, ADDR) == BTC_OPEN && had_event(BTH_EV_CONNECTED));
    // SET_PROTOCOL(boot) on the device's control channel
    const Acl *a = &acls[nacls - 1];
    CHECK(a->n == 9 && u16(a->p + 6) == 0x0050 && a->p[8] == HID_SET_PROTOCOL_BOOT && (u16(a->p) & 0x0FFF) == 0x0B);
    // Input: keyboard report on the interrupt channel
    uint8_t kb[10] = {HID_DATA_INPUT, 0x01, 0x02, 0, 0x04, 0, 0, 0, 0, 0};
    dev_l2cap(&h, 0x0B, lintr, kb, 10);
    CHECK(hid_reports == 1 && last_report_n == 9 && last_report[0] == 0x01 && last_report[3] == 0x04);
    dev_l2cap(&h, 0x0B, lctrl, kb, 10);                               // DATA on control: not input
    dev_l2cap(&h, 0x0B, 0x0099, kb, 10);                              // unknown channel
    CHECK(hid_reports == 1);
    // Fragmented PDU: start with 6 bytes, continuation with the rest
    {
        uint8_t full[14] = {10, 0, (uint8_t)lintr, (uint8_t)(lintr >> 8), HID_DATA_INPUT, 0x01, 0, 0, 0x05, 0, 0, 0, 0, 0};
        uint8_t p1[4 + 6] = {0x0B, 0x20, 6, 0};
        memcpy(p1 + 4, full, 6);
        uint8_t p2[4 + 8] = {0x0B, 0x10, 8, 0};
        memcpy(p2 + 4, full + 6, 8);
        bthost_acl(&h, p1, sizeof p1);
        CHECK(hid_reports == 1);
        bthost_acl(&h, p2, sizeof p2);
        CHECK(hid_reports == 2 && last_report[3] == 0x05);
    }
    // Information and echo requests get answers
    uint8_t ir[2] = {0x02, 0x00};
    dev_sig(&h, 0x0B, L2CAP_INFO_REQ, 0x41, ir, 2);
    uint8_t id = 0;
    const uint8_t *irsp = host_sig(L2CAP_INFO_RSP, &id, NULL);
    CHECK(irsp && id == 0x41 && u16(irsp) == 2 && u16(irsp + 2) == 0);
    dev_sig(&h, 0x0B, L2CAP_ECHO_REQ, 0x42, ir, 2);
    CHECK(host_sig(L2CAP_ECHO_RSP, &id, NULL) && id == 0x42);
    dev_sig(&h, 0x0B, 0x55, 0x43, ir, 2);                             // unknown signaling code
    CHECK(host_sig(L2CAP_CMD_REJECT, &id, NULL) && id == 0x43);
    // Device closes the interrupt channel, then the link
    uint8_t dr[4] = {(uint8_t)lintr, (uint8_t)(lintr >> 8), 0x51, 0x00};
    dev_sig(&h, 0x0B, L2CAP_DISCONN_REQ, 0x44, dr, 4);
    const uint8_t *drsp = host_sig(L2CAP_DISCONN_RSP, &id, NULL);
    CHECK(drsp && id == 0x44 && u16(drsp) == lintr && bthost_state(&h, ADDR) == BTC_CHANNELS);
    uint8_t dc[4] = {0, 0x0B, 0, 0x13};
    feed(&h, HCI_EV_DISCONN_COMPLETE, dc, 4);
    CHECK(bthost_state(&h, ADDR) == BTC_FREE && evs[nev - 1] == BTH_EV_DISCONNECTED && evval[nev - 1] == 0x13);
}

static void test_reconnect_incoming(void) {
    BtHost h;
    bthost_init(&h, &kOps);
    reset_capture();
    have_key = 1;
    memset(stored_key, 0x5A, 16);
    uint8_t cr[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x40, 0x25, 0x00, 0x01};   // keyboard class, ACL
    feed(&h, HCI_EV_CONN_REQUEST, cr, 10);
    CHECK(last_cmd()->op == HCI_OP_ACCEPT_CONN_REQ && last_cmd()->p[6] == 0x00);
    cs(&h, HCI_OP_ACCEPT_CONN_REQ, 0);
    uint8_t ccmp[11] = {0, 0x0C, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01, 0x00};
    feed(&h, HCI_EV_CONN_COMPLETE, ccmp, 11);
    CHECK(last_cmd()->op == HCI_OP_AUTH_REQUESTED);
    cs(&h, HCI_OP_AUTH_REQUESTED, 0);
    // The device opens the control channel before encryption: answered "pending"
    uint8_t creq[4] = {0x11, 0x00, 0x60, 0x00};
    dev_sig(&h, 0x0C, L2CAP_CONN_REQ, 0x21, creq, 4);
    uint8_t id = 0;
    const uint8_t *rsp = host_sig(L2CAP_CONN_RSP, &id, NULL);
    CHECK(rsp && id == 0x21 && u16(rsp + 2) == 0x0060 && u16(rsp + 4) == 1 && u16(rsp + 6) == 1);
    uint16_t lctrl = rsp ? u16(rsp) : 0;
    feed(&h, HCI_EV_LINK_KEY_REQUEST, ADDR, 6);
    CHECK(last_cmd()->op == HCI_OP_LINK_KEY_REPLY && last_cmd()->n == 22 && last_cmd()->p[6] == 0x5A);
    cc(&h, HCI_OP_LINK_KEY_REPLY);
    uint8_t ac[3] = {0, 0x0C, 0};
    feed(&h, HCI_EV_AUTH_COMPLETE, ac, 3);
    cs(&h, HCI_OP_SET_CONN_ENCRYPT, 0);
    reset_capture();
    uint8_t ec[4] = {0, 0x0C, 0, 1};
    feed(&h, HCI_EV_ENCRYPT_CHANGE, ec, 4);
    rsp = host_sig(L2CAP_CONN_RSP, &id, NULL);
    CHECK(rsp && id == 0x21 && u16(rsp) == lctrl && u16(rsp + 4) == 0);      // now success
    CHECK(host_sig(L2CAP_CONN_REQ, NULL, NULL) == NULL);                    // not the initiator: we don't open
    const uint8_t *cq = host_sig(L2CAP_CONF_REQ, &id, NULL);
    CHECK(cq && u16(cq) == 0x0060);
    uint8_t crsp[6] = {(uint8_t)lctrl, (uint8_t)(lctrl >> 8), 0, 0, 0, 0};
    dev_sig(&h, 0x0C, L2CAP_CONF_RSP, id, crsp, 6);
    uint8_t dreq[4] = {(uint8_t)lctrl, (uint8_t)(lctrl >> 8), 0, 0};
    dev_sig(&h, 0x0C, L2CAP_CONF_REQ, 0x22, dreq, 4);
    // Interrupt channel after encryption: accepted at once
    uint8_t ireq[4] = {0x13, 0x00, 0x61, 0x00};
    dev_sig(&h, 0x0C, L2CAP_CONN_REQ, 0x23, ireq, 4);
    rsp = host_sig(L2CAP_CONN_RSP, &id, NULL);
    CHECK(rsp && id == 0x23 && u16(rsp + 4) == 0);
    uint16_t lintr = rsp ? u16(rsp) : 0;
    cq = host_sig(L2CAP_CONF_REQ, &id, NULL);
    uint8_t crsp2[6] = {(uint8_t)lintr, (uint8_t)(lintr >> 8), 0, 0, 0, 0};
    dev_sig(&h, 0x0C, L2CAP_CONF_RSP, id, crsp2, 6);
    uint8_t dreq2[4] = {(uint8_t)lintr, (uint8_t)(lintr >> 8), 0, 0};
    dev_sig(&h, 0x0C, L2CAP_CONF_REQ, 0x24, dreq2, 4);
    CHECK(bthost_state(&h, ADDR) == BTC_OPEN && had_event(BTH_EV_CONNECTED));
    // A second connection request for the same device while connected is rejected
    cc(&h, HCI_OP_WRITE_LINK_POLICY);
    feed(&h, HCI_EV_CONN_REQUEST, cr, 10);
    CHECK(last_cmd()->op == HCI_OP_REJECT_CONN_REQ);
    // Virtual cable unplug from the device: forget + disconnect
    forgot = 0;
    uint8_t un = HID_CONTROL_UNPLUG;
    dev_l2cap(&h, 0x0C, lctrl, &un, 1);
    CHECK(forgot == 1 && had_event(BTH_EV_UNPLUGGED));
    bthost_poll(&h, 10.0);
    bthost_poll(&h, 11.0);
    CHECK(sent(HCI_OP_DISCONNECT));
}

static void test_reject_and_failures(void) {
    BtHost h;
    bthost_init(&h, &kOps);
    reset_capture();
    accept_ok = 0;
    uint8_t cr[10] = {9, 9, 9, 9, 9, 9, 0x0C, 0x02, 0x5A, 0x01};
    feed(&h, HCI_EV_CONN_REQUEST, cr, 10);
    CHECK(last_cmd()->op == HCI_OP_REJECT_CONN_REQ && last_cmd()->p[6] == 0x0F);
    accept_ok = 1;
    cr[9] = 0x00;                                                  // SCO request: rejected
    cc(&h, HCI_OP_REJECT_CONN_REQ);
    feed(&h, HCI_EV_CONN_REQUEST, cr, 10);
    CHECK(last_cmd()->op == HCI_OP_REJECT_CONN_REQ);
    // Page timeout
    reset_capture();
    cc(&h, 0);
    CHECK(bthost_connect(&h, ADDR, 1, 0) == 0);
    cs(&h, HCI_OP_CREATE_CONN, 0);
    uint8_t ccmp[11] = {0x04, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01, 0x00};
    feed(&h, HCI_EV_CONN_COMPLETE, ccmp, 11);
    CHECK(bthost_state(&h, ADDR) == BTC_FREE && evs[nev - 1] == BTH_EV_FAILED && evval[nev - 1] == 0x04);
    // Command Status failure for Create Connection
    CHECK(bthost_connect(&h, ADDR, 1, 0) == 0);
    cs(&h, HCI_OP_CREATE_CONN, 0x0C);
    CHECK(bthost_state(&h, ADDR) == BTC_FREE && evs[nev - 1] == BTH_EV_FAILED && evval[nev - 1] == 0x0C);
    // The device lost its key: auth fails with 0x06 after we offered ours -> forgotten
    have_key = 1;
    forgot = 0;
    CHECK(bthost_connect(&h, ADDR, 1, 0) == 0);
    cs(&h, HCI_OP_CREATE_CONN, 0);
    ccmp[0] = 0;
    ccmp[1] = 0x0D;
    feed(&h, HCI_EV_CONN_COMPLETE, ccmp, 11);
    feed(&h, HCI_EV_LINK_KEY_REQUEST, ADDR, 6);
    uint8_t ac[3] = {0x06, 0x0D, 0};
    feed(&h, HCI_EV_AUTH_COMPLETE, ac, 3);
    CHECK(forgot == 1 && evs[nev - 1] == BTH_EV_FAILED);
    // Cancel while paging
    BtHost h2;
    bthost_init(&h2, &kOps);
    reset_capture();
    CHECK(bthost_connect(&h2, ADDR, 0, 0) == 0);
    cs(&h2, HCI_OP_CREATE_CONN, 0);
    CHECK(bthost_disconnect(&h2, ADDR) == 0 && last_cmd()->op == HCI_OP_CREATE_CONN_CANCEL);
    uint8_t cancelled[11] = {0x02, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01, 0};
    feed(&h2, HCI_EV_CONN_COMPLETE, cancelled, 11);
    CHECK(bthost_state(&h2, ADDR) == BTC_FREE);
}

static void test_cmd_flow(void) {
    BtHost h;
    bthost_init(&h, &kOps);
    reset_capture();
    uint8_t x = 0;
    for (int i = 0; i < 5; i++) bthost_cmd(&h, HCI_OP_WRITE_SCAN_ENABLE, &x, 1);
    CHECK(ncmds == 1);                                   // one slot until the controller says more
    cc(&h, HCI_OP_WRITE_SCAN_ENABLE);
    CHECK(ncmds == 2);
    bthost_poll(&h, 100.0);
    bthost_poll(&h, 100.2);
    CHECK(ncmds == 2);
    bthost_poll(&h, 100.7);                              // nothing heard for > 0.5 s: one more
    CHECK(ncmds == 3);
    uint8_t ccn[4] = {5, 0x1A, 0x0C, 0};                // 5 slots
    feed(&h, HCI_EV_CMD_COMPLETE, ccn, 4);
    CHECK(ncmds == 5);
    for (int i = 0; i < BTH_CMDQ + 5; i++) bthost_cmd(&h, HCI_OP_WRITE_SCAN_ENABLE, &x, 1);
    CHECK(h.dropped_cmds > 0);
}

static void test_hidboot(void) {
    HidKbState st;
    memset(&st, 0, sizeof st);
    HidKeyEvent ev[32];
    uint8_t r1[8] = {0x02, 0, 0x04, 0, 0, 0, 0, 0};      // LShift + a
    CHECK(hid_kb_diff(&st, r1, 8, ev) == 2 && ev[0].usage == 0xE1 && ev[0].down && ev[1].usage == 0x04 && ev[1].down);
    uint8_t r2[8] = {0x02, 0, 0x04, 0x05, 0, 0, 0, 0};   // + b
    CHECK(hid_kb_diff(&st, r2, 8, ev) == 1 && ev[0].usage == 0x05 && ev[0].down);
    uint8_t r3[8] = {0x00, 0, 0x05, 0, 0, 0, 0, 0};      // release shift and a
    int n = hid_kb_diff(&st, r3, 8, ev);
    CHECK(n == 2 && ev[0].usage == 0xE1 && !ev[0].down && ev[1].usage == 0x04 && !ev[1].down);
    uint8_t roll[8] = {0, 0, 1, 1, 1, 1, 1, 1};
    CHECK(hid_kb_diff(&st, roll, 8, ev) == -1 && st.keys[0] == 0x05);
    uint8_t r4[8] = {0};
    CHECK(hid_kb_diff(&st, r4, 8, ev) == 1 && ev[0].usage == 0x05 && !ev[0].down);
    CHECK(hid_kb_diff(&st, r4, 7, ev) == -1);
    uint8_t dup[8] = {0, 0, 0x06, 0x06, 0, 0, 0, 0};
    CHECK(hid_kb_diff(&st, dup, 8, ev) == 1);
    HidMouse m;
    uint8_t mr[4] = {0x01, 0xFE, 0x05, 0xFF};
    CHECK(hid_mouse_parse(mr, 4, &m) == 0 && m.buttons == 1 && m.dx == -2 && m.dy == 5 && m.wheel == -1);
    CHECK(hid_mouse_parse(mr, 3, &m) == 0 && m.wheel == 0);
    CHECK(hid_mouse_parse(mr, 2, &m) == -1);
    CHECK(hid_usage_to_mac(0x04) == 0x00 && hid_usage_to_mac(0x1D) == 0x06 && hid_usage_to_mac(0x27) == 0x1D);
    CHECK(hid_usage_to_mac(0x28) == 0x24 && hid_usage_to_mac(0x2C) == 0x31 && hid_usage_to_mac(0x52) == 0x7E);
    CHECK(hid_usage_to_mac(0xE3) == 0x37 && hid_usage_to_mac(0xE1) == 0x38 && hid_usage_to_mac(0x3A) == 0x7A);
    CHECK(hid_usage_to_mac(0x00) == -1 && hid_usage_to_mac(0x65) == -1 && hid_usage_to_mac(0xF0) == -1);
    // every mapped usage gives a distinct code within the 7-bit range (except the two \ variants)
    int seen[128] = {0}, dups = 0;
    for (int u = 0; u < 256; u++) {
        int k = hid_usage_to_mac((uint8_t)u);
        if (k < 0) continue;
        CHECK(k < 128);
        if (seen[k]++ && u != 0x32 && u != 0x68 && u != 0x69 && u != 0x6A) dups++;
    }
    CHECK(dups == 0);
}

static void test_fuzz(void) {
    srand(4321);
    BtHost h;
    for (int round = 0; round < 3000; round++) {
        bthost_init(&h, &kOps);
        reset_capture();
        have_key = rand() & 1;
        for (int step = 0; step < 60; step++) {
            uint8_t buf[2 + 255];
            int r = rand() % 4;
            if (r == 0) bthost_connect(&h, ADDR, (uint8_t)rand(), (uint16_t)rand());
            int len = rand() % 40;
            for (int i = 0; i < len; i++) buf[2 + i] = (uint8_t)rand();
            if (r == 1 || r == 2) {
                static const uint8_t codes[] = {0x03, 0x04, 0x05, 0x06, 0x08, 0x0E, 0x0F, 0x16, 0x17, 0x18,
                                                0x31, 0x33, 0x36, 0x3B};
                buf[0] = codes[rand() % (int)sizeof codes];
                buf[1] = (uint8_t)len;
                if (len >= 9 && (rand() & 1)) memcpy(buf + 2 + 3, ADDR, 6);   // sometimes about our device
                if (len >= 6 && (rand() & 1)) memcpy(buf + 2, ADDR, 6);
                HciEvent ev;
                if (hci_event(buf, (size_t)len + 2, &ev) == 0) bthost_event(&h, &ev);
            } else if (r == 3) {
                uint8_t pkt[64];
                int n = 4 + rand() % 60;
                for (int i = 0; i < n; i++) pkt[i] = (uint8_t)rand();
                if (rand() & 1) { pkt[0] = h.c[0].handle & 0xFF; pkt[1] = (uint8_t)((pkt[1] & 0xF0) | (h.c[0].handle >> 8)); }
                if (rand() & 1) { pkt[2] = (uint8_t)(n - 4); pkt[3] = 0; }
                if (n >= 8 && (rand() & 1)) { pkt[4] = (uint8_t)(n - 8); pkt[5] = 0; pkt[6] = 1; pkt[7] = 0; }
                bthost_acl(&h, pkt, (size_t)n);
            }
            bthost_poll(&h, step * 0.3);
        }
    }
    CHECK(1);   // ran under ASan/UBSan without faults
}

int main(void) {
    test_pair_keyboard();
    test_reconnect_incoming();
    test_reject_and_failures();
    test_cmd_flow();
    test_hidboot();
    test_fuzz();
    printf("bthost: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
