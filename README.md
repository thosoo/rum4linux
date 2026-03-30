# rum4linux

Development scaffold for a Linux replacement driver for the D-Link DWA-111 (USB ID `07d1:3c06`) in the style of OpenBSD `rum(4)`.

This is **not** a finished driver. It is a starting point for a clean-room Linux/mac80211 implementation that borrows the *architecture* of OpenBSD `rum(4)`.

## Current phase status

Implemented now (Phase 1-4 scaffold):

- safe probe path with `bind=0` default and endpoint layout logging
- isolated USB vendor-control register I/O layer
- dedicated EEPROM subsystem (`src/dwa111_rum_eeprom.c`) with parsed fields:
  - MAC address validation
  - RF revision / antenna fields
  - NIC config2 LNA bits
  - RSSI correction / freq offset / txpower table / BBP PROM words
- dedicated firmware subsystem (`src/dwa111_rum_fw.c`) with:
  - candidate firmware name selection
  - chunked upload helper into MCU code memory
  - explicit MCU handoff and post-handoff readiness wait
- stricter early hardware init sequencing and concise summary logging

Still intentionally incomplete:

- TX descriptor formatting and datapath
- RX datapath and descriptor parsing
- BBP/RF programming tables and channel calibration details
- confirmed firmware edge-cases across all RT2571W revisions
- association / operational station functionality

All uncertain hardware behavior remains marked `TODO(openbsd-rum-port)`.

## Safety

This scaffold defaults to **not binding** to the device (`bind=0` module parameter). That is deliberate.

## Layout

- `dkms.conf` — DKMS metadata (`rum4linux`, module `dwa111_rum`)
- `Makefile` — Kbuild wrapper
- `src/dwa111_rum_main.c` — USB + mac80211 scaffold
- `src/dwa111_rum_hw.c` — hardware init and register I/O core
- `src/dwa111_rum_eeprom.c` — EEPROM read/parse subsystem
- `src/dwa111_rum_fw.c` — firmware upload subsystem
- `docs/openbsd-rum-port-notes.md` — OpenBSD mapping notes

## Suggested workflow

1. Keep in-tree `rt73usb` available until the replacement can complete deterministic init safely.
2. Develop with `bind=0` first and use dynamic debug / dmesg tracing.
3. Compare `dwa111_rum` init logs against historical rt73usb failures and OpenBSD rum traces.
4. Once EEPROM + firmware + BBP/RF init are stable, move to TX/RX descriptor work.
