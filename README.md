# MacWifi: USB Wi-Fi for Apple Silicon Macs (AIC8800 / UGREEN AX900)

[![CI](https://github.com/fpm8588/macwifi-usb/actions/workflows/ci.yml/badge.svg)](https://github.com/fpm8588/macwifi-usb/actions/workflows/ci.yml)

A free, open-source driver that makes **AICsemi AIC8800D80 USB Wi-Fi adapters**, like the **UGREEN AX900
(CM762)**, work on **Apple Silicon Macs**. No kernel extension, no DriverKit, no changes to System Integrity
Protection, no paid Apple developer account.

macOS has no way for third parties to add Wi-Fi hardware, so MacWifi runs as a small background service that
drives the adapter over USB and gives macOS an ordinary network connection. You pick networks from its own menu
bar icon (the adapter doesn't appear in the system Wi-Fi menu); everything else (Safari, apps, DNS, IPv6) just
uses it like any other connection.

> **Status: public beta (0.6).** So far it has been tested on one Mac (macOS 27) with a UGREEN AX900. Bug reports
> with the diagnostics report (see [Reporting a problem](#reporting-a-problem)) are very welcome.

## Contents

- [What works](#what-works) · [Supported adapters](#supported-adapters) · [Install](#install) ·
  [Using it](#using-it) · [Uninstall](#uninstall) · [Troubleshooting](docs/TROUBLESHOOTING.md) ·
  [Reporting a problem](#reporting-a-problem) · [How it works](#how-it-works) · [Privacy and security](#privacy-and-security) ·
  [Building from source](#building-from-source) · [Contributing](CONTRIBUTING.md) · [License](#license)

## What works

- WPA3-Personal (SAE, both the original "hunting-and-pecking" and the newer "hash-to-element" password
  derivation, used automatically when the access point advertises it) with management frame protection, WPA2-Personal, WPA2/WPA3 "transition" networks (WPA3 is
  used; WPA2 is the fallback) and open networks, 2.4 and 5 GHz, 802.11n/ac/ax access points. WPA3 is new and has
  only been run against a simulated access point so far.
- Captive portals (hotel, café, airport "sign in to continue" pages): detected over the adapter, and the menu
  opens the sign-in page, like macOS does for its own Wi-Fi
- A menu bar app: networks with signal strength, lock and band; join; disconnect; forget; Wi-Fi on/off
- Remembered networks, joined automatically when the adapter is plugged in, after a drop, and after sleep
- Sleep and wake: the Mac sleeps normally; MacWifi disconnects cleanly first and reconnects after waking
- macOS sees a normal network service ("UGREEN USB Wi-Fi (macwifi)") and configures it itself (DHCP and DNS;
  IPv6 is enabled too but hasn't been tested yet); it's placed after your other connections, so wired Ethernet
  or the built-in Wi-Fi still win when they're connected

Not yet: WPA2 networks that require management frame protection, WPA2/WPA3-Enterprise, WEP, hidden networks, 6 GHz, roaming between access points of one network while connected, picking your regulatory
country (a worldwide channel set is used: channels 1–14 at 2.4 GHz and 36–165 at 5 GHz; access points on DFS
radar channels 52–144 are scanned but joining them hasn't been tested). See [CHANGELOG.md](CHANGELOG.md).

## Supported adapters

| Adapter | USB IDs | Status |
|---|---|---|
| UGREEN AX900 (CM762) | `a69c:5723` (driver CD) → `a69c:8d81` | **Tested** |
| Other AIC8800D80 adapters (e.g. some Tenda models) | `a69c:5721`–`5725`, `a69c:572f` → `a69c:8d80`/`8d81` | Should work (same chip), untested |
| AIC8800 D40, DC, DW and other variants | `a69c:8800`, `8d40`, … | Detected but not supported yet (different firmware) |

Not sure what you have? With the adapter plugged in, run `system_profiler SPUSBDataType | grep -B3 -A6 a69c`, or
install MacWifi and run `macwifictl list --all`.

**Mac:** Apple Silicon (M1 or later), macOS 13 Ventura or newer. Developed on macOS 27.

## Install

1. Download `MacWifi-<version>.pkg` from the [latest release](https://github.com/fpm8588/macwifi-usb/releases).
2. Install it. The package isn't signed with a paid Apple certificate, so double-clicking may say it "can't be
   opened". Either right-click it → **Open** → **Open**, or install from Terminal:
   ```bash
   sudo installer -pkg ~/Downloads/MacWifi-0.6.0.pkg -target /
   ```
   The installer downloads the adapter's firmware (AICsemi's; it isn't redistributed here) from pinned commits
   of the Linux driver and checks every file's SHA-256, so the Mac needs internet access while installing.
3. A Wi-Fi icon appears in the menu bar. Plug in the adapter, click the icon, pick your network, type its password.

**Upgrading:** install the newer package the same way; remembered networks are kept.
Prefer to build it yourself? See [Building from source](#building-from-source).

## Using it

- **Menu bar icon**: lists nearby networks (strongest first) with signal bars, a lock for secured networks and
  the band. Click one to join; you're asked for the password only the first time. Right-click a remembered
  network to forget it. The switch at the top turns the adapter's Wi-Fi on and off.
- **Plug and play**: when you plug the adapter in, MacWifi switches it out of its "driver CD" mode, loads the
  firmware into it (nothing is written to the adapter itself), and joins the strongest remembered network.
- **Sleep**: MacWifi never keeps your Mac awake. It leaves the network before sleep and rejoins after wake.
- **Quit** in the menu closes only the menu; the connection stays up. The menu comes back at the next login.
- **Terminal**: `macwifictl status` shows what the service is doing, `macwifictl networks` scans. The service's
  log is `/Library/Logs/macwifid.log`.
- **Which connection does macOS use?** The adapter's service is last in the service order, so a connected wired
  Ethernet or the built-in Wi-Fi is preferred. To make the adapter preferred, use System Settings → Network → ⋯ →
  Set Service Order, or just turn the built-in Wi-Fi off.

## Uninstall

```bash
sudo "/Library/Application Support/macwifi/uninstall.sh"
```

This removes the service, the menu bar app, the remembered networks and the logs. Unplug the adapter afterwards;
it returns to its factory state (the firmware only ever lives in its RAM).

## Reporting a problem

Run this and paste the output into a [new issue](https://github.com/fpm8588/macwifi-usb/issues/new/choose):

```bash
sh "/Library/Application Support/macwifi/report.sh"
```

It masks MAC addresses, IP addresses and network names; please look it over anyway, and never paste a password.
Common problems and fixes are in [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## How it works

```
 Safari, apps ── macOS networking ── feth4100 (virtual Ethernet, a normal network service: DHCP, DNS, IPv6)
                                         │  frames
 menu bar app ── control socket ──  macwifid  (root background service, user space)
                                         │  USB (IOKit), Wi-Fi firmware messages, WPA2 handshake, bridge
                                    UGREEN AX900 (AIC8800D80, "FullMAC": the chip does the 802.11 work)
```

The adapter's chip is "FullMAC": its firmware (loaded by MacWifi at plug-in) handles the radio, association and
encryption. MacWifi talks to it with the same messages as the Linux driver, runs the WPA2 4-way handshake itself,
installs the keys in the chip, and moves Ethernet frames between the chip and a macOS `feth` virtual Ethernet
interface. Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); the control socket API:
[docs/PROTOCOL.md](docs/PROTOCOL.md); research and design notes: [docs/RESEARCH.md](docs/RESEARCH.md).

## Privacy and security

- Your Wi-Fi password goes from the menu (or the terminal) straight to the service and is **never logged**.
  Remembered networks are kept in a file only root can read (`/Library/Application Support/macwifi/networks`):
  for WPA2-only networks as the derived key (PMK), not the password; for networks that offer WPA3 the password
  itself is kept too, because WPA3 runs a fresh password exchange at every sign-in (every WPA3 device has to
  keep it; macOS keeps its own in the keychain).
- The service runs as root (it has to, to create a network interface); the menu talks to it over a socket only
  administrators can open.
- Nothing is sent anywhere except your normal network traffic and, at install time, the firmware download.
- More, and how to report a vulnerability: [SECURITY.md](SECURITY.md).

## Building from source

You need the free Xcode Command Line Tools (`xcode-select --install`); no Xcode, no Homebrew.

```bash
git clone https://github.com/fpm8588/macwifi-usb.git && cd macwifi-usb
scripts/fetch-firmware.sh
./build.sh test && ./build.sh pkg
sudo installer -pkg build/MacWifi.pkg -target /
```

`./build.sh test` runs the hardware-free tests, including a full simulation of the adapter, an access point and
macOS (see [CONTRIBUTING.md](CONTRIBUTING.md)). A package you build yourself includes the firmware you
downloaded: fine for your own Macs, but please don't share it; the release package downloads the firmware at
install time instead.

### Developer commands

With the service uninstalled (these talk to the adapter directly and refuse while the service owns it):

```bash
./build.sh cli
build/macwifictl list --all                  # find the adapter
build/macwifictl scan                        # bring it up and list networks
build/macwifictl connect "YourNetwork"       # join, ask DHCP for an offer, count traffic, leave
sudo build/macwifictl join "YourNetwork"     # join and give macOS a network interface until Ctrl-C
```

`MACWIFI_DEBUG=1` in front of any command dumps the USB traffic (key material is never printed). The full list:
`build/macwifictl --help`. The phase-by-phase development plan is in [docs/PROMPT.md](docs/PROMPT.md) and every
hardware test in [docs/HARDWARE_LOG.md](docs/HARDWARE_LOG.md).

## License

GPL-2.0-only, because parts port the GPL-2.0 AIC8800 Linux driver
([shenmintao/aic8800d80](https://github.com/shenmintao/aic8800d80), derived from AICsemi's driver). Files written
from scratch for this project are also available under the MIT license; [NOTICE.md](NOTICE.md) lists which. The
adapter firmware is AICsemi's, has no open-source license, and is never part of this repository or its release
packages.

## Acknowledgements

The AIC8800 Linux driver authors and maintainers (the hardware facts come from their code); ZeroTier, whose
`MacEthernetTapAgent` documented how macOS `feth` interfaces can carry Ethernet frames for a user-space program.
