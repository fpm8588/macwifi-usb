# Hardware log

Results from the real adapter, newest last. Every hardware constant in `Sources/Shared/aic8800.h` that was
confirmed here says so.

**Adapter:** UGREEN AX900 (CM762), AICsemi AIC8800D80. **Mac:** Apple Silicon, macOS 27 Golden Gate.

## 2026-11-07: Phase 1 passed

Commit `c214f14`, built on the Mac with `./build.sh test && ./build.sh cli` (all unit tests passed on Darwin).

| Step | Result |
|---|---|
| `list` | `a69c:5723  Aic MSC`, driver-CD mode |
| `info` (driver CD) | bcdDevice 0x0200, high speed (480 Mb/s), 1 interface: class 0x08 / 0x06 / 0x50 (mass storage, SCSI, bulk-only), bulk OUT 0x02 and IN 0x82, 512 B; bMaxPower 100 mA |
| `modeswitch` | `diskutil eject /dev/disk4` worked; the adapter came back as Wi-Fi mode right away |
| `list` | `a69c:8d80  AIC Wlan` |
| `info` (Wi-Fi) | bcdDevice 0x0100, 480 Mb/s, 1 interface: class 0xff / 0xff / 0xff (no macOS driver claims it), **bulk OUT 0x01, bulk IN 0x82**, 512 B each; bMaxPower 500 mA |
| `identify` | ROM answered both DBG_MEM_READs (see below) |

`identify` with `MACWIFI_DEBUG=1`. Request = 20 bytes, reply = 24-byte config packet (type 0x11, id 0x0401
DBG_MEM_READ_CFM, dest 100, src 1, param_len 8, pattern `2a de de ad`, then {addr, data}):

```
OUT 10 00 11 00 00 00 00 00 00 04 01 00 64 00 04 00 00 00 50 40
IN  14 00 11 00 01 04 64 00 01 00 08 00 2a de de ad 00 00 50 40 20 88 07 f9
OUT 10 00 11 00 00 00 00 00 00 04 01 00 64 00 04 00 04 00 00 00
IN  14 00 11 00 01 04 64 00 01 00 08 00 2a de de ad 04 00 00 00 21 08 00 00
```

- `[0x40500000] = 0xf9078820` → chip revision **0x07 = U03** (bits 23..16). We had guessed U02 (0x03); the Linux
  loader treats U02 and U03 identically (same `_u02` firmware set, same addresses).
- Bits 7..6 of the revision are clear → **not** the "H" variant → `fmacfw_8800d80_u02.bin`.
- Bit 25 is clear → Linux's `chip_mcu_id = 1` → `system_config_8800d80` sets bit 0 of `0x40100020`.
- `[0x00000004] = 0x00000821` → sub-revision 0x82 (bits 11..4).
- The framing in `aic_msg.c` (header, 4-byte pattern before params) is confirmed by real traffic.

Notes:
- zsh doesn't treat `#` as a comment when pasting interactively, so a pasted line ending in `# comment` passes
  the comment to the command (`xcode-select: error: invalid argument '#'`). README command blocks are now
  comment-free.
- The driver CD comes back after every plug-in, so `modeswitch` is needed each time.

## 2026-11-07: Phase 2a, attempt 1 (SDK V5 firmware): stopped at 0x170000

Commit `d4aad06`. `fetch-firmware.sh` OK. Output:

```
Chip: [0x40500000] = 0xf9078820 -> revision 0x07 (U03), MCU id
System config: [0x40100020] 0x00000002 -> 0x00000003
Patch table: 7 records, ADID -> 0x00201940, patch -> 0x001e0000, 1 ext patch(es), version "- Aug 01 2025 11:05:26 - git a26f071"
  fw_adid_8800d80_u02.bin             1708 bytes -> 0x00201940  ok (2 chunks, 0.0 s)
  fw_patch_8800d80_u02.bin           32700 bytes -> 0x001e0000 . ok (32 chunks, 0.0 s)
  fw_patch_8800d80_u02_ext0.bin      16136 bytes -> 0x0020b43c  ok (16 chunks, 0.0 s)
  patch table: 140 register writes ok
  fmacfw_8800d80_u02.bin            358072 bytes -> 0x00120000 ..........no reply (message 0x040c) within 2000 ms
Upload of fmacfw_8800d80_u02.bin failed at offset 327680 (address 0x00170000).
```

Diagnosis: the chip is an **MCU1** part (bit 25 of `[0x40500000]` clear) and **product 0x8820** (low 16 bits).
Linux users hit the same wall: shenmintao/aic8800d80 issue #58, "[A69C:8D80][chip_mcu_id=1] Firmware upload
times out at 0x170400" (Linux notices the missing confirmation one chunk late, so its address is one chunk past
ours). The SDK V5 FMAC (358,072 B) runs past 0x170000; the older SDK V3 FMAC (327,037 B) ends at 0x16FD7D. The
maintainers' validated fix is the `legacy-mcu1` branch: the complete V3 firmware set with its patch table and
fixed patch-buffer layout, plus the MCU1 cache write. Issue #118 (UGREEN `a69c:5723`, chip_id 7, MCU1, product
0x8820) and PR #119 add two system-config writes for product 0x8820 that MM_START needs later. Both are now the
`legacy-mcu1` loader profile, chosen automatically for MCU1 chips.

## 2026-11-07: Phase 2a passed (legacy-mcu1 profile)

Commit `f88e71f`. (A first try right after the failed attempt returned "no reply (message 0x0401)": the chip was
still wedged; it was not unplugged in between. Unplug/replug always resets it.) After replug + `modeswitch`:

```
Chip: [0x40500000] = 0xf9078820 -> revision 0x07 (U03), product 0x8820, chip_mcu_id 1
Loader profile: legacy-mcu1 (SDK V3 firmware), firmware from firmware/aic8800D80-legacy-mcu1
System config: [0x40100020] 0x00000002 -> 0x00000003 (MCU1 cache fix)
System config: 2 product-0x8820 writes
Patch table: 7 records, ADID -> 0x00201940, patch -> 0x001e0000, 0 ext patch(es), version "- Dec 05 2023 15:53:41 - git 487f432"
  fw_adid_8800d80_u02.bin             1708 bytes -> 0x00201940  ok (2 chunks, 0.0 s)
  fw_patch_8800d80_u02.bin           25300 bytes -> 0x001e0000  ok (25 chunks, 0.1 s)
  patch table: 92 register writes ok
  fmacfw_8800d80_u02.bin            327037 bytes -> 0x00120000 .......... ok (320 chunks, 1.7 s)
Firmware read-back matches the file (this image has no version word).
Patch config: 12 writes (config base 0x00183214, patch struct 0x00170000, pairs at 0x001d7000)
START_APP sent (boot 0x00120000).
The adapter came back as a69c:8d81 (AIC8800D80/D81 running firmware) 4.8 s after the upload started.
```

`a69c:8d81`, bcdDevice 0x0100, 480 Mb/s, wTotalLength 215, 3 interfaces, 500 mA:

| Interface | Class | Endpoints | Use |
|---|---|---|---|
| 0 (IAD first=0 count=2, class 0xe0) | 0xe0/0x01/0x01 Bluetooth | 0x83 IN interrupt 64 B; 0x84 IN bulk; 0x04 OUT bulk | HCI events / ACL |
| 1, alt 0-5 | 0xe0/0x01/0x01 Bluetooth | 0x85 IN / 0x05 OUT isochronous, 0/9/17/25/33/49 B | SCO audio |
| 2 | 0xff/0xff/0xff vendor | **0x01 OUT bulk (data), 0x81 IN bulk, 0x02 OUT bulk (messages)** | Wi-Fi (Linux `aic8800_fdrv`) |

Matches Linux #58 (interfaces 0-1 to `btusb`, interface 2 to `aic8800_fdrv`) and `kAicIds` (8d81 sends
messages on the second bulk OUT). Bluetooth is out of scope: macOS does not use third-party USB Bluetooth
controllers.

## 2026-11-07: Phase 2b passed

Commit `806f69e`. The adapter was still running the firmware from the 2a run (not replugged), so `up` skipped
straight to the bring-up:

```
Firmware already running (a69c:8d81).
Wi-Fi interface: pipes in 2, out 1, msg 3
Firmware answers: [0x40500000] = 0xf9078820 (profile legacy-mcu1 (SDK V3 firmware))
User config: 96 settings from firmware/aic8800D80-legacy-mcu1/aic_userconfig_8800d80.txt
Stack started: 5 GHz supported, vendor info 0x00
Firmware version: zh Dec 29 2023 16:46:29 - gf876b0f
TX power levels: sent
RF calibration done (rx gain tables 0x00181b2c / 0x00182540, tx 0x00182904 / 0x00182a8c)
MAC address: 6c:1f:f7:8b:81:46
MAC reset; LMAC 0x06090100, MAC HW 0x0002fdfb/0x00014047, PHY 0x5ee24111/0x01020000, features 0x01e877d5, 32 stations, 4 interfaces
Phase 2b hardware check passed.
```

- USBDeviceOpen on the composite `8d81` worked (no exclusive-access fallback needed); pipes 1 = OUT 0x01,
  2 = IN 0x81, 3 = OUT 0x02 (messages).
- The MAC's OUI 6C:1F:F7 is registered to Ugreen Group Limited, i.e. the eFuse MAC is real.
- 5 GHz supported; LMAC 6.9.1.0; firmware supports 32 stations and 4 virtual interfaces.
- No `[firmware]` log packets were sent during the bring-up.

## Phase 3a: scan (waiting for a run)

Expected: after the 2b output, "Capabilities and channels set (14 at 2.4 GHz, 25 at 5 GHz...)", "Radio started;
station interface N", then a table of nearby networks sorted by signal. The firmware's `SCANU_START_CFM` gives a
result count; every `SCANU_RESULT_IND` carries one beacon or probe response.
