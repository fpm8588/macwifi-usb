// aic_fwload.c — see aic_fwload.h. Ported from the AIC8800 Linux driver (GPL-2.0-only); Copyright (C) AICSemi
// 2018-2020. Differences from the original: no file I/O, every length is bounds-checked, and the result is a
// list of memory writes for the caller to send instead of sending them directly.

#include "aic_fwload.h"

#include <string.h>

#include "aic8800.h"
#include "aic_msg.h"

int aic_pt_parse(const uint8_t *buf, size_t len, uint32_t default_adid, uint32_t default_patch, AicPatchTable *pt) {
    memset(pt, 0, sizeof *pt);
    pt->addr_adid = default_adid;
    pt->addr_patch = default_patch;

    // aicbt_patch_table_alloc compares sizeof("AICBT_PT_TAG") = 13 bytes (tag + NUL), then skips 16.
    if (!buf || len < 16 || memcmp(buf, AIC_PT_TAG, sizeof AIC_PT_TAG) != 0) return -1;
    size_t p = 16;
    while (p < len) {
        if (pt->n >= AIC_PT_MAX_RECORDS || len - p < 24) return -1;
        AicPtRecord *r = &pt->rec[pt->n++];
        memcpy(r->name, buf + p, 16);
        r->name[16] = '\0';
        r->type = aic_le32(buf + p + 16);
        r->len = aic_le32(buf + p + 20);
        p += 24;
        if (r->type >= 1000 || r->len == 0) {   // "Temp Workaround" in the original: no data follows
            r->len = 0;
            r->pairs = NULL;
            continue;
        }
        if (r->len > (len - p) / 8) return -1;
        r->pairs = buf + p;
        p += (size_t)r->len * 8;
    }

    // aicbt_patch_info_unpack: only the first record, and only if it is INF.
    if (pt->n == 0 || pt->rec[0].type != AIC_PT_INF || pt->rec[0].len == 0) return -1;
    AicPtRecord *inf = &pt->rec[0];
    const uint32_t base_len = 4;   // (adid_addrinf .. adid_flag) = 8 words = 4 pairs
    uint32_t copy_len;
    if (inf->len > base_len) {
        pt->info_len = base_len;
        copy_len = base_len + 1;   // includes the ext patch count
    } else {
        pt->info_len = inf->len;
        copy_len = inf->len;
    }
    const uint8_t *w = inf->pairs;
#define W(i) aic_le32(w + 4 * (i))
    if (copy_len >= 1) { pt->adid_addrinf = W(0); pt->addr_adid = W(1); }
    if (copy_len >= 2) { pt->patch_addrinf = W(2); pt->addr_patch = W(3); }
    if (copy_len >= 3) { pt->reset_addr = W(4); pt->reset_val = W(5); }
    if (copy_len >= 4) { pt->adid_flag_addr = W(6); pt->adid_flag = W(7); }
    if (copy_len >= 5) { pt->ext_patch_nb_addr = W(8); pt->ext_patch_nb = W(9); }
#undef W
    if (pt->ext_patch_nb > 0) {
        // The {id, addr} list follows the copied pairs inside the INF record.
        if (pt->ext_patch_nb > inf->len - copy_len) return -1;
        pt->ext_patch_param = inf->pairs + (size_t)copy_len * 8;
    }
    inf->len = pt->info_len;   // the original shortens the record, so only these pairs are written

    for (int i = 0; i < pt->n; i++) {
        const AicPtRecord *r = &pt->rec[i];
        if (r->type != AIC_PT_VER || !r->pairs) continue;
        size_t n = (size_t)r->len * 8;
        if (n > sizeof pt->version - 1) n = sizeof pt->version - 1;
        memcpy(pt->version, r->pairs, n);
        pt->version[n] = '\0';
        break;
    }
    return 0;
}

int aic_pt_ext_patch(const AicPatchTable *pt, uint32_t i, uint32_t *id, uint32_t *addr) {
    if (i >= pt->ext_patch_nb || !pt->ext_patch_param) return -1;
    *id = aic_le32(pt->ext_patch_param + 8 * i);
    *addr = aic_le32(pt->ext_patch_param + 8 * i + 4);
    return 0;
}

int aic_pt_writes(const AicPatchTable *pt, AicMemOp *ops, size_t cap) {
    static const uint32_t bt_values[9] = {
        1u,   // hwinfo < 0
        AIC_BT_HWINFO, AIC_BT_CPMODE, AIC_BT_BTMODE_D80, AIC_BT_BTPORT, AIC_BT_UART_BAUD, AIC_BT_UART_FLOWCTRL,
        AIC_BT_LPM_ENABLE, AIC_BT_TXPWR_LVL_D80,
    };
    size_t n = 0;
    for (int i = 0; i < pt->n; i++) {
        const AicPtRecord *r = &pt->rec[i];
        if (r->type == AIC_PT_BTMODE && r->len < 9) return -1;
        if (r->type == AIC_PT_VER) continue;
        for (uint32_t k = 0; k < r->len; k++) {
            if (n >= cap) return -1;
            ops[n].addr = aic_le32(r->pairs + 8 * k);
            ops[n].value = (r->type == AIC_PT_BTMODE && k < 9) ? bt_values[k] : aic_le32(r->pairs + 8 * k + 4);
            ops[n].delay_ms_after = 0;
            n++;
        }
        // The original sleeps after a power-on record; a record without pairs has nothing to attach it to.
        if (r->type == AIC_PT_PWRON && r->len > 0) ops[n - 1].delay_ms_after = AIC_PT_PWRON_DELAY_MS;
    }
    return (int)n;
}

int aic_d80_patch_config_writes(uint32_t config_base, uint32_t patch_str_base, uint32_t fw_version,
                                uint32_t patch_buff_base, AicMemOp *ops, size_t cap) {
    // patch_tbl_d80 as aic_load_fw builds it (no USE_5G, CONFIG_PLATFORM_HI, CONFIG_POWER_LIMIT,
    // CONFIG_PRBREQ_REPORT, CONFIG_RADAR_OR_IR_DETECT or CONFIG_WOWLAN in its Makefile). Offsets from config_base.
    static const uint32_t tbl[][2] = {
        {0x00b4, 0xf3010000},
        {0x0170, 0x0001000A},   // rx aggregation counter
        {0x0188, 0x00000001},   // user_ext_flags = USER_PWROFST_COVER_CALIB_FLAG
    };
    const uint32_t cnt = sizeof tbl / sizeof tbl[0];
    uint32_t start = aic_d80_needs_patch_buff(fw_version) ? patch_buff_base : AIC_D80_PATCH_PAIRS_DEFAULT;
    size_t n = 0;
#define OP(a, v) do { if (n >= cap) return -1; ops[n].addr = (a); ops[n].value = (v); ops[n].delay_ms_after = 0; n++; } while (0)
    OP(patch_str_base + AIC_PATCH_OFS_MAGIC, AIC_PATCH_MAGIC);
    OP(patch_str_base + AIC_PATCH_OFS_MAGIC_2, AIC_PATCH_MAGIC_2);
    OP(patch_str_base + AIC_PATCH_OFS_PAIR_START, start);
    OP(patch_str_base + AIC_PATCH_OFS_PAIR_COUNT, cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        OP(start + 8 * i, tbl[i][0] + config_base);
        OP(start + 8 * i + 4, tbl[i][1]);
    }
    for (uint32_t b = 0; b < 4; b++) OP(patch_str_base + AIC_PATCH_OFS_BLOCK_SIZE + 4 * b, 0);
#undef OP
    return (int)n;
}
