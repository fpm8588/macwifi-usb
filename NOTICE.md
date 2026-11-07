# Notices

## Linux mt76 (BSD-3-Clause-Clear)

Hardware facts in `Sources/Shared/mt76x2u.h` (USB IDs, vendor requests, register addresses, retry policy,
firmware names) come from the Linux mt76 driver, https://github.com/openwrt/mt76
(`usb.c`, `mt76.h`, `mt76x02.h`, `mt76x02_regs.h`, `mt76x2/usb.c`, `mt76x2/usb_init.c`, `mt76x2/mt76x2.h`).
The RXWI/TXWI/DMA buffer formats in `Sources/Shared/mt76x02_frames.[ch]` come from `mt76x02_mac.h`,
`mt76x02_mac.c`, `mt76x02_dma.h`, `dma.h` and `mt76x02_usb_core.c` (re-implemented, not copied).
Copyright (C) Felix Fietkau, Lorenzo Bianconi, MediaTek Inc. and contributors.
Files ported from mt76 in later phases keep their original copyright and SPDX lines and are listed here.

## Firmware

`mt7662_rom_patch.bin` and `mt7662.bin` are MediaTek firmware from linux-firmware, redistributable under
`LICENCE.ralink_a_mediatek_company_firmware`. They are downloaded by `scripts/fetch-firmware.sh` and are not
part of this repository.

## Prior art consulted

- mt7601u-macos (MIT OR Apache-2.0): architecture of a user-space Wi-Fi station on Apple Silicon.
- userspace-wifi and Kismet (GPL-2): consulted for facts only; no code copied.
- ZeroTier's MacEthernetTapAgent (feth + BPF + AF_NDRV design): consulted for facts only.
