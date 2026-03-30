# OpenBSD rum(4) port notes for rum4linux

## Primary reference files/concepts

- OpenBSD `sys/dev/usb/if_rum.c`
  - endpoint selection pattern
  - EEPROM extraction (`rum_read_eeprom`)
  - firmware upload and MCU run handoff (`rum_load_microcode`)
  - wakeup sequencing around `MAC_CSR1` / `MAC_CSR12`
- OpenBSD `sys/dev/usb/if_rumreg.h`
  - vendor request IDs
  - register map naming
  - EEPROM offsets
  - RF identifiers (`RT2528` etc.)

## Ported confidently in this phase

- vendor request IDs:
  - MCU control `0x01`
  - write MAC `0x02`
  - read MAC `0x03`
  - read EEPROM `0x09`
- core register names and base addresses used by init path:
  - `MAC_CSR0`, `MAC_CSR1`, `MAC_CSR12`, `TXRX_CSR0`, MCU code base
- EEPROM offsets and field extraction logic:
  - MAC/BBP rev, address, antenna word, config2, RSSI offsets, freq offset, txpower table, BBP PROM table
- firmware structure:
  - chunked 32-bit writes to MCU code space
  - explicit MCU_RUN handoff request
  - post-handoff ready polling hook

## Still uncertain / intentionally not claimed complete

- exact firmware requirements per sub-revision and fallback file naming on Linux distros
- whether DWA-111 variant always re-enumerates after firmware handoff
- full BBP/RF initialization table sequencing and channel-specific RF programming
- complete interpretation of all EEPROM words beyond fields currently mapped
- TX/RX descriptor programming and data path behavior

All uncertain parts are tagged in code as `TODO(openbsd-rum-port)`.

## Next phase recommendation

1. Port BBP init defaults and EEPROM-derived BBP overrides from OpenBSD `rum_bbp_init`.
2. Port RF channel programming tables for RT2528 path with strict 2.4 GHz scope first.
3. Add read-back sanity checks around BBP/RF register writes.
4. Only after stable init, begin TX descriptor builder and controlled null-data TX smoke tests.
