# Notices

## License of this repository

The repository as a whole is distributed under the **GNU General Public License v2.0 only** (see `LICENSE`),
because later phases port code from the AIC8800 Linux driver, which is GPL-2.0-only.

Files written from scratch for this project are **additionally** available under the MIT license
(Copyright (c) 2026 fpm8588), so they can be reused elsewhere:
`Sources/Shared/aic8800.h`, `Sources/Shared/aic_msg.[ch]`, `Sources/Shared/ieee80211.[ch]`,
`Sources/Shared/wpa.[ch]`, `Sources/Shared/crypto_backend.c`, `Sources/macwifictl/*`, `Tests/*`, `build.sh`,
`scripts/*`.

## AIC8800 Linux driver (GPL-2.0-only)

Hardware facts in `Sources/Shared/aic8800.h` (USB IDs, message ids and layouts, framing, addresses, firmware
names) come from github.com/shenmintao/aic8800d80 (derived from AICsemi's reference driver; AIC approved
GPLv2 for the released source, see that repository's README and radxa-pkg/aic8800). Files ported from it in
later phases keep their original copyright lines and are listed here:

- (none yet)

## Firmware

The AIC8800D80 firmware (`fw_patch_*`, `fw_adid_*`, `fmacfw_*`) is AICsemi's. It has no stated open-source
license, so it is **not** part of this repository; `scripts/fetch-firmware.sh` downloads it onto the user's
Mac from the driver repository at a pinned commit and verifies SHA-256 hashes.

## Prior art consulted (facts only)

- mt7601u-macos (MIT OR Apache-2.0): user-space Wi-Fi station architecture on Apple Silicon.
- ZeroTier MacEthernetTapAgent: feth + BPF + AF_NDRV design.
- userspace-wifi / Kismet (GPL-2): user-space USB Wi-Fi on macOS.
