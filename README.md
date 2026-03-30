# rum4linux

Development scaffold for a Linux replacement driver for the D-Link DWA-111 (USB ID `07d1:3c06`) in the style of OpenBSD `rum(4)`.

This is **not** a finished driver. It is a clean Linux `mac80211` + `usb_driver` replacement scaffold, not an `rt2x00` patch.

## Current phase status

Implemented now (Phase 1-7 scaffold):

- safe probe path with `bind=0` default and endpoint layout logging
- isolated USB vendor-control register I/O layer
- dedicated EEPROM subsystem with parsed MAC/RF/antenna/config data
- dedicated firmware subsystem with chunk upload + MCU handoff hooks
- BBP initialization scaffold:
  - BBP read/write helpers via PHY_CSR3 path
  - minimal OpenBSD-derived BBP defaults (`RT2573_DEF_BBP`)
  - EEPROM-derived BBP override application with structured per-entry logs
- RF/channel path for RT2528 2.4GHz:
  - strict RF identity gate (`rf_rev == RT2528`)
  - confirmed channel table for channels 1-14
  - EEPROM-derived calibration currently integrated for:
    - `rffreq` -> RF4 offset bits
    - per-channel EEPROM txpower -> RF3 txpower bits (clamped)
- post-channel sanity + bounded recovery:
  - MAC/PHY/BBP sanity reads
  - one bounded recovery attempt (BBP re-init + channel re-apply)
  - explicit state accounting in init summary logs
- bounded TX scaffold:
  - dedicated TX module with descriptor builder + bulk-out URB plumbing
  - TX submission is blocked until descriptor semantics are fully confirmed
  - optional one-shot `tx_smoke_test=1` hook (disabled by default)

Still intentionally incomplete:

- confirmed full RT2571W TX descriptor semantics
- RX datapath and descriptor parsing
- association / operational station functionality

All uncertain hardware behavior remains marked `TODO(openbsd-rum-port)`.

## Module and DKMS naming

- DKMS package name: `rum4linux`
- kernel module name/output: `rum4linux` (`rum4linux.ko`)

## Safety

This scaffold defaults to **not binding** to the device (`bind=0` module parameter).

## Layout

- `dkms.conf` — DKMS metadata (`rum4linux`, module `rum4linux`, built from module root)
- `Makefile` — Kbuild wrapper for `rum4linux.ko`
- `src/dwa111_rum_main.c` — USB + mac80211 scaffold
- `src/dwa111_rum_hw.c` — hardware init and register I/O core
- `src/dwa111_rum_eeprom.c` — EEPROM read/parse subsystem
- `src/dwa111_rum_fw.c` — firmware upload subsystem
- `src/rum4linux_bbp.c` — BBP init scaffold
- `src/rum4linux_rf.c` — RF/channel and calibration path
- `src/rum4linux_tx.c` — bounded TX descriptor/URB scaffold
- `docs/openbsd-rum-port-notes.md` — OpenBSD/Linux reference notes

## Suggested workflow

1. Keep in-tree `rt73usb` available until replacement init is deterministic.
2. Develop with `bind=0` first and compare kernel logs carefully.
3. Validate EEPROM/firmware/BBP/RF sequencing against OpenBSD/Linux behavior.
4. Validate post-channel sanity/recovery behavior on real hardware.
5. Confirm TX descriptor semantics before enabling real frame transmission.
