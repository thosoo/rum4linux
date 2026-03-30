# dwa111-rum-port-0.1

Development scaffold for a Linux replacement driver for the D-Link DWA-111 (USB ID `07d1:3c06`) in the style of OpenBSD `rum(4)`.

This is **not** a finished driver. It is a starting point for a clean-room Linux/mac80211 implementation that borrows the *architecture* of OpenBSD `rum(4)`:

- dedicated USB register access layer
- firmware upload + post-upload reset path
- EEPROM parse path
- BBP/RF init sequencing
- mac80211 glue kept thin

## Why this direction?

Your logs consistently show:

- `device firmware changed`
- `Vendor Request 0x09 failed for offset 0x0000 with error -19`
- later `Vendor Request 0x07 failed for offset 0x3000 with error -19`
- `rt: 0000, rf: 0000, rev: 0000`
- `Invalid RT chipset detected`
- repeated disconnect/re-enumeration with `too many configurations: 60`

That suggests the current Linux `rt73usb` / `rt2x00` stack is the wrong place to keep patching.

## Safety

This scaffold defaults to **not binding** to the device (`bind=0` module parameter). That is deliberate.

## Layout

- `dkms.conf` — DKMS metadata
- `Makefile` — Kbuild wrapper
- `src/dwa111_rum_main.c` — USB + mac80211 scaffold
- `src/dwa111_rum_hw.c` — hardware-facing stubs
- `src/dwa111_rum_debug.h` — logging helpers
- `CODEX_PROMPT.md` — detailed prompt for ChatGPT Codex

## Development milestones

1. USB probe/remove + safe teardown
2. Control transfer helpers mirroring OpenBSD `rum(4)` register read/write style
3. Firmware upload and post-upload settle sequence
4. EEPROM parse and MAC address extraction
5. BBP/RF init tables
6. Channel switching
7. RX/TX descriptor handling
8. Association in station mode on open/WEP networks
9. WPA via mac80211 software crypto

## Suggested workflow

1. Keep the in-tree `rt73usb` available until your replacement can at least probe safely.
2. Develop with `bind=0` first and use dynamic debug / pr_debug.
3. Once EEPROM parsing works, switch to `bind=1` and blacklist `rt73usb` for controlled tests.
