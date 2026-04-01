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
- source-backed 2.4GHz-only band profile apply path (OpenBSD rum_select_band subset) for BBP17/35/96/97/98/104, ext_2ghz_lna BBP75/86/88 handling, and PHY_CSR0 PA_PE_2GHZ selection
- bounded post-channel sanity check and one bounded recovery attempt
- bounded TX descriptor path with runtime bulk-OUT submission and URB-completion status handoff to mac80211
- bounded RX bulk-IN URB pipeline with strict descriptor/frame sanity checks and conservative mac80211 delivery
- minimal station-mode interface/BSSID runtime programming hooks, including BSSID clear on disassociation/teardown
- source-backed station runtime register programming for MAC address, RX filter, basic rates, TSF sync, and ERP timing knobs
- truthful current TX rate surface: only 2.4GHz CCK (1/2/5.5/11 Mbps) is advertised/operational for the narrow descriptor path; OFDM TX remains deferred
- initial and runtime 2.4GHz channel applies now share one bounded sequence (BBP profile -> RF set -> post-channel sanity -> one bounded recovery attempt)
- channel-apply observability counters/stage tracking now record init vs runtime applies, first-pass failures, bounded recovery outcomes, and last stage/channel/error
- channel-apply diagnostics now include conservative error-class buckets (invalid/unsupported/timeout/io/sanity/unknown) derived from stage+errno for faster field triage
- channel-apply diagnostics now also include conservative error-origin attribution (bbp_profile/rf_set/sanity-read/sanity-pattern and recovery equivalents) with per-origin counters
- origin counters are failure-attribution-only (not stage-visit counters), and channel summaries now include one compact preserved last-failure snapshot with any captured sanity values
- probe-time EEPROM MAC adoption for mac80211/hardware identity coherence (random fallback only on EEPROM failure)
- RUN-state sequencing now mirrors OpenBSD rum(4) ordering for channel/slot/MRR/preamble/basic-rates/BSSID/TSF sync and aborts TSF sync on RUN exit
- conservative TX retry-limit/fallback plumbing now programs confirmed TXRX_CSR4 fields; TX status still avoids claiming ACK success without hardware feedback
- no confirmed host-visible RT2573 per-frame TX ACK/retry result ingestion path is wired yet; tx status remains transport-completion-limited
- RX CCK rate decoding now follows source-backed raw 100kbps descriptor values (10/20/55/110) instead of low-bit masking
- station RX filter parity tightened to rt73usb semantics (ACK/CTS follows FIF_CONTROL, control follows FIF_CONTROL|FIF_PSPOLL)
- conservative BBP17/VGC tuner added for the narrow associated station path using source-backed RSSI/FCS/false-CCA inputs (false_cca > 512 raises gain, < 100 lowers gain within guarded bounds)
- BBP17 tuner now keys off the current 2.4GHz base profile value instead of a hardcoded 0x20 baseline
- RX software delivery is now coherent with configured FIF_FCSFAIL / FIF_PLCPFAIL policy: allowed failed frames are delivered with mac80211 failure flags and counted separately

Still intentionally incomplete:

- full, validated TX descriptor/status semantics across `rum(4)`-family variants
- OFDM TX descriptor/status support is still deferred; OFDM rates may be RX-decoded but are not advertised as TX-operational in this narrow station path
- full RX descriptor confidence across all rum(4)-family variants
- full confirmation of all RT2573 RXD_W0_DROP causes remains TODO(openbsd-rum-port); current mapping treats it as PLCP/PHY-failure class only for narrow filter coherence
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
