// bthost.h — a small Bluetooth Classic host: ACL connections (several at once), Secure Simple Pairing (the
// controller does the cryptography; the host answers its questions and keeps the link keys), L2CAP basic-mode
// channels and the HID host role (keyboards, mice) over PSM 0x11/0x13. Original code (GPL-2.0); protocol facts from
// the Bluetooth Core Specification v5.4 (Vol 4 Part E: HCI; Vol 3 Part A: L2CAP) and the HID Profile v1.1.1.
//
// No I/O here: commands and ACL packets leave through callbacks, HCI events and ACL packets come in through
// bthost_event / bthost_acl. Not thread-safe: call everything from one thread (macwifid's Bluetooth thread).

#ifndef BTHOST_H
#define BTHOST_H

#include <stddef.h>
#include <stdint.h>

#include "hci.h"

#define BTH_MAX_CONN 7          // a BR/EDR piconet: up to 7 active peripherals

// HCI commands used [Vol 4 Part E 7.1 / 7.3]
#define HCI_OP_CREATE_CONN             HCI_OP(0x01, 0x0005)
#define HCI_OP_DISCONNECT              HCI_OP(0x01, 0x0006)
#define HCI_OP_CREATE_CONN_CANCEL      HCI_OP(0x01, 0x0008)
#define HCI_OP_ACCEPT_CONN_REQ         HCI_OP(0x01, 0x0009)
#define HCI_OP_REJECT_CONN_REQ         HCI_OP(0x01, 0x000A)
#define HCI_OP_LINK_KEY_REPLY          HCI_OP(0x01, 0x000B)
#define HCI_OP_LINK_KEY_NEG_REPLY      HCI_OP(0x01, 0x000C)
#define HCI_OP_PIN_CODE_REPLY          HCI_OP(0x01, 0x000D)
#define HCI_OP_AUTH_REQUESTED          HCI_OP(0x01, 0x0011)
#define HCI_OP_SET_CONN_ENCRYPT        HCI_OP(0x01, 0x0013)
#define HCI_OP_IO_CAPABILITY_REPLY     HCI_OP(0x01, 0x002B)
#define HCI_OP_USER_CONFIRM_REPLY      HCI_OP(0x01, 0x002C)
#define HCI_OP_EXIT_SNIFF_MODE         HCI_OP(0x02, 0x0004)   // 7.2.3
#define HCI_OP_WRITE_LINK_POLICY       HCI_OP(0x02, 0x000D)   // 7.2.10
#define HCI_OP_WRITE_LOCAL_NAME        HCI_OP(0x03, 0x0013)
#define HCI_OP_WRITE_SCAN_ENABLE       HCI_OP(0x03, 0x001A)
#define HCI_OP_WRITE_CLASS_OF_DEVICE   HCI_OP(0x03, 0x0024)
#define HCI_OP_WRITE_SSP_MODE          HCI_OP(0x03, 0x0056)

// Events handled [7.7]
#define HCI_EV_CONN_COMPLETE           0x03
#define HCI_EV_CONN_REQUEST            0x04
#define HCI_EV_DISCONN_COMPLETE        0x05
#define HCI_EV_AUTH_COMPLETE           0x06
#define HCI_EV_ENCRYPT_CHANGE          0x08
#define HCI_EV_NUM_COMPLETED_PKTS      0x13
#define HCI_EV_MODE_CHANGE             0x14
#define HCI_EV_PIN_CODE_REQUEST        0x16
#define HCI_EV_LINK_KEY_REQUEST        0x17
#define HCI_EV_LINK_KEY_NOTIFY         0x18
#define HCI_EV_IO_CAPABILITY_REQUEST   0x31
#define HCI_EV_IO_CAPABILITY_RESPONSE  0x32
#define HCI_EV_USER_CONFIRM_REQUEST    0x33
#define HCI_EV_SIMPLE_PAIRING_COMPLETE 0x36
#define HCI_EV_USER_PASSKEY_NOTIFY     0x3B

// L2CAP [Vol 3 Part A 4]
#define L2CAP_CID_SIGNALING   0x0001
#define L2CAP_PSM_SDP         0x0001
#define L2CAP_PSM_HID_CTRL    0x0011
#define L2CAP_PSM_HID_INTR    0x0013
#define L2CAP_PSM_AVCTP       0x0017
#define L2CAP_PSM_AVDTP       0x0019
#define L2CAP_DEFAULT_MTU     672
#define L2CAP_CMD_REJECT      0x01
#define L2CAP_CONN_REQ        0x02
#define L2CAP_CONN_RSP        0x03
#define L2CAP_CONF_REQ        0x04
#define L2CAP_CONF_RSP        0x05
#define L2CAP_DISCONN_REQ     0x06
#define L2CAP_DISCONN_RSP     0x07
#define L2CAP_ECHO_REQ        0x08
#define L2CAP_ECHO_RSP        0x09
#define L2CAP_INFO_REQ        0x0A
#define L2CAP_INFO_RSP        0x0B
#define L2CAP_FIRST_DYN_CID   0x0040

// HID transactions [HID Profile 7.4]: high nibble = type, low nibble = parameter
#define HID_HANDSHAKE         0x00
#define HID_CONTROL_UNPLUG    0x15   // HID_CONTROL, VIRTUAL_CABLE_UNPLUG
#define HID_SET_PROTOCOL_BOOT 0x70   // SET_PROTOCOL, boot protocol
#define HID_DATA_INPUT        0xA1   // DATA, input report

typedef enum {
    BTC_FREE = 0,
    BTC_PAGING,        // Create Connection sent
    BTC_SECURING,      // ACL up; authentication / pairing / encryption running
    BTC_CHANNELS,      // encrypted; opening the HID channels
    BTC_OPEN,          // HID control + interrupt channels open: input flows
    BTC_CLOSING
} BtConnState;

typedef enum { CH_CLOSED = 0, CH_WAIT_CONN_RSP, CH_WAIT_SECURITY, CH_CONFIG, CH_OPEN } BtChanState;

typedef struct {
    uint16_t psm, lcid, rcid;
    uint8_t state;           // BtChanState
    uint8_t cfg_out_ok;      // our Configuration Request was accepted
    uint8_t cfg_in_ok;       // we accepted theirs
    uint8_t rsp_id;          // incoming connection answered "pending": the request's identifier
    uint16_t rmtu;           // the largest SDU the other side accepts (its MTU option; 672 by default)
} BtChan;

#define BTH_EXT_CHANS 4      // channels for other profiles (SDP, AVDTP signaling + media, AVCTP)

typedef struct {
    uint8_t state;           // BtConnState
    uint8_t addr[6];
    uint16_t handle;
    uint8_t initiator, encrypted, have_key_rq, protocol_sent;
    uint32_t cod;
    uint16_t first_psm;      // as initiator: the profile channel opened once encrypted (HID control or AVDTP)
    BtChan ctrl, intr;
    BtChan ext[BTH_EXT_CHANS];
    uint8_t rx[1024];        // L2CAP reassembly
    size_t rx_have, rx_need;
} BtConn;

// Events reported to the owner.
typedef enum {
    BTH_EV_CONNECTING = 1,   // paging / incoming connection accepted
    BTH_EV_PAIRING,          // pairing started (no stored key)
    BTH_EV_PASSKEY,          // show `value` (6 digits): type it on the keyboard, then Return
    BTH_EV_CONNECTED,        // HID channels open
    BTH_EV_DISCONNECTED,     // `value` = HCI reason / status
    BTH_EV_FAILED,           // couldn't connect or pair; `value` = HCI status
    BTH_EV_UNPLUGGED,        // the device asked to be forgotten (virtual cable unplug)
    BTH_EV_MODE              // link mode changed: value = mode (0 active, 2 sniff) | interval in 0.625 ms slots << 8
} BtHostEvent;

typedef struct {
    void *ctx;
    int (*send_cmd)(void *ctx, uint16_t opcode, const uint8_t *params, size_t plen);
    int (*send_acl)(void *ctx, const uint8_t *pkt, size_t n);
    // Link keys (16 bytes). load returns 0 when found.
    int (*key_load)(void *ctx, const uint8_t addr[6], uint8_t key[16]);
    void (*key_store)(void *ctx, const uint8_t addr[6], const uint8_t key[16], uint8_t key_type);
    void (*key_forget)(void *ctx, const uint8_t addr[6]);
    // Whether to accept an incoming connection (paired devices reconnecting).
    int (*accept)(void *ctx, const uint8_t addr[6], uint32_t cod);
    void (*event)(void *ctx, const uint8_t addr[6], BtHostEvent ev, uint32_t value);
    // An input report from the HID interrupt channel (report ID first, then the data).
    void (*hid_input)(void *ctx, const uint8_t addr[6], const uint8_t *report, size_t n);
    // Other profiles. accept: whether to take an incoming channel to `psm` (1 = yes). open / data / closed: a
    // channel `cid` (our channel id) to `psm` became usable, carried data, went away.
    int (*l2cap_accept)(void *ctx, const uint8_t addr[6], uint16_t psm);
    void (*l2cap_open)(void *ctx, const uint8_t addr[6], uint16_t psm, uint16_t cid);
    void (*l2cap_data)(void *ctx, const uint8_t addr[6], uint16_t psm, uint16_t cid, const uint8_t *d, size_t n);
    void (*l2cap_closed)(void *ctx, const uint8_t addr[6], uint16_t psm, uint16_t cid);
} BtHostOps;

#define BTH_CMDQ 24
typedef struct {
    BtHostOps ops;
    BtConn c[BTH_MAX_CONN];
    uint8_t sig_id;
    uint16_t next_cid;
    // HCI command flow control: commands wait here until the controller has room (Num_HCI_Command_Packets).
    struct { uint16_t op; uint8_t len; uint8_t p[255]; } q[BTH_CMDQ];
    int qh, qn, credits;
    double last_send;
    unsigned long dropped_cmds;
    // ACL data flow control (Read Buffer Size, Number Of Completed Packets)
    uint16_t acl_mtu;
    int acl_total, acl_credits;
} BtHost;

void bthost_init(BtHost *h, const BtHostOps *ops);
// Forgets every connection (after an HCI Reset or when the controller is gone). Reports nothing.
void bthost_reset(BtHost *h);

// Feed every HCI event that isn't the answer to a bt_call, and every ACL packet.
void bthost_event(BtHost *h, const HciEvent *ev);
void bthost_acl(BtHost *h, const uint8_t *pkt, size_t n);
// Call now and then (e.g. every 200 ms) with the current time in seconds: sends queued commands if the controller
// hasn't reported free command slots for a while.
void bthost_poll(BtHost *h, double now);

// Connects to a classic device (from an inquiry result: page scan mode and clock offset speed up paging) and,
// once the link is encrypted, opens the HID channels. 0, or -1 if already connecting/connected or no free slot.
int bthost_connect(BtHost *h, const uint8_t addr[6], uint8_t psrm, uint16_t clock_offset);
// The same for another profile: opens a channel to `first_psm` (e.g. AVDTP) when encrypted.
int bthost_connect_profile(BtHost *h, const uint8_t addr[6], uint8_t psrm, uint16_t clock_offset, uint16_t first_psm);

// The controller's ACL buffers (Read Buffer Size): fragment size and how many packets it can hold.
void bthost_set_acl(BtHost *h, uint16_t mtu, int count);
// Other-profile channels on an encrypted connection. connect returns our channel id (the open callback follows), or
// -1. send: 0, -1 on error (no such channel, bigger than the other side's MTU), -2 when the controller's buffers
// are full (try later; audio drops the packet). close: 0 or -1.
int bthost_l2cap_connect(BtHost *h, const uint8_t addr[6], uint16_t psm);
int bthost_l2cap_send(BtHost *h, const uint8_t addr[6], uint16_t cid, const uint8_t *d, size_t n);
int bthost_l2cap_close(BtHost *h, const uint8_t addr[6], uint16_t cid);
// The other side's MTU for a channel (0 if none).
uint16_t bthost_l2cap_mtu(const BtHost *h, const uint8_t addr[6], uint16_t cid);
int bthost_disconnect(BtHost *h, const uint8_t addr[6]);
void bthost_disconnect_all(BtHost *h);
// State of the connection to addr (BTC_FREE if none).
BtConnState bthost_state(const BtHost *h, const uint8_t addr[6]);

// Queues an HCI command (flow-controlled). 0, or -1 if the queue is full.
int bthost_cmd(BtHost *h, uint16_t opcode, const void *params, size_t plen);

#endif
