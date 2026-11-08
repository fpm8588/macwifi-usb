// aic_msg.c — see aic_msg.h. Original code (MIT).
#include "aic_msg.h"

#include <string.h>

#include "aic8800.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

size_t aic_build_cmd(uint8_t *out, size_t cap, uint16_t id, uint16_t dest_task, const void *params,
                     uint16_t param_len) {
    size_t msg_len = 8u + param_len;
    size_t total = msg_len + 8u;
    if (!out || total > cap || msg_len + 4 > 0x0fff) return 0;   // 12-bit length field
    memset(out, 0, total);
    put16(out, (uint16_t)((msg_len + 4) & 0x0fff));
    out[2] = AIC_USB_TYPE_CFG_CMD_RSP;
    out[3] = 0;
    // out[4..7]: dummy word, zero
    put16(out + 8, id);
    put16(out + 10, dest_task);
    put16(out + 12, AIC_DRV_TASK_ID);
    put16(out + 14, param_len);
    if (param_len) memcpy(out + 16, params, param_len);
    return total;
}

size_t aic_build_mem_read(uint8_t *out, size_t cap, uint32_t addr) {
    uint8_t p[4];
    put32(p, addr);
    return aic_build_cmd(out, cap, AIC_DBG_MEM_READ_REQ, AIC_TASK_DBG, p, sizeof p);
}

size_t aic_build_mem_write(uint8_t *out, size_t cap, uint32_t addr, uint32_t data) {
    uint8_t p[8];
    put32(p, addr);
    put32(p + 4, data);
    return aic_build_cmd(out, cap, AIC_DBG_MEM_WRITE_REQ, AIC_TASK_DBG, p, sizeof p);
}

size_t aic_build_mem_mask_write(uint8_t *out, size_t cap, uint32_t addr, uint32_t mask, uint32_t data) {
    uint8_t p[12];
    put32(p, addr);
    put32(p + 4, mask);
    put32(p + 8, data);
    return aic_build_cmd(out, cap, AIC_DBG_MEM_MASK_WRITE_REQ, AIC_TASK_DBG, p, sizeof p);
}

size_t aic_build_block_write(uint8_t *out, size_t cap, uint32_t addr, const uint8_t *data, uint32_t len) {
    if (!data || len == 0 || len > AIC_BLOCK_WRITE_MAX) return 0;
    uint8_t p[8 + AIC_BLOCK_WRITE_MAX];
    memset(p, 0, sizeof p);
    put32(p, addr);
    put32(p + 4, len);
    memcpy(p + 8, data, len);
    // Like the Linux driver, always send the whole struct (8 + 1024 bytes); `memsize` says how much is valid.
    return aic_build_cmd(out, cap, AIC_DBG_MEM_BLOCK_WRITE_REQ, AIC_TASK_DBG, p, (uint16_t)sizeof p);
}

size_t aic_build_start_app(uint8_t *out, size_t cap, uint32_t boot_addr, uint32_t boot_type) {
    uint8_t p[8];
    put32(p, boot_addr);
    put32(p + 4, boot_type);
    return aic_build_cmd(out, cap, AIC_DBG_START_APP_REQ, AIC_TASK_DBG, p, sizeof p);
}

int aic_rx_next(const uint8_t *buf, size_t len, size_t *offset, AicRxPacket *pkt) {
    if (!buf || !offset || !pkt) return -1;
    size_t off = *offset;
    if (off + 4 > len) return 0;
    uint16_t plen = get16(buf + off);
    if (plen == 0) return 0;                                       // no more packets (aicwf_another_ptk)
    memset(pkt, 0, sizeof *pkt);
    pkt->type = buf[off + 2];
    pkt->packet = buf + off;
    if ((pkt->type & AIC_USB_TYPE_CFG) == AIC_USB_TYPE_CFG) {
        size_t adj = ((size_t)plen + (AIC_RX_ALIGNMENT - 1)) & ~(size_t)(AIC_RX_ALIGNMENT - 1);
        size_t occupied = adj + 4;
        if (off + 4 + plen > len) return -1;
        pkt->packet_len = occupied;
        if ((pkt->type & 0x7f) == AIC_USB_TYPE_CFG_CMD_RSP) {
            if (plen < 12) return -1;                              // id, dest, src, param_len, pattern
            const uint8_t *m = buf + off + 4;
            pkt->is_msg = 1;
            pkt->id = get16(m);
            pkt->dest = get16(m + 2);
            pkt->src = get16(m + 4);
            pkt->param_len = get16(m + 6);
            if (12u + pkt->param_len > plen) return -1;
            pkt->params = m + 12;
        }
        *offset = off + (occupied < len - off ? occupied : len - off);
    } else {
        // Data packets follow each other with no padding: aicwf_process_rxframes advances by
        // pkt_len + RX_HWHRD_LEN exactly (only config packets are rounded up to RX_ALIGNMENT).
        size_t aggr = (size_t)plen + AIC_RX_HW_HDR_LEN;
        if (off + aggr > len) return -1;
        pkt->packet_len = aggr;
        *offset = off + aggr;
    }
    return 1;
}

int aic_parse_mem_read_cfm(const AicRxPacket *pkt, uint32_t *addr, uint32_t *data) {
    if (!pkt || !pkt->is_msg || pkt->id != AIC_DBG_MEM_READ_CFM || pkt->param_len < 8) return -1;
    if (addr) *addr = aic_le32(pkt->params);
    if (data) *data = aic_le32(pkt->params + 4);
    return 0;
}
