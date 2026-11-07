// mt76x02_frames.c — see mt76x02_frames.h. Original code (MIT); formats from Linux mt76.
#include "mt76x02_frames.h"

#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

size_t mt76_80211_hdrlen(const uint8_t *f, size_t len) {
    if (len < 2) return 0;
    uint16_t fc = le16(f);
    unsigned type = (fc >> 2) & 3, st = (fc >> 4) & 0xF;
    size_t h;
    switch (type) {
        case 0: h = 24; break;                                   // management
        case 1:                                                  // control
            h = (st == 0xC || st == 0xD) ? 10 : 16;              // CTS/ACK : RTS/BAR/BA/PS-Poll
            break;
        case 2:                                                  // data
            h = 24;
            if ((fc & 0x0300) == 0x0300) h += 6;                 // 4-address
            if (st & 0x8) {                                      // QoS
                h += 2;
                if (fc & 0x8000) h += 4;                         // HT Control
            }
            break;
        default: return 0;
    }
    if (type == 0 && (fc & 0x8000)) h += 4;                      // +HTC on management frames
    return h <= len ? h : 0;
}

long mt76x2u_rx_unpack(const uint8_t *buf, size_t len, Mt76RxInfo *info, uint8_t *out, size_t cap) {
    if (!buf || !info || !out) return -1;
    if (len < MT_DMA_HDR_LEN + MT_RX_RXWI_LEN + MT_FCE_INFO_LEN) return -1;
    uint16_t dma_len = le16(buf);                                // mt76u_get_rx_entry_len
    if (!dma_len || (dma_len & 3) || (size_t)dma_len + MT_DMA_HDR_LEN > len || dma_len < MT_RX_RXWI_LEN) return -1;

    const uint8_t *w = buf + MT_DMA_HDR_LEN;
    memset(info, 0, sizeof *info);
    info->rxinfo = le32(w);
    info->ctl = le32(w + 4);
    info->tid_sn = le16(w + 8);
    info->rate = le16(w + 10);
    for (int i = 0; i < 4; i++) info->rssi[i] = (int8_t)w[12 + i];
    info->wcid = (uint8_t)(info->ctl & 0xFF);
    info->mpdu_len = (uint16_t)((info->ctl >> 16) & 0x3FFF);
    info->phy = (info->rate >> 13) & 7;
    info->mcs = info->rate & 0x3F;
    info->bw = (info->rate >> 7) & 3;
    info->decrypted = (info->rxinfo & MT_RXINFO_DECRYPT) != 0;
    info->crc_error = (info->rxinfo & MT_RXINFO_CRCERR) != 0;
    info->icv_error = (info->rxinfo & MT_RXINFO_ICVERR) != 0;
    info->mic_error = (info->rxinfo & MT_RXINFO_MICERR) != 0;
    info->unicast = (info->rxinfo & MT_RXINFO_UNICAST) != 0;

    const uint8_t *f = w + MT_RX_RXWI_LEN;
    size_t avail = (size_t)dma_len - MT_RX_RXWI_LEN;            // bytes of frame data inside the DMA length
    size_t l2pad = (info->rxinfo & MT_RXINFO_L2PAD) ? 2 : 0;
    size_t mpdu = info->mpdu_len;
    if (mpdu == 0 || mpdu + l2pad > avail) return -1;

    size_t hdr = mt76_80211_hdrlen(f, mpdu);
    if (!hdr) return -1;
    size_t iv = 0;
    unsigned pn_len = (info->rxinfo >> MT_RXINFO_PN_LEN_SHIFT) & 7;
    if (info->decrypted && pn_len && !(info->rxinfo & MT_RXINFO_FRAG)) iv = (size_t)pn_len * 4;  // as Linux does
    if (hdr + iv > mpdu) return -1;

    size_t body = mpdu - hdr - iv;
    size_t outlen = hdr + body;
    if (outlen > cap) return -1;
    memcpy(out, f, hdr);
    memcpy(out + hdr, f + hdr + l2pad + iv, body);
    info->frame_len = outlen;
    return (long)outlen;
}

size_t mt76x2u_tx_pack(const uint8_t *frame, size_t len, const Mt76TxParams *p, uint8_t *out, size_t cap) {
    if (!frame || !p || !out || len > 0x3FFF) return 0;
    size_t hdr = mt76_80211_hdrlen(frame, len);
    if (!hdr) return 0;
    size_t pad = (hdr & 3) ? 4 - (hdr & 3) : 0;                 // mt76_insert_hdr_pad: align the body
    size_t xfer = MT_TXWI_LEN + len + pad;                      // TXWI + padded frame
    size_t xfer_aligned = (xfer + 3) & ~(size_t)3;
    size_t total = MT_TXINFO_LEN + xfer_aligned + 4;            // + trailing zero word
    if (total > cap) return 0;
    memset(out, 0, total);

    uint32_t txinfo = (uint32_t)(xfer_aligned & 0xFFFF) |
                      ((uint32_t)MT_WLAN_PORT << MT_TXD_INFO_DPORT_SHIFT) |
                      ((uint32_t)(p->qsel & 3) << MT_TXD_INFO_QSEL_SHIFT) |
                      MT_TXD_INFO_80211;
    if (!p->hw_key) txinfo |= MT_TXD_INFO_WIV;
    put32(out, txinfo);

    uint8_t *w = out + MT_TXINFO_LEN;
    uint16_t fc = le16(frame);
    unsigned type = (fc >> 2) & 3, st = (fc >> 4) & 0xF;
    uint16_t flags = 0;
    if (type == 0 && (st == 0x5 || st == 0x8)) flags |= MT_TXWI_FLAGS_TS;   // probe resp / beacon timestamps
    put16(w, flags);
    put16(w + 2, p->rate);
    w[4] = (uint8_t)((p->ack ? MT_TXWI_ACK_CTL_REQ : 0) | (p->hw_seq ? MT_TXWI_ACK_CTL_NSEQ : 0));
    w[5] = p->wcid;
    put16(w + 6, (uint16_t)len);                                // MPDU length without the pad
    // iv/eiv (8..15), aid (16), txstream (17), ctl2 (18) stay 0 unless set later by the station code
    w[19] = p->pktid;

    uint8_t *f = w + MT_TXWI_LEN;
    memcpy(f, frame, hdr);
    memcpy(f + hdr + pad, frame + hdr, len - hdr);
    return total;
}
