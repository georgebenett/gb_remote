# USB serial protocol

The config tool talks to the remote over the USB-Serial-JTAG port. The same
port carries the ESP-IDF log and `#>DATA key=value` text lines, so a host
must skip bytes until a start byte and reject frames whose CRC fails.
`firmware/main/usb_serial.h` is the authoritative command list.

## Frame

```
0xAA | cmd | len lo | len hi | payload[len] | crc lo | crc hi
```

- `len` ≤ 512, little-endian. All multi-byte fields are little-endian.
- CRC-16-CCITT (poly 0x1021, init 0xFFFF, no reflection) over `cmd`, both
  `len` bytes and the payload.
- A frame whose bytes stop for more than 50 ms is dropped.
- A bad CRC is answered with `RSP_ACK [cmd, 0x03]`.

Commands that change state answer `RSP_ACK (0x80) [cmd, error]`, with `error`
from `error_code_t` (`0x00` OK). Commands marked *full* answer
`ERR_NOT_SUPPORTED` while the remote is on its charging screen.

## Commands

| ID | Name | Payload | Reply |
|---|---|---|---|
| 0x01 | PING | – | ACK |
| 0x02 | GET_FIRMWARE_VERSION | – | 0x82: major, minor, patch, then three length-prefixed strings: build date, model, IDF version |
| 0x03 | GET_CONFIG | – | 0x83 (legacy; prefer GET_ALL) |
| 0x04 | GET_CALIBRATION | – | 0x84: 1, throttle min u32, max u32, [brake min, max], latest mapped value u32; or ACK `NOT_CALIBRATED` |
| 0x05 | CALIBRATE_THROTTLE *full* | – | 0x88 progress frames (~6 s): sample u16, total u16, percent, throttle cur/min/max u32, brake cur/min/max u32; then ACK (`0x0B`–`0x0E` give the failure reason) |
| 0x06 | RESET_ODOMETER | – | ACK (`NO_RECEIVER` without a link) |
| 0x07 | SET_SPEED_UNIT | 0 km/h, 1 mph | ACK |
| 0x08 | SET_BACKLIGHT | percent 0–100 | ACK |
| 0x09 | INVERT_THROTTLE *full* | – (toggles, LITE) | ACK |
| 0x10 | START_STREAMING *full* | – | 0x90 frames, see below |
| 0x11 | STOP_STREAMING *full* | – | ACK |
| 0x12 | SET_STREAM_RATE *full* | Hz u16, 1–100 | ACK |
| 0x13 / 0x14 | INCREASE / DECREASE_BLE_TRIM *full* | – | ACK (`OUT_OF_RANGE` at ±127) |
| 0x15 | GET_BLE_TRIM *full* | – | 0x85: trim i8 |
| 0x16 | CHECK_COREDUMP | – | 0x86: exists, size u32 |
| 0x17 | GET_COREDUMP | chunk offset u16 | 0x87 chunk |
| 0x1E | BOOT_FULL_MODE | – | ACK, leaves the charging screen |
| 0x1F | SET_HAPTIC_INTENSITY | percent 0–100 | ACK |
| 0x24 | TOGGLE_DUAL_CONNECTION *full* | – | ACK; opens a pairing window when enabled |
| 0x25 | SET_BATTERY_CELLS | cells (0 or 5–20) [, cell type] | ACK |
| 0x26 / 0x27 / 0x2A | TOGGLE_SMART_REVERSE / ASSIST_PUSH / NO_REVERSE | – | ACK |
| 0x28 / 0x29 | SET / GET_ASSIST_PARAMS | strength %, decay rpm/s | ACK / 0x91 |
| 0x2B / 0x2C | SET / GET_THROTTLE_CURVE | mode, acc i8, brake i8 (tenths) | ACK / 0x92 |
| 0x2D | SET_RIDE_SETTINGS | profile 0–3, speed limit km/h (0 or 20–50) | ACK |
| 0x2F | GET_ALL | – | 0x94, below |
| 0x30 | START_PAIRING *full* | – | ACK; accepts a new receiver for 60 s |
| 0x31 | FORGET_RECEIVERS *full* | – | ACK; deletes every bonded receiver |

## GET_ALL (0x94)

`[format version = 1] [tag, len, value]...` A host skips tags it does not
know; the remote omits what it does not have. Tags are the `ALL_TAG_*`
defines in `usb_serial.h` (flags, backlight, motor, speed, trim, haptic,
battery, calibration, assist, curve, ride).

## Stream (0x90)

| Bytes | Field |
|---|---|
| 0–3 | uptime ms |
| 4 | flags: bit0 receiver connected, bit1 calibrated, bit2 low battery (throttle cut) |
| 5–8 | throttle ADC average |
| 9–12 | brake ADC average (0 on LITE) |
| 13 | the command byte actually sent to the receiver: 0 full brake, 128 neutral, 255 full throttle |
