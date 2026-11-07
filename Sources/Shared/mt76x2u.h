// mt76x2u.h — hardware facts for MediaTek MT7612U / MT7632U / MT7662U USB Wi-Fi (the "mt76x2u" family).
//
// Pure C, no OS headers. Every value is a fact read from the Linux mt76 driver (github.com/openwrt/mt76,
// BSD-3-Clause-Clear licensed, which permits reuse with attribution — see NOTICE.md):
//   [U] usb.c                 vendor-request transport, retry policy
//   [D] mt76x2/usb.c          USB ID table, probe order
//   [R] mt76x02_regs.h        register addresses
//   [H] mt76.h / mt76x02.h    vendor request numbers, chip checks
//   [I] mt76x2/usb_init.c     power-on sequence (Phase 2)

#ifndef MT76X2U_H
#define MT76X2U_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- Supported adapters  [D]
typedef struct { uint16_t vid, pid; const char *name; } Mt76x2uId;

static const Mt76x2uId kMt76x2uIds[] = {
    {0x0e8d, 0x7612, "MT7612U reference (Alfa AWUS036ACM, Aukey USBAC1200, many generics)"},
    {0x0e8d, 0x7632, "MT7632U reference (HC-M7662BU1)"},
    {0x0b05, 0x1833, "Asus USB-AC54"},
    {0x0b05, 0x17eb, "Asus USB-AC55"},
    {0x0b05, 0x180b, "Asus USB-N53 B1"},
    {0x057c, 0x8503, "AVM FRITZ!WLAN AC860"},
    {0x7392, 0xb711, "Edimax EW-7722UAC"},
    {0x056e, 0x400a, "ELECOM WDC-867SU3S"},
    {0x2c4e, 0x0103, "Mercury UD13"},
    {0x0846, 0x9014, "Netgear WNDA3100v3"},
    {0x0846, 0x9053, "Netgear A6210"},
    {0x2357, 0x0137, "TP-Link TL-WDN6200"},
    {0x0471, 0x2126, "LiteOn WN4516R module"},
    {0x0471, 0x7600, "LiteOn WN4519R module"},
};
#define MT76X2U_NUM_IDS (sizeof(kMt76x2uIds) / sizeof(kMt76x2uIds[0]))

static inline const Mt76x2uId *mt76x2u_lookup(uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < MT76X2U_NUM_IDS; i++)
        if (kMt76x2uIds[i].vid == vid && kMt76x2uIds[i].pid == pid) return &kMt76x2uIds[i];
    return NULL;
}

// ---------------------------------------------------------------- Vendor requests  [H][U]
// Register access = USB vendor control request to the device:
//   read : bmRequestType 0xC0 (IN | vendor | device), wValue = addr >> 16, wIndex = addr & 0xffff, 4 bytes LE
//   write: bmRequestType 0x40 (OUT | vendor | device), same addressing, 4 bytes LE
// Bits 31/30 of the 32-bit "address" select the EEPROM / CFG spaces, which use different bRequest values
// and are stripped before sending.
#define MT_RT_VENDOR_IN          0xC0
#define MT_RT_VENDOR_OUT         0x40

#define MT_VEND_TYPE_EEPROM      0x80000000u
#define MT_VEND_TYPE_CFG         0x40000000u
#define MT_VEND_TYPE_MASK        (MT_VEND_TYPE_EEPROM | MT_VEND_TYPE_CFG)
#define MT_VEND_ADDR_CFG(n)      (MT_VEND_TYPE_CFG | (uint32_t)(n))

enum {
    MT_VEND_DEV_MODE     = 0x01,
    MT_VEND_WRITE        = 0x02,
    MT_VEND_POWER_ON     = 0x04,
    MT_VEND_MULTI_WRITE  = 0x06,
    MT_VEND_MULTI_READ   = 0x07,
    MT_VEND_READ_EEPROM  = 0x09,
    MT_VEND_WRITE_FCE    = 0x42,
    MT_VEND_WRITE_CFG    = 0x46,
    MT_VEND_READ_CFG     = 0x47,
    MT_VEND_READ_EXT     = 0x63,
    MT_VEND_WRITE_EXT    = 0x66,
    MT_VEND_FEATURE_SET  = 0x91,
};

#define MT_VEND_REQ_MAX_RETRY    10      // [U]
#define MT_VEND_REQ_TIMEOUT_MS   300     // [U]

// Which bRequest the Linux driver uses for a read/write of a given 32-bit address.  [U]
static inline uint8_t mt76x2u_read_req(uint32_t addr) {
    switch (addr & MT_VEND_TYPE_MASK) {
        case MT_VEND_TYPE_EEPROM: return MT_VEND_READ_EEPROM;
        case MT_VEND_TYPE_CFG:    return MT_VEND_READ_CFG;
        default:                  return MT_VEND_MULTI_READ;
    }
}
static inline uint8_t mt76x2u_write_req(uint32_t addr) {
    return (addr & MT_VEND_TYPE_MASK) == MT_VEND_TYPE_CFG ? MT_VEND_WRITE_CFG : MT_VEND_MULTI_WRITE;
}
static inline uint16_t mt76x2u_wvalue(uint32_t addr) { return (uint16_t)((addr & ~MT_VEND_TYPE_MASK) >> 16); }
static inline uint16_t mt76x2u_windex(uint32_t addr) { return (uint16_t)(addr & 0xffff); }

// ---------------------------------------------------------------- Registers  [R]
#define MT_ASIC_VERSION          0x0000u   // chip id in bits 31..16, revision in 15..0
#define MT_EFUSE_CTRL            0x0024u
#define MT_MAC_CSR0              0x1000u   // MAC revision
#define MT_WLAN_MTC_CTRL_CFG     MT_VEND_ADDR_CFG(0x148)   // [I] power-on target (Phase 2)

static inline uint16_t mt76x2u_chip(uint32_t asic_version) { return (uint16_t)(asic_version >> 16); }
static inline int mt76x2u_is_supported_chip(uint32_t asic_version) {   // [H] is_mt76x2()
    uint16_t c = mt76x2u_chip(asic_version);
    return c == 0x7612 || c == 0x7632 || c == 0x7662 || c == 0x7602;
}

// ---------------------------------------------------------------- Firmware (Phase 2)
// Both files come from linux-firmware (mediatek/), redistributable under
// LICENCE.ralink_a_mediatek_company_firmware. Not committed here; scripts/fetch-firmware.sh downloads them.
#define MT7662_ROM_PATCH         "mt7662_rom_patch.bin"   // [I] mt76x2.h; shared by PCIe and USB parts
#define MT7662_FIRMWARE          "mt7662.bin"

#ifdef __cplusplus
}
#endif
#endif // MT76X2U_H
