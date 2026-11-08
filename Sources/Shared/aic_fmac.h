// aic_fmac.h — talking to the running AIC8800D80 FullMAC firmware (a69c:8d81): message ids, the user-config
// file, request builders and confirmation parsers. Pure C, no I/O; unit tested in Tests/test_fmac.c.
//
// Ported from the AIC8800 Linux Wi-Fi driver (GPL-2.0-only), github.com/shenmintao/aic8800d80 branch
// legacy-mcu1 @ bbf6cb4015c8da4e7e113a6a6282f191213e0854, drivers/aic8800/aic8800_fdrv/:
//   lmac_msg.h      message ids (enum mm_msg_tag) and structure layouts; ids and sizes were checked by compiling
//                   that header with the driver's build flags (CONFIG_RWNX_FULLMAC, AICWF_ARP_OFFLOAD, USE_5G)
//   rwnx_main.c     rwnx_cfg80211_init / rwnx_ic_system_init / rwnx_ic_rf_init: order of the bring-up
//   rwnx_msg_tx.c   rwnx_send_set_stack_start_req, _get_fw_version_req, _txpwr_lvl_v3_req, _txpwr_lvl_adj_req,
//                   _txpwr_ofst2x_req, _rf_calib_req, _get_macaddr_req, _reset, _version_req
//   rwnx_platform.c userconfig_info defaults, rwnx_plat_userconfig_parsing, rwnx_plat_nvram_set_value, rwnx_atoi
//   aicwf_compat_8800d80.c  aicwf_set_rf_config_8800d80
// Copyright (C) AICSemi 2018-2020; Copyright (C) RivieraWaves 2012-2019 (rwnx_* files). Rewritten for macOS user
// space, 2026. GPL-2.0-only; see NOTICE.md.
//
// Bring-up order for the D80 after firmware start (Linux "PRODUCT_ID_AIC8800D81" path, testmode 0, USE_5G):
//   DBG_MEM_READ [0x40500000] (system_config_8800d80) -> user-config file -> MM_SET_STACK_START(1, 0, BIT(5), 0)
//   -> MM_GET_FW_VERSION -> TXPWR_LVL v3 / LVL_ADJ / OFST2X (each only if enabled) -> MM_SET_RF_CALIB
//   -> MM_GET_MAC_ADDR -> MM_RESET -> MM_VERSION   (then ME_CONFIG, ME_CHAN_CONFIG, interface: phase 3)

#ifndef AIC_FMAC_H
#define AIC_FMAC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- message ids (TASK_MM = 0 unless noted)
enum {
    AIC_MM_RESET_REQ              = 0x0000, AIC_MM_RESET_CFM              = 0x0001,
    AIC_MM_START_REQ              = 0x0002,
    AIC_MM_VERSION_REQ            = 0x0004, AIC_MM_VERSION_CFM            = 0x0005,
    AIC_MM_ADD_IF_REQ             = 0x0006, AIC_MM_ADD_IF_CFM             = 0x0007,
    AIC_MM_SET_RF_CALIB_REQ       = 0x0069, AIC_MM_SET_RF_CALIB_CFM       = 0x006a,
    AIC_MM_GET_MAC_ADDR_REQ       = 0x0073, AIC_MM_GET_MAC_ADDR_CFM       = 0x0074,
    AIC_MM_SET_TXPWR_IDX_LVL_REQ  = 0x0077, AIC_MM_SET_TXPWR_IDX_LVL_CFM  = 0x0078,
    AIC_MM_SET_TXPWR_OFST_REQ     = 0x0079, AIC_MM_SET_TXPWR_OFST_CFM     = 0x007a,
    AIC_MM_SET_STACK_START_REQ    = 0x007b, AIC_MM_SET_STACK_START_CFM    = 0x007c,
    AIC_MM_GET_FW_VERSION_REQ     = 0x0080, AIC_MM_GET_FW_VERSION_CFM     = 0x0081,
    AIC_MM_SET_TXPWR_LVL_ADJ_REQ  = 0x0089, AIC_MM_SET_TXPWR_LVL_ADJ_CFM  = 0x008a,
    AIC_MM_START_CFM              = 0x0003,
    AIC_MM_SET_COEX_REQ           = 0x0065, AIC_MM_SET_COEX_CFM           = 0x0066,
    AIC_ME_CONFIG_REQ             = 0x1400, AIC_ME_CONFIG_CFM             = 0x1401,   // TASK_ME = 5
    AIC_ME_CHAN_CONFIG_REQ        = 0x1402, AIC_ME_CHAN_CONFIG_CFM        = 0x1403,
    AIC_SCANU_START_REQ           = 0x1000, AIC_SCANU_START_CFM           = 0x1001,   // TASK_SCANU = 4
    AIC_SCANU_RESULT_IND          = 0x1004,
    AIC_SCANU_START_CFM_ADDTIONAL = 0x1009,   // (sic) immediate acknowledgement of SCANU_START_REQ
    AIC_MM_KEY_ADD_REQ            = 0x0024, AIC_MM_KEY_ADD_CFM            = 0x0025,
    AIC_ME_SET_CONTROL_PORT_REQ   = 0x1404, AIC_ME_SET_CONTROL_PORT_CFM   = 0x1405,
    AIC_SM_CONNECT_REQ            = 0x1800, AIC_SM_CONNECT_CFM            = 0x1801,   // TASK_SM = 6
    AIC_SM_CONNECT_IND            = 0x1802,
    AIC_SM_DISCONNECT_REQ         = 0x1803, AIC_SM_DISCONNECT_CFM         = 0x1804,
    AIC_SM_DISCONNECT_IND         = 0x1805,
};
#define AIC_TASK_MM_ID 0
#define AIC_USB_TYPE_CFG_PRINT 0x13   // firmware log text (aicwf_usb.h); payload starts at packet + 4

// Parameter sizes (sizeof the Linux request structures; unions use their largest member).
#define AIC_SZ_STACK_START_REQ   4
#define AIC_SZ_GET_MAC_ADDR_REQ  4
#define AIC_SZ_GET_FW_VER_REQ    1    // sizeof(u8), zero
#define AIC_SZ_TXPWR_LVL_REQ     95   // union { ..., txpwr_lvl_conf_v3_t (69), txpwr_lvl_conf_v4_t (95) }
#define AIC_SZ_TXPWR_LVL_ADJ_REQ 10
#define AIC_SZ_TXPWR_OFST_REQ    86
#define AIC_SZ_RF_CALIB_REQ      24
#define AIC_SZ_ME_CONFIG_REQ     112
#define AIC_SZ_ME_CHAN_CONFIG_REQ 254  // chan2G4[14] + chan5G[28] (6 bytes each) + two counts
#define AIC_SZ_MM_START_REQ      72   // phy_cfg (64, zero) + uapsd_timeout u32 + lp_clk_accuracy u16 + pad
#define AIC_SZ_COEX_REQ          16
#define AIC_SZ_ADD_IF_REQ        10
#define AIC_SZ_SCANU_START_REQ   376
#define AIC_SZ_SM_CONNECT_REQ    320
#define AIC_SZ_KEY_ADD_REQ       44
#define AIC_SZ_CTRL_PORT_REQ     2
#define AIC_SZ_SM_DISCONNECT_REQ 4

// ---------------------------------------------------------------- user config (aic_userconfig_8800d80.txt)
// Raw images of the Linux structures, so builders can copy them byte for byte.
typedef struct {
    uint8_t v3[69];       // txpwr_lvl_conf_v3_t: enable, 11b_11ag_2g4[12], 11n_11ac_2g4[10], 11ax_2g4[12],
                          //                      11a_5g[12], 11n_11ac_5g[10], 11ax_5g[12]   (s8 values)
    uint8_t adj[10];      // txpwr_lvl_adj_conf_t: enable, adj_2g4[3], adj_5g[6]
    uint8_t ofst2x[28];   // txpwr_ofst2x_conf_t: enable, tbl_2g4[3][3], tbl_5g[3][6]
    uint8_t loss[4];      // txpwr_loss_conf_t: enable_2g4, value_2g4, enable_5g, value_5g
    uint8_t xtal[3];      // xtal_cap_conf_t: enable, xtal_cap, xtal_cap_fine
} AicUserConfig;

void aic_userconfig_defaults(AicUserConfig *uc);   // userconfig_info initializer
// rwnx_plat_userconfig_parsing: "key=value" lines, '#' comments; only lines ending in \n or \r count; value
// characters other than '-' and digits are skipped; unknown keys are ignored. Returns the number of keys applied.
int aic_userconfig_parse(AicUserConfig *uc, const char *buf, size_t len);

// ---------------------------------------------------------------- request parameters
void aic_fmac_stack_start_params(uint8_t out[AIC_SZ_STACK_START_REQ], uint8_t on, uint8_t efuse_valid,
                                 uint8_t set_vendor_info, uint8_t fwtrace_redir);
#define AIC_D80_STACK_SET_VENDOR_INFO 0x20   // CO_BIT(5) for D81 / D80 with USE_5G
void aic_fmac_get_mac_params(uint8_t out[AIC_SZ_GET_MAC_ADDR_REQ]);          // {u32 get = 1}

// Each returns the parameter length to send, or 0 if Linux would not send the message (feature disabled).
size_t aic_fmac_txpwr_lvl_v3_params(const AicUserConfig *uc, uint8_t out[AIC_SZ_TXPWR_LVL_REQ]);
size_t aic_fmac_txpwr_lvl_adj_params(const AicUserConfig *uc, uint8_t out[AIC_SZ_TXPWR_LVL_ADJ_REQ]);
size_t aic_fmac_txpwr_ofst2x_params(const AicUserConfig *uc, uint8_t out[AIC_SZ_TXPWR_OFST_REQ]);
// MM_SET_RF_CALIB for the D80/D81: cal_cfg 0x0f8f / 0x0f0f, param_alpha 0x0c34c008, bt_calib_param 0x264203,
// crystal trim from the user config when enabled. Always AIC_SZ_RF_CALIB_REQ bytes.
size_t aic_fmac_rf_calib_params(const AicUserConfig *uc, uint8_t out[AIC_SZ_RF_CALIB_REQ]);

// ---------------------------------------------------------------- phase 3: radio start, interface, scan
// ME_CONFIG: the HT/VHT/HE capabilities rwnx_handle_dynparams + rwnx_set_{vht,he,ht}_capa compute for
// PRODUCT_ID_AIC8800D81 with the default module parameters (1 spatial stream, 80 MHz, LDPC, STBC RX, SGI, A-MSDU,
// beamformee, power save on, antenna diversity on). Verified against the bytes the Linux code produces.
size_t aic_fmac_me_config_params(uint8_t out[AIC_SZ_ME_CONFIG_REQ]);
// ME_CHAN_CONFIG for the driver's "00" (world) regulatory domain: 2.4 GHz channels 1-14 and the 25 standard
// 5 GHz channels, none disabled, 20 dBm each (handle_channel_custom with the regdb.c "00" rules).
size_t aic_fmac_me_chan_config_params(uint8_t out[AIC_SZ_ME_CHAN_CONFIG_REQ]);
size_t aic_fmac_start_params(uint8_t out[AIC_SZ_MM_START_REQ]);          // uapsd_timeout 300, lp_clk 20 ppm
size_t aic_fmac_coex_params(uint8_t out[AIC_SZ_COEX_REQ]);               // bt_on 1, nullcts 1 (rwnx_open)
size_t aic_fmac_add_if_params(uint8_t out[AIC_SZ_ADD_IF_REQ], const uint8_t mac[6]);   // MM_STA, not P2P
// SCANU_START over every channel ME_CHAN_CONFIG enabled. `ssid` NULL or "" -> one wildcard SSID (active scan for
// any network, like `iw scan`); otherwise a directed probe for that SSID (up to 32 bytes).
size_t aic_fmac_scan_params(uint8_t out[AIC_SZ_SCANU_START_REQ], uint8_t vif_idx, const char *ssid);

int aic_fmac_channel_count_2g4(void);
int aic_fmac_channel_count_5g(void);

typedef struct {
    uint16_t length, framectrl, center_freq;
    uint8_t band, sta_idx, inst_nbr;
    int8_t rssi;               // dBm
    const uint8_t *frame;      // the beacon / probe response (802.11 header first), inside the caller's buffer
    size_t frame_len;
} AicScanResult;
typedef struct { uint8_t vif_idx, status, result_cnt; } AicScanCfm;

int aic_fmac_parse_add_if_cfm(const uint8_t *p, size_t n, uint8_t *status, uint8_t *inst_nbr);
int aic_fmac_parse_scan_result(const uint8_t *p, size_t n, AicScanResult *r);
int aic_fmac_parse_scan_cfm(const uint8_t *p, size_t n, AicScanCfm *c);

// ---------------------------------------------------------------- phase 3b: connect, keys, control port
// SM_CONNECT_REQ as rwnx_send_sm_connect_req builds it from wpa_supplicant's nl80211 connect for WPA2-PSK:
// flags CONTROL_PORT_HOST | WPA_WPA2_IN_USE (0x09) when `ie` holds an RSN element (0 for an open network), EAPOL
// as the control-port ethertype, open-system authentication, uapsd_queues = AC_VO, listen interval 0.
size_t aic_fmac_connect_params(uint8_t out[AIC_SZ_SM_CONNECT_REQ], const uint8_t *ssid, size_t ssid_len,
                               const uint8_t bssid[6], uint16_t freq, uint8_t band, uint8_t vif_idx,
                               const uint8_t *ie, size_t ie_len);
// MM_KEY_ADD_REQ: pairwise key (sta_idx = the AP's index from SM_CONNECT_IND) or group key (sta_idx 0xFF,
// key_idx = GTK index). cipher: MAC_CIPHER_CCMP = 2.
#define AIC_MAC_CIPHER_CCMP 2
size_t aic_fmac_key_add_params(uint8_t out[AIC_SZ_KEY_ADD_REQ], uint8_t vif_idx, uint8_t sta_idx, int pairwise,
                               const uint8_t *key, uint8_t key_len, uint8_t key_idx, uint8_t cipher);
size_t aic_fmac_ctrl_port_params(uint8_t out[AIC_SZ_CTRL_PORT_REQ], uint8_t sta_idx, int open);
size_t aic_fmac_disconnect_params(uint8_t out[AIC_SZ_SM_DISCONNECT_REQ], uint16_t reason, uint8_t vif_idx);

typedef struct {
    uint16_t status_code;       // 802.11 status code; 0 = success
    uint8_t bssid[6];
    uint8_t roamed, vif_idx, ap_idx, ch_idx, qos, acm;
    uint16_t assoc_req_ie_len, assoc_rsp_ie_len;
    const uint8_t *assoc_ies;   // request IEs then response IEs, inside the caller's buffer
    uint16_t aid;
    uint8_t band;
    uint16_t center_freq;
    uint8_t width;
} AicConnectInd;
int aic_fmac_parse_connect_ind(const uint8_t *p, size_t n, AicConnectInd *c);
int aic_fmac_parse_disconnect_ind(const uint8_t *p, size_t n, uint16_t *reason, uint8_t *vif_idx);

// ---------------------------------------------------------------- confirmations
typedef struct { uint8_t is_5g_support, vendor_info; } AicStackStartCfm;
typedef struct {
    uint32_t version_lmac, version_machw_1, version_machw_2, version_phy_1, version_phy_2, features;
    uint16_t max_sta_nb;
    uint8_t max_vif_nb;
} AicVersionCfm;
typedef struct { uint32_t rxgain_24g_addr, rxgain_5g_addr, txgain_24g_addr, txgain_5g_addr; } AicRfCalibCfm;

// Each returns 0, or -1 if the parameters are too short.
int aic_fmac_parse_stack_start_cfm(const uint8_t *p, size_t n, AicStackStartCfm *c);
int aic_fmac_parse_mac_cfm(const uint8_t *p, size_t n, uint8_t mac[6]);
int aic_fmac_parse_fw_version_cfm(const uint8_t *p, size_t n, char *out, size_t cap);   // NUL-terminated
int aic_fmac_parse_version_cfm(const uint8_t *p, size_t n, AicVersionCfm *c);
int aic_fmac_parse_rf_calib_cfm(const uint8_t *p, size_t n, AicRfCalibCfm *c);
// Linux treats a MAC whose first four bytes are zero as "no MAC address in efuse".
int aic_fmac_mac_valid(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
#endif
