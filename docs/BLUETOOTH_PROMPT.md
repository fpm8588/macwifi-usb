# Reference prompt: Bluetooth and dual-band for macwifi-usb

Use this file as the brief for every Bluetooth and band-related change. Read it together with
`docs/PROMPT.md` (Wi-Fi phases, constraints) and `CLAUDE.md`. Update the checklists here as phases pass on
the adapter, and record each hardware result (redacted) in `docs/HARDWARE_LOG.md`.

## Goal

Make the UGREEN AX900 (AICsemi AIC8800D80, `a69c:8d81` after the mode switch) useful on an Apple Silicon Mac
for both of its radios:

1. **Wi-Fi on 2.4 GHz and 5 GHz**, chosen automatically or by the user, with no regressions.
2. **Bluetooth** (the same chip's Bluetooth 5 controller), as far as it can go in user space for $0.

## What we know (facts, with sources)

- After the mode switch the adapter exposes three USB interfaces (hardware log, Phase 2a):
  - 0: Bluetooth HCI, class e0/01/01. EP 0x83 interrupt IN (events), 0x84 bulk IN / 0x04 bulk OUT (ACL).
  - 1: Bluetooth SCO, isochronous 0x85/0x05, alt settings 0 to 5.
  - 2: vendor Wi-Fi (our existing driver).
- The firmware load we already do (ADID, ROM patch, patch table, then Wi-Fi firmware; profile
  `legacy-mcu1`) is the same sequence Linux `aic_load_fw` runs before the **standard** Linux `btusb` driver
  binds to interface 0. The Linux tree (`README.md`, `diagnose_bt.sh`) says no custom Bluetooth transport is
  needed: the controller speaks plain HCI over USB.
- One quirk (Linux `aic_zlp_quirk.c`): ACL bulk OUT transfers whose length is a multiple of the endpoint's max
  packet size need a trailing zero-length packet.
- HCI over USB (Bluetooth Core spec, Vol 4 Part B): commands go out as a class control request on the default
  pipe (bmRequestType 0x20, bRequest 0, wIndex = interface 0); events come in on the interrupt endpoint; ACL
  data on the bulk pair; SCO on interface 1's isochronous pair.
- macOS's Bluetooth service (`bluetoothd`) only drives Apple's own controllers. A kext cannot hand it a
  third-party HCI controller on Apple Silicon (no Lilu/BlueToolFixup there), and macOS uses one controller at a
  time. **So the Bluetooth host stack runs in our own process.** Lowering startup security is not needed for any
  phase below and must never be required; it buys nothing for Bluetooth.
- The adapter is one 1x1 radio: Wi-Fi uses one band at a time. "Dual-band" means: scan both bands, join
  either, prefer the better one, let the user pin a band.

## Hard constraints (unchanged)

$0, no kexts, no DriverKit, no Homebrew in the product, never ask to disable SIP, never touch the Mac's built-in
Wi-Fi or Bluetooth. Pure logic in `Sources/Shared` with tests; `./build.sh test` before every commit; CI green.
GPL-2.0 repo. Hardware facts in `Sources/Shared/aic8800.h` (Wi-Fi) or `Sources/Shared/hci.h` (Bluetooth, spec
section cited). No personal data (addresses, names of the user's devices or networks) in the repo or log.

## Architecture

```
            macwifid (root LaunchDaemon, owns the adapter)
            ├── Wi-Fi: interface 2 (existing)
            └── Bluetooth: interface 0 (+1 for audio)
                ├── hci.c        HCI commands/events, LE advertising, inquiry, EIR      (Shared, tested)
                ├── l2cap.c      L2CAP basic mode, signalling, fixed channels           (Shared, tested)
                ├── smp.c        LE pairing (LE Secure Connections, uses p256.c)        (Shared, tested)
                ├── att/gatt.c   GATT client                                            (Shared, tested)
                ├── ssp.c        BR/EDR Secure Simple Pairing (P-256, uses p256.c)      (Shared, tested)
                ├── sdp.c, avdtp.c, sbc.c, a2dp.c, avrcp.c (audio)                      (Shared, tested)
                └── bt_usb.c     USB transport (control/interrupt/bulk/iso), ZLP quirk  (macOS only)
            control socket: new JSON verbs bt_status, bt_scan, bt_devices, bt_pair, bt_connect, bt_forget
MacWifi menu: a "Bluetooth" section (on/off, nearby devices, paired devices, connect/disconnect)
Audio: an AudioServerPlugIn (user-space HAL plug-in, $0, the Mbox3Driver approach) that exposes
       "UGREEN Bluetooth" as an output device and feeds PCM to macwifid over shared memory.
Input: BLE / classic HID reports turned into keyboard and pointer events with CGEvent (needs the user's
       Accessibility approval; no kext, no restricted entitlement).
```

## Requirements the user set (apply to every phase)

### Several Bluetooth devices at once (2026-11-11)
- The host stack keeps a **connection table**, not a single link: one entry per ACL (classic) or LE connection,
  keyed by its HCI connection handle, with its own L2CAP channels, security state and profile (HID, audio...).
- HCI flow control is shared: track the controller's free ACL/LE buffers (Read Buffer Size, LE Read Buffer
  Size, Number Of Completed Packets) across all connections and schedule fairly, so a busy audio link can't
  starve a keyboard.
- Several devices of each kind: e.g. keyboard + mouse + headphones together, or two BLE sensors. The menu lists
  every paired device with its own connect/disconnect and battery level.
- Remembered devices reconnect on their own (like Wi-Fi auto-join), one at a time, in the order last used.
- Known limits to explain, not hide: the controller has a fixed number of simultaneous links (read it from
  the controller and show it); one radio is shared by Wi-Fi and Bluetooth, so heavy use of both lowers both;
  two A2DP headphones at once is out of scope (bandwidth and timing), one audio device plus input devices is
  the target.
- Tests: the simulator gets a fake controller with several concurrent connections, interleaved traffic, and
  disconnects in every order.

### Sleep must never be blocked (2026-11-11)
- Closing the lid or choosing Sleep must put the Mac to sleep promptly with the adapter plugged in, whatever
  Wi-Fi and Bluetooth are doing.
- No power assertions anywhere (no IOPMAssertion, no caffeinate, no audio stream left running that holds one).
  `pmset -g assertions` must never list macwifid or MacWifi.
- On will-sleep the Bluetooth side, like Wi-Fi, stops scanning, disconnects every link (HCI Disconnect,
  reason 0x15 "power off") and closes the USB interface within the same short budget (`MW_SLEEP_WAIT_MS`,
  3 s, daemon.c); if anything is still busy when the budget ends, sleep goes ahead anyway.
- The HAL audio plug-in reports the device as gone before sleep so Core Audio doesn't keep it running.
- The adapter must not wake the Mac: don't arm USB remote wakeup; check `pmset -g log` after a night asleep
  for wakes caused by the adapter.
- After wake: Wi-Fi first (auto-join), then reconnect remembered Bluetooth devices.
- Every phase's hardware checkpoint includes: close the lid with the feature active, wait a minute, open it,
  confirm it slept (`pmset -g log | grep -E " Sleep | Wake " | tail -4`) and recovered.

## Phases (stop at each hardware checkpoint and ask the user to run it)

### W1: band control (Wi-Fi)
- [x] Scan and join on both 2.4 GHz and 5 GHz (hardware log: both bands scanned and joined).
- [x] Auto prefers 5 GHz over a slightly stronger 2.4 GHz BSS of the same network.
- [ ] A band setting: Auto / 5 GHz only / 2.4 GHz only, stored by macwifid, shown in the menu, honoured by
      auto-join and manual joins, covered by the simulator.
- [ ] Menu shows the current channel and band of the connection.

### B1: reach the Bluetooth controller (checkpoint)
- [ ] `macwifictl bt [--seconds N]`: open interface 0 next to the running Wi-Fi driver; HCI Reset; Read Local
      Version; Read BD_ADDR; LE scan plus classic inquiry for N seconds; list devices (type, name, RSSI).
- Pass: version and address read, and nearby devices listed. If interface 0 cannot be opened or Reset times
  out, record the exact error, then try with the firmware freshly loaded.

### B2: Bluetooth in the service and the menu
- [ ] macwifid opens interface 0 after firmware load, keeps an event reader, survives unplug, sleep/wake.
- [ ] Control verbs `bt_status`, `bt_scan`, `bt_devices`; menu section listing nearby devices.
- [ ] Bluetooth on/off switch (HCI Reset + scan off; the radio stays powered for Wi-Fi).

### B3: LE devices (GATT, pairing)
- [ ] L2CAP fixed channels, ATT/GATT client, LE connection create/cancel/disconnect.
- [ ] SMP LE Secure Connections (Just Works and passkey), bonding keys stored root-only like Wi-Fi passwords.
- [ ] Battery service readout in the menu. Checkpoint: pair a BLE device, read its battery level.

### B4: input devices
- [ ] HID over GATT (BLE keyboards/mice) and classic HID (L2CAP PSM 0x11/0x13) to CGEvent.
- [ ] Checkpoint: type and point with a Bluetooth keyboard/mouse through the dongle.

### B5: audio
- [ ] BR/EDR connections, SSP pairing, SDP, AVDTP, SBC encoder (tested against reference vectors), A2DP source,
      AVRCP volume/play-pause.
- [ ] HAL plug-in "UGREEN Bluetooth" output device; latency target under 250 ms.
- [ ] Checkpoint: play music from the Mac to Bluetooth headphones through the dongle.
- Later: HFP/mSBC microphone over the SCO interface.

## Working style

Smallest testable step first. Every protocol parser gets unit tests and a fuzz loop. Each phase ends with a
hardware checkpoint, a CHANGELOG line and an updated checklist here. Explain to the user what each step can
and cannot do (for example: devices paired through the dongle do not appear in System Settings > Bluetooth).

Recovery if anything misbehaves: unplug the adapter (its firmware lives only in RAM) and plug it back in; to
remove everything, `sudo "/Library/Application Support/macwifi/uninstall.sh"` (README, "Uninstall").
