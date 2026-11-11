# Reference prompt: speed and feature parity for macwifi-usb

Start this after the Bluetooth phases in `docs/BLUETOOTH_PROMPT.md` (the user's order). Read it together with
`docs/PROMPT.md` and `CLAUDE.md`; the same hard constraints apply ($0, no kexts, no DriverKit, tests before
every commit, hardware checkpoints, no personal data in the repo).

## The complaint

At home, on a 1 Gb/s internet line, the adapter tops out around **150 Mb/s** (user report, 2026-11-11).

## What the ceiling really is (set expectations first)

- The UGREEN AX900 is a **1x1** Wi-Fi 6 adapter: one spatial stream. Its best link rate is about 600 Mb/s
  (5 GHz, 80 MHz, HE-MCS 11) and about 286 Mb/s on 2.4 GHz (40 MHz). Real TCP throughput is typically 55-70%
  of the link rate.
- It is a **USB 2.0** device (480 Mb/s signalling, hardware log). After USB protocol overhead, USB 2.0 bulk
  transfers carry at most about 280-320 Mb/s in practice.
- So 1 Gb/s is not reachable with this adapter on any OS. A realistic target is **250-300 Mb/s down** on 5 GHz
  at 80 MHz near the router, and as close to that as possible up. Tell the user this plainly.

## Measure before changing anything (phase S0)

Add `macwifictl stats` (and a JSON `stats` verb on the control socket) that reports, while connected:
- the negotiated PHY: band, channel, bandwidth (20/40/80), mode (HT/VHT/HE), MCS, NSS, guard interval, and the
  firmware's current TX/RX rate (ask the firmware; see Linux `rwnx_send_get_sta_info`/`MM_GET_STA_INFO` and
  the rate fields in `rwnx_rx_vector`), RSSI;
- the host side: USB bulk IN transfers/s and bytes per transfer, RX queue depth, reorder-buffer timeouts and
  drops, bulk OUT transfers/s and bytes per transfer, feth/BPF read sizes and AF_NDRV write rate, CPU time of
  macwifid.
Then measure with `networkQuality -s` and an iperf3-style test on the LAN, both directions, at 2.4 and 5 GHz.
Record everything (redacted) in `docs/HARDWARE_LOG.md`. Each step below must show a measured gain.

## Progress

- 2026-11-11, first finding: every outgoing data frame was one synchronous USB transfer (write, wait for
  completion, next). Downloads need a steady stream of TCP ACKs going out, so the per-transfer round trip
  capped the packet rate. Linux keeps up to 200 TX URBs queued and limits each station to 64 pending
  (`aicwf_usb.h`: AICWF_USB_TX_URBS, AICWF_USB_FC_PERSTA_HIGH_WATER; CONFIG_USB_TX_AGGR is off by default, so
  no multi-frame aggregation). Now: `rx_async_tx`, up to 64 bulk-OUT transfers in flight on the async run
  loop, EAPOL and SAE frames still synchronous after a drain. The session's end line logs TX transfers, the
  most in flight and waits for a free slot. **Needs a before/after measurement on the adapter.**
- Checked against Linux and unchanged: ME_CONFIG capabilities (HT 20/40 SGI, VHT 80 MHz SGI80, HE MCS 0-11,
  phy_bw_max 80), ps_on (Linux default true, firmware dynamic PS). Home network already associates at 80 MHz
  on 5 GHz (hardware log, "width code 2").

## Likely bottlenecks and fixes, in order

1. **Link rate.** Check that our SM_CONNECT_REQ and the capabilities we give the firmware
   (`ME_CONFIG_REQ`: HT/VHT/HE capabilities, 40/80 MHz, short GI, LDPC, STBC, A-MSDU size, A-MPDU
   parameters) match what Linux sends for the D80, and that the channel context is 80 MHz on 5 GHz. A link
   stuck at 20 MHz or HT would by itself explain ~150 Mb/s.
2. **USB RX aggregation.** The Linux driver receives several 802.11 frames per bulk-IN transfer (RX
   aggregation, `AICWF_RX_REORDER`/`usb_rx` sizes). Make sure we request the largest transfer size the
   firmware supports, keep enough reads in flight (currently 16), and parse every frame in a transfer.
3. **USB TX aggregation.** Linux batches several frames per bulk-OUT transfer (`aicwf_usb_tx_flow_ctrl`,
   `AICWF_USB_AGGR`); we send one per transfer. Add batching with a small deadline (for example 1 ms or N
   frames), with ZLPs where required, and flow control from the firmware's TX credits.
4. **Host path.** The feth/BPF bridge copies every packet through user space. Read BPF with large buffers
   (BIOCSBLEN 512 KB+, immediate mode off with a short timeout), write with batched AF_NDRV sends, avoid
   per-packet allocations, pin the RX and TX threads at a user-interactive QoS class, and remove per-packet
   logging. Measure the CPU cost per Mb/s.
5. **Band choice.** Prefer 5 GHz 80 MHz when its RSSI is good enough (already a preference; tune with data),
   and add the band setting from `BLUETOOTH_PROMPT.md` W1.
6. **Power save.** Make sure power save is off while traffic flows (firmware PS can add latency).

## Feature parity with the reference repo (brkNx/AIC8800-macOS-WiFi-Driver)

That repository is a DriverKit skeleton whose driver sources are placeholders, so "parity" means the
features its README advertises. Status here:

| Feature it lists | Here |
|---|---|
| Dual-band 2.4 / 5 GHz | Done (W1 adds a band setting) |
| WPA2-Personal, WPA3-Personal | Done (WPA3 incl. hash-to-element) |
| Hardware encryption offload | Done: keys go to the firmware (FullMAC) |
| MU-MIMO, beamforming | Firmware features: report them in `stats`, enable if Linux enables them |
| USB 2.0, FullMAC | Done |
| Apple Silicon | Done |
| **Intel Mac support** | To do: build universal (arm64 + x86_64) binaries and app; CI builds both |
| macOS 13+ | Done |
| Companion app | Done (menu bar). To do: a "Details" window with adapter, firmware, MAC, band/channel/width, link rate, live throughput, signal graph and the log, plus the Bluetooth section |
| Device detection (plug/unplug) | Done in the service; show it in the menu |
| No paid account needed | Better than the reference: it needs the $99/year program for DriverKit; this doesn't |

## Done means

Measured numbers before and after each change in the hardware log; no regressions in the simulator or on
the adapter (WPA2, WPA3, open, captive, sleep/wake); CHANGELOG entry; README "What works" updated with real,
measured throughput.
