# Troubleshooting

First steps for almost anything: look at the menu's status line, then the service log:

```bash
macwifictl status
tail -50 /Library/Logs/macwifid.log
```

If you open an issue, include the output of `sh "/Library/Application Support/macwifi/report.sh"` (it masks MAC
addresses, IPs and network names).

## The menu says "The macwifid service isn't running"

```bash
sudo launchctl bootstrap system /Library/LaunchDaemons/io.github.fpm8588.macwifid.plist
sudo launchctl print system/io.github.fpm8588.macwifid | grep -E "state|last exit"
```

"Bootstrap failed: 5" just means it's already loaded. If it keeps exiting, the end of the log says why.

## "Plug in the UGREEN adapter" although it's plugged in

- Check that macOS sees it: `system_profiler SPUSBDataType | grep -B3 -A6 a69c`. Nothing listed: try another
  port or cable; avoid unpowered hubs.
- It shows `a69c:5723` but MacWifi doesn't switch it: macOS may not have mounted its virtual driver CD yet; give it
  ten seconds. A Finder window showing the CD is harmless (MacWifi ejects it).
- Your adapter has a different ID: see [Supported adapters](../README.md#supported-adapters).

## "The adapter firmware isn't installed"

The installer couldn't download it (no internet at install time). Run:

```bash
sudo MW_FW_ROOT="/Library/Application Support/macwifi" "/Library/Application Support/macwifi/fetch-firmware.sh"
```

## "The adapter didn't start; unplug it and plug it back in"

The chip is in a state the service can't recover from by software (for example after being interrupted halfway
through a firmware load). Unplugging resets it completely; the service picks it up again when you plug it back in.

## A network I can see in the Mac's own Wi-Fi menu is missing or greyed out

- Greyed out: its security isn't supported yet; hover over it for the reason. Enterprise (802.1X) networks and WPA2
  networks that require PMF fall in this group.
- Missing: hidden networks aren't listed, and 6 GHz and 5 GHz channels above 165 aren't scanned.
- While connected, the list keeps networks seen in the last three minutes; click **Scan Again**.

## "The password seems wrong"

The access point stopped after the second handshake message, which is what happens with a wrong password. Retry
with the right one (right-click the network → Join with a different password). If you're sure it's right, check
that the network offers WPA2 (a WPA3-only network is shown greyed out).

## "This network uses WPA3: enter its password again"

The network was remembered before MacWifi supported WPA3 (only its WPA2 key was kept) and now accepts WPA3 only.
Right-click it → **Join with a different password…** and type the password once more.

## WPA3 doesn't work but WPA2 did

On a WPA2/WPA3 network MacWifi falls back to WPA2 by itself and the log says why WPA3 failed ("WPA3 sign-in didn't
work (…); trying WPA2"). Please open an issue with the diagnostics report; that line is what's needed.

## Connected, but no internet

- `macwifictl status` shows `"ip":""` for long: DHCP isn't answering. Check `tail /Library/Logs/macwifid.log`
  for "macOS configured feth4100".
- Another connection is preferred: MacWifi's service is last in the service order. Unplug Ethernet or turn the
  built-in Wi-Fi off, or change System Settings → Network → ⋯ → Set Service Order.
- A VPN or firewall that only allows known interfaces may block `feth4100`.

## A hotel/café network shows "sign-in needed" but the page doesn't load

The sign-in window uses macOS's normal routing, so while wired Ethernet or the Mac's own Wi-Fi is connected the
page may be fetched over those instead. Unplug Ethernet or turn the built-in Wi-Fi off for a moment, then click
**Sign In…** again. The menu clears the notice by itself once the network lets you through (it re-checks every
15 s).

## It disconnects now and then

Look for "the network dropped us: reason N" in the log. Reason 34 means the access point thought we weren't
acknowledging its frames; versions before 0.6 read USB one transfer at a time and could cause this under heavy
download. Please report other reasons with the diagnostics report.

## It doesn't reconnect after sleep

The log should show `the Mac is going to sleep` … `asleep` … `the Mac is awake` … `auto-joining a remembered
network`. If it stops at an error, unplug and replug the adapter, and report it with the diagnostics report.

## Bluetooth: a device doesn't show up

Only devices in **pairing mode** (or advertising over Bluetooth LE) can be found. Put it in pairing mode (a Magic
Keyboard or Mouse: switch it off and on with no computer to reconnect to; AirPods: open the case and hold its
button until the light flashes white), then click **Scan Again** and wait about 15 seconds. A device connected to
another computer stays quiet: disconnect it there first. `grep "Bluetooth scan" /Library/Logs/macwifid.log` shows
how many classic and LE replies each scan got.

## Bluetooth: the keyboard or mouse is connected but does nothing

macOS only accepts the input once MacWifi is allowed in System Settings → Privacy & Security → **Accessibility**.
After installing an update, switch MacWifi off and on there (the app is ad-hoc signed, so macOS treats each build as
new). The Bluetooth tab shows an orange banner while this is missing.

## Bluetooth: the mouse feels slow or jumpy

The service keeps mice out of Bluetooth "sniff" power-saving mode and logs the report rate:
`grep "Bluetooth mouse" /Library/Logs/macwifid.log`. A long Wi-Fi scan briefly takes the shared radio away from
Bluetooth; heavy Wi-Fi traffic can also cost some smoothness.

## Bluetooth: headphones are connected but there's no sound

Choose **UGREEN Bluetooth** as the output (Sound settings or the volume menu). Right-click the headphones in the
Bluetooth tab → **Play Test Sound** plays a chime through the service alone. If that is silent too, send
`grep "Bluetooth audio" /Library/Logs/macwifid.log | tail -30`: it lists every step of setting up the stream.
If "UGREEN Bluetooth" is missing from the output list, run `sudo killall coreaudiod` once.

## The developer commands say the service owns the adapter

`macwifictl scan/connect/join/fwload/up` drive the adapter directly and refuse while the service is running.
Uninstall the service first, or use `macwifictl status` / `macwifictl networks`.

## Remove everything

```bash
sudo "/Library/Application Support/macwifi/uninstall.sh"
```

If MacWifi was killed in a way that left its network service or interfaces behind, `sudo macwifictl cleanup` (or
the next start of the service) removes them.
