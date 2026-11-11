// hci.h — Bluetooth HCI: command packets, event parsing, LE advertising reports, inquiry results and EIR/AD
// data. Original code (GPL-2.0 like the rest of the repo); every constant comes from the Bluetooth Core
// Specification (v5.4), section cited next to it. No USB here: bt_usb.c moves the bytes.

#ifndef HCI_H
#define HCI_H

#include <stddef.h>
#include <stdint.h>

// Opcodes = (OGF << 10) | OCF  [Vol 4 Part E 5.4.1]
#define HCI_OP(ogf, ocf)               ((uint16_t)(((ogf) << 10) | (ocf)))
#define HCI_OP_INQUIRY                 HCI_OP(0x01, 0x0001)   // 7.1.1
#define HCI_OP_INQUIRY_CANCEL          HCI_OP(0x01, 0x0002)   // 7.1.2
#define HCI_OP_SET_EVENT_MASK          HCI_OP(0x03, 0x0001)   // 7.3.1
#define HCI_OP_RESET                   HCI_OP(0x03, 0x0003)   // 7.3.2
#define HCI_OP_READ_LOCAL_NAME         HCI_OP(0x03, 0x0014)   // 7.3.12
#define HCI_OP_WRITE_INQUIRY_MODE      HCI_OP(0x03, 0x0045)   // 7.3.50
#define HCI_OP_READ_LOCAL_VERSION      HCI_OP(0x04, 0x0001)   // 7.4.1
#define HCI_OP_READ_LOCAL_FEATURES     HCI_OP(0x04, 0x0003)   // 7.4.3
#define HCI_OP_READ_BUFFER_SIZE        HCI_OP(0x04, 0x0005)   // 7.4.5
#define HCI_OP_READ_BD_ADDR            HCI_OP(0x04, 0x0009)   // 7.4.6
#define HCI_OP_LE_SET_EVENT_MASK       HCI_OP(0x08, 0x0001)   // 7.8.1
#define HCI_OP_LE_READ_BUFFER_SIZE     HCI_OP(0x08, 0x0002)   // 7.8.2
#define HCI_OP_LE_SET_SCAN_PARAMS      HCI_OP(0x08, 0x000B)   // 7.8.10
#define HCI_OP_LE_SET_SCAN_ENABLE      HCI_OP(0x08, 0x000C)   // 7.8.11

// Event codes [Vol 4 Part E 7.7]
#define HCI_EV_INQUIRY_COMPLETE        0x01
#define HCI_EV_INQUIRY_RESULT          0x02
#define HCI_EV_CMD_COMPLETE            0x0E
#define HCI_EV_CMD_STATUS              0x0F
#define HCI_EV_HW_ERROR                0x10
#define HCI_EV_INQUIRY_RESULT_RSSI     0x22
#define HCI_EV_EXT_INQUIRY_RESULT      0x2F
#define HCI_EV_LE_META                 0x3E
#define HCI_EV_VENDOR                  0xFF
#define HCI_LE_ADV_REPORT              0x02   // LE Meta subevent 7.7.65.2
#define HCI_LE_EXT_ADV_REPORT          0x0D   // 7.7.65.13

// Set_Event_Mask bits [7.3.1]: the default (bits 0-44) plus Inquiry Result with RSSI (33, already in the
// default), LE Meta (61) and Extended Inquiry Result (62).
#define HCI_EVENT_MASK_SCAN            0x60001FFFFFFFFFFFull
// LE_Set_Event_Mask [7.8.1]: the default 0x1F (connection complete, advertising report, ...).
#define HCI_LE_EVENT_MASK_DEFAULT      0x000000000000001Full
#define HCI_GIAC_LAP                   0x9E8B33u   // General inquiry access code [Assigned Numbers 2.2]

// HCI over USB [Vol 4 Part B 2.2]: commands use a class control request to the interface.
#define HCI_USB_CMD_REQTYPE            0x20
#define HCI_USB_CMD_REQUEST            0x00

#define HCI_MAX_EVENT                  (2 + 255)
#define HCI_NAME_MAX                   248

// Builds a command packet (opcode LE16, length, params). Returns its length, or 0 if cap is too small or
// plen > 255.
size_t hci_cmd(uint8_t *out, size_t cap, uint16_t opcode, const void *params, size_t plen);

typedef struct {
    uint8_t code;
    uint8_t len;
    const uint8_t *p;           // len bytes of parameters (points into the caller's buffer)
} HciEvent;

// Splits an event packet. Returns 0, or -1 if it's truncated or its length byte disagrees with n.
int hci_event(const uint8_t *buf, size_t n, HciEvent *ev);

// Command Complete / Command Status. ret points at the return parameters after the status byte.
typedef struct {
    uint8_t ncmd;
    uint16_t opcode;
    uint8_t status;
    const uint8_t *ret;
    size_t ret_len;
} HciCmdDone;
int hci_cmd_complete(const HciEvent *ev, HciCmdDone *out);   // -1 unless a well-formed Command Complete
int hci_cmd_status(const HciEvent *ev, HciCmdDone *out);     // -1 unless a well-formed Command Status

typedef struct {
    uint8_t hci_ver, lmp_ver;
    uint16_t hci_rev, manufacturer, lmp_subver;
} HciVersion;
int hci_parse_version(const HciCmdDone *d, HciVersion *v);   // Read_Local_Version_Information return
const char *hci_version_name(uint8_t ver);                   // "5.3" etc. [Assigned Numbers 2.1]

// One device seen during a scan.
#define HCI_DEV_LE      1
#define HCI_DEV_CLASSIC 2
typedef struct {
    uint8_t kind;               // HCI_DEV_LE / HCI_DEV_CLASSIC
    uint8_t addr[6];            // little-endian as on the wire; print with hci_addr_str
    uint8_t addr_type;          // LE: 0 public, 1 random (2/3 resolved); classic: 0
    int8_t rssi;                // 127 = unknown
    uint8_t connectable;        // LE: from the event type; classic: 1
    uint32_t cod;               // classic class of device (0 for LE)
    uint16_t appearance;        // AD type 0x19, 0 if none
    uint16_t company;           // first manufacturer-specific AD (0xFF), 0xFFFF if none
    int has_company;
    char name[HCI_NAME_MAX + 1]; // complete or shortened local name, "" if none
    int name_complete;
} HciDevice;

// Fills name/appearance/company from AD or EIR data [Core Supplement Part A]: a sequence of
// (length, type, data). Stops quietly at the first malformed field. Doesn't touch other fields.
void hci_parse_ad(const uint8_t *ad, size_t n, HciDevice *d);

// Calls fn once per device in an LE Advertising Report, LE Extended Advertising Report, Inquiry Result,
// Inquiry Result with RSSI or Extended Inquiry Result event. Returns the number of devices, or -1 if the event
// is one of those but malformed (devices before the bad one have already been reported), or 0 for other events.
typedef void (*HciDeviceFn)(void *ctx, const HciDevice *d);
int hci_scan_event(const HciEvent *ev, HciDeviceFn fn, void *ctx);

// "AA:BB:CC:DD:EE:FF" (most significant byte first, the usual way to show it). out needs 18 bytes.
void hci_addr_str(const uint8_t addr[6], char out[18]);

// A small description of a class of device / appearance for listings ("Audio", "Phone", "Keyboard"...),
// or NULL when unknown.
const char *hci_cod_kind(uint32_t cod);          // major device class [Assigned Numbers 2.8]
const char *hci_appearance_kind(uint16_t app);   // appearance category [Assigned Numbers 2.6]

// A deduplicated list of scan results: a later report for the same (kind, address) updates RSSI and fills in a
// name/appearance/company that was missing.
#define HCI_SEEN_MAX 128
typedef struct {
    HciDevice dev[HCI_SEEN_MAX];
    int n;
    int overflow;
} HciSeen;
void hci_seen_add(HciSeen *s, const HciDevice *d);
// The merge hci_seen_add applies when d is another report from the device already in e.
void hci_merge(HciDevice *e, const HciDevice *d);

#endif
