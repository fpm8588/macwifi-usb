# macwifi-usb

A free, open-source **user-space driver for the UGREEN AX900 (AICsemi AIC8800) USB Wi-Fi adapter** on
**Apple Silicon Macs running macOS 27 Golden Gate**: no kernel extension, no DriverKit, no SIP changes, no paid
Apple account.

Apple's driver frameworks don't support third-party Wi-Fi hardware, so this runs as a background program that
drives the adapter over USB and hands traffic to macOS through a virtual Ethernet interface. You pick networks
in its own menu bar app; it won't appear in the system Wi-Fi menu. Background and design:
[docs/RESEARCH.md](docs/RESEARCH.md).

**Adapters:** AICsemi AIC8800 family; developed and tested on a UGREEN AX900 that enumerates as `a69c:5723`
(driver-CD mode). Other AIC8800D80 adapters (`a69c:57xx` → `a69c:8d80/8d81`) should behave the same.

## Status

| Phase | What | State |
|---|---|---|
| 1 | `macwifictl`: find the adapter, switch it out of driver-CD mode, read the chip revision through the ROM; message framing + WPA2 crypto + 802.11 parsing libraries with tests | **Passed on the adapter** (2026-11-07) |
| 2a | `macwifictl fwload`: upload the ROM patches and Wi-Fi firmware, start it | **Passed on the adapter** (2026-11-07) |
| 2b | `macwifictl up`: start the Wi-Fi firmware, calibrate, read the MAC address | **Passed on the adapter** (2026-11-07) |
| 3a | `macwifictl scan`: start the radio and list nearby networks | **Passed on the adapter** (2026-11-07) |
| 3b | `macwifictl connect "SSID"`: data path, association, WPA2-PSK handshake, key install, DHCP probe | **Passed on the adapter** (2026-11-07) |
| 4 | `sudo macwifictl join "SSID"`: macOS networking through a feth (virtual Ethernet) pair | **Passed on the adapter** (2026-11-07: Safari and a speed test over the UGREEN) |
| 5 | LaunchDaemon, menu bar app, installer | Next |

The phase-by-phase plan, written as a Claude Code prompt: [docs/PROMPT.md](docs/PROMPT.md). Results from the
real adapter: [docs/HARDWARE_LOG.md](docs/HARDWARE_LOG.md).

## Try it

Install the free Xcode Command Line Tools once with `xcode-select --install`. Then paste this (it has no
comments, so it pastes cleanly into zsh):

```bash
git clone https://github.com/fpm8588/macwifi-usb.git && cd macwifi-usb
./build.sh test && ./build.sh cli
scripts/fetch-firmware.sh
build/macwifictl scan
build/macwifictl connect "YourNetwork"
sudo build/macwifictl join "YourNetwork"
```

What each command does:

- `./build.sh test` runs the hardware-free tests; `./build.sh cli` builds `build/macwifictl` (arm64).
- `scripts/fetch-firmware.sh` downloads AICsemi's firmware into `firmware/` (not part of this repository) from
  pinned commits of the Linux driver and checks every file's SHA-256. There are two matched sets: newer chips
  take the SDK V5 set; older "MCU1" chips, like the UGREEN AX900 tested here, need the SDK V3 set
  (`fwload` picks automatically and prints which).
- `sudo macwifictl join "SSID" [--seconds N]` connects like `connect`, then gives macOS a network interface,
  `feth4100`, and passes traffic between it and the adapter until you press Ctrl-C (or for N seconds). While it
  runs, `feth4100` is a regular network service ("UGREEN USB Wi-Fi (macwifi)", last in the service order), so
  macOS configures it by DHCP and switches to it when your other connections go away. It needs `sudo` because creating a network interface needs root. It
  prints a self-test (ping through `feth4100`, which interface macOS prefers, DNS). For the full test, turn the
  Mac's own Wi-Fi off while `join` runs and use Safari, `ping apple.com` and `networkQuality`; turn Wi-Fi back
  on before pressing Ctrl-C. On exit the service, `feth4100` and its peer `feth9100` are removed.
- `macwifictl connect "SSID" [--seconds N]` runs `up`, finds the network, asks for its password on the
  terminal (typed hidden; it is never printed, logged or saved), joins with WPA2-PSK, then asks the DHCP server
  for an offer (without taking an address) and counts the traffic it hears for N seconds (default 15) before
  leaving. It's the current end-to-end test. WPA2/WPA3 transition networks work through their WPA2 side;
  WPA3-only networks and networks that require PMF aren't supported yet.
- `macwifictl scan [SSID]` runs `up`, starts the radio and lists nearby networks (SSID, channel, signal,
  security).
- `macwifictl up` does everything below in one go: switch modes, load the firmware, then start the Wi-Fi
  firmware and read the adapter's MAC address. Each step also exists on its own:
- `macwifictl modeswitch` ejects the adapter's virtual driver CD, which switches it to Wi-Fi mode (`a69c:8d80`).
  Needed after every plug-in.
- `macwifictl fwload` uploads the firmware to the adapter's RAM and starts it; the adapter then reconnects as
  `a69c:8d81`. Nothing is written to the adapter's flash; unplugging it returns it to factory state. If it
  stops partway, unplug the adapter and plug it back in before trying again.
- Also: `list` (find the adapter), `info` (USB descriptors), `identify` (chip revision through the ROM).

If anything looks off, rerun with `MACWIFI_DEBUG=1` in front of the command to see every USB transfer.

## Recovery

Unplugging the adapter resets it completely (the firmware only lives in its RAM). If `join` was killed without
cleaning up, remove its network service ("UGREEN USB Wi-Fi (macwifi)" in System Settings > Network) and its two
interfaces; `join` also does this by itself the next time it runs:

```bash
sudo build/macwifictl cleanup
```

Phase 5 will add a background daemon; its removal command will be listed here.

## License

GPL-2.0-only (later phases port the GPL-2.0 AIC8800 Linux driver). Files written from scratch for this project
are also available under MIT; see [NOTICE.md](NOTICE.md). Firmware is AICsemi's and is downloaded by
`scripts/fetch-firmware.sh`, never committed.
