# macwifid control protocol

The menu bar app (and `macwifictl status|networks|disconnect|ctl`) talk to `macwifid` over a Unix socket:

- Path: `/var/run/macwifid.sock`, owner `root:admin`, mode `0660` (administrators only).
- One request per connection: send one JSON object followed by `\n`, read one JSON object followed by `\n`.
- Requests are flat objects (string, number, boolean, null values). Every reply has `"ok"` (boolean) and, when
  it's false, `"error"`: a sentence for the user.
- Strings are UTF-8; network names that aren't valid UTF-8 are shown with U+FFFD replacement characters.

Try it: `macwifictl ctl '{"cmd":"status"}'`.

## Requests

### status

```json
{"cmd":"status"}
```
```json
{"ok":true,"state":"connected","adapter":true,"power":true,"ssid":"Home","rssi":-44,"freq":5745,
 "ip":"192.168.1.20","security":"wpa3","captive":"no","error":"","version":"0.6.0"}
```

`state` is one of `off`, `no_adapter`, `starting`, `idle`, `scanning`, `connecting`, `connected`, `asleep`,
`error`. `ssid`, `rssi` (dBm), `freq` (MHz) and `ip` (macOS's IPv4 address on `feth4100`, empty until DHCP
finishes) are present while connecting or connected, with `captive`: `"unknown"` until the check has run,
`"no"`, or `"yes"` plus `portal` (where to sign in: the network's redirect target, else the check page). The
check fetches `http://captive.apple.com/hotspot-detect.html` over the adapter only, right after macOS gets an
address, then every 15 s while a portal is in the way and every 5 minutes otherwise. `error` is the last problem, or empty.

### scan / networks

```json
{"cmd":"scan"}       {"cmd":"networks"}
```
```json
{"ok":true,"age":0,"networks":[
  {"ssid":"Home","rssi":-44,"freq":5745,"secured":true,"security":"wpa2/wpa3","usable":true,"known":true},
  {"ssid":"Office","rssi":-71,"freq":5180,"secured":true,"usable":false,"known":false,"why":"needs WPA3, enterprise or WEP (not supported yet)"}]}
```

`scan` scans first (a few seconds); `networks` returns the current list. One entry per network name (its
strongest access point), strongest first, hidden networks omitted. Networks stay listed for three minutes after a
scan last saw them. `age` is seconds since the last scan (-1: none yet).

### join

```json
{"cmd":"join","ssid":"Home","password":"correct horse battery"}
{"cmd":"join","ssid":"Home"}
```

Without `password`: an open network, or a remembered one. The reply comes when the connection is up (or failed),
up to about a minute. On success the network is remembered: as its WPA2 key (PMK), plus the password itself when
the network offers WPA3 (which needs it at every sign-in). `security` in `status` says what the connection uses
(`wpa3`, `wpa2`, `open`); in `networks` what the network offers (`wpa3`, `wpa2/wpa3`, `wpa2`, `open`).

### disconnect

`{"cmd":"disconnect"}`: leaves the network and stops auto-joining until the next join, replug or power cycle.

### forget

`{"cmd":"forget","ssid":"Home"}`: deletes the remembered key (and disconnects if it's the current network).

### power

`{"cmd":"power","on":false}`: turns the adapter's Wi-Fi off (and releases the adapter); the setting is kept
across restarts.

### known

`{"cmd":"known"}` → `{"ok":true,"known":[{"ssid":"Home","secured":true}]}`
