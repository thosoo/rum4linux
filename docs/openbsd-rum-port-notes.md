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
- Protection requests (RTS/CTS or CTS-to-self) for supported station data TX now follow OpenBSD `rum_tx_data()` shape by emitting a dedicated protection frame first, followed by the data frame.
- Protection TX uses a narrow source-backed profile for this target branch: 2.4 GHz station path, 1 Mbps CCK protection frame descriptor rate, RTS protection frame requests ACK, CTS-to-self does not.
- Unsupported protection cases are rejected conservatively and counted (non-data frame requests, contradictory RTS+CTS requests, or protection frame synthesis/submit failures); protected data is not sent unprotected.
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
- In-flight accounting is per URB, including protected data transmissions that submit protection + data URBs, so queue stop/wake and watchdog timing track true USB outstanding work.
- Submit/completion/cancel/reset paths update in-flight accounting coherently and keep skb ownership/reporting single-path, including reset-cancel vs teardown-cancel distinction.

### Capability truthfulness

- `IEEE80211_HW_SUPPORTS_PS` is no longer advertised.
- Probe USB ID match table is narrowed to the target DWA-111 ID (`07d1:3c06`).
- Runtime RF acceptance is also narrowed to RT2528 only for this target-first branch.

## Deliberately not implemented (not source-confirmed or broader than target)

- host-visible per-frame RT2573 ACK/retry truth path
- full family-wide protection behavior beyond the narrow DWA-111 station target (no monitor/AP/IBSS extension, no broader USB ID enablement)
- broad rum(4) family USB ID enablement
- 5 GHz bring-up path

## Remaining blockers after this patchset

- Real-hardware confirmation of long-run stability (association churn, poor-RF edge cases).
- Better hardware-backed TX result visibility if a confirmed status path can be wired without guessing.
- Additional real-hardware validation for mixed B/G protection edge cases and long-run retry/reset interaction.

Any uncertain behavior stays explicitly conservative and source-scoped.
