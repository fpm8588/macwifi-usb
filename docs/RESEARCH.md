# A $0 driver for the UGREEN AX900 (AICsemi AIC8800) on Apple Silicon, macOS 27 Golden Gate

Research date: 7 November 2026. Claims were checked against primary sources: Apple developer documentation,
the AIC8800 Linux driver source and udev rules, the source and READMEs of the projects named below, your own
System Information output, and cryptographic reference vectors. Sources are at the end.

---

## 1. Your adapter

| Field (System Information) | Value | Meaning |
|---|---|---|
| Name / manufacturer | "Aic MSC" / aicsemi | AICsemi chip, **mass-storage ("driver CD") mode** |
| USB ID | `a69c:5723` | Listed in the Linux driver's udev rules as an AIC8800 driver-CD ID; UGREEN's own Linux installer for the **CM762 AX900** switches exactly this ID |
| Product version | `0x0200` | bcdDevice 2.00 |
| Link speed | 480 Mb/s | USB 2.0 high speed (what the chip needs) |

UGREEN sells the "AX900" with more than one chip. Yours is the **AIC8800 family**: UGREEN's 2025 CM762 Linux
package is `aic8800d80fdrvpackage`, and other AX900 units report as Realtek RTL8851BU (`0bda:b851`).

**Two modes.** It first appears as a small virtual CD holding the Windows driver. Linux's rules switch it by
**ejecting** that disc, after which it re-enumerates as the Wi-Fi device, expected to be `a69c:8d80`
(AIC8800D80) or `a69c:8d81`. `macwifictl modeswitch` does this with `diskutil eject`. The Wi-Fi-mode ID still
has to be confirmed on your unit.

## 2. Why nothing works on Apple Silicon today

| Route | Status | Why |
|---|---|---|
| Vendor macOS drivers | **None for Apple Silicon** | AICsemi/UGREEN ship Windows and Linux drivers only |
| Kernel extensions | **Dead end** | Need Reduced Security on Apple Silicon; the existing Wi-Fi kexts (Realtek USB, itlwm, AirportMtkWlan) are Intel-only |
| DriverKit (NetworkingDriverKit) | **No Wi-Fi support** | Apple: "Ethernet is the only networking interface currently supported by NetworkingDriverKit." DriverKit also needs a paid account in practice |
| macOS Wi-Fi menu (IO80211 family) | **Closed** | Private to Apple's own Wi-Fi hardware |
| **User-space driver + virtual Ethernet interface** | **Works, $0** | Proven on Apple Silicon by mt7601u-macos (full WPA2 → DHCP → internet, verified on hardware 16 Oct 2026) |

So the driver shows up as a **network interface**, not in the Wi-Fi menu, and you choose networks in its own
menu bar app. Built-in Wi-Fi, AirDrop and Continuity are untouched.

## 3. Why the AIC8800 is a good fit for a user-space driver

The AIC8800 is a **FullMAC** chip (its Linux driver, `aic8800_fdrv`, descends from the CEVA RivieraWaves
"rwnx" FullMAC design):

- **The firmware runs the MAC**: scanning, authentication, association, rate control, aggregation and
  encryption happen on the chip. The host sends commands as messages (`SCANU_START_REQ`, `SM_CONNECT_REQ`,
  `MM_KEY_ADD_REQ`…) and gets confirmations and indications back.
- **Data frames cross USB as Ethernet frames** plus a small descriptor, so bridging to a macOS virtual
  Ethernet interface needs almost no 802.11 handling on our side.
- **The WPA2 4-way handshake runs on the host** (as with Linux `wpa_supplicant`): EAPOL frames arrive on the
  data path and keys go back to the chip with `MM_KEY_ADD_REQ`. Our tested WPA2 library (`Sources/Shared/wpa.c`)
  covers exactly that.

Compared with SoftMAC chips (MediaTek mt76x2u, Realtek rtw89), there's far less code to get right: no RF
calibration tables, no TX/RX descriptor-level 802.11, no rate control.

## 4. Protocol facts verified so far (all in `Sources/Shared/aic8800.h`)

| Item | Fact | Linux source |
|---|---|---|
| Command framing | `[len+4 (12-bit)] [0x11] [0] [4 zero bytes] [u16 id] [u16 dest task] [u16 src=100] [u16 param_len] [params]`, sent as `len + 8` bytes | `aicbluetooth_cmds.c` `aicwf_set_cmd_tx` |
| Message ids | `(task << 10) | index`; debug task 1: `DBG_MEM_READ_REQ 0x400`, `_CFM 0x401`, `MEM_WRITE 0x402`, `BLOCK_WRITE 0x40B` (always a full 8+1024-byte struct), `START_APP 0x40D`, `MASK_WRITE 0x411` | `aicbluetooth_cmds.h` |
| RX stream | packets `[u16 len][type]…`; config packets (type & 0x10) carry a message at +4 with a 4-byte pattern before params and occupy `roundup(len,4)+4`; data packets occupy `roundup(len+60,4)` | `aic_txrxif.c` |
| Endpoints | first bulk IN = RX; first bulk OUT = TX; second bulk OUT = message endpoint on 8801/D81-family only | `aicwf_usb.c` `aicwf_parse_usb` |
| Chip revision | `DBG_MEM_READ 0x40500000` → rev = bits 23..16 (U02 = 0x03); `0x00000004` → sub-rev = bits 11..4 | `aicwf_usb.c` `system_config_8800` |
| D80 firmware load | patch table → ADID → patch → ext patch → patch-table entries → `fmacfw_8800d80_u02.bin` → patch config → `DBG_START_APP` | `aic_compat_8800d80.c` `aicfw_download_fw_8800d80` |
| Firmware files | `fw_patch_table_8800d80_u02.bin` (1,384 B), `fw_adid_8800d80_u02.bin` (1,708 B), `fw_patch_8800d80_u02.bin` (32,700 B), `fw_patch_8800d80_u02_ext0.bin` (16,136 B), `fmacfw_8800d80_u02.bin` (358,072 B), plus an "H" variant | `fw/aic8800D80/` at commit `2e75f88` |

The ROM answers debug-task messages **before** any firmware is loaded, which is what `macwifictl identify` uses
to prove the USB link works.

## 5. Architecture

```
 ┌──────────────── macOS (Apple Silicon) ──────────────────────────────────────────────┐
 │  Apps ── TCP/IP ── fethA (virtual Ethernet; macOS runs DHCP, ARP, IPv6, DNS)        │
 │                      │ peered with fethB (raw I/O: BPF read, AF_NDRV write)          │
 │  macwifid (root LaunchDaemon)                                                        │
 │    ├─ bridge:  Ethernet frames  <->  AIC TX/RX descriptors (FullMAC, no 802.11 work) │
 │    ├─ station: scan / connect / disconnect commands; WPA2-PSK 4-way + group keys     │
 │    └─ chip:    mode switch, firmware upload (patch + fmacfw), message transport      │
 │           │ IOKit USB (bulk), no kext, no DriverKit                                  │
 │  Menu bar app ── Unix socket ── macwifid                                             │
 └───────────┼──────────────────────────────────────────────────────────────────────────┘
             ▼  UGREEN AX900 (AIC8800D80), USB 2.0
```

**feth vs utun.** `feth` pairs are built into macOS and are what ZeroTier uses instead of the old tap kext;
they need root but no entitlement. Being layer 2, macOS's own DHCP client, ARP and IPv6 work unchanged, and a
FullMAC chip already speaks Ethernet. `utun` (what mt7601u-macos uses) is layer 3: it needs a custom DHCP
client, and that project notes DNS on macOS 15+ is only "supplemental" through such a tunnel. Whether a feth
service becomes the **primary** DNS service on macOS 27 is the main open question. Phase 4 tests it, with
`utun` as the fallback.

## 6. Licensing decision

- The AIC8800 Linux driver is **GPL-2.0-only** (AIC approved GPLv2 for the released driver source, per the
  shenmintao/aic8800d80 and radxa-pkg/aic8800 records). Phase 2 onward ports parts of it (firmware-load
  sequence, patch tables, message structs), so **this repository is GPL-2.0-only**. It's still $0 and open
  source; the only obligation is that redistributed modified versions also ship their source.
- Files written from scratch for this project (`Sources/Shared/ieee80211.*`, `wpa.*`, `crypto_backend.c`,
  `aic_msg.*`, `aic8800.h`, `macwifictl`) are **also** offered under MIT (see `NOTICE.md`).
- **Firmware** blobs aren't open source and have no stated redistribution license, so they're **never
  committed**. `scripts/fetch-firmware.sh` downloads them onto your Mac from the driver repository, pinned to a
  commit and checked against SHA-256 hashes.

## 7. What it won't do (honest limits)

- Appear in the Wi-Fi menu, or carry AirDrop/Continuity through the dongle.
- **WPA3-SAE** and **802.1X Enterprise** at first: WPA2-Personal and open networks only. The firmware may
  support SAE offload; that's investigated later.
- Need no admin rights: a root helper is installed once.
- Match Linux throughput: user-space copies over USB 2.0 bulk. Measured in Phase 4, not guessed.

## 8. Sources

- Your System Information screenshots (a69c:5723, "Aic MSC", aicsemi, 480 Mb/s).
- github.com/shenmintao/aic8800d80 @ `2e75f88`: `aic.rules` (a69c:5723/5724 eject rules, "ugreenax900"),
  `drivers/aic8800/aic_load_fw/{aicwf_usb.[ch], aicbluetooth_cmds.[ch], aic_txrxif.[ch],
  aic_compat_8800d80.[ch]}`, `drivers/aic8800/aic8800_fdrv/aicwf_usb.h`, `fw/aic8800D80/`, `README.md`
  (license history).
- github.com/davidcawork/ugreen-AX900-USB-WiFi6 (`install.sh`: `usb_modeswitch -v a69c -p 5723`, UGREEN CM762
  `aic8800d80fdrvpackage.deb`).
- 24wireless.info, "UGREEN CM762… AICsemi製チップセットを搭載"; techinfodepot / retailer listings (RTL8851BU
  AX900 variants); Manjaro forum "Ugreen AX900 et Linux" (0bda:b851).
- Apple: NetworkingDriverKit documentation; Apple Community thread 256283724.
- github.com/ZemichaelMD/MacOS-M1-mt7601u-userspace-wifi-driver-app (`README.md`, `docs/macos-networking.md`).
- ZeroTier `osdep/MacEthernetTapAgent.c`; openssh-unix-dev "Support for macOS feth devices" (2020).
- IEEE 802.11i-2004 Annex H; RFC 3394 (crypto test vectors in `Tests/test_wifi.c`).
