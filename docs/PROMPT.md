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

## Phase 1: reach the chip (PASSED on the adapter 2026-11-07, see HARDWARE_LOG.md)

| Step | Command | Expected |
|---|---|---|
| 1 | `./build.sh test && ./build.sh cli` | tests pass, arm64 `build/macwifictl` |
| 2 | `build/macwifictl list` | `a69c:5723` marked "driver-CD mode" |
| 3 | `build/macwifictl info` | mass-storage interface (class 0x08) |
| 4 | `build/macwifictl modeswitch` | "Switched: … Wi-Fi mode" (expected `a69c:8d80` or `8d81`) |
| 5 | `build/macwifictl list` and `build/macwifictl info` | Wi-Fi mode; vendor-class (0xFF) interface with bulk IN/OUT endpoints. **Record the IDs and endpoints.** |
| 6 | `build/macwifictl identify` | `[0x40500000]` → chip revision (the user's adapter: 0x07 = U03), sub-revision; "ROM answered" |

If step 4 fails (macOS doesn't create a disk, or eject doesn't switch): try Finder's eject; then try sending
the SCSI START STOP UNIT (eject) command directly over the mass-storage interface's bulk endpoints (a 31-byte
CBW, `55534243…` with CDB `1b 00 00 00 02 00`) after `USBInterfaceOpenSeize`. Record what works.
If the Wi-Fi-mode ID isn't in `kAicIds`, add it, citing your hardware log. If the device answers on a different
endpoint layout than Linux expects, record it before changing code.

## Phase 2: load the firmware

### 2a: upload and start (PASSED with the legacy-mcu1 profile, 2026-11-07)

**Two loader profiles** (`AicD80Profile`), chosen from `[0x40500000]` bit 25 like Linux's `chip_mcu_id`:
`v5` (branch `main`, SDK V5 firmware) for MCU0 chips, `legacy-mcu1` (branch `legacy-mcu1` @ `bbf6cb4`, SDK V3
firmware) for MCU1 chips such as the user's. Legacy differences: V3 firmware set (no ext patch, no H image,
FMAC ends below 0x170000), product-0x8820 syscfg writes (`0x40500010 = 2`, mask-write `0x40506024` mask 0xff =
0xdf), patch config always at 0x1D7000 with `{0xb4, 0xf3010000}, {0x170, 0x2}`. See HARDWARE_LOG.md and Linux
issues #58, #118, #119. For phase 2b, port the fdrv from the same branch as the firmware.

Ported from `aicfw_download_fw_8800d80` (`aic_load_fw/aic_compat_8800d80.c`) and what it calls. Pure logic in
`Sources/Shared/aic_fwload.[ch]` (patch-table parser, its register writes with the D80 BT-mode values,
`aicwf_patch_config_8800d80` writes, 1 KiB chunking), tested in `Tests/test_fwload.c`, including against the
real pinned firmware in CI. I/O in `macwifictl fwload`:

1. `system_config_8800d80`: read `[0x40500000]`; U02/U03 only; if bit 25 is clear set bit 0 of `0x40100020`.
2. Parse `fw_patch_table_8800d80_u02.bin`; its INF record gives the ADID/patch addresses and the ext patches.
3. Upload ADID, patch, ext patches (`DBG_MEM_BLOCK_WRITE`, CFM per chunk), then the patch-table writes
   (`DBG_MEM_WRITE`, 100 ms after the power-on record).
4. Upload `fmacfw_8800d80_u02.bin` (or `_h_` per `IS_CHIP_ID_H()`) to 0x120000.
5. Read version / config base / patch struct / patch buffer back from the image (must equal the file, else stop
   before starting), then the 14 patch-config writes.
6. `DBG_START_APP(0x120000, AUTO)`: no confirmation; watch for re-enumeration.

| Step | Command | Expected |
|---|---|---|
| 1 | `scripts/fetch-firmware.sh` | "OK: firmware in firmware/aic8800D80 and firmware/aic8800D80-legacy-mcu1" |
| 2 | `build/macwifictl modeswitch` | `a69c:8d80` |
| 3 | `build/macwifictl fwload` | each upload "ok"; "read-back matches the file"; "came back as a69c:8d81"; descriptors of the new device. **Record the 8d81 interfaces and endpoints** (Linux uses a second bulk OUT for messages on 8d81, and may expose a BT interface). |

If it fails, rerun with `MACWIFI_DEBUG=1` and record the last transfers. If the adapter comes back with another
ID, add it to `kAicIds` with a hardware-log citation.

### 2b: FullMAC bring-up (PASSED 2026-11-07: MAC 6c:1f:f7:8b:81:46, 5 GHz, LMAC 6.9.1.0)

`macwifictl up` = `modeswitch` (if in driver-CD mode) + `fwload` (if not already `8d81`) + bring-up on the
running firmware. Ported from the Linux Wi-Fi driver on the same branch as the firmware (`legacy-mcu1`,
`aic8800_fdrv`), pure logic in `Sources/Shared/aic_fmac.[ch]` with `Tests/test_fmac.c`. Message ids and struct
sizes were checked by compiling the driver's `lmac_msg.h` with its build flags; the user-config parser was
checked byte for byte against the driver's own parser (defaults, both real files, 100,000 mutated files).

Sequence (Linux `rwnx_cfg80211_init` for PRODUCT_ID_AIC8800D81, testmode 0, USE_5G):

1. Open `a69c:8d81` interface 2 (0xff/0xff/0xff); messages on the second bulk OUT (0x02), replies on 0x81.
   If USBDeviceOpen is refused because macOS's composite driver holds the device, open the interface alone.
2. `DBG_MEM_READ [0x40500000]` through the firmware (fdrv `system_config_8800d80`); choose the profile.
3. Parse `aic_userconfig_8800d80.txt` (defaults from `userconfig_info`).
4. `MM_SET_STACK_START(1, 0, BIT(5), 0)` -> 5 GHz support, vendor info.
5. `MM_GET_FW_VERSION` -> version string.
6. `MM_SET_TXPWR_IDX_LVL` (v3 table, 95-byte union; with the legacy file: sent), `MM_SET_TXPWR_LVL_ADJ` and
   `MM_SET_TXPWR_OFST` (2x) only if enabled (legacy file: neither), `MM_SET_RF_CALIB` (0x0f8f / 0x0f0f /
   0x0c34c008 / 0 / 0x264203, crystal trim off).
7. `MM_GET_MAC_ADDR` -> **checkpoint: the MAC printed on the adapter's label**.
8. `MM_RESET`, `MM_VERSION`.

Firmware log packets (type 0x13) are printed as `[firmware] ...`.

Next (phase 3 start): `ME_CONFIG` (HT/VHT/HE capabilities), `ME_CHAN_CONFIG` (channel list, country "00"),
`MM_START`, `MM_ADD_IF` (station), then scanning. Issue #118 notes `MM_START` needs the product-0x8820 writes,
which phase 2a already does.

## Phase 3: scan and connect

1. **Scan** (PASSED 2026-11-07: both bands, results match the Mac's Wi-Fi menu). `macwifictl scan [SSID]` = `up` + the Linux driver's
   interface-open sequence + one scan, all in `Sources/Shared/aic_fmac.c`:
   `ME_CONFIG` (HT/VHT/HE capabilities) -> `ME_CHAN_CONFIG` (world "00" domain: channels 1-14 and 25 5 GHz
   channels, 20 dBm, none disabled) -> `MM_START` -> `MM_SET_COEX` (CONFIG_COEX) -> `MM_ADD_IF` (station, our MAC)
   -> `SCANU_START` (all 39 channels, one wildcard or directed SSID, no extra IEs) -> `SCANU_RESULT_IND`
   (beacon/probe response at offset 12 -> `wifi_parse_bss`) until `SCANU_START_CFM`.
   ME_CONFIG / ME_CHAN_CONFIG / MM_START are byte-identical to what the Linux code produces: the harness compiles
   `rwnx_set_{vht,he,ht}_capa` and the three senders from `aic8800_fdrv` with stand-in kernel types (constants
   from the Linux kernel's `include/linux/ieee80211-*.h`) and the D81 path of `rwnx_handle_dynparams`; channel
   power follows `handle_channel_custom` in the kernel's `net/wireless/reg.c` for the self-managed regdomain.
   The resulting bytes are pinned in `Tests/fmac_golden.h`. SCANU_START / ADD_IF / COEX were compared with the
   driver's own structs filled as `rwnx_send_*` fills them.
   → **Checkpoint: `macwifictl scan` lists your networks with sensible RSSI on 2.4 and 5 GHz.**
Steps 2-5 PASSED on the adapter 2026-11-07 (WPA2/WPA3 transition home network, 5 GHz 80 MHz: handshake,
keys, encrypted DHCP offer, 89 broadcast/multicast frames in 15 s; see HARDWARE_LOG.md).

2. **Data path** (built, `Sources/Shared/aic_data.c`): TX on bulk OUT 0x01 = 4-byte header (length, type
   `0x01`) + `struct hostdesc` (28 bytes, ethertype in network order, staid = the AP's station index for all
   station-mode traffic, tid 0 → BE) + the payload without its Ethernet header; EAPOL asks for a TX confirmation
   (`status_desc_addr = BIT31 | idx`), which comes back as bulk IN type `0x12` (u32 status: bit0 done, bit3 acked;
   u32 idx). RX data = 56-byte `hw_rxhdr` + 4 pad + the 802.11 frame (CCMP header still present) → 802.3 as
   `rwnx_rxdataind_aicwf` does, including A-MSDU and the NETGEAR quirk.
3. **Connect** (built): `SM_CONNECT_REQ` (320 bytes; flags `CONTROL_PORT_HOST | WPA_WPA2_IN_USE`, EAPOL as the
   control-port ethertype, open auth, uapsd AC_VO, our RSN element in `ie_buf`) → `SM_CONNECT_CFM` →
   `SM_CONNECT_IND` (status, AP station index, AID) ; `SM_DISCONNECT_IND` handled throughout; `SM_DISCONNECT_REQ`
   (reason 3) on exit. All request layouts were compared byte for byte with the driver's structs filled as
   `rwnx_send_*` fills them, and are pinned in `Tests/test_fmac.c`.
4. **WPA2-PSK supplicant** (built, `Sources/Shared/eapol.c`, original code): msg 1 → 2, msg 3 checked (MIC,
   replay counter, ANonce, RSN element against the beacon's, GTK unwrap) → 4, group rekeys. Then, in
   wpa_supplicant's order: wait for msg 4's TX confirmation, `MM_KEY_ADD_REQ` pairwise (sta = AP index) and group
   (sta 0xFF), `ME_SET_CONTROL_PORT_REQ(open)`. Key material is never printed, even with `MACWIFI_DEBUG=1`; the
   passphrase is read with `readpassphrase` (no echo) and wiped after PBKDF2.
5. **Probe** (`Sources/Shared/dhcp.c`): one DHCPDISCOVER (no REQUEST, so no address is taken), report the offer,
   count received frames by type. `Tests/sim_connect.c` runs the whole command against a simulated firmware + AP
   (happy path, wrong password, not found, PMF required, association refused, short password).
   → **Checkpoint: `macwifictl connect "<SSID>"` reports "keys installed, port open", a DHCP offer, and
   broadcast traffic from your network.**

## Phase 4: macOS networking

PASSED 2026-11-07 (`sudo macwifictl join "SSID"`): with the Mac's wired connection unplugged, Safari and a speed
test ran over the UGREEN through feth4100. Also simulated in `Tests/sim_connect.c` (fake firmware + AP + fake feth,
RX aggregated like the chip). Found on the way: data packets in an aggregated RX transfer are not padded.

1. `feth` pair (root, `Sources/macwifictl/feth.c`): `feth4100` for macOS (lladdr = the adapter's MAC, MTU 1500),
   `feth9100` its peer for raw I/O; BPF on the peer (BIOCSBLEN 128 KiB, immediate, see-sent off, header-complete
   on, promiscuous, 250 ms read timeout) + `AF_NDRV` bound and connected to the peer, as ZeroTier's tap agent
   does. A leftover pair from an earlier run is removed; a foreign `feth4100`/`feth9100` is left alone.
2. Bridge (`Sources/Shared/bridge.c` rules, tested): RX loop → Ethernet → AF_NDRV; a second thread: BPF →
   AIC TX descriptor → bulk OUT 0x01 (a mutex serialises the data pipe with EAPOL replies). Dropped: EAPOL
   either way (the supplicant owns it), frames from macOS with another source address or over 1518 bytes,
   unencrypted frames once keys are installed, unicast for other addresses, and **our own broadcasts reflected by
   the AP** (otherwise macOS sees its own ARP/ND and reports address conflicts). Group rekeys keep working.
3. A persistent network service for feth4100 (SystemConfiguration preferences: `/NetworkServices/<fixed id>`
   with Interface {DeviceName feth4100, Ethernet}, IPv4 DHCP, IPv6 Automatic, linked into the current set and
   appended to its ServiceOrder; removed on exit, at the next `join`, or by `macwifictl cleanup`). Attempt 1 used
   `ipconfig set feth4100 DHCP`: macOS bound a lease, then stopped the temporary service 13 s later at a network
   configuration change ("DHCP feth4100: stop / RELEASE", "EAPOLController: feth4100 is no longer configured");
   ipconfig(8) says such services only last until the next change. `ipconfig set` stays as the fallback, re-set
   by a watchdog. Then a self-test bound to `feth4100` (ping router and 1.1.1.1 with `ping -b`,
   default-route interface, resolvers mentioning feth4100, `scutil --nwi` order); `ipconfig set feth4100 NONE`
   and interface removal on exit (Ctrl-C, SIGTERM, SIGHUP). The open question stays: does the `ipconfig`
   (temporary) service get DNS and become primary when the built-in Wi-Fi is off? If not: register a proper
   network service via SystemConfiguration; fallback `utun` + in-daemon DHCP only if both fail.
   → **Checkpoint: built-in Wi-Fi off, UGREEN connected; Safari loads pages, `ping apple.com` works,
   `networkQuality` numbers logged.**

## Phase 5: product

Built and run on the adapter 2026-11-07 (install, join, sleep/wake twice with automatic rejoin; see HARDWARE_LOG).
Release polish for 0.6.0: RX with 16 asynchronous reads (`rx_async.c`, `rxq.c`) after an AP dropped us under load
(reason 34), A-MPDU receive reordering (`reorder.c`), 5 GHz preference, versioning, firmware download at install,
log rotation, diagnostics report, release workflow, user documentation (README, docs/ARCHITECTURE,
TROUBLESHOOTING, PROTOCOL, SECURITY, CONTRIBUTING, CHANGELOG).

- `macwifid` (`Sources/macwifictl/daemon.c`, compiled into the same binary as `macwifictl`; LaunchDaemon
  `io.github.fpm8588.macwifid`, KeepAlive, log `/Library/Logs/macwifid.log`): owns the adapter; polls for it,
  mode-switches, loads firmware, starts the radio, auto-joins the strongest remembered network (`knownnet.c`),
  rejoins after drops with backoff, handles unplug (IN-pipe failure), Wi-Fi on/off (persisted). The connect steps
  are shared with the CLI (`scan_pick`, `conn_scan` through `pump`, `conn_associate`, `bridge_*`, `conn_leave`).
  Open networks are supported too.
- Control socket `/var/run/macwifid.sock`, root:admin 0660, one JSON request per connection (`json.c`), a thread
  per client; commands are handed to the main thread through a one-slot mailbox.
- Remembered networks: SSID + PMK (never the passphrase), `/Library/Application Support/macwifi/networks`, 0600.
- Sleep/wake (`power.c`, IORegisterForSystemPower): no power assertions; idle-sleep questions answered yes;
  on kIOMessageSystemWillSleep the main thread leaves the network, removes the interface and service and releases
  USB, then sleep is allowed (callback waits at most 20 s); on kIOMessageSystemHasPoweredOn the adapter is found
  again in whatever mode it woke (driver CD, ROM, running firmware) and the remembered network is rejoined.
- Menu bar app (`App/MacWifi`, SwiftUI MenuBarExtra, macOS 13+, built with `swiftc`): networks with signal bars,
  lock, band; join (password prompt only for new secured networks), disconnect, forget, on/off; LaunchAgent
  `io.github.fpm8588.macwifi.menu` starts it at login (Quit keeps it closed until the next login).
- `./build.sh pkg`: ad-hoc-signed component package (non-relocatable app), pre/postinstall scripts that stop and
  start the service and the app; `scripts/uninstall.sh` (also installed next to the service).
   → **Checkpoint: install the package, join from the menu, unplug/replug, sleep the Mac and wake it; the
   connection comes back by itself.**

## WPA3-Personal (added 2026-11-11, simulated; adapter test pending)

- Firmware side, from the Linux driver: `SM_CONNECT_REQ` with `auth_type` WLAN_AUTH_SAE (3) and flags
  `CONTROL_PORT_HOST | WPA_WPA2_IN_USE | MFP_IN_USE` (0x19) → `SM_EXTERNAL_AUTH_REQUIRED_IND` (0x1806; vif, ssid
  at 1, bssid at 34, akm at 40) → host runs SAE → `SM_EXTERNAL_AUTH_REQUIRED_RSP` (0x1807; vif, status at 2),
  cfm 0x180C → association → 4-way handshake. Authentication frames: TX as `rwnx_start_mgmt_xmit` (hostdesc
  flags `TXU_CNTRL_MGMT`, staid 0xFF, tid 0xFF, hardware queue VO, whole 802.11 frame as payload); RX on the data
  path with `flags_is_80211_mpdu`. IGTK: `MM_KEY_ADD_REQ` cipher `MAC_CIPHER_BIP_CMAC_128` (5), key index 4/5,
  sta 0xFF.
- Host side (original code): `p256.c` (Montgomery arithmetic, Jacobian points, fixed-length scalar
  multiplication; checked against OpenSSL), `sae.c` (group 19, hunting-and-pecking with 40 fixed rounds,
  anti-clogging tokens, confirm verification; IEEE 802.11-2020 Annex J.10 vector), `wpa.c` (KDF-SHA256, AES-CMAC),
  `eapol.c` (key descriptor version 0, IGTK KDE, RSN element with AKM 8 + PMF + BIP).
- Policy: WPA3 when the AP offers it and the password is known, WPA2 fallback on transition networks when SAE
  fails for a non-password reason; password stored (root-only) only for networks that offer WPA3.
- Not done: hash-to-element (H2E; H2E-only APs are shown as unsupported), PMKSA caching, SAE-PK, other groups.
- To learn on the adapter: does the firmware send the IND and forward Authentication frames as assumed; does it
  take the BIP key (a refusal is logged and tolerated).

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
