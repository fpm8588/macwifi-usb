# macwifi-usb: notes for Claude Code

Read `docs/PROMPT.md` (plan, constraints, phase checklists) and `docs/RESEARCH.md` before changing anything.
Bluetooth and band work follows `docs/BLUETOOTH_PROMPT.md`; after that, speed and feature parity follow
`docs/PERFORMANCE_PROMPT.md`.
Short version:

- Target: UGREEN AX900 = AICsemi AIC8800D80 (`a69c:5723` driver-CD mode → Wi-Fi mode after eject). The user
  owns it and tests every hardware checkpoint.
- $0 only: no paid Apple account, no DriverKit, no kexts, never ask to disable SIP, no Homebrew in the product.
- Apple Silicon, macOS 13+ target, tested on macOS 27 Golden Gate.
- Hardware facts live in `Sources/Shared/aic8800.h` and cite the Linux file they came from.
- Repo is GPL-2.0-only (porting the AIC8800 Linux driver, github.com/shenmintao/aic8800d80). Keep original
  copyright lines in ported files and list them in NOTICE.md. Never commit firmware.
- Pure logic goes in `Sources/Shared` with tests; `./build.sh test` before every commit; CI must stay green.
- The Mac's built-in Wi-Fi is never touched. Stop at each hardware checkpoint, ask the user to run it, record
  the result in `docs/HARDWARE_LOG.md`, and remind them of the recovery commands in README.md.
