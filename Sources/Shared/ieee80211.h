// ieee80211.h — 802.11 frame parsing and building that doesn't depend on any chip or OS.
// Pure C11. Unit tested in Tests/test_wifi.c.

#ifndef MACWIFI_IEEE80211_H
#define MACWIFI_IEEE80211_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_ETH_ALEN 6

// Frame control (first two bytes, little-endian on air)
#define WIFI_FC_TYPE(fc)      (((fc) >> 2) & 0x3)
#define WIFI_FC_SUBTYPE(fc)   (((fc) >> 4) & 0xF)
#define WIFI_FC_TODS          0x0100
#define WIFI_FC_FROMDS        0x0200
#define WIFI_FC_PROTECTED     0x4000
#define WIFI_FC_ORDER         0x8000
#define WIFI_TYPE_MGMT        0
#define WIFI_TYPE_CTRL        1
#define WIFI_TYPE_DATA        2
#define WIFI_STYPE_ASSOC_REQ  0x0
#define WIFI_STYPE_ASSOC_RESP 0x1
#define WIFI_STYPE_PROBE_REQ  0x4
#define WIFI_STYPE_PROBE_RESP 0x5
#define WIFI_STYPE_BEACON     0x8
#define WIFI_STYPE_DISASSOC   0xA
#define WIFI_STYPE_AUTH       0xB
#define WIFI_STYPE_DEAUTH     0xC

typedef enum {
    WIFI_SEC_OPEN = 0,
    WIFI_SEC_WEP,
    WIFI_SEC_WPA1,          // WPA (TKIP era) only
    WIFI_SEC_WPA2_PSK,
    WIFI_SEC_WPA2_WPA3,     // transition mode: PSK and SAE both offered
    WIFI_SEC_WPA3_SAE,      // SAE only
    WIFI_SEC_ENTERPRISE,    // 802.1X
} WifiSecurity;

typedef struct {
    uint8_t  bssid[WIFI_ETH_ALEN];
    char     ssid[33];          // NUL-terminated; may contain any bytes (hidden networks: empty)
    uint8_t  ssid_len;
    int      channel;           // from DS Parameter Set, else HT Operation primary channel, else 0
    uint16_t beacon_interval;   // TUs
    uint16_t capability;
    WifiSecurity security;
    bool     rsn_ccmp;          // RSN offers CCMP as a pairwise cipher (what we support)
    bool     rsn_pmf_required;  // MFPR bit set
    bool     rsn_pmf_capable;   // MFPC bit set
    bool     rsn_group_ccmp;    // group cipher is CCMP (00-0F-AC:4)
    bool     rsn_akm_psk;       // plain WPA2-PSK (AKM 00-0F-AC:2) offered; what `connect` uses
    bool     rsn_akm_sae;       // WPA3-Personal SAE (AKM 00-0F-AC:8) offered
    bool     rsn_mgmt_bip_cmac; // group management cipher is BIP-CMAC-128 (stated or by default)
    bool     sae_h2e_only;      // BSS membership selector 123: SAE hash-to-element only
    bool     sae_h2e;           // RSNXE bit 5: the AP supports SAE hash-to-element
    bool     ht, vht, he;
    uint8_t  rsn_ie[2 + 255];   // the RSN element as sent (id, length, body); rsn_ie_len 0 if absent
    uint16_t rsn_ie_len;
} WifiBss;

const char *wifi_security_name(WifiSecurity s);

// Parse a beacon or probe response (full 802.11 frame, no FCS). Returns 0, or -1 if not a valid one.
int wifi_parse_bss(const uint8_t *frame, size_t len, WifiBss *out);

// 5 GHz / 2.4 GHz channel <-> center frequency (MHz). 0 if unknown.
int wifi_channel_to_mhz(int channel);

// ---------------------------------------------------------------- Ethernet <-> 802.11 data (station mode)
// The OS sees an Ethernet interface; the driver converts at the edge, exactly as Linux mac80211 does.
//
// eth -> 802.11: To-DS data frame, A1 = BSSID, A2 = SA (must be our MAC), A3 = DA, then RFC 1042 / 802.1H
// LLC/SNAP + the ethertype + payload. `qos` adds a QoS Control field (TID 0) and uses subtype QoS Data,
// which APs that negotiated WMM/HT expect. Returns the frame length, or 0 if `cap` is too small / bad input.
size_t wifi_eth_to_80211(const uint8_t *eth, size_t eth_len, const uint8_t bssid[WIFI_ETH_ALEN], bool qos,
                         uint8_t *out, size_t cap);

// 802.11 -> eth: accepts From-DS data and QoS-data frames that are already decrypted (Protected bit clear,
// or `assume_decrypted` with IV/MIC already stripped by the hardware). Returns the Ethernet frame length,
// 0 for frames that carry no payload (null data), or -1 for anything that isn't convertible.
long wifi_80211_to_eth(const uint8_t *frame, size_t len, bool assume_decrypted, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
