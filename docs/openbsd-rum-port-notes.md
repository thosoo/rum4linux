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
  - BSSID programming via MAC_CSR4/5
  - conservative association state tracking
  - explicit BSSID clear programming on disassociation/remove/stop/disconnect

## Explicitly incomplete / deferred

- complete TX descriptor and hardware ACK/retry status coverage for all `rum(4)`-family variants
- TX rate programming beyond conservative CCK mappings used by the current minimal descriptor path
- high-confidence RX descriptor validation across additional variants/revisions beyond the current conservative mapping
- association / operational station behavior
- broad USB ID enumeration and per-device bring-up policies
- broad firmware naming certainty across the whole family

Uncertain parts should stay tagged as `TODO(openbsd-rum-port)` until confirmed from primary sources.

Current RX descriptor assumptions are intentionally narrow and source-backed:

- `word0` data-byte-count extraction and drop/CRC/ofdm flag usage from Linux `rt73usb` receive path.
- `word1` signal/rssi/frame-offset extraction pattern from Linux `rt73usb`.
- OpenBSD `if_rum.c` framing model (USB payload includes descriptor metadata plus frame body).

Any broader bit semantics or per-revision behavior remain `TODO(openbsd-rum-port)`.

## Device-ID and coverage policy

- Naming/docs are generalized to `rum(4)` family scope.
- Actual runtime binding remains intentionally conservative.
- If only a narrow USB ID set is enabled in code, that is intentional until broader per-device validation is complete.

## Workflow policy at this stage

- Keep safety-first defaults (`bind=0`).
- Do not treat this tree as ready for functional verification.
