# rum4linux

Early Linux `mac80211` + `usb_driver` implementation for a narrow OpenBSD `rum(4)`-backed path.

## Exact supported target

This tree currently targets only:

- **D-Link DWA-111**
- USB ID: **`07d1:3c06`**
- chipset/RF path: **RT2571W + RT2528**
- band scope: **2.4 GHz only**
- mode scope: **station (client) only**

Everything else is intentionally out of scope until separately source-backed and validated.

## Current implemented scope (narrow, source-backed)

- safe probe path with `bind=0` default
- USB register I/O + EEPROM/firmware/BBP/RF init scaffolding
- source-backed RT2528 2.4 GHz channel apply path with bounded recovery attempt
- mac80211 station hooks for MAC/BSSID, RX filter, basic rates, ERP timing, TSF sync
- RUN-state ordering aligned to OpenBSD `if_rum.c` station path
- symmetric disassociate/reassociate/stop/disconnect BSSID+TSF handling
- conservative TX descriptor programming for CCK+OFDM PLCP fields
- bounded USB TX in-flight model with mac80211 queue stop/wake backpressure
- bounded TX watchdog detects stalled in-flight TX and requests recovery
- software duration updates using OpenBSD `rum_ack_rate()`/`rum_txtime()` formulas
- retry-limit programming is implemented through `TXRX_CSR4` fields; no separate distinct MRR control path is currently exposed beyond that narrow register programming
- conservative RX descriptor parse/delivery for scan/auth/assoc/EAPOL/data traffic
- bounded reset/recovery workqueue path for realistic TX/RX USB fault classes
- reset storm control with cooldown suppresses repeated immediate resets
- reset observability counters/log summary for request reasons and last recovery stage/failure point

## Truthful limitations that remain

- no confirmed host-visible RT2573 per-frame ACK/retry status ingestion path is wired
- tx status remains conservative and does not claim hardware ACK truth
- no confirmed dedicated RT2573 hardware AID register/field from OpenBSD sources; AID remains software-tracked
- mac80211 RTS/CTS and CTS-to-self requests on supported station data TX now emit a dedicated protection frame (OpenBSD `rum_tx_data()` shape: protection frame first, then data frame)
- protection requests on non-data frames, contradictory RTS+CTS requests, or protection-frame synthesis/submit failures are rejected conservatively (counted) rather than silently bypassed
- only USB ID `07d1:3c06` is matched in this target-first branch
- no 5 GHz support

## Safety defaults and binding

- module parameter default is `bind=0` (no attach)
- enable binding explicitly while testing:

```bash
sudo modprobe rum4linux bind=1
```

## Layout

- `dkms.conf` — DKMS metadata (`rum4linux`)
- `Makefile` — Kbuild wrapper for `rum4linux.ko`
- `src/rum4linux_core.c` — USB + mac80211 entry points, station state sequencing
- `src/rum4linux_hw.c` / `src/rum4linux_hw.h` — register/control hardware core
- `src/rum4linux_eeprom.c` / `src/rum4linux_eeprom.h` — EEPROM subsystem
- `src/rum4linux_fw.c` / `src/rum4linux_fw.h` — firmware subsystem
- `src/rum4linux_bbp.c` / `src/rum4linux_bbp.h` — BBP subsystem
- `src/rum4linux_rf.c` / `src/rum4linux_rf.h` — RF/channel subsystem
- `src/rum4linux_tx.c` / `src/rum4linux_tx.h` — TX path
- `src/rum4linux_rx.c` / `src/rum4linux_rx.h` — RX path
- `src/rum4linux_debug.h` — logging helpers
- `docs/openbsd-rum-port-notes.md` — source/porting notes
