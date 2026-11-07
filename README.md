# macwifi-usb

A free, open-source **user-space driver for MediaTek MT7612U USB Wi-Fi adapters** on **Apple Silicon Macs
running macOS 27 Golden Gate**: no kernel extension, no DriverKit, no SIP changes, no paid Apple account.

Apple's driver frameworks have no Wi-Fi support for third-party hardware, so this driver runs as a background
program that drives the adapter over USB itself and hands traffic to macOS through a virtual Ethernet interface.
You pick networks in its own menu bar app; it won't show up in the system Wi-Fi menu. Details:
[docs/RESEARCH.md](docs/RESEARCH.md).

**Supported adapters:** MT7612U / MT7632U / MT7662U, e.g. Alfa AWUS036ACM, Netgear A6210, Asus USB-AC55/AC54,
TP-Link TL-WDN6200, Edimax EW-7722UAC (full list in `Sources/Shared/mt76x2u.h`).

> Want something that works today on a 2.4 GHz-only budget? The MT7601U already runs end to end on Apple Silicon with
> [mt7601u-macos](https://github.com/ZemichaelMD/MacOS-M1-mt7601u-userspace-wifi-driver-app).

## Status

| Phase | What | State |
|---|---|---|
| 1 | `macwifictl`: find the adapter, dump descriptors, read chip registers; WPA2 crypto, 802.11 framing and MT76x2U RX/TX descriptor library with tests | Code done; **needs a run with the adapter** |
| 2 | Power on, firmware, calibration, RX, passive scan | Next |
| 3 | TX, association, WPA2-PSK 4-way handshake, data path | Planned |
| 4 | macOS networking via a feth (virtual Ethernet) pair | Planned |
| 5 | LaunchDaemon, menu bar app, installer | Planned |

The phase-by-phase plan, written as a Claude Code prompt, is [docs/PROMPT.md](docs/PROMPT.md).

## Try Phase 1

```bash
xcode-select --install            # once, free
git clone https://github.com/fpm8588/macwifi-usb.git && cd macwifi-usb
./build.sh test                   # hardware-free tests (WPA2 vectors, 802.11 parsing/framing)
./build.sh cli
build/macwifictl list --all       # what's plugged in; supported adapters are marked *
build/macwifictl info
build/macwifictl identify         # reads the chip ID register
```

## Recovery

Later phases install a background daemon. To remove everything it sets up:

```bash
sudo launchctl bootout system/io.github.fpm8588.macwifid 2>/dev/null
for i in $(ifconfig -l | tr ' ' '\n' | grep '^feth'); do sudo ifconfig $i destroy; done
```

## License

MIT for this repository's code. Parts ported from Linux mt76 keep their BSD-3-Clause-Clear notices (see
[NOTICE.md](NOTICE.md)). Firmware is MediaTek's, downloaded from linux-firmware by `scripts/fetch-firmware.sh`
and never committed.
