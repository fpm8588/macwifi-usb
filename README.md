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
| 2a | `macwifictl fwload`: upload the ROM patches and Wi-Fi firmware, start it | Code done; **needs a run on the adapter** |
| 2b | FullMAC bring-up, MAC address | Next |
| 3 | Scan, connect, WPA2-PSK handshake, data path | Planned |
| 4 | macOS networking via a feth (virtual Ethernet) pair | Planned |
| 5 | LaunchDaemon, menu bar app, installer | Planned |

The phase-by-phase plan, written as a Claude Code prompt: [docs/PROMPT.md](docs/PROMPT.md). Results from the
real adapter: [docs/HARDWARE_LOG.md](docs/HARDWARE_LOG.md).

## Try it

Install the free Xcode Command Line Tools once with `xcode-select --install`. Then paste this (it has no
comments, so it pastes cleanly into zsh):

```bash
git clone https://github.com/fpm8588/macwifi-usb.git && cd macwifi-usb
./build.sh test && ./build.sh cli
scripts/fetch-firmware.sh
build/macwifictl modeswitch
build/macwifictl fwload
```

What each command does:

- `./build.sh test` runs the hardware-free tests; `./build.sh cli` builds `build/macwifictl` (arm64).
- `scripts/fetch-firmware.sh` downloads AICsemi's firmware into `firmware/` (not part of this repository) from a
  pinned commit of the Linux driver and checks every file's SHA-256.
- `macwifictl modeswitch` ejects the adapter's virtual driver CD, which switches it to Wi-Fi mode (`a69c:8d80`).
  Needed after every plug-in.
- `macwifictl fwload` uploads the firmware to the adapter's RAM and starts it; the adapter then reconnects as
  `a69c:8d81`. Nothing is written to the adapter's flash; unplugging it returns it to factory state.
- Also: `list` (find the adapter), `info` (USB descriptors), `identify` (chip revision through the ROM).

If anything looks off, rerun with `MACWIFI_DEBUG=1` in front of the command to see every USB transfer.

## Recovery

Later phases install a background daemon. To remove everything it sets up:

```bash
sudo launchctl bootout system/io.github.fpm8588.macwifid 2>/dev/null
for i in $(ifconfig -l | tr ' ' '\n' | grep '^feth'); do sudo ifconfig $i destroy; done
```

## License

GPL-2.0-only (later phases port the GPL-2.0 AIC8800 Linux driver). Files written from scratch for this project
are also available under MIT; see [NOTICE.md](NOTICE.md). Firmware is AICsemi's and is downloaded by
`scripts/fetch-firmware.sh`, never committed.
