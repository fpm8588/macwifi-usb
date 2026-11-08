// aic_fwload.h — pure (no I/O) parts of the AIC8800D80 firmware loader. Unit tested in Tests/test_fwload.c.
//
// Ported from the AIC8800 Linux driver (GPL-2.0-only), github.com/shenmintao/aic8800d80:
//   drivers/aic8800/aic_load_fw/aicbluetooth.c        aicbt_patch_table_alloc, aicbt_patch_info_unpack,
//                                                     aicbt_patch_table_load, rwnx_plat_bin_fw_upload_android
//   drivers/aic8800/aic_load_fw/aic_compat_8800d80.c  aicwf_patch_config_8800d80, patch_tbl_d80,
//                                                     system_config_8800d80 (main and legacy-mcu1 branches)
// Copyright (C) AICSemi 2018-2020 (as stated in that driver's aic_load_fw sources).
// Rewritten for macOS user space by the macwifi-usb contributors, 2026. GPL-2.0-only; see NOTICE.md.
//
// The I/O half lives in Sources/macwifictl/main.c (cmd_fwload). The order of operations is:
//   system config -> parse patch table -> upload ADID, patch, ext patches -> patch-table writes ->
//   upload fmacfw -> read 4 words from the uploaded image -> patch-config writes -> START_APP.

#ifndef AIC_FWLOAD_H
#define AIC_FWLOAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_PT_MAX_RECORDS 32

typedef struct {
    char     name[17];      // NUL-terminated copy of the 16-byte name
    uint32_t type;
    uint32_t len;           // number of {addr, value} pairs (0 for records without data)
    const uint8_t *pairs;   // len * 8 bytes inside the caller's buffer, little-endian
} AicPtRecord;

typedef struct {
    AicPtRecord rec[AIC_PT_MAX_RECORDS];
    int n;
    // From the INF record (aicbt_patch_info_unpack). info_len pairs of INF are also written to the chip.
    uint32_t info_len;
    uint32_t adid_addrinf, addr_adid, patch_addrinf, addr_patch;
    uint32_t reset_addr, reset_val, adid_flag_addr, adid_flag;
    uint32_t ext_patch_nb_addr, ext_patch_nb;
    const uint8_t *ext_patch_param;   // ext_patch_nb pairs {id, addr}
    char version[64];                 // VER record text, if any
} AicPatchTable;

// Parses a patch table file. `default_adid` / `default_patch` are the per-revision addresses used when the INF
// record doesn't provide them (Linux sets them before unpacking). Returns 0, or -1 (bad tag, truncated or
// out-of-range record, too many records, no INF record first, or ext patch list past the INF record).
// `pt` points into `buf`; keep the buffer alive.
int aic_pt_parse(const uint8_t *buf, size_t len, uint32_t default_adid, uint32_t default_patch, AicPatchTable *pt);

// Ext patch i (0-based): file id and load address. Returns 0, or -1 if i is out of range.
int aic_pt_ext_patch(const AicPatchTable *pt, uint32_t i, uint32_t *id, uint32_t *addr);

typedef struct {
    uint32_t addr, value;
    uint32_t delay_ms_after;   // sleep after this write (power-on records)
    uint32_t mask;             // 0: DBG_MEM_WRITE; otherwise DBG_MEM_MASK_WRITE with this mask
} AicMemOp;

// Which Linux loader the sequence follows.
typedef enum {
    AIC_D80_PROFILE_V5 = 0,          // main branch, SDK V5 firmware (chip_mcu_id = 0)
    AIC_D80_PROFILE_LEGACY_MCU1 = 1, // legacy-mcu1 branch, SDK V3 firmware (chip_mcu_id = 1)
} AicD80Profile;

static inline int aic_d80_is_mcu1(uint32_t chip_word) { return ((chip_word >> 25) & 1u) == 0; }
static inline AicD80Profile aic_d80_choose_profile(uint32_t chip_word) {
    return aic_d80_is_mcu1(chip_word) ? AIC_D80_PROFILE_LEGACY_MCU1 : AIC_D80_PROFILE_V5;
}
const char *aic_d80_profile_name(AicD80Profile p);

// System-config writes after the cache read-modify-write (which the caller does for MCU1 chips in both
// profiles). Legacy profile + product 0x8820 -> two writes; otherwise none. Returns count or -1.
int aic_d80_syscfg_writes(AicD80Profile p, uint32_t chip_word, AicMemOp *ops, size_t cap);

// The DBG_MEM_WRITEs of aicbt_patch_table_load, with the D80 BT-mode values patched in. Returns the number of
// operations written to `ops`, or -1 if `cap` is too small or the BTMODE record is shorter than 9 pairs.
int aic_pt_writes(const AicPatchTable *pt, AicMemOp *ops, size_t cap);

// The DBG_MEM_WRITEs of aicwf_patch_config_8800d80 (normal mode, no adaptivity test), given the words read back
// from the uploaded firmware at AIC_D80_RAM_FMAC_FW_ADDR + 0x198 / 0x1A0 / 0x1C / 0x1A4.
// V5: `patch_buff_base` is used when fw_version > AIC_D80_PATCH_BUFF_MIN_VER. Legacy: always the fixed pair
// buffer at 0x1D7000 (version and patch_buff_base are ignored). Returns count or -1.
int aic_d80_patch_config_writes(AicD80Profile p, uint32_t config_base, uint32_t patch_str_base, uint32_t fw_version,
                                uint32_t patch_buff_base, AicMemOp *ops, size_t cap);

// True if the patch-config step must read the patch buffer pointer.
static inline int aic_d80_needs_patch_buff(AicD80Profile p, uint32_t fw_version) {
    return p == AIC_D80_PROFILE_V5 && fw_version > 0x06090100u;
}

// Chunk length for uploading byte `offset` of a `size`-byte file with DBG_MEM_BLOCK_WRITE (1 KiB chunks, the
// last one shorter). Same split as rwnx_plat_bin_fw_upload_android. Returns 0 when offset >= size.
static inline uint32_t aic_upload_chunk(uint32_t size, uint32_t offset) {
    if (offset >= size) return 0;
    return (size - offset) > 1024u ? 1024u : (size - offset);
}

#ifdef __cplusplus
}
#endif
#endif
