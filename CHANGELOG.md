# Changelog

## 0.6.0 (first public beta)

The first release meant for other people: install a package, pick a network from the menu bar, done.

**New**
- `macwifid`, a background service that owns the adapter: switches it out of driver-CD mode, loads the firmware,
  joins the strongest remembered network, rejoins after drops, handles unplugging, Wi-Fi on/off.
- MacWifi menu bar app: networks with signal bars, lock and band; join (password asked once); disconnect;
  forget; on/off switch. Starts at login.
- Sleep and wake: never keeps the Mac awake; disconnects cleanly before sleep and rejoins after wake.
- Remembered networks are stored as WPA2 keys (PMKs) in a root-only file, never as passwords.
- 5 GHz is preferred over 2.4 GHz unless it's clearly weaker.
- Open (password-less) networks.
- Installer package (the release package downloads the firmware at install time and checks its SHA-256) and an
  uninstaller; `report.sh` prints a diagnostics report with MAC/IP addresses and network names masked;
  `macwifictl --version`, `status`, `networks`.

**Fixed**
- Access points could drop the connection under heavy download ("reason 34", too many unacknowledged frames):
  USB receive now keeps 16 reads in flight, as the Linux driver keeps ~20.
- Frames of block-ack sessions are now put back in order (64-frame window, 50 ms), as the Linux driver does;
  out-of-order delivery made TCP slow down.
- Several packets in one USB transfer were misread after an odd-length packet (data packets aren't padded).
- macOS dropped the temporary network service `ipconfig set` creates at the next configuration change; the
  adapter now gets a persistent network service while connected.

**Known limitations**
- Tested on one Mac (macOS 27) with a UGREEN AX900 so far.
- Not yet: WPA3-only (SAE), PMF-required networks, enterprise, WEP, hidden networks, 6 GHz, roaming between
  access points while connected, regulatory country selection. Untested: IPv6, access points on DFS channels.
- The package is ad-hoc signed: right-click → Open, or `sudo installer -pkg …`.

## Earlier development (not released)

Phases 1–4 (2026-11-07): USB mode switch and chip identification, firmware loading (SDK V3 profile for MCU1 chips),
FullMAC bring-up, scanning, WPA2-PSK connection, and bridging to macOS through a `feth` pair. Every hardware run is
recorded in [docs/HARDWARE_LOG.md](docs/HARDWARE_LOG.md).
