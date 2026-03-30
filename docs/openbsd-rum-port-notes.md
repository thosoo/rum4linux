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
- Linux `rt73usb` lineage (`drivers/net/wireless/ralink/rt2x00/rt73usb.c/.h`)
  - `rf_vals_bg_2528[]`
  - `TXPOWER_TO_DEV` clamping range (0..31), default 24

## Ported confidently in this phase

- module build/output named `rum4linux.ko`; DKMS module location audited to module root
- vendor request IDs and core register offsets used by init path
- EEPROM extraction and parsed identity fields
- firmware chunk upload + MCU handoff/wait scaffolding
- BBP helpers and init flow:
  - BBP busy/read/write transaction pattern via `PHY_CSR3`
  - OpenBSD `RT2573_DEF_BBP` default table application
  - EEPROM BBP PROM override handling and structured logging
- RF/channel and calibration implementation:
  - strict `rf_rev == RT2528` gate
  - strict 2.4GHz channels 1-14 gate
  - confirmed RT2528 channel table from Linux `rt73usb` `rf_vals_bg_2528[]`
  - OpenBSD-style 3-phase RF3 bit2 toggle sequence
  - calibration wiring:
    - `rffreq` into RF4 (`<< 10` in RF value domain per OpenBSD rum flow)
    - EEPROM txpower byte mapped/clamped into RF3 txpower bits (`<< 7` in RF value domain)
- post-channel checks:
  - MAC/PHY/BBP sanity reads
  - one bounded recovery attempt (BBP re-init + channel re-apply)

## Still uncertain / intentionally not claimed complete

- complete RT2571W TX descriptor semantics required for safe actual TX submission
- whether additional per-channel RF/BBP calibration writes are required before stable traffic
- RX descriptor and receive datapath
- association / operational station mode

All uncertain parts are tagged in code as `TODO(openbsd-rum-port)` and return truthful errors when unconfirmed steps are reached.

## Next phase recommendation

1. Confirm full RT2571W TX descriptor field semantics from trustworthy references.
2. Enable one confirmed management/null frame TX path and validate URB completion behavior.
3. Start bounded RX descriptor parsing only after TX descriptor correctness is established.
