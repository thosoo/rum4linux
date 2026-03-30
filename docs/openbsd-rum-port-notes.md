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
- bounded TX descriptor scaffolding with runtime submission intentionally blocked

## Explicitly incomplete / deferred

- complete TX descriptor coverage for all `rum(4)`-family variants
- RX descriptor parsing and RX datapath
- association / operational station behavior
- broad USB ID enumeration and per-device bring-up policies
- broad firmware naming certainty across the whole family

Uncertain parts should stay tagged as `TODO(openbsd-rum-port)` until confirmed from primary sources.

## Device-ID and coverage policy

- Naming/docs are generalized to `rum(4)` family scope.
- Actual runtime binding remains intentionally conservative.
- If only a narrow USB ID set is enabled in code, that is intentional until broader per-device validation is complete.

## Workflow policy at this stage

- Keep safety-first defaults (`bind=0`).
- Do not treat this tree as ready for functional verification.
