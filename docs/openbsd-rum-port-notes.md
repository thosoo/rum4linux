# OpenBSD rum(4) port notes for rum4linux

## Intent

`rum4linux` is a narrow Linux station-client porting path guided primarily by OpenBSD `if_rum.c` and `if_rumreg.h`, with Linux `rt73usb/rt2x00` used only as behavioral cross-check where needed.

Current target is **DWA-111 (`07d1:3c06`, RT2571W + RT2528, 2.4 GHz)**.

## Primary references

- OpenBSD `sys/dev/usb/if_rum.c`
- OpenBSD `sys/dev/usb/if_rumreg.h`
- Linux `drivers/net/wireless/ralink/rt2x00/rt73usb.c/.h` (cross-check only)

## Implemented in this patchset

### Station state / RUN sequencing

- BSS RUN programming remains OpenBSD-ordered: channel -> ERP/slot -> retry/MRR -> basic rates -> BSSID -> TSF sync.
- `bss_info_changed()` now keeps station transitions symmetric and coherent for association, disassociation, reassociation, stop, and disconnect.
- Runtime BSS shadowing is kept for bounded reset/recovery re-entry.
- AID remains software-tracked only; no dedicated RT2573 hardware AID register/field was confirmed from OpenBSD sources.

### TX path (narrow RT2573 subset)

- PLCP CCK/OFDM programming remains OpenBSD-shaped (`rum_setup_tx_desc()`).
- ACK-rate and duration programming now follow OpenBSD formulas (`rum_ack_rate()`, `rum_txtime()`) for unicast data duration updates.
- Protection requests (RTS/CTS or CTS-to-self) use a bounded policy: non-data frames are rejected, while data frames may use conservative bypass (tracked + logged); full OpenBSD-style separate protection frame emission is still not implemented.
- TX status remains conservative and transport-completion based; no ACK success is claimed without confirmed hardware status ingestion.
- Retry-limit programming uses confirmed `TXRX_CSR4` fields in the narrow path; there is no separate distinct MRR control step exposed beyond this register programming.

### RX path

- RX descriptor and frame handling remains conservative/source-backed for RT2573 narrow path.
- Delivery path supports scan/auth/assoc/EAPOL/data traffic classes with filter-driven failed-frame behavior.

### Reset/recovery

- Added bounded reset/recovery workqueue path for realistic recoverable USB fault classes:
  - TX submit failures
  - TX/RX completion errors (non-cancel statuses)
- Recovery performs stop/reinit/restart and restores station runtime programming when possible.
- Reset scheduling/execution is now race-hardened with reset flags + mutex and compact counters for request reason/success/failure/last stage.
- Reset replay now has explicit started-unassociated restore before optional associated RUN re-entry.
- Added TX watchdog (bounded timeout) to detect stalled in-flight TX and request reset.
- Added reset storm cooldown to suppress repeated immediate resets under persistent fault loops.

### TX pressure / skb lifecycle

- USB TX is now bounded by a small in-flight URB cap with mac80211 queue stop/wake backpressure.
- Submit/completion/cancel/reset paths update in-flight accounting coherently and keep skb ownership/reporting single-path, including reset-cancel vs teardown-cancel distinction.

### Capability truthfulness

- `IEEE80211_HW_SUPPORTS_PS` is no longer advertised.
- Probe USB ID match table is narrowed to the target DWA-111 ID (`07d1:3c06`).
- Runtime RF acceptance is also narrowed to RT2528 only for this target-first branch.

## Deliberately not implemented (not source-confirmed or broader than target)

- host-visible per-frame RT2573 ACK/retry truth path
- full OpenBSD-equivalent protection-frame offload mechanics (separate RTS/CTS/CTS-self frame TX)
- broad rum(4) family USB ID enablement
- 5 GHz bring-up path

## Remaining blockers after this patchset

- Real-hardware confirmation of long-run stability (association churn, poor-RF edge cases).
- Better hardware-backed TX result visibility if a confirmed status path can be wired without guessing.
- Full protection-frame behavior parity if needed for difficult mixed B/G environments.

Any uncertain behavior stays explicitly conservative and source-scoped.
