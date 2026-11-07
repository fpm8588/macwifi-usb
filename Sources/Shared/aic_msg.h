// aic_msg.h — framing of AIC8800 USB command/response messages. Pure C11, unit tested. Original code (MIT);
// the wire format is a fact taken from the Linux driver (see aic8800.h for sources).
//
// Host -> device command (bulk OUT):
//   [0..1] (msg_len + 4) & 0x0fff, little-endian      msg_len = 8 + param_len
//   [2]    0x11 (CFG_CMD_RSP)   [3] 0
//   [4..7] 0 (dummy word)
//   [8..]  u16 id, u16 dest_task, u16 src_task (100 = driver), u16 param_len, params...
//   Total transfer: msg_len + 8 bytes.
//
// Device -> host stream (bulk IN), possibly several packets per transfer, each starting with:
//   [0..1] len (LE)   [2] type
//   type & 0x10 (config): message at +4: u16 id, u16 dest, u16 src, u16 param_len, u32 pattern, params...;
//                         packet occupies roundup(len, 4) + 4 bytes
//   otherwise (data):     packet occupies roundup(len + 60, 4) bytes (60-byte RX hardware header)

#ifndef AIC_MSG_H
#define AIC_MSG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Builds a command. Returns the number of bytes to send, or 0 if `cap` is too small / params too long.
size_t aic_build_cmd(uint8_t *out, size_t cap, uint16_t id, uint16_t dest_task, const void *params,
                     uint16_t param_len);

// Convenience builders for the debug task (used by the ROM before any firmware is loaded).
size_t aic_build_mem_read(uint8_t *out, size_t cap, uint32_t addr);
size_t aic_build_mem_write(uint8_t *out, size_t cap, uint32_t addr, uint32_t data);
size_t aic_build_mem_mask_write(uint8_t *out, size_t cap, uint32_t addr, uint32_t mask, uint32_t data);
size_t aic_build_block_write(uint8_t *out, size_t cap, uint32_t addr, const uint8_t *data, uint32_t len);
size_t aic_build_start_app(uint8_t *out, size_t cap, uint32_t boot_addr, uint32_t boot_type);

typedef struct {
    uint8_t  type;          // packet type byte
    int      is_msg;        // 1 for config/message packets
    uint16_t id, dest, src, param_len;
    const uint8_t *params;  // points into the input buffer (valid while it is)
    const uint8_t *packet;  // start of the packet
    size_t   packet_len;    // bytes the packet occupies in the stream (including alignment)
} AicRxPacket;

// Iterates packets in a bulk-IN buffer. Call with *offset = 0, then repeatedly.
// Returns 1 and fills `pkt` for each packet, 0 at the end of the buffer, -1 if the stream is malformed.
int aic_rx_next(const uint8_t *buf, size_t len, size_t *offset, AicRxPacket *pkt);

// Parses DBG_MEM_READ_CFM params. Returns 0 or -1.
int aic_parse_mem_read_cfm(const AicRxPacket *pkt, uint32_t *addr, uint32_t *data);

static inline uint32_t aic_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef __cplusplus
}
#endif
#endif
