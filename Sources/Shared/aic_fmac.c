// aic_fmac.c — see aic_fmac.h. Ported from the AIC8800 Linux Wi-Fi driver (GPL-2.0-only); Copyright (C) AICSemi
// 2018-2020. Differences from the original: no globals, bounded buffers in the user-config parser, requests
// are returned as parameter bytes for the caller to send.

#include "aic_fmac.h"

#include <string.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Offsets inside txpwr_lvl_conf_v3_t (measured from lmac_msg.h).
enum { V3_EN = 0, V3_11B_2G4 = 1, V3_11N_2G4 = 13, V3_11AX_2G4 = 23, V3_11A_5G = 35, V3_11N_5G = 47, V3_11AX_5G = 57 };

void aic_userconfig_defaults(AicUserConfig *uc) {
    // rwnx_platform.c userconfig_info: txpwr_lvl_v3 as below, txpwr_lvl_adj not initialised (all zero),
    // txpwr_loss zero, txpwr_ofst2x disabled and zero, xtal_cap {enable 0, cap 24, fine 31}.
    static const int8_t b2g4[12] = {20, 20, 20, 20, 20, 20, 20, 20, 18, 18, 16, 16};
    static const int8_t n2g4[10] = {20, 20, 20, 20, 18, 18, 16, 16, 16, 16};
    static const int8_t ax2g4[12] = {20, 20, 20, 20, 18, 18, 16, 16, 16, 16, 15, 15};
    static const int8_t a5g[12] = {-128, -128, -128, -128, 20, 20, 20, 20, 18, 18, 16, 16};   // 0x80 = n/a
    static const int8_t n5g[10] = {20, 20, 20, 20, 18, 18, 16, 16, 16, 15};
    static const int8_t ax5g[12] = {20, 20, 20, 20, 18, 18, 16, 16, 16, 15, 14, 14};
    memset(uc, 0, sizeof *uc);
    uc->v3[V3_EN] = 1;
    memcpy(uc->v3 + V3_11B_2G4, b2g4, 12);
    memcpy(uc->v3 + V3_11N_2G4, n2g4, 10);
    memcpy(uc->v3 + V3_11AX_2G4, ax2g4, 12);
    memcpy(uc->v3 + V3_11A_5G, a5g, 12);
    memcpy(uc->v3 + V3_11N_5G, n5g, 10);
    memcpy(uc->v3 + V3_11AX_5G, ax5g, 12);
    uc->xtal[0] = 0;
    uc->xtal[1] = 24;
    uc->xtal[2] = 31;
}

enum { AIC_UC_V3, AIC_UC_ADJ, AIC_UC_OFST2X, AIC_UC_LOSS, AIC_UC_XTAL };
typedef struct { const char *key; int buf; uint8_t off; } UcKey;

// rwnx_plat_nvram_set_value, the keys that reach the structures the D80 bring-up sends. Generated from the Linux
// source (key -> field[index] -> byte offset); see the header comment for the commit.
static const UcKey kUcKeys[] = {
    {"enable", AIC_UC_V3, 0},
    {"lvl_11b_11ag_1m_2g4", AIC_UC_V3, 1},
    {"lvl_11b_11ag_2m_2g4", AIC_UC_V3, 2},
    {"lvl_11b_11ag_5m5_2g4", AIC_UC_V3, 3},
    {"lvl_11b_11ag_11m_2g4", AIC_UC_V3, 4},
    {"lvl_11b_11ag_6m_2g4", AIC_UC_V3, 5},
    {"lvl_11b_11ag_9m_2g4", AIC_UC_V3, 6},
    {"lvl_11b_11ag_12m_2g4", AIC_UC_V3, 7},
    {"lvl_11b_11ag_18m_2g4", AIC_UC_V3, 8},
    {"lvl_11b_11ag_24m_2g4", AIC_UC_V3, 9},
    {"lvl_11b_11ag_36m_2g4", AIC_UC_V3, 10},
    {"lvl_11b_11ag_48m_2g4", AIC_UC_V3, 11},
    {"lvl_11b_11ag_54m_2g4", AIC_UC_V3, 12},
    {"lvl_11n_11ac_mcs0_2g4", AIC_UC_V3, 13},
    {"lvl_11n_11ac_mcs1_2g4", AIC_UC_V3, 14},
    {"lvl_11n_11ac_mcs2_2g4", AIC_UC_V3, 15},
    {"lvl_11n_11ac_mcs3_2g4", AIC_UC_V3, 16},
    {"lvl_11n_11ac_mcs4_2g4", AIC_UC_V3, 17},
    {"lvl_11n_11ac_mcs5_2g4", AIC_UC_V3, 18},
    {"lvl_11n_11ac_mcs6_2g4", AIC_UC_V3, 19},
    {"lvl_11n_11ac_mcs7_2g4", AIC_UC_V3, 20},
    {"lvl_11n_11ac_mcs8_2g4", AIC_UC_V3, 21},
    {"lvl_11n_11ac_mcs9_2g4", AIC_UC_V3, 22},
    {"lvl_11ax_mcs0_2g4", AIC_UC_V3, 23},
    {"lvl_11ax_mcs1_2g4", AIC_UC_V3, 24},
    {"lvl_11ax_mcs2_2g4", AIC_UC_V3, 25},
    {"lvl_11ax_mcs3_2g4", AIC_UC_V3, 26},
    {"lvl_11ax_mcs4_2g4", AIC_UC_V3, 27},
    {"lvl_11ax_mcs5_2g4", AIC_UC_V3, 28},
    {"lvl_11ax_mcs6_2g4", AIC_UC_V3, 29},
    {"lvl_11ax_mcs7_2g4", AIC_UC_V3, 30},
    {"lvl_11ax_mcs8_2g4", AIC_UC_V3, 31},
    {"lvl_11ax_mcs9_2g4", AIC_UC_V3, 32},
    {"lvl_11ax_mcs10_2g4", AIC_UC_V3, 33},
    {"lvl_11ax_mcs11_2g4", AIC_UC_V3, 34},
    {"lvl_11a_1m_5g", AIC_UC_V3, 35},
    {"lvl_11a_2m_5g", AIC_UC_V3, 36},
    {"lvl_11a_5m5_5g", AIC_UC_V3, 37},
    {"lvl_11a_11m_5g", AIC_UC_V3, 38},
    {"lvl_11a_6m_5g", AIC_UC_V3, 39},
    {"lvl_11a_9m_5g", AIC_UC_V3, 40},
    {"lvl_11a_12m_5g", AIC_UC_V3, 41},
    {"lvl_11a_18m_5g", AIC_UC_V3, 42},
    {"lvl_11a_24m_5g", AIC_UC_V3, 43},
    {"lvl_11a_36m_5g", AIC_UC_V3, 44},
    {"lvl_11a_48m_5g", AIC_UC_V3, 45},
    {"lvl_11a_54m_5g", AIC_UC_V3, 46},
    {"lvl_11n_11ac_mcs0_5g", AIC_UC_V3, 47},
    {"lvl_11n_11ac_mcs1_5g", AIC_UC_V3, 48},
    {"lvl_11n_11ac_mcs2_5g", AIC_UC_V3, 49},
    {"lvl_11n_11ac_mcs3_5g", AIC_UC_V3, 50},
    {"lvl_11n_11ac_mcs4_5g", AIC_UC_V3, 51},
    {"lvl_11n_11ac_mcs5_5g", AIC_UC_V3, 52},
    {"lvl_11n_11ac_mcs6_5g", AIC_UC_V3, 53},
    {"lvl_11n_11ac_mcs7_5g", AIC_UC_V3, 54},
    {"lvl_11n_11ac_mcs8_5g", AIC_UC_V3, 55},
    {"lvl_11n_11ac_mcs9_5g", AIC_UC_V3, 56},
    {"lvl_11ax_mcs0_5g", AIC_UC_V3, 57},
    {"lvl_11ax_mcs1_5g", AIC_UC_V3, 58},
    {"lvl_11ax_mcs2_5g", AIC_UC_V3, 59},
    {"lvl_11ax_mcs3_5g", AIC_UC_V3, 60},
    {"lvl_11ax_mcs4_5g", AIC_UC_V3, 61},
    {"lvl_11ax_mcs5_5g", AIC_UC_V3, 62},
    {"lvl_11ax_mcs6_5g", AIC_UC_V3, 63},
    {"lvl_11ax_mcs7_5g", AIC_UC_V3, 64},
    {"lvl_11ax_mcs8_5g", AIC_UC_V3, 65},
    {"lvl_11ax_mcs9_5g", AIC_UC_V3, 66},
    {"lvl_11ax_mcs10_5g", AIC_UC_V3, 67},
    {"lvl_11ax_mcs11_5g", AIC_UC_V3, 68},
    {"lvl_adj_enable", AIC_UC_ADJ, 0},
    {"lvl_adj_2g4_chan_1_4", AIC_UC_ADJ, 1},
    {"lvl_adj_2g4_chan_5_9", AIC_UC_ADJ, 2},
    {"lvl_adj_2g4_chan_10_13", AIC_UC_ADJ, 3},
    {"lvl_adj_5g_chan_42", AIC_UC_ADJ, 4},
    {"lvl_adj_5g_chan_58", AIC_UC_ADJ, 5},
    {"lvl_adj_5g_chan_106", AIC_UC_ADJ, 6},
    {"lvl_adj_5g_chan_122", AIC_UC_ADJ, 7},
    {"lvl_adj_5g_chan_138", AIC_UC_ADJ, 8},
    {"lvl_adj_5g_chan_155", AIC_UC_ADJ, 9},
    {"loss_enable_2g4", AIC_UC_LOSS, 0},
    {"loss_value_2g4", AIC_UC_LOSS, 1},
    {"loss_enable_5g", AIC_UC_LOSS, 2},
    {"loss_value_5g", AIC_UC_LOSS, 3},
    {"ofst_enable", AIC_UC_OFST2X, 0},
    {"ofst_2g4_11b_chan_1_4", AIC_UC_OFST2X, 1},
    {"ofst_2g4_11b_chan_5_9", AIC_UC_OFST2X, 2},
    {"ofst_2g4_11b_chan_10_13", AIC_UC_OFST2X, 3},
    {"ofst_2g4_ofdm_highrate_chan_1_4", AIC_UC_OFST2X, 4},
    {"ofst_2g4_ofdm_highrate_chan_5_9", AIC_UC_OFST2X, 5},
    {"ofst_2g4_ofdm_highrate_chan_10_13", AIC_UC_OFST2X, 6},
    {"ofst_2g4_ofdm_lowrate_chan_1_4", AIC_UC_OFST2X, 7},
    {"ofst_2g4_ofdm_lowrate_chan_5_9", AIC_UC_OFST2X, 8},
    {"ofst_2g4_ofdm_lowrate_chan_10_13", AIC_UC_OFST2X, 9},
    {"ofst_5g_ofdm_lowrate_chan_42", AIC_UC_OFST2X, 10},
    {"ofst_5g_ofdm_lowrate_chan_58", AIC_UC_OFST2X, 11},
    {"ofst_5g_ofdm_lowrate_chan_106", AIC_UC_OFST2X, 12},
    {"ofst_5g_ofdm_lowrate_chan_122", AIC_UC_OFST2X, 13},
    {"ofst_5g_ofdm_lowrate_chan_138", AIC_UC_OFST2X, 14},
    {"ofst_5g_ofdm_lowrate_chan_155", AIC_UC_OFST2X, 15},
    {"ofst_5g_ofdm_highrate_chan_42", AIC_UC_OFST2X, 16},
    {"ofst_5g_ofdm_highrate_chan_58", AIC_UC_OFST2X, 17},
    {"ofst_5g_ofdm_highrate_chan_106", AIC_UC_OFST2X, 18},
    {"ofst_5g_ofdm_highrate_chan_122", AIC_UC_OFST2X, 19},
    {"ofst_5g_ofdm_highrate_chan_138", AIC_UC_OFST2X, 20},
    {"ofst_5g_ofdm_highrate_chan_155", AIC_UC_OFST2X, 21},
    {"ofst_5g_ofdm_midrate_chan_42", AIC_UC_OFST2X, 22},
    {"ofst_5g_ofdm_midrate_chan_58", AIC_UC_OFST2X, 23},
    {"ofst_5g_ofdm_midrate_chan_106", AIC_UC_OFST2X, 24},
    {"ofst_5g_ofdm_midrate_chan_122", AIC_UC_OFST2X, 25},
    {"ofst_5g_ofdm_midrate_chan_138", AIC_UC_OFST2X, 26},
    {"ofst_5g_ofdm_midrate_chan_155", AIC_UC_OFST2X, 27},
    {"xtal_enable", AIC_UC_XTAL, 0},
    {"xtal_cap", AIC_UC_XTAL, 1},
    {"xtal_cap_fine", AIC_UC_XTAL, 2},
};

static uint8_t *uc_field(AicUserConfig *uc, int buf) {
    switch (buf) {
        case AIC_UC_V3: return uc->v3;
        case AIC_UC_ADJ: return uc->adj;
        case AIC_UC_OFST2X: return uc->ofst2x;
        case AIC_UC_LOSS: return uc->loss;
        default: return uc->xtal;
    }
}

// rwnx_atoi: optional leading '-', then digits; any other character makes the result 0.
static int uc_atoi(const char *v) {
    int flag = 1, result = 0;
    if (*v == '-') { flag = -1; v++; }
    for (; *v; v++) {
        result *= 10;
        if (*v >= '0' && *v <= '9') result += *v - '0';
        else return 0;
    }
    return result * flag;
}

static int uc_set(AicUserConfig *uc, const char *cmd, const char *val) {
    int applied = 0;
    for (size_t i = 0; i < sizeof kUcKeys / sizeof kUcKeys[0]; i++) {
        if (strcmp(cmd, kUcKeys[i].key) != 0) continue;
        uc_field(uc, kUcKeys[i].buf)[kUcKeys[i].off] = (uint8_t)uc_atoi(val);   // int -> u8/s8, as in C
        applied = 1;
    }
    return applied;
}

int aic_userconfig_parse(AicUserConfig *uc, const char *buf, size_t len) {
    enum { INIT, CMD, GET_VALUE, PRINT } state = INIT;
    char command[64], value[100];
    size_t n = 0;
    int applied = 0;
    memset(command, 0, sizeof command);
    memset(value, 0, sizeof value);
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
            if (command[0] && value[0] && state == GET_VALUE) applied += uc_set(uc, command, value);
            memset(command, 0, sizeof command);
            memset(value, 0, sizeof value);
            n = 0;
            state = INIT;
            continue;
        }
        if (state == INIT) state = (c == '#') ? PRINT : CMD;
        if (state == PRINT) continue;   // the original logs comment lines
        if (state == CMD) {
            if (command[0] && c == '=') { state = GET_VALUE; n = 0; continue; }
            if (n < sizeof command - 1) command[n++] = c;
        } else if (state == GET_VALUE) {
            if (c != '-' && (c < '0' || c > '9')) continue;
            if (n < sizeof value - 1) value[n++] = c;
        }
    }
    return applied;   // like the original, a last line without a newline is not applied
}

void aic_fmac_stack_start_params(uint8_t out[AIC_SZ_STACK_START_REQ], uint8_t on, uint8_t efuse_valid,
                                 uint8_t set_vendor_info, uint8_t fwtrace_redir) {
    out[0] = on;
    out[1] = efuse_valid;
    out[2] = set_vendor_info;
    out[3] = fwtrace_redir;
}

void aic_fmac_get_mac_params(uint8_t out[AIC_SZ_GET_MAC_ADDR_REQ]) { put32(out, 1); }

size_t aic_fmac_txpwr_lvl_v3_params(const AicUserConfig *uc, uint8_t out[AIC_SZ_TXPWR_LVL_REQ]) {
    uint8_t v3[69];
    memcpy(v3, uc->v3, sizeof v3);
    // rwnx_send_txpwr_lvl_v3_req applies the user's antenna loss before checking `enable`.
    if (uc->loss[0] == 1) {
        int8_t loss = (int8_t)uc->loss[1];
        for (int i = 0; i < 12; i++) v3[V3_11B_2G4 + i] = (uint8_t)((int8_t)v3[V3_11B_2G4 + i] - loss);
        for (int i = 0; i < 10; i++) v3[V3_11N_2G4 + i] = (uint8_t)((int8_t)v3[V3_11N_2G4 + i] - loss);
        for (int i = 0; i < 12; i++) v3[V3_11AX_2G4 + i] = (uint8_t)((int8_t)v3[V3_11AX_2G4 + i] - loss);
    }
    if (uc->loss[2] == 1) {
        int8_t loss = (int8_t)uc->loss[3];
        for (int i = 0; i < 12; i++) v3[V3_11A_5G + i] = (uint8_t)((int8_t)v3[V3_11A_5G + i] - loss);
        for (int i = 0; i < 10; i++) v3[V3_11N_5G + i] = (uint8_t)((int8_t)v3[V3_11N_5G + i] - loss);
        for (int i = 0; i < 12; i++) v3[V3_11AX_5G + i] = (uint8_t)((int8_t)v3[V3_11AX_5G + i] - loss);
    }
    if (v3[V3_EN] == 0) return 0;
    memset(out, 0, AIC_SZ_TXPWR_LVL_REQ);
    memcpy(out, v3, sizeof v3);
    return AIC_SZ_TXPWR_LVL_REQ;
}

size_t aic_fmac_txpwr_lvl_adj_params(const AicUserConfig *uc, uint8_t out[AIC_SZ_TXPWR_LVL_ADJ_REQ]) {
    if (uc->adj[0] == 0) return 0;
    memcpy(out, uc->adj, AIC_SZ_TXPWR_LVL_ADJ_REQ);
    return AIC_SZ_TXPWR_LVL_ADJ_REQ;
}

size_t aic_fmac_txpwr_ofst2x_params(const AicUserConfig *uc, uint8_t out[AIC_SZ_TXPWR_OFST_REQ]) {
    if (uc->ofst2x[0] == 0) return 0;
    memset(out, 0, AIC_SZ_TXPWR_OFST_REQ);
    memcpy(out, uc->ofst2x, sizeof uc->ofst2x);
    return AIC_SZ_TXPWR_OFST_REQ;
}

size_t aic_fmac_rf_calib_params(const AicUserConfig *uc, uint8_t out[AIC_SZ_RF_CALIB_REQ]) {
    memset(out, 0, AIC_SZ_RF_CALIB_REQ);
    put32(out + 0, 0x0f8f);       // cal_cfg_24g (PRODUCT_ID_AIC8800D81)
    put32(out + 4, 0x0f0f);       // cal_cfg_5g
    put32(out + 8, 0x0c34c008);   // param_alpha
    put32(out + 12, 0);           // bt_calib_en
    put32(out + 16, 0x264203);    // bt_calib_param
    if (uc->xtal[0]) {
        out[20] = uc->xtal[1];
        out[21] = uc->xtal[2];
    }
    return AIC_SZ_RF_CALIB_REQ;
}

int aic_fmac_parse_stack_start_cfm(const uint8_t *p, size_t n, AicStackStartCfm *c) {
    if (!p || n < 2) return -1;
    c->is_5g_support = p[0];
    c->vendor_info = p[1];
    return 0;
}

int aic_fmac_parse_mac_cfm(const uint8_t *p, size_t n, uint8_t mac[6]) {
    if (!p || n < 6) return -1;
    memcpy(mac, p, 6);
    return 0;
}

int aic_fmac_parse_fw_version_cfm(const uint8_t *p, size_t n, char *out, size_t cap) {
    if (!p || n < 1 || !out || cap == 0) return -1;
    size_t len = p[0];
    if (len > 63) len = 63;
    if (len > n - 1) len = n - 1;
    if (len > cap - 1) len = cap - 1;
    memcpy(out, p + 1, len);
    out[len] = '\0';
    size_t s = strlen(out);   // the firmware may NUL-pad inside len
    while (s && (out[s - 1] == '\n' || out[s - 1] == '\r' || out[s - 1] == ' ')) out[--s] = '\0';
    return 0;
}

int aic_fmac_parse_version_cfm(const uint8_t *p, size_t n, AicVersionCfm *c) {
    if (!p || n < 27) return -1;
    c->version_lmac = get32(p);
    c->version_machw_1 = get32(p + 4);
    c->version_machw_2 = get32(p + 8);
    c->version_phy_1 = get32(p + 12);
    c->version_phy_2 = get32(p + 16);
    c->features = get32(p + 20);
    c->max_sta_nb = (uint16_t)(p[24] | (p[25] << 8));
    c->max_vif_nb = p[26];
    return 0;
}

int aic_fmac_parse_rf_calib_cfm(const uint8_t *p, size_t n, AicRfCalibCfm *c) {
    if (!p || n < 16) return -1;
    c->rxgain_24g_addr = get32(p);
    c->rxgain_5g_addr = get32(p + 4);
    c->txgain_24g_addr = get32(p + 8);
    c->txgain_5g_addr = get32(p + 12);
    return 0;
}

int aic_fmac_mac_valid(const uint8_t mac[6]) { return (mac[0] | mac[1] | mac[2] | mac[3]) != 0; }
