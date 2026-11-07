// aic8800.h — hardware facts for AICsemi AIC8800-family USB Wi-Fi adapters (UGREEN AX900 / CM762 and others).
//
// Pure C, no OS headers. Facts (IDs, message numbers, struct layouts, addresses) were read from the Linux
// AIC8800 driver, github.com/shenmintao/aic8800d80 (GPL-2.0; AICsemi reference driver), and its udev rules.
// Source files are cited per item:
//   [UH] drivers/aic8800/aic_load_fw/aicwf_usb.h            USB IDs, chip revisions, firmware names, addresses
//   [UC] drivers/aic8800/aic_load_fw/aicwf_usb.c            endpoint selection, system_config_8800 (chip id)
//   [D8] drivers/aic8800/aic_load_fw/aic_compat_8800d80.h   D80 product id
//   [CM] drivers/aic8800/aic_load_fw/aicbluetooth_cmds.[ch] task ids, DBG message ids/structs, command framing
//   [TX] drivers/aic8800/aic_load_fw/aic_txrxif.[ch]        RX stream format
//   [RU] aic.rules                                          driver-CD (mass storage) IDs, switched by eject
//   [BT] drivers/aic8800/aic_load_fw/aicbluetooth.c         patch table format and loading, BT mode defaults
// Verified on your hardware (docs/HARDWARE_LOG.md): a69c:5723 "Aic MSC" -> eject -> a69c:8d80 "AIC Wlan",
// bulk OUT 0x01 / IN 0x82 (512 B, 480 Mb/s); ROM answers DBG_MEM_READ; chip revision 0x07 (U03), sub 0x82.

#ifndef AIC8800_H
#define AIC8800_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- USB IDs
#define AIC_VID          0xA69C   // [UH]
#define AIC_VID_V2       0x368B   // [UH]

typedef enum { AIC_MODE_NONE = 0, AIC_MODE_DRIVER_CD, AIC_MODE_WIFI } AicMode;

typedef struct { uint16_t vid, pid; AicMode mode; const char *name; int msg_ep; } AicId;

// msg_ep = 1: commands go to the SECOND bulk OUT endpoint when present (8801/D81 family) [UC]
static const AicId kAicIds[] = {
    // Driver-CD (USB mass storage) mode: ejecting the virtual disc switches to Wi-Fi mode. [RU]
    {AIC_VID, 0x5721, AIC_MODE_DRIVER_CD, "AIC8800 driver CD", 0},
    {AIC_VID, 0x5722, AIC_MODE_DRIVER_CD, "AIC8800 driver CD", 0},
    {AIC_VID, 0x5723, AIC_MODE_DRIVER_CD, "AIC8800 driver CD (UGREEN CM762 AX900, Tenda)", 0},
    {AIC_VID, 0x5724, AIC_MODE_DRIVER_CD, "AIC8800 driver CD (UGREEN AX900)", 0},
    {AIC_VID, 0x5725, AIC_MODE_DRIVER_CD, "AIC8800 driver CD (Tenda v2)", 0},
    {AIC_VID, 0x572f, AIC_MODE_DRIVER_CD, "AIC8800 driver CD", 0},
    // Wi-Fi mode  [UH][D8]
    {AIC_VID, 0x8800, AIC_MODE_WIFI, "AIC8800", 0},
    {AIC_VID, 0x8801, AIC_MODE_WIFI, "AIC8801", 1},
    {AIC_VID, 0x8d80, AIC_MODE_WIFI, "AIC8800D80", 0},
    {AIC_VID, 0x8d81, AIC_MODE_WIFI, "AIC8800D80/D81 running firmware", 1},
    {AIC_VID, 0x8d40, AIC_MODE_WIFI, "AIC8800D40", 0},
    {AIC_VID, 0x8d41, AIC_MODE_WIFI, "AIC8800D41", 1},
    {AIC_VID_V2, 0x8d81, AIC_MODE_WIFI, "AIC8800D81 (v2 VID)", 1},
};
#define AIC_NUM_IDS (sizeof(kAicIds) / sizeof(kAicIds[0]))

static inline const AicId *aic_lookup(uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < AIC_NUM_IDS; i++)
        if (kAicIds[i].vid == vid && kAicIds[i].pid == pid) return &kAicIds[i];
    return NULL;
}

// ---------------------------------------------------------------- Chip revision  [UC][UH]
#define AIC_ADDR_CHIP_ID      0x40500000u   // chip revision = (value >> 16) & 0xff
#define AIC_ADDR_CHIP_SUB_ID  0x00000004u   // sub revision  = (value >> 4)  & 0xff
#define AIC_CHIP_REV_U01      0x01
#define AIC_CHIP_REV_U02      0x03
#define AIC_CHIP_REV_U03      0x07
#define AIC_CHIP_REV_U04      0x0f
#define AIC_CHIP_REV_U05      0x1f
#define AIC_CHIP_SUB_REV_U04  0x20

static inline const char *aic_chip_rev_name(uint8_t rev) {
    switch (rev) {
        case AIC_CHIP_REV_U01: return "U01";
        case AIC_CHIP_REV_U02: return "U02";
        case AIC_CHIP_REV_U03: return "U03";
        case AIC_CHIP_REV_U04: return "U04";
        case AIC_CHIP_REV_U05: return "U05";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------- Message protocol  [CM]
// Firmware tasks; message id = (task << 10) | index.
enum { AIC_TASK_MM = 0, AIC_TASK_DBG = 1, AIC_TASK_SCAN = 2, AIC_TASK_TDLS = 3, AIC_TASK_SCANU = 4,
       AIC_TASK_ME = 5, AIC_TASK_SM = 6, AIC_TASK_APM = 7, AIC_TASK_BAM = 8, AIC_TASK_MESH = 9,
       AIC_TASK_RXU = 10, AIC_TASK_API = 11 };
#define AIC_DRV_TASK_ID          100
#define AIC_MSG_ID(task, idx)    ((uint16_t)(((task) << 10) | (idx)))

enum {
    AIC_DBG_MEM_READ_REQ        = AIC_MSG_ID(AIC_TASK_DBG, 0),   // 0x400 {u32 addr}
    AIC_DBG_MEM_READ_CFM        = AIC_MSG_ID(AIC_TASK_DBG, 1),   // 0x401 {u32 addr, u32 data}
    AIC_DBG_MEM_WRITE_REQ       = AIC_MSG_ID(AIC_TASK_DBG, 2),   // 0x402 {u32 addr, u32 data}
    AIC_DBG_MEM_WRITE_CFM       = AIC_MSG_ID(AIC_TASK_DBG, 3),
    AIC_DBG_ERROR_IND           = AIC_MSG_ID(AIC_TASK_DBG, 8),
    AIC_DBG_MEM_BLOCK_WRITE_REQ = AIC_MSG_ID(AIC_TASK_DBG, 11),  // 0x40B {u32 addr, u32 size, u8 data[<=1024]}
    AIC_DBG_MEM_BLOCK_WRITE_CFM = AIC_MSG_ID(AIC_TASK_DBG, 12),
    AIC_DBG_START_APP_REQ       = AIC_MSG_ID(AIC_TASK_DBG, 13),  // 0x40D {u32 bootaddr, u32 boottype}
    AIC_DBG_START_APP_CFM       = AIC_MSG_ID(AIC_TASK_DBG, 14),  //       {u32 bootstatus}
    AIC_DBG_MEM_MASK_WRITE_REQ  = AIC_MSG_ID(AIC_TASK_DBG, 17),  // 0x411 {u32 addr, u32 mask, u32 data}
    AIC_DBG_MEM_MASK_WRITE_CFM  = AIC_MSG_ID(AIC_TASK_DBG, 18),
};
#define AIC_BLOCK_WRITE_MAX      1024

// USB packet type byte (offset 2 of every packet)  [UH]
#define AIC_USB_TYPE_DATA         0x00
#define AIC_USB_TYPE_CFG          0x10
#define AIC_USB_TYPE_CFG_CMD_RSP  0x11   // host->device commands and device->host responses
#define AIC_USB_TYPE_CFG_DATA_CFM 0x12

#define AIC_RX_HW_HDR_LEN         60     // [TX] RX_HWHRD_LEN, added to data packets' length field
#define AIC_RX_ALIGNMENT          4      // [TX]
#define AIC_USB_MAX_PKT           2048   // [UH]

// ---------------------------------------------------------------- Firmware (Phase 2)  [D8], fw/aic8800D80/
// Not committed; scripts/fetch-firmware.sh downloads the matched set from the Linux driver repository.
// U02 and U03 chips use the same "_u02" files (aic_compat_8800d80.c, aicfw_download_fw_8800d80). Your adapter
// is U03 (chip revision 0x07, see docs/HARDWARE_LOG.md).
#define AIC_D80_FW_PATCH          "fw_patch_8800d80_u02.bin"
#define AIC_D80_FW_PATCH_EXT_FMT  "fw_patch_8800d80_u02_ext%u.bin"   // %u = id from the patch table
#define AIC_D80_FW_PATCH_TABLE    "fw_patch_table_8800d80_u02.bin"
#define AIC_D80_FW_ADID           "fw_adid_8800d80_u02.bin"
#define AIC_D80_FMACFW            "fmacfw_8800d80_u02.bin"
#define AIC_D80_FMACFW_H          "fmacfw_8800d80_h_u02.bin"         // when IS_CHIP_ID_H()

// Addresses for U02/U03 [D8]. The patch table's INF record overrides the ADID and patch addresses.
#define AIC_D80_RAM_FMAC_FW_ADDR      0x00120000u   // RAM_FMAC_FW_ADDR_8800D80_U02; also the START_APP address
#define AIC_D80_RAM_ADID_ADDR         0x00201940u   // FW_RAM_ADID_BASE_ADDR_8800D80_U02 (default)
#define AIC_D80_RAM_PATCH_ADDR        0x0020B43Cu   // FW_RAM_PATCH_BASE_ADDR_8800D80_U02 (default)
#define AIC_D80_CHIP_ID_H_MASK        0xC0          // CHIP_ID_H_MASK: (rev & mask) == mask -> "H" chip
#define AIC_D80_MCU_BIT               25            // system_config_8800d80: bit 25 of [0x40500000] clear -> MCU id
#define AIC_D80_CACHE_CTRL_ADDR       0x40100020u   //   ...then set bit 0 here (read-modify-write)
#define AIC_HOST_START_APP_AUTO       1             // aicbluetooth_cmds.h

// aicwf_patch_config_8800d80 (aic_compat_8800d80.c): the firmware image holds pointers the host reads after upload.
#define AIC_D80_FW_OFS_VERSION        0x01C         // firmware version word
#define AIC_D80_FW_OFS_CONFIG_BASE    0x198         // config_base  (patch table entries are offsets from it)
#define AIC_D80_FW_OFS_PATCH_STR      0x1A0         // aic_patch_t structure address
#define AIC_D80_FW_OFS_PATCH_BUFF     0x1A4         // patch pair buffer (read only when version > 0x06090100)
#define AIC_D80_PATCH_BUFF_MIN_VER    0x06090100u
#define AIC_D80_PATCH_PAIRS_DEFAULT   0x001D7000u   // pair buffer for older firmware
#define AIC_PATCH_MAGIC               0x48435450u   // "PTCH"
#define AIC_PATCH_MAGIC_2             0x50544348u   // "HCTP"
// aic_patch_t: magic_num 0, pair_start 4, magic_num_2 8, pair_count 12, block_dst[4] 16, block_src[4] 32,
// block_size[4] 48 (all u32).
#define AIC_PATCH_OFS_MAGIC           0
#define AIC_PATCH_OFS_PAIR_START      4
#define AIC_PATCH_OFS_MAGIC_2         8
#define AIC_PATCH_OFS_PAIR_COUNT      12
#define AIC_PATCH_OFS_BLOCK_SIZE      48

// Patch table file (aicbluetooth.c aicbt_patch_table_alloc / _info_unpack / _load; aicbluetooth_cmds.h).
// Layout: 16-byte tag "AICBT_PT_TAG", then records {char name[16]; u32 type; u32 len; u32 pairs[len][2]} (LE).
// Records with type >= 1000 or len == 0 carry no data.
#define AIC_PT_TAG                    "AICBT_PT_TAG"
enum { AIC_PT_INF = 0, AIC_PT_TRAP = 1, AIC_PT_B4 = 2, AIC_PT_BTMODE = 3, AIC_PT_PWRON = 4, AIC_PT_AF = 5,
       AIC_PT_VER = 6 };
#define AIC_PT_PWRON_DELAY_MS         100

// Values aicbt_patch_table_load writes into the BTMODE record for the D80 (aicbluetooth.c defaults:
// hwinfo -1, cpmode WORK, btmode BT_ONLY_COANT, btport MB, 1.5 Mbaud, flow control on, LPM off, txpwr 0x6F2F).
#define AIC_BT_HWINFO                 0xFFFFFFFFu   // (int)-1
#define AIC_BT_CPMODE                 0u
#define AIC_BT_BTMODE_D80             5u
#define AIC_BT_BTPORT                 1u
#define AIC_BT_UART_BAUD              1500000u
#define AIC_BT_UART_FLOWCTRL          1u
#define AIC_BT_LPM_ENABLE             0u
#define AIC_BT_TXPWR_LVL_D80          0x00006F2Fu

// After START_APP the D80 re-enumerates; the Linux Wi-Fi driver (aic8800_fdrv/aicwf_usb.c id table) binds
// a69c:8d81 (vendor-specific interface 0xff/0xff/0xff), and aic_load_fw ignores 8d81 once firmware is loaded.
#define AIC_PID_D80_ROM               0x8d80
#define AIC_PID_D80_FW                0x8d81

#ifdef __cplusplus
}
#endif
#endif // AIC8800_H
