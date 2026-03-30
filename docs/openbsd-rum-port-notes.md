# OpenBSD rum(4) port notes for rum4linux

## Primary reference files/concepts

- OpenBSD `sys/dev/usb/if_rum.c`
  - `rum_read_eeprom`
  - `rum_load_microcode`
  - `rum_bbp_read` / `rum_bbp_write` / `rum_bbp_init`
  - `rum_rf_write` and channel-programming structure in `rum_set_chan`
- OpenBSD `sys/dev/usb/if_rumreg.h`
  - vendor request IDs
  - register offsets and BBP/RF bit definitions
  - EEPROM offsets and RF identity values
  - `RT2573_DEF_BBP`

## Ported confidently in this phase

- module build/output renamed to `rum4linux.ko`
- vendor request IDs and core register offsets used by init path
- EEPROM extraction and parsed identity fields
- firmware chunk upload + MCU handoff/wait scaffolding
- BBP helpers and init flow:
  - BBP busy/read/write transaction pattern via `PHY_CSR3`
  - OpenBSD `RT2573_DEF_BBP` default table application
  - EEPROM BBP PROM override handling and structured logging
- RF/channel scaffolding:
  - strict `rf_rev == RT2528` gate
  - strict 2.4GHz channels 1-14 gate
  - table-driven per-channel plan structure

## Still uncertain / intentionally not claimed complete

- exact RT2528 channel RF constants for `RF1..RF4` programming
- whether additional per-channel calibration writes are required before RF lock
- complete post-programming verification strategy beyond current sanity checks
- TX/RX descriptor datapath

All uncertain parts are tagged in code as `TODO(openbsd-rum-port)` and return truthful errors when unconfirmed steps are reached.

## Next phase recommendation

1. Port/verify RT2528-specific RF constants from trusted references (OpenBSD history + Linux RT73 family references).
2. Enable confirmed RF writes for channels 1-14 and keep read-back sanity logs.
3. Add bounded smoke tests for BBP/RF init completion before touching datapath.
4. Begin TX descriptor scaffolding only after RF/channel programming is confirmed stable.
