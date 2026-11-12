# Changelog

## 3.0.9

Bundles receiver v3.0.9.

Fixes
- A bonded remote now bonds to a newly paired receiver: the receiver advertises the remote's MAC and the remote connects when it sees its own.

## 3.0.8

Bundles receiver v3.0.8.

Safety
- Low remote battery cuts throttle but keeps the brake (was: neutral, which also blocked braking).
- Receiver name must match exactly; the remote only connects to bonded receivers outside a pairing window.
- Throttle and commands are sent only on encrypted links; the settings sent on connect now reach the receiver.
- Calibration is applied atomically and only once saved.

Fixes
- USB serial reads no longer crawl at ~50 B/s.
- NVS errors no longer abort; settings load once into RAM.
- Haptic requests no longer block or get dropped.
- Telemetry decode free of signed-shift UB; read_coredump.sh reads the right partition.

Changes
- Shared `gb_protocol.h` with gb_receiver; USB `CMD_START_PAIRING` (0x30) and `CMD_FORGET_RECEIVERS` (0x31).
- `build.sh <variant>`, releases built in CI from a pushed tag (`release.sh`).
- Dynamic frequency scaling; backlight fades in hardware; UI updates on LVGL timers.
- Snake easter egg is off unless `CONFIG_GB_SNAKE_GAME` is set.
- License: PolyForm Noncommercial 1.0.0.

## 3.0.7

Bundles receiver v3.0.7. Earlier history: `git log`.
