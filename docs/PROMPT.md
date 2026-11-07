# Build prompt: macwifi-usb (paste into Claude Code on the Mac, from the repo root)

---

You're continuing **macwifi-usb**, a free, open-source user-space driver that makes the **UGREEN AX900
(AICsemi AIC8800D80, USB `a69c:5723` in driver-CD mode)** usable as a network interface on an **Apple Silicon
Mac running macOS 27 Golden Gate**. The user owns this adapter and tests on it. Read `docs/RESEARCH.md` first
(why user space, why FullMAC helps, why feth), then `Sources/Shared/aic8800.h` and `aic_msg.h`.

## Hard constraints

1. **$0.** Free Xcode Command Line Tools and system frameworks (IOKit, CoreFoundation, CommonCrypto,
   SystemConfiguration, SwiftUI), ad-hoc signing, free GitHub Actions on this public repo. No paid Apple
   account, no DriverKit, no kernel extensions, **never ask to disable SIP or lower boot security**, no
   Homebrew dependencies in the product.
2. **Apple Silicon (arm64), macOS 13+ deployment target, tested on macOS 27.**
3. **Licensing.** The repo is **GPL-2.0-only** because you'll port from the AIC8800 Linux driver
   (github.com/shenmintao/aic8800d80, GPL-2.0-only; AIC approved GPLv2 for it). Keep the original copyright
   lines in ported files and list each ported file in `NOTICE.md`. **Never commit firmware**:
   `scripts/fetch-firmware.sh` downloads it (pinned commit + SHA-256).
4. **One source of truth** for hardware facts: `Sources/Shared/aic8800.h` (+ new headers for message structs),
   each fact citing the Linux file it came from. When the hardware disagrees, the hardware wins: update the
   header, the tests and `docs/HARDWARE_LOG.md`.
5. **Test without hardware wherever possible.** Message builders/parsers, firmware-file parsing (patch table),
   the station state machine, the EAPOL handshake and the TX/RX descriptor code live in `Sources/Shared` with
   unit tests in `Tests/`, run by `./build.sh test` on macOS **and** Linux. CI must be green before a phase
   counts as done.
6. **Never leave the Mac without networking.** Built-in Wi-Fi is never touched. Every install has an uninstall.
   Start each hardware session by giving the user the recovery commands:
   ```bash
   sudo launchctl bootout system/io.github.fpm8588.macwifid 2>/dev/null
   for i in $(ifconfig -l | tr ' ' '\n' | grep '^feth'); do sudo ifconfig $i destroy; done
   ```
7. **Small verified steps.** At each hardware checkpoint, stop, tell the user exactly what to run and what to
   expect, wait for the output, record it in `docs/HARDWARE_LOG.md`. Use `MACWIFI_DEBUG=1` hex dumps when
   something doesn't match.
8. **Secrets:** Wi-Fi passphrases are stored only in the System keychain (or a root-only 0600 file as a first
   step) and never logged.

## Layout

```
Sources/Shared/     aic8800.h, aic_msg.[ch], ieee80211.[ch], wpa.[ch], crypto_backend.c  (pure C, tested)
Sources/macwifictl/ CLI (IOKit): list, modeswitch, info, identify, memr/memw, later fwload/scan/connect
Sources/aic/        Phase 2-3: USB transport, firmware loader, FullMAC command layer (ported, GPL-2.0)
Sources/daemon/     Phase 4-5: macwifid (station, supplicant, feth bridge, control socket)
Sources/App/        Phase 5: SwiftUI menu bar app
Installer/          LaunchDaemon plist, pkg scripts, uninstaller
scripts/            fetch-firmware.sh
Tests/              unit tests and recorded fixtures
docs/               RESEARCH.md, PROMPT.md, HARDWARE_LOG.md
```

## Phase 1: reach the chip (code written; run it on the adapter)

| Step | Command | Expected |
|---|---|---|
| 1 | `./build.sh test && ./build.sh cli` | tests pass, arm64 `build/macwifictl` |
| 2 | `build/macwifictl list` | `a69c:5723` marked "driver-CD mode" |
| 3 | `build/macwifictl info` | mass-storage interface (class 0x08) |
| 4 | `build/macwifictl modeswitch` | "Switched: … Wi-Fi mode" (expected `a69c:8d80` or `8d81`) |
| 5 | `build/macwifictl list` and `build/macwifictl info` | Wi-Fi mode; vendor-class (0xFF) interface with bulk IN/OUT endpoints. **Record the IDs and endpoints.** |
| 6 | `build/macwifictl identify` | `[0x40500000]` → chip revision (U02 = 0x03 expected for D80), sub-revision; "ROM answered" |

If step 4 fails (macOS doesn't create a disk, or eject doesn't switch): try Finder's eject; then try sending
the SCSI START STOP UNIT (eject) command directly over the mass-storage interface's bulk endpoints (a 31-byte
CBW, `55534243…` with CDB `1b 00 00 00 02 00`) after `USBInterfaceOpenSeize`. Record what works.
If the Wi-Fi-mode ID isn't in `kAicIds`, add it, citing your hardware log. If the device answers on a different
endpoint layout than Linux expects, record it before changing code.

## Phase 2: load the firmware

Port `aicfw_download_fw_8800d80` (`aic_load_fw/aic_compat_8800d80.c`) and what it calls:

1. `macwifictl fwload` (as root isn't needed for USB): `system_config` writes for the chip revision
   (`aicwf_usb.c` `system_config_8800*` / D80 equivalents); parse `fw_patch_table_8800d80_u02.bin`
   (`aicbt_patch_table_alloc` / `aicbt_patch_info_unpack`, pure parser with unit tests against the real file's
   SHA-256-pinned layout); upload ADID, patch, ext patch (`aicbt_ext_patch_data_load`), patch-table entries
   (`aicbt_patch_table_load`), then `fmacfw_8800d80_u02.bin` (or the `_h_` variant per `IS_CHIP_ID_H()`), all
   via `DBG_MEM_BLOCK_WRITE` in 1 KiB chunks with CFM checks (`rwnx_plat_bin_fw_upload_android`);
   `aicwf_patch_config_8800d80`; `DBG_START_APP` (`HOST_START_APP_AUTO`).
2. After start-app the device may **re-enumerate**: handle reopening (Linux splits this into the `aic_load_fw`
   and `aic8800_fdrv` modules).
3. FullMAC bring-up, from `aic8800_fdrv/rwnx_main.c` (`rwnx_platform_on` area, ~lines 9000-9430):
   D80 RF config (`aicwf_set_rf_config_8800d80`: txpwr lvl v3, lvl adj, ofst2x, `rf_calib`), userconfig file
   (`aic_userconfig_8800d80.txt` → `rwnx_cfgfile.c`), `MM_GET_MAC_ADDR` (eFuse MAC), `MM_RESET`, `ME_CONFIG`,
   `ME_CHAN_CONFIG` (country code; default "00"/world-safe), `MM_START`, `MM_ADD_IF` (station).
   → **Checkpoint: `macwifictl fwload` prints the firmware version and the MAC address printed on your adapter.**

## Phase 3: scan and connect

1. **Scan**: `SCANU_START_REQ` (`rwnx_send_scanu_req`), collect `SCANU_RESULT_IND` (beacon/probe-response
   frames → `wifi_parse_bss`), finish on `SCANU_START_CFM`. → **Checkpoint: `macwifictl scan` lists your
   networks with sensible RSSI on 2.4 and 5 GHz.**
2. **Data path**: port the USB TX descriptor (`rwnx_tx.c` USB path / `txdesc_api` + host descriptor, type byte
   `0x00`) and RX parsing (`rwnx_rx.c`: the 60-byte RX header, flags telling 802.3 data from forwarded 802.11
   management frames). Put both codecs in `Sources/Shared` with tests built from captured transfers.
3. **Connect**: `SM_CONNECT_REQ` (`rwnx_send_sm_connect_req`: SSID, BSSID, channel, RSN IE for WPA2-PSK/CCMP,
   auth type) → `SM_CONNECT_IND` (status, AID, AP's RSN IE). Handle `SM_DISCONNECT_IND`.
4. **WPA2-PSK supplicant** (`Sources/Shared/eapol.c`): EAPOL-Key frames (ethertype 0x888E) arrive on the data
   path; msg 1 → 2 (SNonce, `wpa_derive_ptk`, `wpa_eapol_mic`), verify msg 3 (MIC, RSN IE), unwrap the GTK with
   `wpa_aes_unwrap`, send msg 4, then `MM_KEY_ADD_REQ` for the pairwise key (and the group key), then
   `MM_SET_CTRL_PORT` / control-port open as the Linux driver does. Test against IEEE 802.11 Annex J vectors and
   a handshake you capture from your own network.
   → **Checkpoint: `sudo macwifictl connect "<SSID>"` reports "connected, keys installed", then receives
   ARP/mDNS broadcasts from your network.**

## Phase 4: macOS networking

1. `feth` pair (root): `fethA` for macOS, `fethB` for raw I/O; `fethA` lladdr = the adapter's MAC; BPF read
   (immediate, see-sent off, header-complete on) + `AF_NDRV` write, as ZeroTier does.
2. Bridge loop: Ethernet from macOS → AIC TX descriptor → USB; USB RX → Ethernet → macOS. Drop while not
   connected; EAPOL goes to the supplicant, not macOS.
3. `ipconfig set fethA DHCP`; check `scutil --nwi`, `route -n get default`, `scutil --dns`. If DNS is only
   supplemental, register a proper network service via SystemConfiguration from the daemon; fallback to `utun`
   + in-daemon DHCP only if both fail.
   → **Checkpoint: built-in Wi-Fi off, UGREEN connected; Safari loads pages, `ping apple.com` works,
   `networkQuality` numbers logged.**

## Phase 5: product

`macwifid` LaunchDaemon (`io.github.fpm8588.macwifid`) that owns the adapter: auto mode-switch on plug-in,
firmware load, reconnect to known networks, replug and sleep/wake handling. A SwiftUI menu bar app that behaves
like the Wi-Fi menu (networks with signal bars, lock, band; join with password; forget; on/off), talking JSON
over `/var/run/macwifid.sock` (root:admin 0660; protocol codec tested). `build.sh pkg` builds an ad-hoc-signed
installer package (note the one-time **Open Anyway**) and an uninstaller.

## Phase 6: later

WPA3-SAE (check for firmware SAE offload first), hidden networks, roaming between APs of one network,
5 GHz DFS channels, regulatory country from the userconfig/eFuse, power save, throughput tuning (TX
aggregation of USB transfers as the Linux driver does).

## Working style

- Before each phase, restate the plan in a few lines; after it, update `README.md` status and
  `docs/HARDWARE_LOG.md`, and commit.
- `./build.sh test` before every commit; push; check CI is green.
- Debugging: `MACWIFI_DEBUG=1` USB dumps, `log stream --predicate 'process == "macwifid"'`,
  `sudo tcpdump -i fethA -n`, Wireshark on fethA.
