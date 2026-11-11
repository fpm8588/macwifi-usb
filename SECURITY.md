# Security

## How MacWifi handles secrets

- **Wi-Fi passwords** travel from the menu bar app (or the terminal prompt, typed without echo) to the service
  over its local socket. They never appear in logs, even with `MACWIFI_DEBUG=1`, and the build fails if a test
  password ever shows up in the test output.
- **Remembered networks** live in `/Library/Application Support/macwifi/networks`, owned by root, mode 0600.
  For a WPA2-only network the entry is the network name and the derived key (PMK); the password is wiped after
  the key is derived. For a network that offers WPA3 the password itself is stored as well (hex-encoded, not
  encrypted): SAE runs a fresh password-authenticated exchange at every sign-in, so there is no long-lived key to
  keep instead. Treat the file like saved passwords; "Forget This Network" and the uninstaller delete it.
- **WPA3 (SAE)** is our own implementation (`Sources/Shared/sae.c`, group 19; hunting-and-pecking with a fixed 40
  rounds, or hash-to-element through the simplified SWU map when the access point advertises it) on our own P-256 arithmetic (`Sources/Shared/p256.c`): macOS offers no public API for the raw curve
  operations SAE needs, and the project takes no third-party dependencies. It is checked against the IEEE
  802.11-2020 Annex J.10 test vector and, in tests, against OpenSSL. Scalar multiplication runs a fixed number of
  steps, but the code has not been audited for side channels the way wpa_supplicant's has; an attacker who can
  measure this Mac's computation timing precisely is outside what it defends against.
- With WPA3 the chip also gets the integrity key for broadcast management frames (BIP-CMAC-128), so forged
  deauthentication frames are rejected.
- On a WPA2/WPA3 transition network WPA3 is tried first; if it fails for a reason other than the password, WPA2
  is used, which such a network accepts from any client anyway. `MACWIFI_NO_WPA3=1` (developer commands) forces
  WPA2.
- **Session keys** (PTK/GTK) live only in memory and in the adapter; they're hidden from debug dumps.
- **The 4-way handshake** is our own implementation (`Sources/Shared/eapol.c`); it checks MICs, replay counters,
  that the ANonce doesn't change, that the access point's RSN element in message 3 matches its beacon (downgrade
  protection), and only accepts encrypted key data. Once keys are installed, unencrypted data frames from the air
  are dropped.

## Privileges

- `macwifid` runs as root: creating a network interface, reading BPF and changing network preferences require
  it. It takes no power assertions and opens no network ports; its only listener is the Unix socket
  `/var/run/macwifid.sock` (`root:admin`, 0660), so only administrators can control it.
- The menu bar app runs as you and has no special privileges.
- Packages are ad-hoc signed (there's no paid Apple developer account behind this project), so macOS can't verify
  the publisher. The release workflow builds them on GitHub's runners from the tagged source and publishes their
  SHA-256 next to each package; you can also build them yourself.

## Firmware

The adapter firmware is AICsemi's. It's downloaded from pinned commits of the Linux driver repository and every
file's SHA-256 is checked before use; it's loaded into the adapter's RAM only.

## Reporting a vulnerability

Please don't open a public issue for a security problem. Use GitHub's private vulnerability reporting
(Security → Report a vulnerability) on this repository.
