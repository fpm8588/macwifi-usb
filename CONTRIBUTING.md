# Contributing

Thanks for helping. The most useful contributions right now are **reports from other adapters and Macs** (with
the diagnostics report from the README) and fixes for the limitations listed in the changelog.

## Build and test

You need the Xcode Command Line Tools (`xcode-select --install`).

```bash
scripts/fetch-firmware.sh      # AICsemi's firmware into firmware/ (never committed)
./build.sh test                # unit tests + the full simulation; runs on Linux too (needs libssl-dev)
./build.sh pkg                 # macwifictl, MacWifi.app and the installer package (or: ./build.sh cli / app)
```

`./build.sh test` must pass before every commit, and CI must stay green. The simulation (`Tests/sim_connect.c`)
drives the real `main.c` and `daemon.c` against a simulated adapter, access point and macOS side; new behaviour
should come with a scenario there or a unit test in `Tests/`. On Linux you can also run it under
ThreadSanitizer and AddressSanitizer (see the commands in `build.sh`).

## Ground rules

- **$0 for users:** no paid Apple account, no kernel extensions, no DriverKit, never ask anyone to disable SIP, no
  Homebrew dependencies in the product.
- **Hardware facts** go in `Sources/Shared/aic8800.h` (or next to the code that uses them) with the Linux file
  and function they come from. When the hardware teaches you something new, record it in
  `docs/HARDWARE_LOG.md`.
- **Licensing:** the repository is GPL-2.0-only. Files that port Linux driver code keep the original copyright
  lines and are listed in `NOTICE.md`. Files written from scratch are also MIT; say so in their header. Don't
  copy code from other projects (GhostBox, ZeroTier, …); facts only.
- **Never commit firmware**, and never put anyone's network names, BSSIDs, IP or MAC addresses in commits,
  logs or issues.
- Pure logic goes in `Sources/Shared` with tests; macOS-specific code in `Sources/macwifictl` or `App/`.

## Reporting hardware results

Run the diagnostics report, describe the adapter (brand, model, USB IDs) and what you tried. If something
works on an adapter that isn't in the supported list, that's worth an issue too.
