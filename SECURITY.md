# Security

## How MacWifi handles secrets

- **Wi-Fi passwords** travel from the menu bar app (or the terminal prompt, typed without echo) to the service
  over its local socket and are used once to derive the WPA2 pairwise master key (PBKDF2, as the standard
  requires). The password is wiped from memory after that, is never written to disk, and never appears in logs,
  even with `MACWIFI_DEBUG=1`. The build fails if a test password ever shows up in the test output.
- **Remembered networks** are stored as network name + PMK in `/Library/Application Support/macwifi/networks`,
  owned by root, mode 0600. A PMK isn't the password, but it lets a device join that network, so treat the file
  like a saved password. "Forget This Network" and the uninstaller delete it.
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
