// test_hci.c — HCI packet building and event parsing, plus a fuzz loop. Original code (GPL-2.0).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hci.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static HciSeen gSeen;
static void collect(void *ctx, const HciDevice *d) { (void)ctx; hci_seen_add(&gSeen, d); }

static void test_cmd(void) {
    uint8_t b[300];
    CHECK(hci_cmd(b, sizeof b, HCI_OP_RESET, NULL, 0) == 3 && b[0] == 0x03 && b[1] == 0x0C && b[2] == 0);
    uint8_t en[2] = {1, 1};
    CHECK(hci_cmd(b, sizeof b, HCI_OP_LE_SET_SCAN_ENABLE, en, 2) == 5 && b[0] == 0x0C && b[1] == 0x20 && b[2] == 2 &&
          b[3] == 1 && b[4] == 1);
    // Event mask: the events we depend on are enabled (bit = code - 1), nothing past LE Meta
    CHECK((HCI_EVENT_MASK_SCAN >> (HCI_EV_EXT_INQUIRY_RESULT - 1)) & 1);
    CHECK((HCI_EVENT_MASK_SCAN >> (HCI_EV_INQUIRY_RESULT_RSSI - 1)) & 1);
    CHECK((HCI_EVENT_MASK_SCAN >> (HCI_EV_LE_META - 1)) & 1);
    CHECK((HCI_EVENT_MASK_SCAN >> (HCI_EV_REMOTE_NAME_COMPLETE - 1)) & 1);
    CHECK((HCI_EVENT_MASK_SCAN >> (0x31 - 1)) & 1 && (HCI_EVENT_MASK_SCAN >> (0x33 - 1)) & 1 &&
          (HCI_EVENT_MASK_SCAN >> (0x36 - 1)) & 1 && (HCI_EVENT_MASK_SCAN >> (0x3B - 1)) & 1);
    CHECK((HCI_EVENT_MASK_SCAN >> 62) == 0);
    CHECK(HCI_OP_READ_LOCAL_VERSION == 0x1001 && HCI_OP_READ_BD_ADDR == 0x1009 && HCI_OP_INQUIRY == 0x0401);
    CHECK(hci_cmd(b, 4, HCI_OP_LE_SET_SCAN_ENABLE, en, 2) == 0);                  // no room
    CHECK(hci_cmd(b, sizeof b, HCI_OP_RESET, b, 256) == 0);                       // too long
}

static void test_complete(void) {
    // Command Complete for Read_Local_Version: ncmd 1, opcode 0x1001, status 0, HCI 5.3 rev 0x000c, LMP 5.3,
    // manufacturer 0x0a69 (made up for the test), subversion 0x1234.
    const uint8_t pkt[] = {0x0E, 0x0C, 0x01, 0x01, 0x10, 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x69, 0x0A, 0x34, 0x12};
    HciEvent ev;
    HciCmdDone d;
    HciVersion v;
    CHECK(hci_event(pkt, sizeof pkt, &ev) == 0 && ev.code == HCI_EV_CMD_COMPLETE && ev.len == 12);
    CHECK(hci_cmd_complete(&ev, &d) == 0 && d.ncmd == 1 && d.opcode == HCI_OP_READ_LOCAL_VERSION && d.status == 0 &&
          d.ret_len == 8);
    CHECK(hci_parse_version(&d, &v) == 0 && v.hci_ver == 12 && v.hci_rev == 0x0C && v.lmp_ver == 12 &&
          v.manufacturer == 0x0A69 && v.lmp_subver == 0x1234);
    CHECK(!strcmp(hci_version_name(v.hci_ver), "5.3") && !strcmp(hci_version_name(200), "?"));
    CHECK(hci_cmd_status(&ev, &d) == -1);
    CHECK(hci_event(pkt, sizeof pkt - 1, &ev) == -1);                             // length byte disagrees
    // NOP Command Complete (opcode 0): no status byte
    const uint8_t nop[] = {0x0E, 0x03, 0x01, 0x00, 0x00};
    CHECK(hci_event(nop, sizeof nop, &ev) == 0 && hci_cmd_complete(&ev, &d) == 0 && d.opcode == 0 && d.ret_len == 0);
    // Command Status: status 0, ncmd 1, opcode Inquiry
    const uint8_t st[] = {0x0F, 0x04, 0x00, 0x01, 0x01, 0x04};
    CHECK(hci_event(st, sizeof st, &ev) == 0 && hci_cmd_status(&ev, &d) == 0 && d.opcode == HCI_OP_INQUIRY &&
          d.status == 0 && d.ncmd == 1);
    CHECK(hci_cmd_complete(&ev, &d) == -1);
}

static void test_le_adv(void) {
    // LE Advertising Report, one ADV_IND from a random address, AD: flags, complete name "Mouse", appearance mouse,
    // manufacturer 0x004C; RSSI -60.
    uint8_t ad[] = {0x02, 0x01, 0x06, 0x06, 0x09, 'M', 'o', 'u', 's', 'e', 0x03, 0x19, 0xC2, 0x03,
                    0x05, 0xFF, 0x4C, 0x00, 0x01, 0x02};
    uint8_t pkt[64];
    size_t n = 0;
    pkt[n++] = HCI_EV_LE_META; pkt[n++] = 0; pkt[n++] = HCI_LE_ADV_REPORT; pkt[n++] = 1;
    pkt[n++] = 0x00; pkt[n++] = 0x01;
    const uint8_t addr[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0xC6};
    memcpy(pkt + n, addr, 6); n += 6;
    pkt[n++] = sizeof ad; memcpy(pkt + n, ad, sizeof ad); n += sizeof ad;
    pkt[n++] = (uint8_t)(int8_t)-60;
    pkt[1] = (uint8_t)(n - 2);
    HciEvent ev;
    memset(&gSeen, 0, sizeof gSeen);
    CHECK(hci_event(pkt, n, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 1 && gSeen.n == 1);
    HciDevice *d = &gSeen.dev[0];
    CHECK(d->kind == HCI_DEV_LE && d->addr_type == 1 && d->connectable && d->rssi == -60 && !memcmp(d->addr, addr, 6));
    CHECK(!strcmp(d->name, "Mouse") && d->name_complete && d->appearance == 0x03C2 && d->has_company && d->company == 0x004C);
    CHECK(!strcmp(hci_appearance_kind(d->appearance), "Mouse"));
    char s[18];
    hci_addr_str(d->addr, s);
    CHECK(!strcmp(s, "C6:55:44:33:22:11"));
    // A scan response for the same device updates RSSI; a shortened name doesn't replace the complete one.
    uint8_t ad2[] = {0x03, 0x08, 'M', 'o'};
    n = 4; pkt[4] = 0x04; n = 6; memcpy(pkt + n, addr, 6); n += 6;
    pkt[n++] = sizeof ad2; memcpy(pkt + n, ad2, sizeof ad2); n += sizeof ad2; pkt[n++] = (uint8_t)(int8_t)-50;
    pkt[1] = (uint8_t)(n - 2);
    CHECK(hci_event(pkt, n, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 1 && gSeen.n == 1);
    CHECK(d->rssi == -50 && !strcmp(d->name, "Mouse"));
    // Truncated report
    CHECK(hci_event(pkt, n - 1, &ev) == -1);
    pkt[1] = (uint8_t)(n - 3);
    CHECK(hci_event(pkt, n - 1, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == -1);
    // Other LE subevents are ignored
    const uint8_t conn[] = {HCI_EV_LE_META, 1, 0x01};
    CHECK(hci_event(conn, sizeof conn, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 0);
}

static void test_ext_adv(void) {
    uint8_t pkt[80];
    size_t n = 0;
    pkt[n++] = HCI_EV_LE_META; pkt[n++] = 0; pkt[n++] = HCI_LE_EXT_ADV_REPORT; pkt[n++] = 2;
    for (int r = 0; r < 2; r++) {
        pkt[n++] = 0x13; pkt[n++] = 0x00;                    // connectable scannable legacy
        pkt[n++] = r ? 0xFF : 0x00;                          // second one anonymous
        for (int i = 0; i < 6; i++) pkt[n++] = (uint8_t)(0xA0 + i);
        pkt[n++] = 1; pkt[n++] = 0; pkt[n++] = 0xFF; pkt[n++] = 0x7F;   // PHYs, SID, TX power n/a
        pkt[n++] = (uint8_t)(int8_t)-70;                     // RSSI
        pkt[n++] = 0; pkt[n++] = 0;                          // periodic interval
        pkt[n++] = 0; for (int i = 0; i < 6; i++) pkt[n++] = 0;
        pkt[n++] = 5; pkt[n++] = 4; pkt[n++] = 0x09; pkt[n++] = 'K'; pkt[n++] = 'b'; pkt[n++] = 'd';
    }
    pkt[1] = (uint8_t)(n - 2);
    HciEvent ev;
    memset(&gSeen, 0, sizeof gSeen);
    CHECK(hci_event(pkt, n, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 1 && gSeen.n == 1);
    CHECK(gSeen.dev[0].rssi == -70 && !strcmp(gSeen.dev[0].name, "Kbd") && gSeen.dev[0].connectable &&
          gSeen.dev[0].addr_type == 0);
}

static void test_inquiry(void) {
    // Inquiry Result with RSSI: one device, class 0x240404 (audio, headset-ish), RSSI -45.
    const uint8_t pkt[] = {HCI_EV_INQUIRY_RESULT_RSSI, 15, 1, 1, 2, 3, 4, 5, 6, 1, 0, 0x04, 0x04, 0x24, 0x00, 0x00,
                           (uint8_t)(int8_t)-45};
    HciEvent ev;
    memset(&gSeen, 0, sizeof gSeen);
    CHECK(hci_event(pkt, sizeof pkt, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 1);
    CHECK(gSeen.dev[0].kind == HCI_DEV_CLASSIC && gSeen.dev[0].cod == 0x240404 && gSeen.dev[0].rssi == -45);
    CHECK(!strcmp(hci_cod_kind(gSeen.dev[0].cod), "Audio"));
    CHECK(!strcmp(hci_cod_kind(0x002540), "Keyboard") && !strcmp(hci_cod_kind(0x002580), "Mouse") &&
          hci_cod_kind(0) == NULL);
    // Old 15-byte layout
    const uint8_t old[] = {HCI_EV_INQUIRY_RESULT_RSSI, 16, 1, 9, 9, 9, 9, 9, 9, 1, 0, 0, 0x0C, 0x02, 0x5A, 0, 0,
                           (uint8_t)(int8_t)-30};
    CHECK(hci_event(old, sizeof old, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 1);
    CHECK(gSeen.n == 2 && gSeen.dev[1].cod == 0x5A020C && gSeen.dev[1].rssi == -30 && !strcmp(hci_cod_kind(0x5A020C), "Phone"));
    // Plain Inquiry Result (no RSSI)
    const uint8_t plain[] = {HCI_EV_INQUIRY_RESULT, 15, 1, 7, 7, 7, 7, 7, 7, 1, 0, 0, 0x00, 0x01, 0x00, 0, 0};
    CHECK(hci_event(plain, sizeof plain, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 1);
    CHECK(gSeen.n == 3 && gSeen.dev[2].cod == 0x000100 && gSeen.dev[2].rssi == 127);
    // clock offsets: RSSI 14-byte layout at 11, plain at 12
    const uint8_t co1[] = {HCI_EV_INQUIRY_RESULT_RSSI, 15, 1, 5, 5, 5, 5, 5, 5, 2, 0, 0, 0, 0, 0x34, 0x12, (uint8_t)(int8_t)-20};
    const uint8_t co2[] = {HCI_EV_INQUIRY_RESULT, 15, 1, 4, 4, 4, 4, 4, 4, 1, 0, 0, 0, 0, 0, 0x78, 0x56};
    HciSeen keep = gSeen;
    memset(&gSeen, 0, sizeof gSeen);
    CHECK(hci_event(co1, sizeof co1, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 1 && gSeen.dev[0].clock_offset == 0x1234 && gSeen.dev[0].psrm == 2);
    CHECK(hci_event(co2, sizeof co2, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 1 && gSeen.dev[1].clock_offset == 0x5678 && gSeen.dev[1].psrm == 1);
    gSeen = keep;
    // Extended Inquiry Result with a name in the EIR
    uint8_t eir[2 + 255] = {HCI_EV_EXT_INQUIRY_RESULT, 255, 1, 1, 2, 3, 4, 5, 6, 1, 0, 0x04, 0x04, 0x24, 0, 0,
                            (uint8_t)(int8_t)-40, 8, 0x09, 'S', 'p', 'e', 'a', 'k', 'e', 'r'};
    CHECK(hci_event(eir, sizeof eir, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == 1);
    CHECK(gSeen.n == 3 && !strcmp(gSeen.dev[0].name, "Speaker") && gSeen.dev[0].rssi == -40);
    // Inquiry with RSSI carried psrm 1 and clock offset 0x0000; remote name request parameters
    CHECK(gSeen.dev[0].psrm == 1);
    uint8_t rn[10];
    CHECK(hci_remote_name_params(&gSeen.dev[0], rn) == 10 && rn[0] == 1 && rn[5] == 6 && rn[6] == 1 && rn[9] == 0x80);
    // Remote Name Request Complete
    uint8_t ne[2 + 255] = {HCI_EV_REMOTE_NAME_COMPLETE, 255, 0, 1, 2, 3, 4, 5, 6, 'M', 'a', 'g', 'i', 'c', 0};
    uint8_t ra[6];
    char nm[HCI_NAME_MAX + 1];
    CHECK(hci_event(ne, sizeof ne, &ev) == 0 && hci_parse_remote_name(&ev, ra, nm) == 0 && !strcmp(nm, "Magic") &&
          ra[0] == 1 && ra[5] == 6);
    ne[2] = 0x04;   // page timeout
    CHECK(hci_parse_remote_name(&ev, ra, nm) == -1);
    // Count too large for the length
    const uint8_t bad[] = {HCI_EV_INQUIRY_RESULT_RSSI, 15, 2, 1, 2, 3, 4, 5, 6, 1, 0, 0, 0, 0, 0, 0, 0};
    CHECK(hci_event(bad, sizeof bad, &ev) == 0 && hci_scan_event(&ev, collect, NULL) == -1);
}

static void test_ad_edges(void) {
    HciDevice d;
    memset(&d, 0, sizeof d);
    const uint8_t ad[] = {0x04, 0x09, 'a', 0x07, 'b', 0x00, 0x09, 0x09};   // control char replaced; then truncated field
    hci_parse_ad(ad, sizeof ad, &d);
    CHECK(!strcmp(d.name, "a?b"));
    const uint8_t pad[] = {0x02, 0x09, 'z', 0x00, 0xFF, 0xFF};
    memset(&d, 0, sizeof d);
    hci_parse_ad(pad, sizeof pad, &d);
    CHECK(!strcmp(d.name, "z") && !d.has_company);
    memset(&gSeen, 0, sizeof gSeen);
    HciDevice x;
    memset(&x, 0, sizeof x);
    x.kind = HCI_DEV_LE;
    for (int i = 0; i < HCI_SEEN_MAX + 5; i++) { x.addr[0] = (uint8_t)i; x.addr[1] = (uint8_t)(i >> 8); hci_seen_add(&gSeen, &x); }
    CHECK(gSeen.n == HCI_SEEN_MAX && gSeen.overflow);
}

static void test_fuzz(void) {
    srand(1234);
    uint8_t pkt[2 + 255];
    long devices = 0;
    for (int it = 0; it < 200000; it++) {
        int len = rand() % 256;
        const uint8_t codes[] = {HCI_EV_LE_META, HCI_EV_INQUIRY_RESULT, HCI_EV_INQUIRY_RESULT_RSSI,
                                 HCI_EV_EXT_INQUIRY_RESULT, HCI_EV_CMD_COMPLETE, HCI_EV_CMD_STATUS};
        pkt[0] = codes[rand() % 6];
        pkt[1] = (uint8_t)len;
        for (int i = 0; i < len; i++) pkt[2 + i] = (uint8_t)rand();
        if (pkt[0] == HCI_EV_LE_META && len) pkt[2] = (rand() & 1) ? HCI_LE_ADV_REPORT : HCI_LE_EXT_ADV_REPORT;
        if (len > 1 && (rand() & 1)) pkt[3] = (uint8_t)(rand() % 3);   // small report counts
        HciEvent ev;
        HciCmdDone d;
        HciVersion v;
        if (hci_event(pkt, (size_t)len + 2, &ev)) continue;
        memset(&gSeen, 0, sizeof gSeen);
        int r = hci_scan_event(&ev, collect, NULL);
        if (r > 0) devices += r;
        for (int i = 0; i < gSeen.n; i++) if (strlen(gSeen.dev[i].name) > HCI_NAME_MAX) failures++;
        if (hci_cmd_complete(&ev, &d) == 0) hci_parse_version(&d, &v);
        hci_cmd_status(&ev, &d);
    }
    CHECK(devices > 0);   // the fuzzer does reach the device paths
}

int main(void) {
    test_cmd();
    test_complete();
    test_le_adv();
    test_ext_adv();
    test_inquiry();
    test_ad_edges();
    test_fuzz();
    printf("hci: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
