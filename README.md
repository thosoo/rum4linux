# rum4linux

Early Linux `mac80211` + `usb_driver` scaffold inspired by OpenBSD `rum(4)`.

This repository is being generalized structurally toward the broader `rum(4)` device family. It is **not** a finished driver, and it should not be treated as functional hardware support for the full family yet.

## Current scope

Implemented scaffold pieces:

- safe probe path with `bind=0` default
- USB vendor-control register I/O layer with bounded retries
- EEPROM read/parse subsystem
- firmware upload scaffold with MCU handoff/wait flow
- BBP init scaffold with OpenBSD-derived defaults plus EEPROM BBP overrides
- RF/channel scaffold for RT2528 2.4GHz channel programming with bounded calibration wiring
- bounded post-channel sanity check and one bounded recovery attempt
- bounded TX descriptor path with runtime bulk-OUT submission and URB-completion status handoff to mac80211
- bounded RX bulk-IN URB pipeline with strict descriptor/frame sanity checks and conservative mac80211 delivery
- minimal station-mode interface/BSSID runtime programming hooks, including BSSID clear on disassociation/teardown

Still intentionally incomplete:

- full, validated TX descriptor/status semantics across `rum(4)`-family variants
- full RX descriptor confidence across all rum(4)-family variants
- association / operational station behavior
- broad USB ID and per-device calibration/firmware coverage

All uncertain behavior remains tagged as `TODO(openbsd-rum-port)`.

## Family generalization status

- Module output remains `rum4linux.ko`.
- DKMS package name remains `rum4linux`.
- The codebase is now named and organized as a family scaffold, but effective device enablement is still conservative and incremental.
- If the USB ID table is narrow in code, that is intentional until EEPROM/firmware/RF/TX/RX behavior is validated for additional devices.

## Safety defaults

- Default safety gate is `bind=0` (no attach by default).
- Functional hardware verification is intentionally deferred at this stage.

## Layout

- `dkms.conf` — DKMS metadata (`rum4linux`)
- `Makefile` — Kbuild wrapper for `rum4linux.ko`
- `src/rum4linux_core.c` — USB + mac80211 scaffold entrypoints
- `src/rum4linux_hw.c` / `src/rum4linux_hw.h` — hardware register/control core
- `src/rum4linux_eeprom.c` / `src/rum4linux_eeprom.h` — EEPROM subsystem
- `src/rum4linux_fw.c` / `src/rum4linux_fw.h` — firmware subsystem
- `src/rum4linux_bbp.c` / `src/rum4linux_bbp.h` — BBP subsystem
- `src/rum4linux_rf.c` / `src/rum4linux_rf.h` — RF/channel subsystem
- `src/rum4linux_tx.c` / `src/rum4linux_tx.h` — bounded TX subsystem
- `src/rum4linux_rx.c` / `src/rum4linux_rx.h` — bounded conservative RX subsystem
- `src/rum4linux_debug.h` — logging helpers
- `docs/openbsd-rum-port-notes.md` — reference and limitation notes
