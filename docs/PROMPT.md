# Build prompt: macwifi-usb (paste into Claude Code on the Mac, from the repo root)

---

You're continuing **macwifi-usb**, a free, open-source user-space driver that makes a **MediaTek MT7612U**
USB Wi-Fi adapter (the mt76x2u family: Alfa AWUS036ACM, Netgear A6210, Asus USB-AC55…) usable on an **Apple
Silicon Mac running macOS 27 Golden Gate**. Read `docs/RESEARCH.md` first (why user space, why this chip, why
feth), then the headers in `Sources/Shared/`.

## Hard constraints

1. **$0.** Free Xcode Command Line Tools, system frameworks (IOKit, CoreFoundation, CommonCrypto, Network,
   SwiftUI), ad-hoc signing, free GitHub Actions on this public repo. No paid Apple account, no DriverKit,
   no kernel extensions, **never ask to disable SIP or lower boot security**, no Homebrew dependencies in
   the shipped product.
2. **Apple Silicon (arm64), macOS 13+ deployment target, tested on macOS 27.**
3. **Licensing.** This repo is MIT. Porting code from Linux **mt76** (BSD-3-Clause-Clear) is allowed: keep its
   copyright and SPDX line at the top of every ported file and list the file in `NOTICE.md`. You may adapt
   **mt7601u-macos** (MIT/Apache-2.0) with attribution. **Don't copy** from userspace-wifi or Kismet (GPL-2):
   read them for facts only. Never commit firmware; `scripts/fetch-firmware.sh` downloads it.
4. **One source of truth** for chip facts: `Sources/Shared/mt76x2u.h` (plus `mt76x2u_regs.h` when Phase 2 adds
   registers). Every constant cites its Linux source file.
5. **Test without hardware wherever possible.** Pure logic (frame building/parsing, descriptor formats,
   TXWI/RXWI encode/decode, the station state machine, the EAPOL handshake against recorded frames) lives in
   `Sources/Shared` with unit tests in `Tests/`, built by `./build.sh test` on macOS **and** Linux. CI must be
   green before any phase counts as done.
6. **Never leave the Mac without networking.** The built-in Wi-Fi is never touched. Every install has an
   uninstall. Start each hardware session by giving the user the recovery commands:
   ```bash
   sudo launchctl bootout system/io.github.fpm8588.macwifid 2>/dev/null
   for i in $(ifconfig -l | tr ' ' '\n' | grep '^feth'); do sudo ifconfig $i destroy; done
   ```
7. **Small, verified steps.** At each hardware checkpoint, stop, tell the user exactly what to run and what to
   expect, wait for the output, and record it in `docs/HARDWARE_LOG.md`.
8. **Secrets:** Wi-Fi passphrases are stored only in the System keychain (or a root-only 0600 file as a first
   step) and never logged.

## Layout

```
Sources/Shared/     mt76x2u.h, ieee80211.[ch], wpa.[ch], crypto_backend.c   (pure C, OS-independent)
Sources/macwifictl/ Phase 1 CLI (IOKit) + descriptor printer
Sources/mt76x2u/    Phase 2-3 chip driver ported from Linux mt76 (C), over a small USB transport interface
Sources/daemon/     Phase 3-5 macwifid: station state machine, WPA2 supplicant, feth bridge, control socket
Sources/App/        Phase 5 SwiftUI menu bar app
Installer/          LaunchDaemon plist, pkg scripts, uninstaller
scripts/            fetch-firmware.sh
Tests/              unit tests (+ recorded frame fixtures)
docs/               RESEARCH.md, PROMPT.md, HARDWARE_LOG.md
```

## Phase 1: reach the chip (code done; needs the adapter)

| Step | Command | Expected |
|---|---|---|
| 1 | `./build.sh test && ./build.sh cli` | all tests pass, arm64 `build/macwifictl` |
| 2 | `build/macwifictl list --all` | your adapter listed with `*` and "supported" |
| 3 | `build/macwifictl info` | high speed (480 Mb/s), vendor-class interface, bulk IN/OUT endpoints |
| 4 | `build/macwifictl identify` | `MT_ASIC_VERSION 0x7612xxxx` (or 7632/7662) → "MT76x2 chip confirmed" |
| 5 | if 4 reads 0 or 0xffffffff: `build/macwifictl reset`, replug, retry | as 4 |

Record descriptor dumps (endpoint addresses and sizes) in `HARDWARE_LOG.md`. Phase 2 needs them.

## Phase 2: bring the radio up and scan

Port from Linux mt76 in this order, each step with a `macwifictl` subcommand that proves it on hardware:

1. **USB transport** (`Sources/mt76x2u/usb_io.c`): open the device, `SetConfiguration(1)`, open interface 0,
   find bulk pipes (data IN + MCU-response IN, data OUT queues), synchronous and async bulk I/O on a CFRunLoop
   thread, and the register helpers from Phase 1 (rr/wr/rmw/poll, plus `MT_VEND_MULTI_WRITE` burst writes and
   `MT_VEND_WRITE_FCE`). Linux sources: `usb.c`, `mt76x02_usb_core.c`.
2. **Power on**: `mt76x2_reset_wlan`, `mt76x2u_power_on` (+ RF power-on), `mt76x02_wait_for_mac`
   (`mt76x2/usb_init.c`). → `macwifictl power-on` prints MAC_CSR0 becoming valid.
3. **Firmware**: `mt76x2u_mcu_fw_init`: ROM patch (`mt7662_rom_patch.bin`, semaphore + patch header, DMA via
   FCE), then `mt7662.bin` (ILM/DLM split, IVB), then MCU init/handshake (`mt76x2/usb_mcu.c`,
   `mt76x02_usb_mcu.c`). Parse the firmware headers in `Sources/Shared` with unit tests against the real file
   layout. → `macwifictl firmware` prints the firmware build/version reported by the MCU.
4. **DMA and MAC init**: `mt76x2u_init_dma`, `mt76x2u_mcu_init`, `mt76x2u_mac_reset`, WCID/shared-key table
   clear, beacon config, `mt76x2_mcu_load_cr`, rx path/tx DAC (`usb_init.c`, `usb_mac.c`, `mac.c`).
5. **EEPROM/eFuse**: read the 512-byte EEPROM (`eeprom.c`): MAC address, country/channel plan, TX power,
   calibration. → `macwifictl eeprom` prints the MAC address printed on the adapter's label.
6. **Channel + calibration**: `mt76x2u_phy_set_channel` and the MCU calibrations (`usb_phy.c`, `phy.c`), RX filter
   for beacons/probe responses, `mt76x02_mac_start`.
7. **RX**: wire bulk-IN transfers to `mt76x2u_rx_unpack` (already written and unit tested in
   `Sources/Shared/mt76x02_frames.c`: DMA length, RXWI, L2 pad, IV stripping). Add calibrated RSSI from the
   EEPROM (`mt76x02_mac_get_rssi`).
8. **Passive scan**: dwell ~120 ms per channel across 2.4 + 5 GHz (non-DFS first), parse with
   `wifi_parse_bss`. → **Checkpoint: `macwifictl scan` lists the user's own networks with sensible RSSI.**

## Phase 3: connect (WPA2-Personal)

1. **TX**: `mt76x2u_tx_pack` (already written and tested: TXINFO + TXWI + padded frame) onto the right bulk-OUT
   endpoint per queue (management vs EDCA, see `mt76u` endpoint mapping), plus TX status polling
   (`mt76x02_mac_poll_tx_status`) so retries and failures are visible.
2. **Active scan** (probe requests) and the **station state machine** in `Sources/Shared/station.c`, which is pure
   and driven by events: auth (open system) → assoc request (with supported rates, HT and VHT capabilities, RSN IE
   for WPA2-PSK/CCMP) → assoc response (AID, status). Timeouts and retries. Unit test it with scripted frames.
3. **Join the BSS in hardware**: own MAC, BSSID, WCID for the AP (`mt76x02_mac_wcid_setup`), basic rates, slot
   time, beacon/TSF tracking.
4. **WPA2-PSK supplicant** (`Sources/Shared/eapol.c`): parse EAPOL-Key frames, msg 1→2 (SNonce, PTK via
   `wpa_derive_ptk`, MIC via `wpa_eapol_mic`, our RSN IE), verify msg 3 (MIC, ANonce, RSN IE match), unwrap the
   GTK KDE with `wpa_aes_unwrap`, send msg 4, then **install keys**: pairwise in the AP's WCID, group in the
   shared-key table (`mt76x02_mac_wcid_set_key`, `mt76x02_mac_shared_key_setup`). Handle group-key handshakes.
   Test against a **recorded handshake** (capture one from your own network with the passphrase you know) and the
   IEEE 802.11 Annex J vectors.
5. **Data**: EAPOL frames (ethertype 0x888E) go to the supplicant; everything else to/from the bridge via
   `wifi_eth_to_80211` / `wifi_80211_to_eth` (hardware does CCMP; set the IV/key index in TXWI as mt76 does).
   → **Checkpoint: `sudo macwifictl connect "<SSID>"` (prompting for the passphrase) reports "4-way handshake
   complete" and then receives decrypted broadcast frames (ARP/mDNS) from the network.**

## Phase 4: real networking in macOS

1. **feth bridge** (`Sources/daemon/feth.c`, root): create `fethA` (the interface macOS uses) and `fethB` (raw
   I/O), peer them, set `fethA`'s lladdr to the adapter's MAC (the AP only accepts that address), MTU 1500. Read
   frames from `fethB` with BPF (immediate mode, see-sent off, header-complete on) and write with an `AF_NDRV`
   socket, the design ZeroTier uses.
2. Bridge loop: Ethernet from macOS → 802.11 → USB TX; USB RX → 802.11 → Ethernet → macOS. Drop frames while
   not associated.
3. Let **macOS configure it**: `ipconfig set fethA DHCP`, and check `scutil --nwi`, `route -n get default`,
   `scutil --dns`. Find out whether the interface becomes a primary service with working DNS on macOS 27. If DNS
   is only supplemental, try registering a proper network service through SystemConfiguration preferences
   (`SCNetworkServiceCreate` on an interface) from the root daemon. Only if both fail, fall back to `utun` + an
   in-daemon DHCP client.
4. → **Checkpoint: built-in Wi-Fi turned off, the dongle connected; Safari loads pages, `ping 1.1.1.1` and
   `ping apple.com` work, `networkQuality` reports throughput. Log the numbers.**

## Phase 5: product

- `macwifid` as a LaunchDaemon (`/Library/LaunchDaemons/io.github.fpm8588.macwifid.plist`) that owns the
  adapter, survives replug and sleep/wake (re-run firmware after wake), auto-reconnects to known networks.
- A SwiftUI **menu bar app** that looks and behaves like the Wi-Fi menu: list networks (signal bars, lock icon,
  band), join with a password prompt, forget, on/off, details. It talks to the daemon over
  `/var/run/macwifid.sock` (root:admin 0660) with a tiny JSON protocol; the protocol encoder/decoder gets tests.
- `build.sh pkg`: an installer package (pkgbuild/productbuild, ad-hoc signed; note the one-time **Open
  Anyway**), and an uninstaller that removes the daemon, app and feth interfaces.

## Phase 6: nice-to-haves (only after Phase 5 is solid)

Rate adaptation tuned from TX status, A-MPDU RX reordering, 5 GHz DFS (passive-only) handling, regulatory
domain from the EEPROM country code, WPA3-SAE (Dragonfly in user space), hidden networks, IPv6 sanity checks,
power save.

## Working style

- Before each phase, restate the plan in a few lines; after it, update `README.md` status and
  `HARDWARE_LOG.md`, and commit.
- When the hardware disagrees with your port, the hardware wins: compare against the Linux driver's behavior
  register by register (`rr` dumps before/after) instead of guessing.
- `./build.sh test` before every commit; push; check that CI is green.
- Debugging: `log stream --predicate 'process == "macwifid"'`, `sudo tcpdump -i fethA -n`, `macwifictl rr`,
  Wireshark on fethA, and for over-the-air issues capture from a second device.
