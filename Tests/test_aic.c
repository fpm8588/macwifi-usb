// Hardware-free tests for the AIC8800 message framing (Sources/Shared/aic_msg.c).
#include "aic8800.h"
#include "aic_msg.h"

#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_ids(void) {
    const AicId *id = aic_lookup(0xa69c, 0x5723);                    // the user's adapter, driver-CD mode
    CHECK(id && id->mode == AIC_MODE_DRIVER_CD);
    CHECK(aic_lookup(0xa69c, 0x5724) && aic_lookup(0xa69c, 0x5724)->mode == AIC_MODE_DRIVER_CD);
    id = aic_lookup(0xa69c, 0x8d80);
    CHECK(id && id->mode == AIC_MODE_WIFI && id->msg_ep == 0);
    id = aic_lookup(0xa69c, 0x8d81);
    CHECK(id && id->msg_ep == 1);
    CHECK(aic_lookup(0x0bda, 0xb851) == NULL);
    CHECK(AIC_DBG_MEM_READ_REQ == 0x400 && AIC_DBG_MEM_READ_CFM == 0x401 && AIC_DBG_MEM_BLOCK_WRITE_REQ == 0x40B);
    CHECK(AIC_DBG_START_APP_REQ == 0x40D && AIC_DBG_MEM_MASK_WRITE_REQ == 0x411);
    CHECK(strcmp(aic_chip_rev_name(0x03), "U02") == 0);
}

static void test_build(void) {
    uint8_t b[2048];
    size_t n = aic_build_mem_read(b, sizeof b, AIC_ADDR_CHIP_ID);
    // msg_len = 8 + 4 = 12 -> header length 16, total 20
    CHECK(n == 20);
    CHECK(b[0] == 16 && b[1] == 0 && b[2] == 0x11 && b[3] == 0);
    CHECK(b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0);
    CHECK(b[8] == 0x00 && b[9] == 0x04);                             // id 0x400 LE
    CHECK(b[10] == 1 && b[11] == 0);                                 // dest TASK_DBG
    CHECK(b[12] == 100 && b[13] == 0);                               // src driver task
    CHECK(b[14] == 4 && b[15] == 0);                                 // param_len
    CHECK(aic_le32(b + 16) == 0x40500000u);

    n = aic_build_mem_mask_write(b, sizeof b, 0x40000000, 0xff, 0x12);
    CHECK(n == 8 + 8 + 12 && b[8] == 0x11 && b[9] == 0x04);

    uint8_t data[300];
    for (int i = 0; i < 300; i++) data[i] = (uint8_t)i;
    n = aic_build_block_write(b, sizeof b, 0x00100000, data, 300);
    CHECK(n == 8 + 8 + 1032);                                        // always the full struct
    CHECK(b[0] == (uint8_t)((8 + 1032 + 4) & 0xff) && b[1] == (uint8_t)(((8 + 1032 + 4) >> 8) & 0x0f));
    CHECK(aic_le32(b + 16) == 0x00100000 && aic_le32(b + 20) == 300);
    CHECK(memcmp(b + 24, data, 300) == 0 && b[24 + 300] == 0);
    CHECK(aic_build_block_write(b, sizeof b, 0, data, 0) == 0);
    CHECK(aic_build_block_write(b, 100, 0, data, 10) == 0);         // too small
    CHECK(aic_build_start_app(b, sizeof b, 0x00110000, 0) == 24 && b[8] == 0x0D && b[9] == 0x04);
}

// Builds a device->host config packet carrying a message, returns bytes written.
static size_t make_msg(uint8_t *o, uint16_t id, const uint8_t *params, uint16_t plen) {
    uint16_t len = (uint16_t)(12 + plen);
    memset(o, 0, 4 + ((len + 3u) & ~3u));
    o[0] = (uint8_t)len; o[1] = (uint8_t)(len >> 8); o[2] = 0x11; o[3] = 0;
    uint8_t *m = o + 4;
    m[0] = (uint8_t)id; m[1] = (uint8_t)(id >> 8);
    m[2] = 100; m[4] = 1;                                            // dest = driver, src = DBG
    m[6] = (uint8_t)plen; m[7] = (uint8_t)(plen >> 8);
    memcpy(m + 12, params, plen);                                    // after the 4-byte pattern
    return 4 + ((len + 3u) & ~3u);
}

static void test_rx(void) {
    uint8_t buf[512];
    memset(buf, 0, sizeof buf);
    uint8_t p1[8] = {0x00, 0x00, 0x50, 0x40, 0x34, 0x12, 0x03, 0x00};  // addr 0x40500000, data 0x00031234
    size_t n = make_msg(buf, AIC_DBG_MEM_READ_CFM, p1, 8);
    // A data packet after it: len 10 + 60-byte RX header = 70 bytes, not padded (the next packet starts at +70).
    uint8_t *d = buf + n;
    d[0] = 10; d[1] = 0; d[2] = 0x00;
    size_t total = n + 70;
    // And a second message with an odd parameter length (forces alignment padding), starting unaligned.
    uint8_t p2[5] = {1, 2, 3, 4, 5};
    total += make_msg(buf + total, AIC_DBG_ERROR_IND, p2, 5);

    size_t off = 0;
    AicRxPacket pk;
    uint32_t a = 0, v = 0;
    CHECK(aic_rx_next(buf, total, &off, &pk) == 1);
    CHECK(pk.is_msg && pk.id == AIC_DBG_MEM_READ_CFM && pk.param_len == 8);
    CHECK(aic_parse_mem_read_cfm(&pk, &a, &v) == 0 && a == 0x40500000u && v == 0x00031234u);
    CHECK(((v >> 16) & 0xff) == AIC_CHIP_REV_U02);
    CHECK(aic_rx_next(buf, total, &off, &pk) == 1);
    CHECK(!pk.is_msg && pk.type == 0x00 && pk.packet_len == 70 && off == n + 70);
    CHECK(aic_parse_mem_read_cfm(&pk, &a, &v) == -1);
    CHECK(aic_rx_next(buf, total, &off, &pk) == 1);
    CHECK(pk.is_msg && pk.id == AIC_DBG_ERROR_IND && pk.param_len == 5 && pk.params[4] == 5);
    CHECK(off == total);
    CHECK(aic_rx_next(buf, sizeof buf, &off, &pk) == 0);             // zero length word = end

    // Malformed: declared length runs past the buffer.
    uint8_t bad[8] = {200, 0, 0x11, 0, 0, 0, 0, 0};
    off = 0;
    CHECK(aic_rx_next(bad, sizeof bad, &off, &pk) == -1);
    // Malformed: param_len larger than the packet.
    n = make_msg(buf, AIC_DBG_MEM_READ_CFM, p1, 8);
    buf[4 + 6] = 50;
    off = 0;
    CHECK(aic_rx_next(buf, n, &off, &pk) == -1);
}

int main(void) {
    test_ids();
    test_build();
    test_rx();
    printf("aic: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
