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
// Verified on your hardware so far: a69c:5723 "Aic MSC" (aicsemi), 480 Mb/s, bcdDevice 0x0200.

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
    {AIC_VID, 0x8d81, AIC_MODE_WIFI, "AIC8800D81", 1},
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

// ---------------------------------------------------------------- Firmware (Phase 2)  [UH], fw/aic8800D80/
// Not committed; scripts/fetch-firmware.sh downloads the matched set from the Linux driver repository.
#define AIC_D80_FW_PATCH          "fw_patch_8800d80_u02.bin"
#define AIC_D80_FW_PATCH_TABLE    "fw_patch_table_8800d80_u02.bin"
#define AIC_D80_FW_ADID           "fw_adid_8800d80_u02.bin"
#define AIC_D80_FMACFW            "fmacfw_8800d80_u02.bin"

#ifdef __cplusplus
}
#endif
#endif // AIC8800_H
