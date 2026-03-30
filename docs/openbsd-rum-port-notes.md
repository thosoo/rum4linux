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
- RF/channel implementation:
  - strict `rf_rev == RT2528` gate
  - strict 2.4GHz channels 1-14 gate
  - confirmed RT2528 table values from Linux `rt73usb` `rf_vals_bg_2528[]`
  - OpenBSD-style 3-phase RF3 bit2 toggle sequence

## Still uncertain / intentionally not claimed complete

- whether additional per-channel calibration writes are required before RF lock
- complete post-programming verification strategy beyond current sanity checks
- TX/RX descriptor datapath

All uncertain parts are tagged in code as `TODO(openbsd-rum-port)` and return truthful errors when unconfirmed steps are reached.

## Next phase recommendation

1. Validate txpower and freq-offset integration against OpenBSD/Linux behavior for RT2528.
2. Add broader post-channel sanity checks and recovery hooks for transient RF/BBP failures.
3. Begin TX descriptor scaffolding only after repeated stable BBP/RF init on real hardware.
