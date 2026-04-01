# OpenBSD rum(4) port notes for rum4linux

## Intent

`rum4linux` is an early Linux scaffold inspired by OpenBSD `rum(4)`, being generalized toward the `rum(4)` family structure rather than a single USB product narrative.

This is a structural generalization pass, not a claim of broad functional enablement.

## Primary references

- OpenBSD `sys/dev/usb/if_rum.c`
  - EEPROM read flow
  - microcode load flow
  - BBP read/write/init flow
  - RF write and channel programming sequencing
- OpenBSD `sys/dev/usb/if_rumreg.h`
  - vendor request IDs
  - register offsets and BBP/RF bit definitions
  - EEPROM offsets and RF identity values
  - `RT2573_DEF_BBP`
- Linux `rt73usb` lineage (`drivers/net/wireless/ralink/rt2x00/rt73usb.c/.h`)
  - `rf_vals_bg_2528[]`
  - txpower clamping conventions

## Implemented scaffold status (conservative)

- module/DKMS identity: `rum4linux`
- vendor request and core register access path for init scaffolding
- EEPROM parse path with structured state capture
- firmware upload scaffold and MCU handoff/wait sequence
- BBP busy/read/write helpers and bounded init defaults
- RF/channel scaffold for RT2528 2.4GHz path with bounded calibration mapping
- narrow OpenBSD `rum_select_band()` 2.4GHz subset wired: BBP 17/35/96/97/98/104 + ext_2ghz_lna BBP 75/86/88 + PHY_CSR0 PA-path bit programming
- bounded post-channel sanity and one bounded recovery attempt
- bounded TX descriptor path with runtime bulk-OUT submission and mac80211 tx status handoff on URB completion
- bounded RX scaffolding:
  - fixed-size bulk-IN URB pool and per-URB buffers
  - explicit start/stop/cancel/free teardown for stop/disconnect/error paths
  - conservative descriptor parse of rt73/rum-style word0/word1 fields (length/drop/crc/ofdm/signal/rssi/frame-offset)
  - debug logging and bounded RX counters
  - URB resubmission while running
  - strict frame boundary checks and conservative `ieee80211_rx_irqsafe()` delivery for packets with source-backed descriptor semantics
- minimal station runtime hooks:
  - single station interface gate
  - station MAC programming via MAC_CSR2/3
  - BSSID programming via MAC_CSR4/5
  - conservative association state tracking
  - explicit BSSID clear programming on disassociation/remove/stop/disconnect
  - RX filter programming via TXRX_CSR0 from mac80211 filter flags
  - TSF/basic-rates/ERP runtime programming via TXRX_CSR4/5/9 and MAC_CSR8/9
  - EEPROM MAC used as primary station identity at probe (random fallback only if EEPROM parse fails)
  - RUN transition ordering aligned to if_rum.c for station path (channel -> slot/ERP -> MRR -> rates -> BSSID -> TSF sync)
  - explicit TSF-sync abort on RUN exit by clearing TXRX_CSR9 low 24 bits
  - conservative TX retry-limit/fallback programming via TXRX_CSR4 (OpenBSD MRR naming and rt73usb OFDM-down/fallback naming treated as aliases for shared bits)
  - TX status reports attempted rate/count but does not claim ACK success without hardware feedback
  - OpenBSD rum(4) path exposes USB completion for TX but no host-visible per-frame ACK/retry result; current Linux port keeps that limitation explicit
  - advertised/operational TX rates are now narrowed to truthful CCK-only (1/2/5.5/11 Mbps) for the current minimal descriptor path; OFDM TX remains deferred
  - initial bring-up and runtime channel transitions now use the same bounded 2.4GHz apply path (BBP profile, RF program, post-channel sanity, one bounded recovery attempt)
  - channel-apply diagnostics now keep bounded counters and last-stage tagging (init/runtime split, first-pass failures, recovery attempt/success/failure, last channel/stage/error)
  - channel-apply failures now also carry conservative error-class tagging (invalid/unsupported/timeout/io/sanity/unknown) based on existing stage+errno only
  - channel-apply failures now include conservative error-origin attribution (sanity-read source vs sanity-pattern vs profile/rf/recovery origin) with bounded counters
  - origin counters are failure-attribution counts only, and the latest failure snapshot is retained (stage/class/origin/errno + any successfully-captured sanity reads)
  - channel-apply failure diagnostics now also retain one bounded delta snapshot that compares latest retained failure vs the immediately previous retained failure (runtime/init, channel/stage/class/origin/errno, and sanity value state as missing/same/changed)
  - RX CCK rate decode follows rum_rxrate()/rt73 semantics (raw 100kbps values 10/20/55/110) with conservative OFDM PLCP mapping
  - station-stop/disconnect logging now includes source-backed STA_CSR0/STA_CSR1 error counters (FCS/PLCP/physical/false-CCA)
  - RX filter parity tightened to rt73usb station behavior (DROP_ACK_CTS follows FIF_CONTROL; DROP_CONTROL follows FIF_CONTROL|FIF_PSPOLL)
  - RX software delivery now follows active filter policy for failed-FCS and failed-PLCP/PHY classes: frames are dropped only when policy requests drop, and delivered failures are flagged/counted for mac80211 observability
  - RX failure taxonomy is now tightened slightly for RT2573: explicit RX_CRC_ERROR remains the only concrete FCS class, while RX_DROP is treated as broader non-CRC descriptor-drop (not asserted as pure PLCP/PHY), still mapped through FIF_PLCPFAIL/RX failed-PLCP flag as a conservative policy proxy
  - RUN-entry TXRX_CSR4 programming now uses one coherent final path for aliased MRR/OFDM-fallback fields
  - conservative BBP17/VGC tuner is wired for associated station mode using rt73usb-backed inputs (RSSI, FCS, false-CCA); false_cca thresholds are intentionally conservative (>512 up, <100 down), and STA_CSR1 low-16 remains non-policy observability only
  - BBP17/VGC tuner now uses the runtime 2.4GHz profile baseline (including ext_2ghz_lna offset) rather than a fixed 0x20 base
  - narrow station timing defaults are now explicitly aligned to OpenBSD RT2573 source values: slot time 9/20us (rum_update_slot), TXRX_CSR9 beacon interval in 1/16ms with STA TSF mode (rum_enable_tsf_sync), MAC_CSR8 SIFS/OFDM-SIFS/EIFS from RT2573_DEF_MAC (0x016c030a), and TXRX_CSR0 RX_ACK_TIMEOUT/TSF_OFFSET from RT2573_DEF_MAC (0x025fb032)
  - RX timing defaults are now applied in the narrow hardware-init path (`dwr_hw_init`) so active station bring-up does not depend on a separate mac80211-start call-site for TXRX_CSR0 timing defaults

## Explicitly incomplete / deferred

- complete TX descriptor and hardware ACK/retry status coverage for all `rum(4)`-family variants
- confirmed RT2573 host-visible TX-result ingestion path (ACK/retry/failure truth) still not wired in this non-rt2x00 port
- TX rate programming beyond conservative CCK mappings used by the current minimal descriptor path
- OFDM RX decode remains conservative/source-backed, but OFDM TX descriptor/status plumbing is intentionally not exposed yet
- high-confidence RX descriptor validation across additional variants/revisions beyond the current conservative mapping
- association / operational station behavior
- full source-backed validation of runtime timing defaults across revisions is still incomplete; current tightening is intentionally narrow to RT2573 station-path values confirmed from OpenBSD sources
- hardware AID programming remains unresolved; primary-source review of OpenBSD `if_rum.c` + `if_rumreg.h` did not confirm a dedicated RT2573 station-path AID register/field, so current path keeps AID software-tracked with TODO(openbsd-rum-port)
- fake-join tx-rate init from if_rum.c has no direct mac80211 equivalent in current narrow path and remains intentionally unported
- broad USB ID enumeration and per-device bring-up policies
- broad firmware naming certainty across the whole family

Uncertain parts should stay tagged as `TODO(openbsd-rum-port)` until confirmed from primary sources.

Current RX descriptor assumptions are intentionally narrow and source-backed:

- `word0` data-byte-count extraction and drop/CRC/ofdm flag usage from Linux `rt73usb` receive path.
- `word1` signal/rssi/frame-offset extraction pattern from Linux `rt73usb`.
- OpenBSD `if_rum.c` framing model (USB payload includes descriptor metadata plus frame body).

Any broader bit semantics or per-revision behavior remain `TODO(openbsd-rum-port)`.

`RXD_W0_DROP`/`RT2573_RX_DROP` remains a broad descriptor bit in available sources; in this narrow port it is treated as a non-CRC descriptor-drop class. Because no dedicated mac80211 “generic RX drop” failure policy exists for this path, `FIF_PLCPFAIL` and RX failed-PLCP flagging are used as conservative proxies only. Fuller cause mapping remains `TODO(openbsd-rum-port)`.

## Device-ID and coverage policy

- Naming/docs are generalized to `rum(4)` family scope.
- Actual runtime binding remains intentionally conservative.
- If only a narrow USB ID set is enabled in code, that is intentional until broader per-device validation is complete.

## Workflow policy at this stage

- Keep safety-first defaults (`bind=0`).
- Do not treat this tree as ready for functional verification.
