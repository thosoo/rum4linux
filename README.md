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
- channel-apply failure diagnostics now also retain a bounded delta snapshot (latest failure vs previous retained failure) covering runtime/init, channel/stage/class/origin/errno, and compact sanity-read value state (missing/same/changed)
- probe-time EEPROM MAC adoption for mac80211/hardware identity coherence (random fallback only on EEPROM failure)
- RUN-state sequencing now mirrors OpenBSD rum(4) ordering for channel/slot/MRR/preamble/basic-rates/BSSID/TSF sync and aborts TSF sync on RUN exit
- conservative TX retry-limit/fallback plumbing now programs confirmed TXRX_CSR4 fields; TX status still avoids claiming ACK success without hardware feedback
- no confirmed host-visible RT2573 per-frame TX ACK/retry result ingestion path is wired yet; tx status remains transport-completion-limited
- RX CCK rate decoding now follows source-backed raw 100kbps descriptor values (10/20/55/110) instead of low-bit masking
- station RX filter parity tightened to rt73usb semantics (ACK/CTS follows FIF_CONTROL, control follows FIF_CONTROL|FIF_PSPOLL)
- conservative BBP17/VGC tuner added for the narrow associated station path using source-backed RSSI/FCS/false-CCA inputs (false_cca > 512 raises gain, < 100 lowers gain within guarded bounds)
- BBP17 tuner now keys off the current 2.4GHz base profile value instead of a hardcoded 0x20 baseline
- RX software delivery is now coherent with configured FIF_FCSFAIL / FIF_PLCPFAIL policy: allowed failed frames are delivered with mac80211 failure flags and counted separately
- RX descriptor failure taxonomy is now slightly tightened for the narrow RT2573 path: explicit CRC bit remains failed-FCS, while `RXD_W0_DROP` is treated as a broader non-CRC descriptor-drop class (not claimed as pure PLCP/PHY), still gated via `FIF_PLCPFAIL` as the closest mac80211 policy hook
- RX framing now follows source-backed RT2573/rt73 shape more closely: descriptor byte-count is used directly as frame length (no unconditional FCS subtraction), and frame start is fixed at descriptor end (24-byte descriptor); non-zero descriptor frame-offset is currently treated as unsupported/TODO-scoped and dropped conservatively
- narrow station timing defaults are now tightened to OpenBSD-backed RT2573 values in the current path: slot 9/20us, SIFS 10us, OFDM-SIFS 3us, EIFS 0x016c, RX_ACK_TIMEOUT 0x32, TSF_OFFSET 24
- RT2573 RX timing defaults are now applied directly in hardware init (`dwr_hw_init`) for the active narrow path, rather than only from mac80211 start call-site wiring
- STA TSF sync programming now explicitly mirrors OpenBSD `rum_enable_tsf_sync()` shape by preserving TXRX_CSR9 timestamp-compensation bits [31:24] and rebuilding the low TSF-control bits from scratch (interval + TSF mode/ticking/TBTT in STA mode)
- TXRX_CSR9 timestamp-compensation high-byte value itself remains unresolved for this narrow path; current code preserves existing hardware/default value (`TODO(openbsd-rum-port)`) instead of inventing a constant
- hardware AID programming is still unresolved: primary-source review of OpenBSD `if_rum.c` + `if_rumreg.h` did not confirm a dedicated RT2573 station-path AID register/field, so AID remains software-tracked only (`TODO(openbsd-rum-port)`)

Still intentionally incomplete:

- full, validated TX descriptor/status semantics across `rum(4)`-family variants
- OFDM TX descriptor/status support is still deferred; OFDM rates may be RX-decoded but are not advertised as TX-operational in this narrow station path
- full RX descriptor confidence across all rum(4)-family variants
- full confirmation of all RT2573 RXD_W0_DROP causes remains TODO(openbsd-rum-port); current mapping is narrowed to “non-CRC descriptor-drop” and only uses PLCP-failure policy/flag as a conservative proxy
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
