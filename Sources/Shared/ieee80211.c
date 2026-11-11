// ieee80211.c — see ieee80211.h. Pure C11, no OS headers. Original code (MIT).
#include "ieee80211.h"

#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

const char *wifi_security_name(WifiSecurity s) {
    switch (s) {
        case WIFI_SEC_OPEN: return "Open";
        case WIFI_SEC_WEP: return "WEP";
        case WIFI_SEC_WPA1: return "WPA";
        case WIFI_SEC_WPA2_PSK: return "WPA2";
        case WIFI_SEC_WPA2_WPA3: return "WPA2/WPA3";
        case WIFI_SEC_WPA3_SAE: return "WPA3";
        case WIFI_SEC_ENTERPRISE: return "Enterprise";
    }
    return "?";
}

int wifi_channel_to_mhz(int ch) {
    if (ch >= 1 && ch <= 13) return 2407 + 5 * ch;
    if (ch == 14) return 2484;
    if (ch >= 32 && ch <= 177) return 5000 + 5 * ch;
    return 0;
}

// RSN element body (after id/len). Fills security flags. Returns 0 or -1 if truncated.
static int parse_rsn(const uint8_t *p, size_t n, WifiBss *b) {
    if (n < 2 || le16(p) != 1) return -1;              // version 1
    size_t off = 2;
    if (off + 4 > n) {                                   // defaults per 802.11: CCMP / CCMP / PSK
        b->security = WIFI_SEC_WPA2_PSK; b->rsn_ccmp = true; b->rsn_group_ccmp = true; b->rsn_akm_psk = true;
        return 0;
    }
    b->rsn_group_ccmp = p[off] == 0x00 && p[off + 1] == 0x0F && p[off + 2] == 0xAC && p[off + 3] == 4;
    off += 4;                                            // group cipher
    bool ccmp = false, psk = false, sae = false, dot1x = false;
    if (off + 2 <= n) {
        uint16_t cnt = le16(p + off); off += 2;
        for (uint16_t i = 0; i < cnt; i++, off += 4) {
            if (off + 4 > n) return -1;
            if (p[off] == 0x00 && p[off + 1] == 0x0F && p[off + 2] == 0xAC && p[off + 3] == 4) ccmp = true;
        }
    }
    if (off + 2 <= n) {
        uint16_t cnt = le16(p + off); off += 2;
        for (uint16_t i = 0; i < cnt; i++, off += 4) {
            if (off + 4 > n) return -1;
            if (!(p[off] == 0x00 && p[off + 1] == 0x0F && p[off + 2] == 0xAC)) continue;
            if (p[off + 3] == 2) b->rsn_akm_psk = true;
            if (p[off + 3] == 8) b->rsn_akm_sae = true;
            switch (p[off + 3]) {
                case 2: case 4: case 6: psk = true; break;      // PSK, FT-PSK, PSK-SHA256
                case 8: case 9: sae = true; break;              // SAE, FT-SAE
                case 1: case 3: case 5: dot1x = true; break;    // 802.1X variants
                default: break;
            }
        }
    }
    b->rsn_mgmt_bip_cmac = true;                          // the default group management cipher when PMF is on
    if (off + 2 <= n) {
        b->rsn_pmf_required = (le16(p + off) & 0x0040) != 0;
        b->rsn_pmf_capable = (le16(p + off) & 0x0080) != 0;
        off += 2;
        if (off + 2 <= n) {                               // PMKID list, then the group management cipher
            off += 2 + (size_t)le16(p + off) * 16;
            if (off + 4 <= n)
                b->rsn_mgmt_bip_cmac = p[off] == 0x00 && p[off + 1] == 0x0F && p[off + 2] == 0xAC && p[off + 3] == 6;
        }
    }
    b->rsn_ccmp = ccmp;
    if (sae && psk) b->security = WIFI_SEC_WPA2_WPA3;
    else if (sae) b->security = WIFI_SEC_WPA3_SAE;
    else if (psk) b->security = WIFI_SEC_WPA2_PSK;
    else if (dot1x) b->security = WIFI_SEC_ENTERPRISE;
    else b->security = WIFI_SEC_WPA2_PSK;
    return 0;
}

int wifi_parse_bss(const uint8_t *f, size_t len, WifiBss *b) {
    if (!f || !b || len < 24 + 12) return -1;
    uint16_t fc = le16(f);
    if (WIFI_FC_TYPE(fc) != WIFI_TYPE_MGMT) return -1;
    unsigned st = WIFI_FC_SUBTYPE(fc);
    if (st != WIFI_STYPE_BEACON && st != WIFI_STYPE_PROBE_RESP) return -1;

    memset(b, 0, sizeof *b);
    memcpy(b->bssid, f + 16, WIFI_ETH_ALEN);           // A3 = BSSID in beacons and probe responses
    b->beacon_interval = le16(f + 24 + 8);
    b->capability = le16(f + 24 + 10);

    bool have_rsn = false, have_wpa1 = false, have_ds = false;
    size_t off = 24 + 12;
    while (off + 2 <= len) {
        uint8_t id = f[off], n = f[off + 1];
        const uint8_t *p = f + off + 2;
        if (off + 2 + n > len) return -1;               // truncated element: reject the whole frame
        switch (id) {
            case 0:                                     // SSID
                if (n > 32) return -1;
                memcpy(b->ssid, p, n);
                b->ssid[n] = '\0';
                b->ssid_len = n;
                break;
            case 1: case 50:                            // (Extended) Supported Rates: membership selectors
                for (uint8_t i = 0; i < n; i++) if (p[i] == (0x80 | 123)) b->sae_h2e_only = true;
                break;
            case 3:                                     // DS Parameter Set
                if (n >= 1) { b->channel = p[0]; have_ds = true; }
                break;
            case 45: b->ht = true; break;               // HT Capabilities
            case 61:                                    // HT Operation: primary channel
                if (n >= 1 && !have_ds) b->channel = p[0];
                break;
            case 191: b->vht = true; break;             // VHT Capabilities
            case 255:                                   // Element ID extension
                if (n >= 1 && p[0] == 35) b->he = true; // HE Capabilities
                break;
            case 48:                                    // RSN
                if (parse_rsn(p, n, b) == 0) have_rsn = true;
                memcpy(b->rsn_ie, f + off, (size_t)n + 2);
                b->rsn_ie_len = (uint16_t)(n + 2);
                break;
            case 221:                                   // Vendor: Microsoft WPA1 = 00:50:F2 type 1
                if (n >= 4 && p[0] == 0x00 && p[1] == 0x50 && p[2] == 0xF2 && p[3] == 0x01) have_wpa1 = true;
                break;
            default: break;
        }
        off += 2u + n;
    }
    if (!have_rsn) {
        if (have_wpa1) b->security = WIFI_SEC_WPA1;
        else if (b->capability & 0x0010) b->security = WIFI_SEC_WEP;   // Privacy bit, no RSN/WPA IE
        else b->security = WIFI_SEC_OPEN;
    }
    return 0;
}

// ---------------------------------------------------------------- data frames

static const uint8_t kRfc1042[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
static const uint8_t kBridgeTunnel[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0xF8};

size_t wifi_eth_to_80211(const uint8_t *eth, size_t eth_len, const uint8_t bssid[WIFI_ETH_ALEN], bool qos,
                         uint8_t *out, size_t cap) {
    if (!eth || !bssid || !out || eth_len < 14) return 0;
    uint16_t type = (uint16_t)((eth[12] << 8) | eth[13]);
    size_t hdr = qos ? 26 : 24;
    size_t payload = eth_len - 14;
    size_t llc = type >= 0x0600 ? 8 : 0;                // < 0x0600 = 802.3 length: payload already has LLC
    size_t total = hdr + llc + payload;
    if (total > cap) return 0;

    out[0] = qos ? 0x88 : 0x08;                         // data / QoS data
    out[1] = 0x01;                                      // To DS
    out[2] = out[3] = 0;                                // duration: filled by hardware
    memcpy(out + 4, bssid, 6);                          // A1 = BSSID (receiver)
    memcpy(out + 10, eth + 6, 6);                       // A2 = SA (us)
    memcpy(out + 16, eth, 6);                           // A3 = DA
    out[22] = out[23] = 0;                              // sequence control: assigned by hardware
    if (qos) { out[24] = 0; out[25] = 0; }              // QoS control: TID 0, normal ack
    uint8_t *p = out + hdr;
    if (llc) {
        memcpy(p, (type == 0x80F3 || type == 0x8137) ? kBridgeTunnel : kRfc1042, 6);  // 802.1H for AARP/IPX
        p[6] = (uint8_t)(type >> 8);
        p[7] = (uint8_t)type;
        p += 8;
    }
    memcpy(p, eth + 14, payload);
    return total;
}

long wifi_80211_to_eth(const uint8_t *f, size_t len, bool assume_decrypted, uint8_t *out, size_t cap) {
    if (!f || !out || len < 24) return -1;
    uint16_t fc = le16(f);
    if (WIFI_FC_TYPE(fc) != WIFI_TYPE_DATA) return -1;
    unsigned st = WIFI_FC_SUBTYPE(fc);
    bool is_qos = (st & 0x8) != 0;
    if (st & 0x4) return 0;                             // null / QoS null: no payload
    if ((fc & WIFI_FC_PROTECTED) && !assume_decrypted) return -1;

    bool tods = (fc & WIFI_FC_TODS) != 0, fromds = (fc & WIFI_FC_FROMDS) != 0;
    size_t hdr = (tods && fromds) ? 30 : 24;
    if (is_qos) {
        if (len < hdr + 2) return -1;
        if (f[hdr] & 0x80) return -1;                   // A-MSDU present: not supported (we never negotiate it)
        hdr += 2;
        if (fc & WIFI_FC_ORDER) hdr += 4;               // HT Control
    }
    if (len < hdr) return -1;

    const uint8_t *a1 = f + 4, *a2 = f + 10, *a3 = f + 16, *a4 = f + 24;
    const uint8_t *da, *sa;
    if (!tods && fromds) { da = a1; sa = a3; }          // AP -> station (the normal case)
    else if (tods && !fromds) { da = a3; sa = a2; }
    else if (!tods && !fromds) { da = a1; sa = a2; }    // IBSS
    else { da = a3; sa = a4; }                          // WDS / 4-address

    const uint8_t *p = f + hdr;
    size_t n = len - hdr;
    size_t outlen;
    if (n >= 8 && (memcmp(p, kRfc1042, 6) == 0 || memcmp(p, kBridgeTunnel, 6) == 0)) {
        uint16_t type = (uint16_t)((p[6] << 8) | p[7]);
        bool keep = memcmp(p, kRfc1042, 6) == 0 && (type == 0x80F3 || type == 0x8137);
        if (!keep) {
            outlen = 14 + (n - 8);
            if (outlen > cap) return -1;
            memcpy(out, da, 6); memcpy(out + 6, sa, 6);
            out[12] = p[6]; out[13] = p[7];
            memcpy(out + 14, p + 8, n - 8);
            return (long)outlen;
        }
    }
    // Not SNAP-encapsulated (or AARP/IPX in RFC 1042): emit an 802.3 frame with a length field.
    if (n > 1500) return -1;
    outlen = 14 + n;
    if (outlen > cap) return -1;
    memcpy(out, da, 6); memcpy(out + 6, sa, 6);
    out[12] = (uint8_t)(n >> 8); out[13] = (uint8_t)n;
    memcpy(out + 14, p, n);
    return (long)outlen;
}
