// Hardware-free tests for the firmware-loader logic (Sources/Shared/aic_fwload.c).
//
//   test_fwload            synthetic patch tables (always runs)
//   test_fwload <fw dir>   also checks the real, SHA-256-pinned firmware from scripts/fetch-firmware.sh
#include "aic8800.h"
#include "aic_fwload.h"
#include "aic_msg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------- building synthetic tables

typedef struct { uint8_t b[4096]; size_t n; } Buf;

static void put32(Buf *b, uint32_t v) {
    b->b[b->n++] = (uint8_t)v; b->b[b->n++] = (uint8_t)(v >> 8);
    b->b[b->n++] = (uint8_t)(v >> 16); b->b[b->n++] = (uint8_t)(v >> 24);
}
static void put_tag(Buf *b) { memset(b->b, 0, 16); memcpy(b->b, "AICBT_PT_TAG", 12); b->n = 16; }
static void put_rec(Buf *b, const char *name, uint32_t type, uint32_t len, const uint32_t *words) {
    memset(b->b + b->n, 0, 16);
    memcpy(b->b + b->n, name, strlen(name) < 16 ? strlen(name) : 16);
    b->n += 16;
    put32(b, type);
    put32(b, len);
    if (type < 1000) for (uint32_t i = 0; i < 2 * len; i++) put32(b, words[i]);
}

// Same shape as the real fw_patch_table_8800d80_u02.bin, with different numbers.
static void make_table(Buf *b) {
    static const uint32_t inf2[16] = {0xA0, 0x1111, 0xA4, 0x2222, 0x40500150, 1, 0x40500154, 1, 1, 2,
                                      0, 0x3000, 7, 0x4000};
    static const uint32_t trap[4] = {0x100, 0xAA, 0x104, 0xBB};
    uint32_t bt[20];
    for (int i = 0; i < 10; i++) { bt[2 * i] = 0x200 + 4u * (uint32_t)i; bt[2 * i + 1] = 0xEE00 + (uint32_t)i; }
    static const uint32_t pwr[4] = {0x300, 0x80, 0x304, 0x80000};
    static const uint32_t ver[4] = {0x41202d20, 0x30206775, 0x00000031, 0};   // " - Aug 01"
    put_tag(b);
    put_rec(b, "AICBT_PINF_T", AIC_PT_INF, 7, inf2);   // 4 info pairs + ext count + 2 ext patches
    put_rec(b, "AICBT_TRAP_T", AIC_PT_TRAP, 2, trap);
    put_rec(b, "AICBT_MODE_T", AIC_PT_BTMODE, 10, bt);
    put_rec(b, "AICBT_POWER_ON", AIC_PT_PWRON, 2, pwr);
    put_rec(b, "AICBT_SKIPPED", 1000, 5, NULL);         // type >= 1000: header only
    put_rec(b, "AICBT_VER_INFO", AIC_PT_VER, 2, ver);
}

static void test_parse(void) {
    Buf b;
    make_table(&b);
    AicPatchTable pt;
    CHECK(aic_pt_parse(b.b, b.n, 0xDEAD, 0xBEEF, &pt) == 0);
    CHECK(pt.n == 6);
    CHECK(pt.info_len == 4 && pt.rec[0].len == 4);
    CHECK(pt.adid_addrinf == 0xA0 && pt.addr_adid == 0x1111);       // INF overrides the defaults
    CHECK(pt.patch_addrinf == 0xA4 && pt.addr_patch == 0x2222);
    CHECK(pt.reset_addr == 0x40500150 && pt.reset_val == 1);
    CHECK(pt.adid_flag_addr == 0x40500154 && pt.adid_flag == 1);
    CHECK(pt.ext_patch_nb == 2);
    uint32_t id = 99, addr = 0;
    CHECK(aic_pt_ext_patch(&pt, 0, &id, &addr) == 0 && id == 0 && addr == 0x3000);
    CHECK(aic_pt_ext_patch(&pt, 1, &id, &addr) == 0 && id == 7 && addr == 0x4000);
    CHECK(aic_pt_ext_patch(&pt, 2, &id, &addr) == -1);
    CHECK(strcmp(pt.rec[4].name, "AICBT_SKIPPED") == 0 && pt.rec[4].len == 0);
    CHECK(strncmp(pt.version, " - Aug 01", 9) == 0);

    AicMemOp ops[64];
    int n = aic_pt_writes(&pt, ops, 64);
    CHECK(n == 4 + 2 + 10 + 2);                                       // VER and the skipped record write nothing
    CHECK(ops[0].addr == 0xA0 && ops[0].value == 0x1111);             // INF pairs are written too
    CHECK(ops[3].addr == 0x40500154 && ops[3].value == 1);
    CHECK(ops[4].addr == 0x100 && ops[4].value == 0xAA);
    // BTMODE: the first 9 values are replaced with the D80 defaults; the 10th is kept.
    CHECK(ops[6].addr == 0x200 && ops[6].value == 1);
    CHECK(ops[7].value == 0xFFFFFFFFu && ops[8].value == 0 && ops[9].value == 5 && ops[10].value == 1);
    CHECK(ops[11].value == 1500000 && ops[12].value == 1 && ops[13].value == 0 && ops[14].value == 0x6F2F);
    CHECK(ops[15].addr == 0x224 && ops[15].value == 0xEE09);
    CHECK(ops[16].delay_ms_after == 0 && ops[17].addr == 0x304 && ops[17].delay_ms_after == 100);
    CHECK(aic_pt_writes(&pt, ops, 5) == -1);                          // too small

    // INF with <= 4 pairs: no ext patches, defaults kept for what it doesn't cover.
    Buf s;
    static const uint32_t inf1[2] = {0xA0, 0x1111};
    put_tag(&s);
    put_rec(&s, "AICBT_PINF_T", AIC_PT_INF, 1, inf1);
    CHECK(aic_pt_parse(s.b, s.n, 0xDEAD, 0xBEEF, &pt) == 0);
    CHECK(pt.info_len == 1 && pt.addr_adid == 0x1111 && pt.addr_patch == 0xBEEF && pt.ext_patch_nb == 0);
}

static void test_parse_rejects(void) {
    Buf b;
    AicPatchTable pt;
    make_table(&b);
    b.b[0] = 'X';
    CHECK(aic_pt_parse(b.b, b.n, 0, 0, &pt) == -1);                  // bad tag
    make_table(&b);
    CHECK(aic_pt_parse(b.b, b.n - 3, 0, 0, &pt) == -1);              // truncated record
    CHECK(aic_pt_parse(b.b, 10, 0, 0, &pt) == -1);
    CHECK(aic_pt_parse(NULL, 0, 0, 0, &pt) == -1);

    // First record isn't INF -> the original's "aicbt_patch_info_unpack fail".
    Buf s;
    static const uint32_t trap[2] = {0x100, 0xAA};
    put_tag(&s);
    put_rec(&s, "AICBT_TRAP_T", AIC_PT_TRAP, 1, trap);
    CHECK(aic_pt_parse(s.b, s.n, 0, 0, &pt) == -1);

    // Length field larger than the file.
    put_tag(&s);
    put_rec(&s, "AICBT_PINF_T", AIC_PT_INF, 1, trap);
    s.b[16 + 20] = 0xFF;
    CHECK(aic_pt_parse(s.b, s.n, 0, 0, &pt) == -1);

    // Ext patch count pointing past the INF record.
    static const uint32_t inf[10] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 3};   // claims 3 ext patches, has room for 0
    put_tag(&s);
    put_rec(&s, "AICBT_PINF_T", AIC_PT_INF, 5, inf);
    CHECK(aic_pt_parse(s.b, s.n, 0, 0, &pt) == -1);

    // BTMODE shorter than the 9 values the loader overwrites.
    static const uint32_t inf1[2] = {0xA0, 0x1111};
    static const uint32_t bt[4] = {1, 2, 3, 4};
    put_tag(&s);
    put_rec(&s, "AICBT_PINF_T", AIC_PT_INF, 1, inf1);
    put_rec(&s, "AICBT_MODE_T", AIC_PT_BTMODE, 2, bt);
    CHECK(aic_pt_parse(s.b, s.n, 0, 0, &pt) == 0);
    AicMemOp ops[16];
    CHECK(aic_pt_writes(&pt, ops, 16) == -1);
}

static void test_patch_config(void) {
    AicMemOp ops[32];
    // Values read from fmacfw_8800d80_u02.bin at the pinned commit (version 0x06090101 > 0x06090100).
    int n = aic_d80_patch_config_writes(0x00177158, 0x00177c00, 0x06090101, 0x0017d318, ops, 32);
    CHECK(n == 14);
    static const uint32_t want[14][2] = {
        {0x177c00, 0x48435450}, {0x177c08, 0x50544348}, {0x177c04, 0x17d318}, {0x177c0c, 3},
        {0x17d318, 0x17720c}, {0x17d31c, 0xf3010000},
        {0x17d320, 0x1772c8}, {0x17d324, 0x0001000A},
        {0x17d328, 0x1772e0}, {0x17d32c, 0x00000001},
        {0x177c30, 0}, {0x177c34, 0}, {0x177c38, 0}, {0x177c3c, 0},
    };
    int ok = (n == 14);
    for (int i = 0; ok && i < 14; i++) ok = ops[i].addr == want[i][0] && ops[i].value == want[i][1];
    CHECK(ok);
    // Older firmware: fixed pair buffer, patch_buff_base ignored.
    n = aic_d80_patch_config_writes(0x1000, 0x2000, 0x06090100, 0x9999, ops, 32);
    CHECK(n == 14 && ops[2].value == 0x001D7000 && ops[4].addr == 0x001D7000 && ops[4].value == 0x10b4);
    CHECK(aic_d80_patch_config_writes(0, 0, 0, 0, ops, 13) == -1);
    CHECK(aic_d80_needs_patch_buff(0x06090101) && !aic_d80_needs_patch_buff(0x06090100));
}

static void test_chunks(void) {
    CHECK(aic_upload_chunk(1708, 0) == 1024 && aic_upload_chunk(1708, 1024) == 684 && aic_upload_chunk(1708, 1708) == 0);
    CHECK(aic_upload_chunk(1024, 0) == 1024 && aic_upload_chunk(1024, 1024) == 0);
    CHECK(aic_upload_chunk(2048, 1024) == 1024 && aic_upload_chunk(5, 0) == 5);
    uint32_t total = 0, chunks = 0;
    for (uint32_t off = 0, c; (c = aic_upload_chunk(358072, off)); off += c) { total += c; chunks++; }
    CHECK(total == 358072 && chunks == 350);   // fmacfw_8800d80_u02.bin
    // A block write always carries the full 1032-byte parameter struct (8 + 1024).
    uint8_t cmd[1100], data[10] = {1, 2, 3};
    CHECK(aic_build_block_write(cmd, sizeof cmd, 0x120000, data, 10) == 16 + 1032);
    CHECK(aic_build_start_app(cmd, sizeof cmd, AIC_D80_RAM_FMAC_FW_ADDR, AIC_HOST_START_APP_AUTO) == 24);
    CHECK(aic_le32(cmd + 16) == 0x120000 && aic_le32(cmd + 20) == 1);
}

static void test_chip_rev(void) {
    uint32_t v = 0xf9078820;   // the user's adapter (docs/HARDWARE_LOG.md)
    uint8_t rev = (uint8_t)(v >> 16);
    CHECK(rev == AIC_CHIP_REV_U03);
    CHECK((rev & AIC_D80_CHIP_ID_H_MASK) != AIC_D80_CHIP_ID_H_MASK);    // not an "H" chip -> fmacfw_8800d80_u02.bin
    CHECK(((v >> AIC_D80_MCU_BIT) & 1u) == 0);                          // MCU id: cache-control write needed
}

// ---------------------------------------------------------------- real firmware (optional)

static uint8_t *slurp(const char *dir, const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void test_real(const char *dir) {
    size_t len = 0;
    uint8_t *t = slurp(dir, AIC_D80_FW_PATCH_TABLE, &len);
    CHECK(t != NULL);
    if (!t) return;
    AicPatchTable pt;
    CHECK(len == 1384 && aic_pt_parse(t, len, AIC_D80_RAM_ADID_ADDR, AIC_D80_RAM_PATCH_ADDR, &pt) == 0);
    CHECK(pt.n == 7 && pt.info_len == 4);
    CHECK(pt.addr_adid == 0x201940 && pt.addr_patch == 0x1e0000);   // the file overrides the patch address
    CHECK(pt.ext_patch_nb == 1);
    uint32_t id = 9, addr = 0;
    CHECK(aic_pt_ext_patch(&pt, 0, &id, &addr) == 0 && id == 0 && addr == 0x20b43c);
    AicMemOp ops[256];
    int n = aic_pt_writes(&pt, ops, 256);
    CHECK(n == 4 + 27 + 57 + 18 + 3 + 31);
    printf("real patch table: %d writes, version \"%s\"\n", n, pt.version);
    free(t);

    uint8_t *fw = slurp(dir, AIC_D80_FMACFW, &len);
    CHECK(fw != NULL && len == 358072);
    if (fw && len > 0x1A8) {
        CHECK(aic_le32(fw + AIC_D80_FW_OFS_VERSION) == 0x06090101);
        CHECK(aic_le32(fw + AIC_D80_FW_OFS_CONFIG_BASE) == 0x00177158);
        CHECK(aic_le32(fw + AIC_D80_FW_OFS_PATCH_STR) == 0x00177c00);
        CHECK(aic_le32(fw + AIC_D80_FW_OFS_PATCH_BUFF) == 0x0017d318);
    }
    free(fw);
    const char *others[] = {AIC_D80_FW_ADID, AIC_D80_FW_PATCH, "fw_patch_8800d80_u02_ext0.bin"};
    const size_t sizes[] = {1708, 32700, 16136};
    for (int i = 0; i < 3; i++) {
        uint8_t *b = slurp(dir, others[i], &len);
        CHECK(b != NULL && len == sizes[i]);
        free(b);
    }
}

int main(int argc, char **argv) {
    test_parse();
    test_parse_rejects();
    test_patch_config();
    test_chunks();
    test_chip_rev();
    if (argc > 1) test_real(argv[1]);
    printf("fwload: %d checks, %d failures%s\n", checks, failures, argc > 1 ? " (with real firmware)" : "");
    return failures ? 1 : 0;
}
