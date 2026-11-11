# Changelog

## Unreleased

**New**
- Bluetooth, phase B1: `macwifictl bt` opens the adapter's own Bluetooth controller from user space (no kext),
  identifies it and lists nearby Bluetooth LE and classic devices. Plan: `docs/BLUETOOTH_PROMPT.md`.

**Changed**
- Faster Wi-Fi (to be measured): outgoing data frames are queued, up to 64 USB transfers in flight like the
  Linux driver, instead of waiting for each transfer to finish. Downloads depend on these outgoing ACKs.
- Sleep: the service now waits at most 3 s (was 20 s) for the adapter to shut down before letting the Mac sleep,
  and logs how long it took, so closing the lid always sleeps promptly. It still never takes a power assertion.

## 0.6.0 (first public beta)

The first release meant for other people: install a package, pick a network from the menu bar, done.

**New**
- `macwifid`, a background service that owns the adapter: switches it out of driver-CD mode, loads the firmware,
  joins the strongest remembered network, rejoins after drops, handles unplugging, Wi-Fi on/off.
- MacWifi menu bar app: networks with signal bars, lock and band; join (password asked once); disconnect;
  forget; on/off switch. Starts at login.
- A wider menu that shows up to ten networks at once, and an app icon (original art, in `App/MacWifi/Icon`).
- Sleep and wake: never keeps the Mac awake; disconnects cleanly before sleep and rejoins after wake.
- Remembered networks are stored as WPA2 keys (PMKs) in a root-only file, never as passwords.
- 5 GHz is preferred over 2.4 GHz unless it's clearly weaker.
- **WPA3-Personal (SAE)**, including WPA3-only networks and WPA2/WPA3 transition networks (WPA3 first, WPA2 as
  the fallback), with management frame protection (the chip gets the BIP integrity key). Own SAE and P-256 code,
  checked against the IEEE 802.11-2020 test vectors and OpenSSL. Both password-element methods: hunting-and-pecking
  and hash-to-element (H2E), which access points that advertise it (RSNXE) or require it (many school and
  enterprise-managed WPA3-Personal networks) get. Simulated only so far: not yet run on an adapter.
  For networks that offer WPA3 the password itself is remembered (root-only file), since WPA3 needs it at every
  sign-in; entries remembered before this version keep using WPA2 until the password is entered again.
- Open (password-less) networks.
- Captive portals: after connecting, the service checks `captive.apple.com` over the adapter; when a sign-in page
  is in the way the menu says so and opens it in a sign-in window, and closes it once you're through.
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
- Not yet: WPA2 with required PMF, enterprise, WEP, hidden networks, 6 GHz, roaming between
  access points while connected, regulatory country selection. Untested: IPv6, access points on DFS channels.
- The package is ad-hoc signed: right-click → Open, or `sudo installer -pkg …`.

## Earlier development (not released)

Phases 1–4 (2026-11-07): USB mode switch and chip identification, firmware loading (SDK V3 profile for MCU1 chips),
FullMAC bring-up, scanning, WPA2-PSK connection, and bridging to macOS through a `feth` pair. Every hardware run is
recorded in [docs/HARDWARE_LOG.md](docs/HARDWARE_LOG.md).
