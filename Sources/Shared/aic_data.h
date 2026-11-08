// aic_data.h — the AIC8800 FullMAC USB data path: Ethernet frames out, received frames in. Pure C, no I/O;
// unit tested in Tests/test_data.c.
//
// Ported from the AIC8800 Linux Wi-Fi driver (GPL-2.0-only), github.com/shenmintao/aic8800d80 branch legacy-mcu1
// @ bbf6cb4015c8da4e7e113a6a6282f191213e0854, drivers/aic8800/aic8800_fdrv/:
//   rwnx_tx.c    rwnx_start_xmit / rwnx_tx_push: the TX descriptor (struct hostdesc in ipc_shared.h)
//   aicwf_usb.c  aicwf_usb_bus_txdata: the 4-byte USB header, padding, zero-length-packet handling
//   rwnx_rx.c    rwnx_rxdataind_aicwf: struct hw_rxhdr (rwnx_rx.h) and the 802.11 -> 802.3 conversion
// Layouts were measured by compiling the driver's own structures. Copyright (C) AICSemi 2018-2020; Copyright (C)
// RivieraWaves 2012-2019. Rewritten for macOS user space, 2026. GPL-2.0-only; see NOTICE.md.
//
// TX (bulk OUT 0x01, the data endpoint):
//   [0..1] total length incl. this header (12 bits)  [2] 0x01 (data)  [3] 0
//   [4..31] struct hostdesc (28 bytes): packet_len, flags_ext, status_desc_addr, eth_dest, eth_src, ethertype
//           (network order), ac, tid, vif_idx, staid, flags
//   [32..]  the Ethernet payload (the 14-byte Ethernet header is dropped; its fields are in the descriptor)
//   EAPOL frames ask the firmware for a TX confirmation (status_desc_addr = BIT(31) | index) and are padded to 4.
//   A transfer that is a multiple of 512 bytes gets one extra zero byte (not counted in the length field), as
//   the driver does when it doesn't rely on zero-length packets.
//
// RX (bulk IN, data packets, i.e. type byte without the 0x10 config bit):
//   [0..55] struct hw_rxhdr: frame length (bytes 0-1), decryption status (byte 36 bits 2-4), RSSI (byte 17),
//           flags (byte 48: bit0 A-MSDU, bit1 802.11 MPDU for the host, bit5 reorder, bit6 upload), vif (49),
//           sta (50), dst sta (51)
//   [56..59] padding    [60..] the 802.11 frame (decrypted by the chip; CCMP header still present)

#ifndef AIC_DATA_H
#define AIC_DATA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_TX_HDR_LEN     4
#define AIC_HOSTDESC_LEN   28
#define AIC_RX_HDR_LEN     60      // hw_rxhdr (56) + 4 bytes of padding
#define AIC_ETHERTYPE_EAPOL 0x888E
#define AIC_USB_TYPE_DATA_TX 0x01

// Builds one data transfer from an Ethernet frame (dst, src, ethertype, payload). `tid` 0..7 (the AC is derived
// as rwnx_tid2hwq does). `cfm_idx` is used only for EAPOL frames. Returns the number of bytes to send (may be
// one more than the length field), or 0 if the frame is shorter than 14 bytes, too long, or `cap` is too small.
size_t aic_data_build_tx(uint8_t *out, size_t cap, const uint8_t *eth, size_t eth_len, uint8_t vif_idx,
                         uint8_t sta_idx, uint8_t tid, uint8_t cfm_idx);
int aic_tid_to_ac(uint8_t tid);   // 0 BK, 1 BE, 2 VI, 3 VO

// TX confirmation (bulk IN packet type 0x12, USB_TYPE_CFG_DATA_CFM; aicwf_usb_host_tx_cfm_handler): after the
// 4-byte packet header, u32 status (union rwnx_hw_txstatus: bit0 tx_done, bit1 retry_required, bit2
// sw_retry_required, bit3 acknowledged) and u32 the index we put in status_desc_addr. Returns 0 or -1.
#define AIC_TXST_DONE  0x1u
#define AIC_TXST_ACKED 0x8u
int aic_data_parse_tx_cfm(const uint8_t *pkt, size_t pkt_len, uint32_t *status, uint32_t *idx);

typedef struct {
    uint16_t frame_len;      // 802.11 frame length (hw_rxhdr.hwvect.len)
    uint8_t decr_status;     // 0 none, 1 WEP, 2 TKIP, 3 CCMP-128, ...
    int8_t rssi;             // rx_vect1.rssi1
    uint8_t is_amsdu, is_80211_mpdu, need_reord, upload;
    uint8_t vif_idx, sta_idx, dst_idx;
    uint16_t prim20_freq;
    const uint8_t *frame;    // 802.11 frame, inside the caller's buffer
} AicRxInfo;

// Parses a data packet as returned by aic_rx_next (pkt = its start, pkt_len = bytes available). Returns 0 or -1.
int aic_data_parse_rx(const uint8_t *pkt, size_t pkt_len, AicRxInfo *ri);

// Calls `cb` for each Ethernet frame (dst, src, ethertype, payload) carried by a received 802.11 data frame:
// one frame, or every A-MSDU subframe. `scratch` (>= 14 + frame length) holds the rebuilt frame for the call.
// Returns the number of frames delivered, or -1 if the frame isn't a data frame the station should see.
typedef void (*AicEthCb)(void *ctx, const uint8_t *eth, size_t len);
int aic_data_rx_to_eth(const AicRxInfo *ri, uint8_t *scratch, size_t scratch_len, AicEthCb cb, void *ctx);

#ifdef __cplusplus
}
#endif
#endif
