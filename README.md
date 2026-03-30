# rum4linux

Development scaffold for a Linux replacement driver for the D-Link DWA-111 (USB ID `07d1:3c06`) in the style of OpenBSD `rum(4)`.

This is **not** a finished driver. It is a clean Linux `mac80211` + `usb_driver` replacement scaffold, not an `rt2x00` patch.

## Current phase status

Implemented now (Phase 1-6 scaffold):

- safe probe path with `bind=0` default and endpoint layout logging
- isolated USB vendor-control register I/O layer
- dedicated EEPROM subsystem with parsed MAC/RF/antenna/config data
- dedicated firmware subsystem with chunk upload + MCU handoff hooks
- BBP initialization scaffold:
  - BBP read/write helpers via PHY_CSR3 path
  - minimal OpenBSD-derived BBP defaults (`RT2573_DEF_BBP`)
  - EEPROM-derived BBP override application with structured per-entry logs
- RF/channel scaffold for RT2528 2.4GHz-first path:
  - strict RF identity gate (`rf_rev == RT2528`)
  - channels 1-14 routing only
  - table-driven channel plan structure
  - confirmed RF table for channels 1-14 (RT2528, Linux rt73usb lineage)

Still intentionally incomplete:

- TX descriptor formatting and datapath
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
- `src/rum4linux_rf.c` — RF/channel scaffold (RT2528-first)
- `docs/openbsd-rum-port-notes.md` — OpenBSD mapping notes

## Suggested workflow

1. Keep in-tree `rt73usb` available until replacement init is deterministic.
2. Develop with `bind=0` first and compare kernel logs carefully.
3. Validate EEPROM/firmware/BBP sequencing against OpenBSD behavior.
4. Validate per-channel RF writes and BBP sanity logs on hardware.
5. Move next to bounded TX descriptor bring-up once channel stability is confirmed.
