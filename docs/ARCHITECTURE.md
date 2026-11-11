# How MacWifi works

This is the map for anyone reading or changing the code. The research behind the design (why user space, why
`feth`, what the chip needs) is in [RESEARCH.md](RESEARCH.md); the order things were built and tested in is in
[PROMPT.md](PROMPT.md) and [HARDWARE_LOG.md](HARDWARE_LOG.md).

## The pieces

```
 ┌───────────────────────────── macOS ─────────────────────────────────────────────────────────────┐
 │  apps ─ TCP/IP ─ feth4100 ◄─peer─► feth9100                                                    │
 │                  (network service      ▲ BPF read (frames macOS sends)                          │
 │                   "UGREEN USB Wi-Fi")  │ AF_NDRV write (frames for macOS)                        │
 │                                        │                                                        │
 │  MacWifi.app ── /var/run/macwifid.sock ── macwifid (root LaunchDaemon) ─ IOKit USB ──► adapter  │
 │  (SwiftUI menu)    JSON, one request       threads: main (adapter), control (socket),           │
 │                    per connection          bridge (macOS→air), USB reader (16 reads in flight),  │
 │                                            power (sleep/wake notifications)                     │
 └─────────────────────────────────────────────────────────────────────────────────────────────────┘
```

`macwifid` and `macwifictl` are the same binary (`Sources/macwifictl/main.c`); `macwifid daemon` runs the service.

## Lifecycle of the adapter

1. **Plug-in.** The UGREEN appears as a USB mass-storage "driver CD" (`a69c:5723`). Ejecting it (`diskutil eject`)
   makes it re-enumerate as `a69c:8d80`: the chip's ROM, waiting for firmware.
2. **Firmware load** (`aic_fwload.c`, from the Linux `aic_load_fw`): pick the firmware set from the chip ID register
   (MCU1 chips like the UGREEN need the older SDK V3 set), upload the ROM patch, the patch table and the Wi-Fi
   firmware in 1 KiB blocks, write the patch configuration, start it. The chip re-enumerates as `a69c:8d81`.
   Nothing is written to flash: unplugging returns the adapter to factory state.
3. **Bring-up** (`aic_fmac.c`, from `rwnx_msg_tx.c`): stack start, TX power tables from the user config, RF
   calibration, MAC address, reset, version; then capabilities, channel list (worldwide set), start, coexistence,
   and a station interface.
4. **Scan / connect.** `SCANU_START` with results as messages; `SM_CONNECT_REQ` with our RSN element; the firmware
   authenticates and associates and reports `SM_CONNECT_IND` (station index of the AP).
5. **WPA3 (SAE)**, when the access point offers it: the connect request says `auth_type` SAE, the firmware answers
   `SM_EXTERNAL_AUTH_REQUIRED_IND`, and MacWifi runs the SAE exchange itself (`sae.c`): Authentication frames go
   out through the management TX path (`TXU_CNTRL_MGMT`) and come back on the data path flagged as 802.11 MPDUs.
   `SM_EXTERNAL_AUTH_REQUIRED_RSP` then lets the firmware associate; the PMK SAE produced feeds the 4-way
   handshake (SHA-256 key derivation, AES-CMAC MICs), and the IGTK is installed for management frame protection.
   **WPA2-PSK** (`eapol.c`, our own supplicant): EAPOL frames arrive on the data path; messages 1→2, 3→4 (MIC,
   replay counter, ANonce, RSN element checked against the beacon's, GTK unwrapped); after message 4's TX
   confirmation the pairwise and group keys go to the chip (`MM_KEY_ADD_REQ`) and the control port opens.
6. **Bridge.** Frames from the chip (802.11, already decrypted) become Ethernet (`aic_data.c`), are put back in
   order if they belong to a block-ack session (`reorder.c`), filtered (`bridge.c`) and written to macOS;
   frames macOS sends are read from the `feth` peer, filtered, and handed to the chip with a TX descriptor.
7. **macOS side** (`feth.c`): the `feth4100`/`feth9100` pair is created with the adapter's MAC; a persistent
   network service for `feth4100` is added to the network preferences (a temporary `ipconfig set` service is
   dropped by macOS at the next configuration change), so macOS runs DHCP and makes it primary when nothing
   better is up. Everything is removed when the connection ends.
8. **Sleep/wake** (`power.c`): before sleep the main thread leaves the network, removes the interface and
   releases USB, then sleep is allowed. After wake the adapter is found again in whatever state it woke in.

## Source map

| File | What | License |
|---|---|---|
| `Sources/Shared/aic8800.h` | Hardware facts (IDs, addresses, message layouts), each citing its Linux source | GPL/MIT, see NOTICE |
| `Sources/Shared/aic_msg.[ch]` | USB message framing; RX stream walking (data packets `len + 60`, unpadded) | MIT |
| `Sources/Shared/aic_fwload.[ch]` | Firmware loader: patch tables, profiles | GPL-2.0 (port) |
| `Sources/Shared/aic_fmac.[ch]` | FullMAC messages: bring-up, scan, connect, keys | GPL-2.0 (port) |
| `Sources/Shared/aic_data.[ch]` | TX descriptor, RX header, 802.11 → Ethernet, TX confirmations | GPL-2.0 (port) |
| `Sources/Shared/eapol.[ch]`, `wpa.[ch]`, `crypto_backend.c` | 4-way handshake supplicant (WPA2 and WPA3 key hierarchies) and its crypto | MIT |
| `Sources/Shared/sae.[ch]`, `p256.[ch]` | WPA3 sign-in (SAE, group 19) and the P-256 arithmetic under it | MIT |
| `Sources/Shared/ieee80211.[ch]` | Beacon/probe-response parsing | MIT |
| `Sources/Shared/reorder.[ch]` | A-MPDU receive reordering (64-frame window, 50 ms) | MIT |
| `Sources/Shared/rxq.[ch]` | Thread-safe queue of USB IN transfers | MIT |
| `Sources/Shared/bridge.[ch]` | BPF record walking, forwarding rules | MIT |
| `Sources/Shared/dhcp.[ch]` | DHCPDISCOVER/OFFER (the `connect` probe) | MIT |
| `Sources/Shared/captive.[ch]` | Captive portal check: request, response classification | MIT |
| `Sources/Shared/json.[ch]`, `knownnet.[ch]` | Control-socket JSON, remembered networks | MIT |
| `Sources/macwifictl/main.c` | USB transport, CLI commands, the connect/bridge steps | GPL-2.0 |
| `Sources/macwifictl/daemon.c` | `macwifid` (included by main.c) | GPL-2.0 |
| `Sources/macwifictl/rx_async.[ch]` | 16 asynchronous bulk-IN reads on a run-loop thread | MIT |
| `Sources/macwifictl/feth.[ch]` | feth pair, BPF/AF_NDRV, the network service | MIT |
| `Sources/macwifictl/power.[ch]` | Sleep/wake notifications | MIT |
| `App/MacWifi` | The SwiftUI menu bar app | MIT |
| `packaging/`, `scripts/` | launchd plists, installer scripts, firmware download, uninstall, report | MIT |

## Threads in macwifid

- **Main**: owns the adapter. Firmware messages, association, key installation, scans, bridge housekeeping, the
  state machine (`off`, `no_adapter`, `starting`, `idle`, `scanning`, `connecting`, `connected`, `asleep`,
  `error`). Everything that touches the chip's control path happens here.
- **USB reader** (`rx_async.c`): keeps 16 bulk-IN reads queued so the chip never waits for the host (one
  synchronous read at a time made access points drop us under load); completed transfers go into an `RxQ`.
- **Bridge** (`host_to_air`): reads what macOS sends (BPF) and transmits it; shares the data pipe with EAPOL
  replies under a mutex.
- **Control** (+ one short-lived thread per client): answers status/networks directly; hands scan, join,
  disconnect, forget and power commands to the main thread through a one-slot mailbox and waits for the result.
- **Power**: IOKit sleep/wake callbacks; before sleep it waits (≤ 3 s) for the main thread to shut down cleanly, and never takes a power assertion.

## Testing without hardware

`Tests/sim_connect.c` compiles `main.c` (and with it `daemon.c`) against stand-in IOKit headers and replaces the
USB device with a simulated firmware and WPA2 access point, the `feth` layer with a fake macOS side, and the USB
reader with a thread of the same interface. It runs `connect`, `join` and the whole service over its real control
socket: scans, wrong and right passwords, DHCP through the bridge, every drop rule, out-of-order block-ack frames,
aggregated USB transfers, an AP dropping us, sleep/wake, power off/on, unplug. It runs in CI (and is kept clean
under ThreadSanitizer and AddressSanitizer), and the build fails if a test password ever reaches the output.
