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

## Phase 2a: firmware upload (waiting for a run)

Expected from the Linux driver and the pinned firmware (also checked by `Tests/test_fwload.c` in CI):
patch table version "- Aug 01 2025 11:05:26 - git a26f071"; ADID (1,708 B) → 0x201940; patch (32,700 B) →
0x1e0000; ext0 (16,136 B) → 0x20b43c; 140 patch-table writes; fmacfw (358,072 B, 350 chunks) → 0x120000;
firmware version 0x06090101; 14 patch-config writes; START_APP → re-enumerates as `a69c:8d81`.
