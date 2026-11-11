// aic_data.c — see aic_data.h. Ported from the AIC8800 Linux Wi-Fi driver (GPL-2.0-only); Copyright (C) AICSemi
// 2018-2020; Copyright (C) RivieraWaves 2012-2019. Differences from the original: no socket buffers, every
// length is bounds-checked, QoS is detected from the frame subtype (the original treats every data frame as
// QoS), and 4-address frames are rejected rather than half-handled.

#include "aic_data.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// rwnx_tid2hwq (rwnx_main.c): TID -> hardware queue (RWNX_HWQ_BK 0, BE 1, VI 2, VO 3).
static const uint8_t kTid2Hwq[8] = {1, 0, 0, 1, 2, 2, 3, 3};

int aic_tid_to_ac(uint8_t tid) { return tid < 8 ? kTid2Hwq[tid] : 3; }

size_t aic_data_build_tx(uint8_t *out, size_t cap, const uint8_t *eth, size_t eth_len, uint8_t vif_idx,
                         uint8_t sta_idx, uint8_t tid, uint8_t cfm_idx) {
    if (!out || !eth || eth_len < 14) return 0;
    size_t payload = eth_len - 14;
    int need_cfm = eth[12] == 0x88 && eth[13] == 0x8E;   // EAPOL (aicwf_usb_bus_txdata's need_cfm path)
    size_t len = AIC_TX_HDR_LEN + AIC_HOSTDESC_LEN + payload;
    if (need_cfm) len = (len + 3) & ~(size_t)3;            // TX_ALIGNMENT, counted in the length field
    if (len > 0x0fff) return 0;                            // 12-bit length field
    size_t xfer = len + ((len % 512) == 0 ? 1 : 0);        // no zero-length packet: one extra byte instead
    if (xfer > cap) return 0;
    memset(out, 0, xfer);
    put16(out, (uint16_t)len);
    out[1] &= 0x0f;
    out[2] = AIC_USB_TYPE_DATA_TX;
    out[3] = 0;
    uint8_t *d = out + AIC_TX_HDR_LEN;                     // struct hostdesc
    put16(d + 0, (uint16_t)payload);                       // packet_len
    put16(d + 2, 0);                                       // flags_ext
    put32(d + 4, need_cfm ? (0x80000000u | cfm_idx) : 0); // status_desc_addr
    memcpy(d + 8, eth, 6);                                 // eth_dest_addr
    memcpy(d + 14, eth + 6, 6);                            // eth_src_addr
    d[20] = eth[12];                                       // ethertype, network byte order (h_proto)
    d[21] = eth[13];
    d[22] = (uint8_t)aic_tid_to_ac(tid);                   // ac = hardware queue
    d[23] = tid;
    d[24] = vif_idx;
    d[25] = sta_idx;
    put16(d + 26, 0);                                      // flags
    memcpy(out + AIC_TX_HDR_LEN + AIC_HOSTDESC_LEN, eth + 14, payload);
    return xfer;
}

int aic_data_parse_rx(const uint8_t *pkt, size_t pkt_len, AicRxInfo *ri) {
    if (!pkt || !ri || pkt_len < AIC_RX_HDR_LEN) return -1;
    memset(ri, 0, sizeof *ri);
    ri->frame_len = get16(pkt);
    ri->decr_status = (uint8_t)((pkt[36] >> 2) & 7);
    ri->rssi = (int8_t)pkt[17];
    ri->is_amsdu = pkt[48] & 0x01;
    ri->is_80211_mpdu = (pkt[48] >> 1) & 1;
    ri->need_reord = (pkt[48] >> 5) & 1;
    ri->upload = (pkt[48] >> 6) & 1;
    ri->vif_idx = pkt[49];
    ri->sta_idx = pkt[50];
    ri->dst_idx = pkt[51];
    ri->prim20_freq = get16(pkt + 42);
    if ((size_t)ri->frame_len > pkt_len - AIC_RX_HDR_LEN) return -1;
    ri->frame = pkt + AIC_RX_HDR_LEN;
    return 0;
}

static int sec_hdr_len(uint8_t decr_status) {
    switch (decr_status) {
        case 1: return 4;                       // WEP
        case 2: case 3: case 4: case 5: case 6: return 8;   // TKIP, CCMP-128/256, GCMP-128/256
        case 7: return 18;                      // WAPI
        default: return 0;
    }
}

static int deliver(uint8_t *scratch, size_t scratch_len, const uint8_t *da, const uint8_t *sa, const uint8_t *type,
                   const uint8_t *payload, size_t plen, AicEthCb cb, void *ctx) {
    if (14 + plen > scratch_len) return 0;
    memcpy(scratch, da, 6);
    memcpy(scratch + 6, sa, 6);
    scratch[12] = type[0];
    scratch[13] = type[1];
    memcpy(scratch + 14, payload, plen);
    if (cb) cb(ctx, scratch, 14 + plen);
    return 1;
}

int aic_data_rx_to_eth(const AicRxInfo *ri, uint8_t *scratch, size_t scratch_len, AicEthCb cb, void *ctx) {
    const uint8_t *f = ri->frame;
    size_t n = ri->frame_len;
    if (!f || n < 24 || ri->is_80211_mpdu) return -1;
    uint8_t fc0 = f[0], fc1 = f[1];
    if (((fc0 >> 2) & 3) != 2) return -1;                  // not a data frame
    uint8_t subtype = fc0 >> 4;
    if (subtype & 0x4) return 0;                           // (QoS) null data: no payload
    uint8_t ds = fc1 & 3;
    if (ds == 3) return -1;                                // 4-address frames: not for a station
    size_t hdr = 24;
    int qos = (subtype & 0x8) != 0, amsdu = 0;
    if (qos) {
        amsdu = (f[24] & 0x80) != 0;                       // QoS control: A-MSDU present
        hdr += 2;
        if (fc1 & 0x80) hdr += 4;                          // +HTC (order bit on a QoS frame)
    }
    const uint8_t *da, *sa;
    if (ds == 2) { da = f + 4; sa = f + 16; }              // from DS: DA = addr1, SA = addr3
    else if (ds == 1) { da = f + 16; sa = f + 10; }        // to DS: DA = addr3, SA = addr2
    else { da = f + 4; sa = f + 10; }                      // IBSS / TDLS
    size_t off = hdr + (size_t)sec_hdr_len(ri->decr_status);
    if (off > n) return -1;
    const uint8_t *body = f + off;
    size_t blen = n - off;
    // rwnx_rxdataind_aicwf: some routers (NETGEAR R7000) set the A-MSDU bit on plain frames; an LLC header
    // (AA AA ...) with an ethertype-sized byte at offset 6 means "not really an A-MSDU".
    if (amsdu && blen >= 8 && body[0] == 0xAA && body[1] == 0xAA && body[6] > 0x06) amsdu = 0;
    if (!amsdu) {
        if (blen < 8) return -1;                           // LLC/SNAP (6) + ethertype (2)
        return deliver(scratch, scratch_len, da, sa, body + 6, body + 8, blen - 8, cb, ctx);
    }
    int count = 0;
    size_t p = 0;
    while (p + 14 <= blen) {                               // subframe: DA, SA, length (BE), LLC + payload
        uint16_t sl = (uint16_t)((body[p + 12] << 8) | body[p + 13]);
        if (sl < 8 || p + 14 + sl > blen) break;
        const uint8_t *sub = body + p + 14;
        count += deliver(scratch, scratch_len, body + p, body + p + 6, sub + 6, sub + 8, sl - 8, cb, ctx);
        p += 14 + sl;
        p = (p + 3) & ~(size_t)3;                          // subframes are padded to 4 bytes
    }
    return count;
}

int aic_data_parse_tx_cfm(const uint8_t *pkt, size_t pkt_len, uint32_t *status, uint32_t *idx) {
    if (!pkt || pkt_len < 12 || (pkt[2] & 0x7f) != 0x12) return -1;
    *status = (uint32_t)pkt[4] | ((uint32_t)pkt[5] << 8) | ((uint32_t)pkt[6] << 16) | ((uint32_t)pkt[7] << 24);
    *idx = (uint32_t)pkt[8] | ((uint32_t)pkt[9] << 8) | ((uint32_t)pkt[10] << 16) | ((uint32_t)pkt[11] << 24);
    return 0;
}

size_t aic_data_build_mgmt_tx(uint8_t *out, size_t cap, const uint8_t *frame, size_t flen, uint8_t vif_idx) {
    if (!out || !frame || flen < 24) return 0;
    size_t len = AIC_TX_HDR_LEN + AIC_HOSTDESC_LEN + flen;
    if (len > 0x0fff) return 0;
    size_t xfer = len + ((len % 512) == 0 ? 1 : 0);
    if (xfer > cap) return 0;
    memset(out, 0, xfer);
    put16(out, (uint16_t)len);
    out[1] &= 0x0f;
    out[2] = AIC_USB_TYPE_DATA_TX;
    uint8_t *d = out + AIC_TX_HDR_LEN;
    put16(d + 0, (uint16_t)flen);               // packet_len
    d[22] = 3;                                  // ac: RWNX_HWQ_VO (the vif's "unknown station" queue)
    d[23] = 0xFF;                               // tid
    d[24] = vif_idx;
    d[25] = 0xFF;                               // staid
    put16(d + 26, 0x0008);                      // flags: TXU_CNTRL_MGMT
    memcpy(d + AIC_HOSTDESC_LEN, frame, flen);
    return xfer;
}
