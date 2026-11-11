// hci.c — Bluetooth HCI packets and scan results. Original code (GPL-2.0). See hci.h for spec references.

#include "hci.h"

#include <stdio.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le24(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }

size_t hci_cmd(uint8_t *out, size_t cap, uint16_t opcode, const void *params, size_t plen) {
    if (plen > 255 || cap < 3 + plen) return 0;
    out[0] = (uint8_t)opcode;
    out[1] = (uint8_t)(opcode >> 8);
    out[2] = (uint8_t)plen;
    if (plen) memcpy(out + 3, params, plen);
    return 3 + plen;
}

int hci_event(const uint8_t *buf, size_t n, HciEvent *ev) {
    if (n < 2 || (size_t)buf[1] + 2 != n) return -1;
    ev->code = buf[0];
    ev->len = buf[1];
    ev->p = buf + 2;
    return 0;
}

int hci_cmd_complete(const HciEvent *ev, HciCmdDone *out) {
    if (ev->code != HCI_EV_CMD_COMPLETE || ev->len < 3) return -1;
    out->ncmd = ev->p[0];
    out->opcode = le16(ev->p + 1);
    if (ev->len >= 4) {
        out->status = ev->p[3];
        out->ret = ev->p + 4;
        out->ret_len = (size_t)ev->len - 4;
    } else {                                // e.g. the NOP opcode 0 announcing free command slots
        out->status = 0;
        out->ret = ev->p + 3;
        out->ret_len = 0;
    }
    return 0;
}

int hci_cmd_status(const HciEvent *ev, HciCmdDone *out) {
    if (ev->code != HCI_EV_CMD_STATUS || ev->len != 4) return -1;
    out->status = ev->p[0];
    out->ncmd = ev->p[1];
    out->opcode = le16(ev->p + 2);
    out->ret = NULL;
    out->ret_len = 0;
    return 0;
}

int hci_parse_version(const HciCmdDone *d, HciVersion *v) {
    if (d->opcode != HCI_OP_READ_LOCAL_VERSION || d->status != 0 || d->ret_len < 8) return -1;
    v->hci_ver = d->ret[0];
    v->hci_rev = le16(d->ret + 1);
    v->lmp_ver = d->ret[3];
    v->manufacturer = le16(d->ret + 4);
    v->lmp_subver = le16(d->ret + 6);
    return 0;
}

const char *hci_version_name(uint8_t ver) {
    static const char *names[] = {"1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1",
                                  "4.2",  "5.0", "5.1", "5.2", "5.3", "5.4", "6.0"};
    return ver < sizeof names / sizeof names[0] ? names[ver] : "?";
}

static void set_name(HciDevice *d, const uint8_t *p, size_t n, int complete) {
    if (d->name[0] && d->name_complete && !complete) return;      // keep a complete name over a shortened one
    while (n && p[n - 1] == 0) n--;
    if (!n) return;
    if (n > HCI_NAME_MAX) n = HCI_NAME_MAX;
    for (size_t i = 0; i < n; i++) d->name[i] = (p[i] < 0x20 || p[i] == 0x7F) ? '?' : (char)p[i];   // terminal-safe
    d->name[n] = 0;
    d->name_complete = complete;
}

void hci_parse_ad(const uint8_t *ad, size_t n, HciDevice *d) {
    size_t i = 0;
    while (i < n) {
        uint8_t flen = ad[i];
        if (flen == 0) break;                                        // early terminator / padding
        if (i + 1 + flen > n) break;                                 // malformed: stop
        uint8_t type = ad[i + 1];
        const uint8_t *v = ad + i + 2;
        size_t vlen = (size_t)flen - 1;
        switch (type) {
        case 0x08: set_name(d, v, vlen, 0); break;                  // Shortened Local Name
        case 0x09: set_name(d, v, vlen, 1); break;                  // Complete Local Name
        case 0x19: if (vlen >= 2) d->appearance = le16(v); break;   // Appearance
        case 0xFF:                                                   // Manufacturer Specific Data
            if (vlen >= 2 && !d->has_company) { d->company = le16(v); d->has_company = 1; }
            break;
        default: break;
        }
        i += 1 + (size_t)flen;
    }
}

static void dev_init(HciDevice *d, uint8_t kind) {
    memset(d, 0, sizeof *d);
    d->kind = kind;
    d->rssi = 127;
    d->company = 0xFFFF;
}

static int le_adv_report(const uint8_t *p, size_t n, HciDeviceFn fn, void *ctx) {
    if (n < 1) return -1;
    int num = p[0], got = 0;
    size_t o = 1;
    for (int r = 0; r < num; r++) {
        if (o + 9 > n) return -1;                                  // type, addr type, addr, data length
        HciDevice d;
        dev_init(&d, HCI_DEV_LE);
        uint8_t etype = p[o];
        d.connectable = etype == 0x00 || etype == 0x01;              // ADV_IND, ADV_DIRECT_IND
        d.addr_type = p[o + 1];
        memcpy(d.addr, p + o + 2, 6);
        size_t dlen = p[o + 8];
        if (dlen > 31 || o + 9 + dlen + 1 > n) return -1;          // legacy advertising data is at most 31 bytes
        hci_parse_ad(p + o + 9, dlen, &d);
        d.rssi = (int8_t)p[o + 9 + dlen];
        o += 9 + dlen + 1;
        fn(ctx, &d);
        got++;
    }
    return got;
}

static int le_ext_adv_report(const uint8_t *p, size_t n, HciDeviceFn fn, void *ctx) {
    if (n < 1) return -1;
    int num = p[0], got = 0;
    size_t o = 1;
    for (int r = 0; r < num; r++) {
        if (o + 24 > n) return -1;
        HciDevice d;
        dev_init(&d, HCI_DEV_LE);
        uint16_t etype = le16(p + o);
        d.connectable = etype & 1;
        d.addr_type = p[o + 2];
        memcpy(d.addr, p + o + 3, 6);
        d.rssi = (int8_t)p[o + 13];
        size_t dlen = p[o + 23];
        if (o + 24 + dlen > n) return -1;
        hci_parse_ad(p + o + 24, dlen, &d);
        o += 24 + dlen;
        if (d.addr_type == 0xFF) continue;                         // anonymous advertisement: nothing to list
        fn(ctx, &d);
        got++;
    }
    return got;
}

// Inquiry results: per device addr(6), page scan repetition mode(1), then either 2 reserved bytes (Inquiry
// Result) or 1 (with RSSI), class of device(3), clock offset(2), [RSSI(1)].
static int inquiry(const uint8_t *p, size_t n, int with_rssi, HciDeviceFn fn, void *ctx) {
    if (n < 1) return -1;
    int num = p[0];
    size_t rec = 14;
    if (with_rssi && num && n - 1 == (size_t)num * 15) rec = 15;    // some controllers keep the old pscan_mode byte
    if (n - 1 < (size_t)num * rec) return -1;
    for (int r = 0; r < num; r++) {
        const uint8_t *q = p + 1 + (size_t)r * rec;
        HciDevice d;
        dev_init(&d, HCI_DEV_CLASSIC);
        d.connectable = 1;
        memcpy(d.addr, q, 6);
        size_t cod_at = with_rssi ? (rec == 15 ? 9 : 8) : 9;
        d.cod = le24(q + cod_at);
        if (with_rssi) d.rssi = (int8_t)q[rec - 1];
        fn(ctx, &d);
    }
    return num;
}

static int ext_inquiry(const uint8_t *p, size_t n, HciDeviceFn fn, void *ctx) {
    if (n < 15 || p[0] != 1) return -1;                             // always exactly one response [7.7.38]
    HciDevice d;
    dev_init(&d, HCI_DEV_CLASSIC);
    d.connectable = 1;
    memcpy(d.addr, p + 1, 6);
    d.cod = le24(p + 9);
    d.rssi = (int8_t)p[14];
    hci_parse_ad(p + 15, n - 15, &d);                               // EIR uses the same field format as AD
    fn(ctx, &d);
    return 1;
}

int hci_scan_event(const HciEvent *ev, HciDeviceFn fn, void *ctx) {
    switch (ev->code) {
    case HCI_EV_INQUIRY_RESULT: return inquiry(ev->p, ev->len, 0, fn, ctx);
    case HCI_EV_INQUIRY_RESULT_RSSI: return inquiry(ev->p, ev->len, 1, fn, ctx);
    case HCI_EV_EXT_INQUIRY_RESULT: return ext_inquiry(ev->p, ev->len, fn, ctx);
    case HCI_EV_LE_META:
        if (ev->len < 1) return -1;
        if (ev->p[0] == HCI_LE_ADV_REPORT) return le_adv_report(ev->p + 1, (size_t)ev->len - 1, fn, ctx);
        if (ev->p[0] == HCI_LE_EXT_ADV_REPORT) return le_ext_adv_report(ev->p + 1, (size_t)ev->len - 1, fn, ctx);
        return 0;
    default: return 0;
    }
}

void hci_addr_str(const uint8_t a[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

const char *hci_cod_kind(uint32_t cod) {
    switch ((cod >> 8) & 0x1F) {
    case 1: return "Computer";
    case 2: return "Phone";
    case 3: return "Network";
    case 4: return "Audio";
    case 5:
        switch ((cod >> 6) & 3) {
        case 1: return "Keyboard";
        case 2: return "Mouse";
        case 3: return "Keyboard+mouse";
        default: return "Input";
        }
    case 6: return "Imaging";
    case 7: return "Wearable";
    case 8: return "Toy";
    case 9: return "Health";
    default: return NULL;
    }
}

const char *hci_appearance_kind(uint16_t app) {
    if (app == 0x03C1) return "Keyboard";
    if (app == 0x03C2) return "Mouse";
    if (app == 0x03C3) return "Joystick";
    if (app == 0x03C4) return "Gamepad";
    switch (app >> 6) {
    case 0x01: return "Phone";
    case 0x02: return "Computer";
    case 0x03: return "Watch";
    case 0x05: return "Display";
    case 0x06: return "Remote";
    case 0x08: return "Tag";
    case 0x0A: return "Media player";
    case 0x0D: return "Heart rate";
    case 0x0F: return "Input";
    case 0x25: return "Audio";
    default: return NULL;
    }
}

void hci_seen_add(HciSeen *s, const HciDevice *d) {
    for (int i = 0; i < s->n; i++) {
        HciDevice *e = &s->dev[i];
        if (e->kind != d->kind || memcmp(e->addr, d->addr, 6) || (d->kind == HCI_DEV_LE && e->addr_type != d->addr_type))
            continue;
        if (d->rssi != 127) e->rssi = d->rssi;
        if (d->name[0] && (!e->name[0] || (d->name_complete && !e->name_complete))) {
            memcpy(e->name, d->name, sizeof e->name);
            e->name_complete = d->name_complete;
        }
        if (!e->appearance) e->appearance = d->appearance;
        if (!e->has_company && d->has_company) { e->company = d->company; e->has_company = 1; }
        if (!e->cod) e->cod = d->cod;
        e->connectable |= d->connectable;
        return;
    }
    if (s->n >= HCI_SEEN_MAX) { s->overflow = 1; return; }
    s->dev[s->n++] = *d;
}
