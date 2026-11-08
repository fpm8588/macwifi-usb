// Hardware-free tests for the FullMAC message layer (Sources/Shared/aic_fmac.c).
//
//   test_fmac                  synthetic inputs (always runs)
//   test_fmac --legacy DIR     also parses the real aic_userconfig_8800d80.txt from the legacy-mcu1 set
#include "aic_fmac.h"
#include "aic_msg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_ids(void) {
    // Values from compiling the Linux driver's lmac_msg.h (legacy-mcu1 @ bbf6cb4) with its build flags.
    CHECK(AIC_MM_SET_STACK_START_REQ == 0x7b && AIC_MM_GET_MAC_ADDR_REQ == 0x73 && AIC_MM_GET_FW_VERSION_REQ == 0x80);
    CHECK(AIC_MM_SET_RF_CALIB_REQ == 0x69 && AIC_MM_SET_TXPWR_IDX_LVL_REQ == 0x77 && AIC_MM_VERSION_REQ == 0x04);
    CHECK(AIC_ME_CONFIG_REQ == ((5 << 10) | 0));
    // Every confirmation is its request + 1 in this firmware's numbering.
    CHECK(AIC_MM_GET_MAC_ADDR_CFM == AIC_MM_GET_MAC_ADDR_REQ + 1 && AIC_MM_SET_RF_CALIB_CFM == AIC_MM_SET_RF_CALIB_REQ + 1);
}

static void test_defaults(void) {
    AicUserConfig uc;
    aic_userconfig_defaults(&uc);
    CHECK(uc.v3[0] == 1 && uc.v3[1] == 20 && uc.v3[9] == 18 && uc.v3[12] == 16);   // 11b/11ag 2.4 GHz
    CHECK(uc.v3[35] == 0x80 && uc.v3[38] == 0x80 && uc.v3[39] == 20);              // 11a 5 GHz: 4 unused rates
    CHECK(uc.v3[68] == 14);                                                         // 11ax MCS11 5 GHz
    CHECK(uc.adj[0] == 0 && uc.ofst2x[0] == 0 && uc.loss[0] == 0 && uc.loss[2] == 0);
    CHECK(uc.xtal[0] == 0 && uc.xtal[1] == 24 && uc.xtal[2] == 31);
}

static void test_parse(void) {
    AicUserConfig uc;
    aic_userconfig_defaults(&uc);
    const char *t =
        "# AIC USERCONFIG\n"
        "enable=1\n"
        "lvl_11b_11ag_1m_2g4=18\r\n"
        "lvl_11ax_mcs11_5g=-3\n"
        "lvl_11a_6m_5g= 1 7\n"          // non-digits in the value are skipped -> 17
        "loss_enable=1\n"               // not a key the driver knows (it wants loss_enable_2g4)
        "loss_enable_2g4=1\n"
        "loss_value_2g4=2\n"
        "lvl_adj_enable=1\n"
        "lvl_adj_5g_chan_155=-1\n"
        "ofst_2g4_ofdm_lowrate_chan_10_13=5\n"
        "ofst_5g_ofdm_midrate_chan_155=7\n"
        " enable=0\n"                   // leading space makes the key unknown
        "xtal_enable=1\n"
        "xtal_cap=30\n"
        "xtal_cap_fine=1-2\n"           // rwnx_atoi: '-' after a digit -> 0
        "xtal_cap=99";                  // no newline at the end: not applied
    int n = aic_userconfig_parse(&uc, t, strlen(t));
    CHECK(n == 13);
    CHECK(uc.v3[1] == 18 && (int8_t)uc.v3[68] == -3 && uc.v3[39] == 17);
    CHECK(uc.loss[0] == 1 && uc.loss[1] == 2);
    CHECK(uc.adj[0] == 1 && (int8_t)uc.adj[9] == -1);
    CHECK(uc.ofst2x[1 + 2 * 3 + 2] == 5 && uc.ofst2x[10 + 2 * 6 + 5] == 7);
    CHECK(uc.xtal[0] == 1 && uc.xtal[1] == 30 && uc.xtal[2] == 0);

    uint8_t p[128];
    // Loss applies to the 2.4 GHz tables only (loss_enable_5g is 0).
    CHECK(aic_fmac_txpwr_lvl_v3_params(&uc, p) == AIC_SZ_TXPWR_LVL_REQ);
    CHECK(p[0] == 1 && p[1] == 16 && p[13] == 18 && (int8_t)p[68] == -3 && p[39] == 17 && p[69] == 0 && p[94] == 0);
    CHECK(aic_fmac_txpwr_lvl_adj_params(&uc, p) == 10 && p[0] == 1 && (int8_t)p[9] == -1);
    CHECK(aic_fmac_txpwr_ofst2x_params(&uc, p) == 0);                        // ofst_enable not set
    uc.ofst2x[0] = 1;
    CHECK(aic_fmac_txpwr_ofst2x_params(&uc, p) == 86 && p[9] == 5 && p[27] == 7 && p[28] == 0 && p[85] == 0);
    CHECK(aic_fmac_rf_calib_params(&uc, p) == 24 && p[20] == 30 && p[21] == 0);

    AicUserConfig d;
    aic_userconfig_defaults(&d);
    d.v3[0] = 0;
    CHECK(aic_fmac_txpwr_lvl_v3_params(&d, p) == 0);                          // disabled -> not sent
    aic_userconfig_defaults(&d);
    CHECK(aic_fmac_txpwr_lvl_adj_params(&d, p) == 0);                         // the D80 default
}

static void test_builders(void) {
    uint8_t p[32], cmd[64];
    AicUserConfig uc;
    aic_userconfig_defaults(&uc);
    CHECK(aic_fmac_rf_calib_params(&uc, p) == 24);
    static const uint8_t want_cal[24] = {0x8f, 0x0f, 0, 0, 0x0f, 0x0f, 0, 0, 0x08, 0xc0, 0x34, 0x0c,
                                         0, 0, 0, 0, 0x03, 0x42, 0x26, 0, 0, 0, 0, 0};
    CHECK(memcmp(p, want_cal, 24) == 0);   // xtal disabled -> 0/0

    aic_fmac_stack_start_params(p, 1, 0, AIC_D80_STACK_SET_VENDOR_INFO, 0);
    CHECK(p[0] == 1 && p[1] == 0 && p[2] == 0x20 && p[3] == 0);

    aic_fmac_get_mac_params(p);
    size_t n = aic_build_cmd(cmd, sizeof cmd, AIC_MM_GET_MAC_ADDR_REQ, AIC_TASK_MM_ID, p, AIC_SZ_GET_MAC_ADDR_REQ);
    static const uint8_t want_mac_req[20] = {0x10, 0x00, 0x11, 0x00, 0, 0, 0, 0, 0x73, 0x00, 0x00, 0x00,
                                             0x64, 0x00, 0x04, 0x00, 0x01, 0, 0, 0};
    CHECK(n == 20 && memcmp(cmd, want_mac_req, 20) == 0);
    n = aic_build_cmd(cmd, sizeof cmd, AIC_MM_RESET_REQ, AIC_TASK_MM_ID, NULL, 0);   // no parameters
    CHECK(n == 16 && cmd[0] == 12 && cmd[8] == 0 && cmd[14] == 0);
    n = aic_build_cmd(cmd, sizeof cmd, AIC_ME_CONFIG_REQ, (uint16_t)(AIC_ME_CONFIG_REQ >> 10), NULL, 0);
    CHECK(n == 16 && cmd[9] == 0x14 && cmd[10] == 5);
}

static void test_cfms(void) {
    // A MM_GET_MAC_ADDR_CFM as the firmware frames it: type 0x11, message at +4, 4-byte pattern before params.
    static const uint8_t rx[] = {0x12, 0x00, 0x11, 0x00, 0x74, 0x00, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00,
                                 0x2a, 0xde, 0xde, 0xad, 0x88, 0x00, 0x33, 0x77, 0x10, 0x99, 0x00, 0x00};
    size_t off = 0;
    AicRxPacket pk;
    CHECK(aic_rx_next(rx, sizeof rx, &off, &pk) == 1 && pk.is_msg && pk.id == AIC_MM_GET_MAC_ADDR_CFM);
    uint8_t mac[6];
    CHECK(aic_fmac_parse_mac_cfm(pk.params, pk.param_len, mac) == 0 && mac[0] == 0x88 && mac[5] == 0x99);
    CHECK(aic_fmac_mac_valid(mac));
    static const uint8_t zero[6] = {0, 0, 0, 0, 0x12, 0x34};
    CHECK(!aic_fmac_mac_valid(zero));
    CHECK(aic_fmac_parse_mac_cfm(pk.params, 5, mac) == -1);

    // Firmware log packet (type 0x13) is not a message.
    static const uint8_t pr[] = {0x05, 0x00, 0x13, 0x00, 'h', 'e', 'l', 'l', 'o', 0, 0, 0};
    off = 0;
    CHECK(aic_rx_next(pr, sizeof pr, &off, &pk) == 1 && !pk.is_msg && (pk.type & 0x7f) == AIC_USB_TYPE_CFG_PRINT);

    uint8_t v[64] = {11};
    memcpy(v + 1, "v6.4.3.0\n", 9);
    char s[64];
    CHECK(aic_fmac_parse_fw_version_cfm(v, sizeof v, s, sizeof s) == 0 && strcmp(s, "v6.4.3.0") == 0);
    v[0] = 200;   // length past the 63-byte field is clamped
    CHECK(aic_fmac_parse_fw_version_cfm(v, 64, s, sizeof s) == 0 && strlen(s) <= 63);
    CHECK(aic_fmac_parse_fw_version_cfm(v, 0, s, sizeof s) == -1);

    uint8_t st[2] = {1, 0x0f};
    AicStackStartCfm ss;
    CHECK(aic_fmac_parse_stack_start_cfm(st, 2, &ss) == 0 && ss.is_5g_support == 1 && ss.vendor_info == 0x0f);
    uint8_t vc[28] = {1, 2, 3, 4};
    vc[24] = 0x20; vc[26] = 4;
    AicVersionCfm ver;
    CHECK(aic_fmac_parse_version_cfm(vc, 28, &ver) == 0 && ver.version_lmac == 0x04030201 && ver.max_sta_nb == 32 &&
          ver.max_vif_nb == 4);
    CHECK(aic_fmac_parse_version_cfm(vc, 26, &ver) == -1);
    uint8_t rc[16] = {0x10, 0, 0x20};
    AicRfCalibCfm cal;
    CHECK(aic_fmac_parse_rf_calib_cfm(rc, 16, &cal) == 0 && cal.rxgain_24g_addr == 0x200010);
}

static void test_real(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/aic_userconfig_8800d80.txt", dir);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    static char b[8192];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    AicUserConfig uc;
    aic_userconfig_defaults(&uc);
    int k = aic_userconfig_parse(&uc, b, n);
    // 1 enable + 34 (2.4 GHz) + 30 (5 GHz: 11a from 6M) + 28 offsets + 3 crystal; loss_enable/loss_value unknown.
    CHECK(k == 96);
    CHECK(uc.v3[0] == 1 && uc.v3[1] == 18 && uc.v3[9] == 16 && uc.v3[35] == 0x80 && uc.v3[39] == 18);
    CHECK(uc.loss[0] == 0 && uc.ofst2x[0] == 0 && uc.xtal[0] == 0);
    uint8_t p[128];
    CHECK(aic_fmac_txpwr_lvl_v3_params(&uc, p) == 95);
    CHECK(aic_fmac_txpwr_lvl_adj_params(&uc, p) == 0 && aic_fmac_txpwr_ofst2x_params(&uc, p) == 0);
    printf("real user config: %d settings\n", k);
}

int main(int argc, char **argv) {
    test_ids();
    test_defaults();
    test_parse();
    test_builders();
    test_cfms();
    int real = 0;
    for (int i = 1; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "--legacy")) { test_real(argv[i + 1]); real = 1; }
    printf("fmac: %d checks, %d failures%s\n", checks, failures, real ? " (with real user config)" : "");
    return failures ? 1 : 0;
}
