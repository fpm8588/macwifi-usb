# A $0 USB Wi-Fi driver for Apple Silicon on macOS 27 Golden Gate

Research date: 7 November 2026. Claims were checked against primary sources: Apple developer documentation,
Linux kernel driver source, the source and READMEs of the existing projects named below, and reference
cryptographic vectors computed independently. Sources are listed at the end.

---

## 1. Why there are no USB Wi-Fi dongle drivers for Apple Silicon Macs

| Route | Status on Apple Silicon | Why |
|---|---|---|
| Vendor kernel extensions (e.g. the Realtek packages) | **Don't work** | chris1111's Wireless-USB-Big-Sur-Adapter (Realtek, up to Tahoe) says outright that it only works on Intel Macs. Third-party kexts on Apple Silicon also need Reduced Security. |
| Apple's DriverKit (NetworkingDriverKit) | **No Wi-Fi support** | Apple's documentation: "Ethernet is the only networking interface currently supported by NetworkingDriverKit." DriverKit also needs a paid developer account in practice. |
| macOS's own Wi-Fi stack (Wi-Fi menu, `airportd`, IO80211 family) | **Closed to third parties** | Private Apple frameworks, used only by Apple's own Wi-Fi hardware. Hackintosh projects (itlwm, AirportMtkWlan) hook into it with **kexts on Intel PCs**, not Apple Silicon. |
| **User-space driver** (USB from an ordinary program, network via a virtual interface) | **Works, $0** | Proven for the MT7601U on Apple Silicon (see §2). No kext, no DriverKit, no SIP change. Needs a root helper for the network plumbing. |

So a $0 driver can do everything **except appear in the Wi-Fi menu**. It shows up as a network interface (like
Ethernet), and you pick networks in its own menu bar app. Built-in Wi-Fi, AirDrop and Continuity keep running on
the Mac's own Wi-Fi hardware, untouched.

## 2. Prior art (all open source)

| Project | Chip | What it proves | License |
|---|---|---|---|
| **mt7601u-macos** (ZemichaelMD, Rust) | MT7601U, 2.4 GHz 802.11n 150 Mbps | **The whole chain on Apple Silicon:** firmware, scan, association, WPA2 4-way handshake, a real DHCP lease, ARP and ping to 8.8.8.8, verified on hardware 16 Oct 2026. Hands IP to macOS through a `utun` tunnel and a root helper. | MIT OR Apache-2.0 |
| **userspace-wifi** (dragorn, Kismet's author) | mt76x2u, mt7921u/mt7925u, rtw88, MM8108 | The real Linux drivers compiled into user space on libusb. On macOS: capture and transmit. On Linux, driven by a full stack (mac80211_hwsim): **mt76x2u scans, connects with WPA2 and passes IP traffic**; **mt7921u/mt7925u can't**, because their firmware doesn't ACK frames in that mode so authentication times out. | GPL-2 |
| **mt76-usb-macos** (Network-Weather) | MT7921U, MT7925U | Monitor-mode capture on macOS, including a pure-C IOKit variant with no dependencies | (see repo) |
| Kismet "USB userspace" source | mt76 + rtw88 | Monitor capture on macOS with no kext/VM; ships the linux-firmware blobs | GPL-2 |
| Wireless-USB-Big-Sur-Adapter (chris1111) | Realtek 8811/8812 etc. | Intel Macs only | (see repo) |
| itlwm / AirportMtkWlan / SimpleMtkWlan / mt76wm | Intel / MediaTek **PCIe** | Native Wi-Fi menu via kexts on Intel hackintoshes; not USB, not Apple Silicon | GPL-2 / various |

## 3. Choosing the chipset

You said you'd use whatever chip it takes, so the choice optimizes for **odds of success × usefulness**:

| Chip | Bands / speed | Station mode from user space | Reference driver | Verdict |
|---|---|---|---|---|
| **MediaTek MT7612U** (mt76x2u) | 2.4 + 5 GHz, 802.11ac 2×2 (867 Mbps PHY) | **Yes**: proven with WPA2 + IP by userspace-wifi | Linux mt76, **BSD-3-Clause-Clear** (permissive) | **Chosen** |
| MediaTek MT7601U | 2.4 GHz only, 150 Mbps | Yes, already working on Apple Silicon | mt7601u-macos (MIT) | **Budget fallback**: buy one and run the existing project today |
| MediaTek MT7921AU / MT7925U | Wi-Fi 6/6E/7 | **No**: firmware offload blocks association in user space (per userspace-wifi) | mt76 | Avoid for now |
| Realtek RTL8812AU/8814AU (rtw88) | 2.4 + 5 GHz ac | Yes for 8814AU via hwsim | rtw88 (Linux) | Possible, but the driver is far bigger and messier |

Why the MT7612U wins: its MAC is **host-controlled** (register tables, TXWI/RXWI descriptors, WCID and key tables,
the same mt76x02 design family as the MT7601U that's already working), the firmware is small, Linux support is
mature, and the license lets us port code directly with attribution.

**What to buy:** any adapter that `macwifictl list --all` identifies as one of the IDs in
`Sources/Shared/mt76x2u.h`. Common ones: **Alfa AWUS036ACM** (`0e8d:7612`), **Netgear A6210** (`0846:9053`),
**Asus USB-AC55/AC54**, **TP-Link TL-WDN6200**, **Edimax EW-7722UAC**, and generics sold as "MT7612U". Check the
USB ID before relying on a product name: vendors sometimes change chips between revisions.

## 4. Architecture

```
 ┌──────────────── macOS (Apple Silicon) ──────────────────────────────────────────────┐
 │  Apps ── TCP/IP ── feth0 (Ethernet-like interface, gets DHCP/ARP/IPv6 from macOS)   │
 │                      │ paired with feth1 (raw frame I/O: BPF read, AF_NDRV write)    │
 │  macwifid (root LaunchDaemon)                                                        │
 │    ├─ bridge:  Ethernet  <->  802.11 data + LLC/SNAP     (Sources/Shared/ieee80211.c)│
 │    ├─ station: scan, auth, assoc, WPA2-PSK 4-way + GTK   (Sources/Shared/wpa.c)      │
 │    └─ chip:    mt76x2u bring-up, firmware, RX/TX rings   (ported from Linux mt76)    │
 │           │ IOKit USB (vendor control + bulk), no kext, no DriverKit                 │
 │  Wi-Fi menu bar app ── Unix socket ── macwifid                                       │
 └───────────┼──────────────────────────────────────────────────────────────────────────┘
             ▼  MT7612U dongle (USB 2.0/3.0)
```

**Why feth instead of utun:** `feth` ("fake Ethernet") interface pairs are built into macOS and have been used
in place of the old tap kext (ZeroTier ships this). They need root but no entitlement. A feth interface is
**layer 2**, so macOS's own DHCP client, ARP, IPv6 and routing run on it unchanged, and the driver only translates
Ethernet frames to 802.11 data frames and back, just like Linux mac80211 does. `utun` is layer 3. The MT7601U
project had to write its own DHCP client, can't do ARP, and notes that on macOS 15+ DNS through a non-system-extension
tunnel is only "supplemental", so some apps ignore it. Whether a feth service can become the **primary** DNS
service without a signed extension is the main open question. Phase 4 tests it, and keeps `utun` + a custom DHCP
client as the fallback.

**What costs $0:** Xcode Command Line Tools, CommonCrypto (system library) for WPA2 crypto, ad-hoc signing,
GitHub Actions on a public repo, firmware from linux-firmware (redistributable). The only purchase is the adapter.

## 5. What it won't do (honest limits)

- Appear in the **Wi-Fi menu**, or support AirDrop/Continuity through the dongle (Apple-private).
- **WPA3-SAE** and **Enterprise (802.1X)** at first: WPA2-Personal (CCMP) and open networks only. SAE can be
  added later in user space.
- Throughput will be below Linux: user-space copies plus USB bulk transfers. Expect "fast enough for normal use",
  not the 867 Mbps link rate. Measure it in Phase 4 rather than guess.
- It needs a root helper (LaunchDaemon), installed once with your password.

## 6. Verification done so far

- WPA2 crypto checked against IEEE 802.11i-2004 Annex H vectors (PMK for "password"/"IEEE",
  `f42c6fc5…a12e`; PRF test case 1, `bcd4c650…0606`), RFC 3394 §4.1 (AES key wrap), and PTK/MIC vectors computed
  independently with Python's `hashlib`/`hmac`. The tests run against **CommonCrypto on macOS** and OpenSSL on
  Linux in CI.
- Register-access encoding (`bRequest`, `wValue`, `wIndex`, CFG/EEPROM address spaces, retry policy) transcribed
  from Linux `mt76/usb.c`, and the USB ID table from `mt76x2/usb.c`.
- Firmware names `mt7662_rom_patch.bin` and `mt7662.bin` (from `mt76x2.h`); linux-firmware ships them under
  `mediatek/` (`mt7662.bin` 81,908 bytes, `mt7662_rom_patch.bin` 26,350 bytes).

## 7. Licensing

- **mt76** (Linux/OpenWrt) is **BSD-3-Clause-Clear**: porting its code is allowed. Keep its copyright notice in
  ported files and in `NOTICE.md`.
- **mt7601u-macos** is MIT/Apache-2.0: ideas and code may be adapted with attribution.
- **userspace-wifi** and **Kismet** are GPL-2: read them for facts only, don't copy code into this MIT repo.
- **Firmware** blobs are MediaTek's, redistributable under linux-firmware's
  `LICENCE.ralink_a_mediatek_company_firmware`. They're downloaded by a script, not committed.

## 8. Sources

- Apple: NetworkingDriverKit documentation ("Ethernet is the only networking interface currently supported");
  USBDriverKit documentation; DriverKit entitlements documentation.
- Apple Community thread 256283724 (no Wi-Fi adapters for Apple Silicon; quotes the NetworkingDriverKit limitation).
- github.com/chris1111/Wireless-USB-Big-Sur-Adapter (Intel-only note).
- github.com/ZemichaelMD/MacOS-M1-mt7601u-userspace-wifi-driver-app (README, `docs/macos-networking.md`).
- github.com/dragorn/userspace-wifi (README: transmit and hwsim tables).
- github.com/Network-Weather/mt76-usb-macos; Kismet docs, "Wi-Fi: USB userspace".
- github.com/openwrt/mt76: `usb.c`, `mt76.h`, `mt76x02.h`, `mt76x02_regs.h`, `mt76x2/usb.c`,
  `mt76x2/usb_init.c`, `mt76x2/mt76x2.h`, `LICENSE`.
- ZeroTier `osdep/MacEthernetTapAgent.c` (feth pair + BPF + AF_NDRV design); openssh-unix-dev, "Support for
  macOS feth devices" (2020).
- IEEE 802.11i-2004 Annex H; RFC 3394.
- github.com/OpenIntelWireless/itlwm, github.com/Arxusion/AirportMtkWlan, github.com/lidiaoo/SimpleMtkWlan,
  github.com/seaurching/mt76wm (Intel-hackintosh PCIe kexts, for contrast).
