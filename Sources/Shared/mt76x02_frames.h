// mt76x02_frames.h — USB RX/TX buffer formats of the MT76x2U (RXWI, TXWI, DMA info words).
// Pure C11, OS-independent, unit tested. Original code; the formats are facts from Linux mt76
// (BSD-3-Clause-Clear): mt76x02_mac.h (RXWI/TXWI fields), mt76x02_dma.h (TXD info), dma.h (lengths),
// usb.c (mt76u_get_rx_entry_len), mt76x02_usb_core.c (mt76x02u_skb_dma_info, tx_prepare_skb),
// mt76x02_mac.c (mt76x02_mac_process_rx, mt76x02_mac_write_txwi).

#ifndef MT76X02_FRAMES_H
#define MT76X02_FRAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_DMA_HDR_LEN     4
#define MT_RX_RXWI_LEN     32
#define MT_FCE_INFO_LEN    4
#define MT_TXWI_LEN        20
#define MT_TXINFO_LEN      4

// RXWI rxinfo bits
#define MT_RXINFO_DATA       (1u << 1)
#define MT_RXINFO_NULL       (1u << 2)
#define MT_RXINFO_FRAG       (1u << 3)
#define MT_RXINFO_UNICAST    (1u << 4)
#define MT_RXINFO_CRCERR     (1u << 8)
#define MT_RXINFO_ICVERR     (1u << 9)
#define MT_RXINFO_MICERR     (1u << 10)
#define MT_RXINFO_AMSDU      (1u << 11)
#define MT_RXINFO_L2PAD      (1u << 14)
#define MT_RXINFO_DECRYPT    (1u << 16)
#define MT_RXINFO_PN_LEN_SHIFT 19        // 3 bits: IV length in 32-bit words
// TXD info word
#define MT_TXD_INFO_80211    (1u << 19)
#define MT_TXD_INFO_WIV      (1u << 24)  // "without IV": hardware doesn't insert an IV (no hw key)
#define MT_TXD_INFO_QSEL_SHIFT  25
#define MT_TXD_INFO_DPORT_SHIFT 27
enum { MT_QSEL_MGMT = 0, MT_QSEL_HCCA = 1, MT_QSEL_EDCA = 2, MT_QSEL_EDCA_2 = 3 };
enum { MT_WLAN_PORT = 0, MT_CPU_RX_PORT = 1, MT_CPU_TX_PORT = 2 };
// TXWI
#define MT_TXWI_FLAGS_TS        (1u << 3)
#define MT_TXWI_ACK_CTL_REQ     (1u << 0)
#define MT_TXWI_ACK_CTL_NSEQ    (1u << 1)
// rate word (shared by RXWI and TXWI)
enum { MT_PHY_CCK = 0, MT_PHY_OFDM = 1, MT_PHY_HT = 2, MT_PHY_HT_GF = 3, MT_PHY_VHT = 4 };
static inline uint16_t mt76_rate(unsigned phy, unsigned idx) { return (uint16_t)(((phy & 7u) << 13) | (idx & 0x3fu)); }
#define MT_RATE_CCK_1M    mt76_rate(MT_PHY_CCK, 0)    // 2.4 GHz management frames
#define MT_RATE_OFDM_6M   mt76_rate(MT_PHY_OFDM, 0)   // 5 GHz management frames

typedef struct {
    uint32_t rxinfo, ctl;
    uint16_t tid_sn, rate;
    int8_t   rssi[4];          // raw per-chain values; calibrated dBm needs EEPROM data (Phase 2)
    uint8_t  wcid;
    uint16_t mpdu_len;         // as reported (includes IV, excludes L2 pad)
    unsigned phy, mcs, bw;     // decoded from `rate`
    bool     decrypted, crc_error, icv_error, mic_error, unicast;
    size_t   frame_len;        // bytes written to the output frame
} Mt76RxInfo;

// Unpacks one USB RX transfer: [dma_len:4][RXWI:32][frame (+L2 pad, +IV)][...][FCE info:4].
// Writes the 802.11 frame to `out` with the L2 pad removed, and, when the hardware decrypted it, the IV
// removed too (CCMP MIC is already stripped by the hardware). Returns frame length, or -1 if malformed.
long mt76x2u_rx_unpack(const uint8_t *buf, size_t len, Mt76RxInfo *info, uint8_t *out, size_t cap);

typedef struct {
    uint8_t  wcid;      // 0xff = no station entry (e.g. before association)
    uint16_t rate;      // mt76_rate(...)
    bool     ack;       // request an ACK (false for broadcast)
    bool     hw_seq;    // let the hardware assign the sequence number
    bool     hw_key;    // a hardware key for this WCID encrypts the frame (clears WIV)
    uint8_t  qsel;      // MT_QSEL_MGMT for management/EAPOL, MT_QSEL_EDCA for data
    uint8_t  pktid;
} Mt76TxParams;

// Packs one 802.11 frame for the bulk OUT endpoint:
//   [TXINFO:4][TXWI:20][hdr][2-byte pad if hdrlen % 4][body][zero pad to 4][4 zero bytes]
// Returns the transfer length, or 0 if `cap` is too small or the frame is malformed.
size_t mt76x2u_tx_pack(const uint8_t *frame, size_t len, const Mt76TxParams *p, uint8_t *out, size_t cap);

// 802.11 header length from the frame control field (data/mgmt/ctrl), or 0 if the frame is too short.
size_t mt76_80211_hdrlen(const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif
#endif
