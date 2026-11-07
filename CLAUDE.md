# macwifi-usb: notes for Claude Code

Read `docs/PROMPT.md` (the plan, the constraints and the phase checklists) and `docs/RESEARCH.md` before
changing anything. Short version:

- $0 only: no paid Apple account, no DriverKit, no kexts, never ask to disable SIP, no Homebrew in the product.
- Apple Silicon, macOS 13+ target, tested on macOS 27 Golden Gate. Target chip: MT7612U (mt76x2u).
- Chip facts live in `Sources/Shared/mt76x2u.h` and cite the Linux mt76 file they came from.
- Licensing: repo is MIT. Porting Linux mt76 (BSD-3-Clause-Clear) is fine with notices kept and NOTICE.md
  updated. mt7601u-macos (MIT/Apache) may be adapted with attribution. Never copy GPL-2 code (userspace-wifi,
  Kismet). Never commit firmware.
- Pure logic goes in `Sources/Shared` with tests; `./build.sh test` before every commit; CI must stay green.
- The Mac's built-in Wi-Fi is never touched. Stop at each hardware checkpoint, ask the user to run it, record
  the result in `docs/HARDWARE_LOG.md`, and remind them of the recovery commands in README.md.
