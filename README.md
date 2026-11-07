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
| 1 | `macwifictl`: find the adapter, switch it out of driver-CD mode, read the chip revision through the ROM; message framing + WPA2 crypto + 802.11 parsing libraries with tests | Code done; **needs a run on the adapter** |
| 2 | Firmware upload, FullMAC bring-up, MAC address | Next |
| 3 | Scan, connect, WPA2-PSK handshake, data path | Planned |
| 4 | macOS networking via a feth (virtual Ethernet) pair | Planned |
| 5 | LaunchDaemon, menu bar app, installer | Planned |

The phase-by-phase plan, written as a Claude Code prompt: [docs/PROMPT.md](docs/PROMPT.md).

## Try Phase 1

```bash
xcode-select --install          # once, free
git clone https://github.com/fpm8588/macwifi-usb.git && cd macwifi-usb
./build.sh test                 # hardware-free tests
./build.sh cli
build/macwifictl list           # should show a69c:5723 in driver-CD mode
build/macwifictl modeswitch     # ejects the virtual driver disc -> Wi-Fi mode
build/macwifictl list           # note the new USB ID
build/macwifictl info           # endpoints of the Wi-Fi interface
build/macwifictl identify       # reads the chip revision through the chip's ROM
```

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
