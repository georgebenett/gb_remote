# USB Serial Binary Protocol Specification

## Overview

This document describes the binary communication protocol used between the host (config tool) and the device (GB Remote firmware) over USB Serial (USB CDC). The protocol uses a packet-based binary format with CRC-16 error checking.

**Protocol Version:** 1.0  
**Target Device:** ESP32-S3  
**Communication:** USB Serial (USB CDC)  
**Baud Rate:** Not applicable (USB CDC)

---

## Packet Structure

All packets follow this structure:

```
[START_BYTE] [CMD_ID] [LENGTH_LSB] [LENGTH_MSB] [PAYLOAD...] [CRC16_LSB] [CRC16_MSB]
```

### Packet Fields

| Field | Size | Description |
|-------|------|-------------|
| START_BYTE | 1 byte | Synchronization marker: `0xAA` |
| CMD_ID | 1 byte | Command identifier (see Command IDs section) |
| LENGTH | 2 bytes | Payload length in bytes (little-endian, does NOT include header/CRC) |
| PAYLOAD | 0-512 bytes | Variable length data (command-specific) |
| CRC16 | 2 bytes | CRC-16-CCITT checksum (little-endian) |

### Minimum Packet Size
- **Minimum:** 6 bytes (START + CMD + LENGTH(2) + CRC(2))
- **Maximum:** 518 bytes (START + CMD + LENGTH(2) + PAYLOAD(512) + CRC(2))

### Byte Order
All multi-byte values are **little-endian** (LSB first).

---

## CRC-16 Calculation

The CRC-16 is calculated using the **CRC-16-CCITT** algorithm (polynomial: `0x1021`, initial value: `0xFFFF`).

### CRC Calculation Scope
The CRC is calculated over: `[CMD_ID] [LENGTH_LSB] [LENGTH_MSB] [PAYLOAD...]`

**Note:** The START_BYTE is NOT included in the CRC calculation.

### CRC-16-CCITT Algorithm (Reference Implementation)

```python
def calculate_crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc = crc << 1
        crc &= 0xFFFF
    return crc
```

---

## Command IDs

### Request Commands (Host → Device)

| Command | ID | Description |
|---------|-----|-------------|
| `CMD_PING` | `0x01` | Ping device (no payload) |
| `CMD_GET_FIRMWARE_VERSION` | `0x02` | Get firmware version info (no payload) |
| `CMD_GET_CONFIG` | `0x03` | Get all configuration (no payload) |
| `CMD_GET_CALIBRATION` | `0x04` | Get throttle/brake calibration values (no payload) |
| `CMD_CALIBRATE_THROTTLE` | `0x05` | Start throttle calibration (no payload) |
| `CMD_RESET_ODOMETER` | `0x06` | Reset trip distance/odometer (no payload) |
| `CMD_SET_SPEED_UNIT` | `0x07` | Set speed unit (payload: `uint8`, 0=km/h, 1=mph) |
| `CMD_SET_BACKLIGHT` | `0x08` | Set LCD backlight brightness (payload: `uint8`, 0-100) |
| `CMD_INVERT_THROTTLE` | `0x09` | Toggle throttle inversion (no payload, Lite mode only) |
| `CMD_START_STREAMING` | `0x10` | Start real-time data streaming (optional payload: `uint16` rate in Hz) |
| `CMD_STOP_STREAMING` | `0x11` | Stop real-time data streaming (no payload) |
| `CMD_SET_STREAM_RATE` | `0x12` | Set streaming rate (payload: `uint16` rate in Hz, 1-100) |

### Response Commands (Device → Host)

| Response | ID | Description |
|----------|-----|-------------|
| `RSP_ACK` | `0x80` | Acknowledge with result code |
| `RSP_ERROR` | `0x81` | Error response with message |
| `RSP_FIRMWARE_VERSION` | `0x82` | Firmware version data |
| `RSP_CONFIG` | `0x83` | Configuration data |
| `RSP_CALIBRATION` | `0x84` | Calibration data |
| `RSP_STREAM_DATA` | `0x90` | Real-time streaming data |

---

## Error Codes

| Code | Value | Description |
|------|-------|-------------|
| `ERR_OK` | `0x00` | Success |
| `ERR_UNKNOWN_CMD` | `0x01` | Unknown command |
| `ERR_INVALID_PAYLOAD` | `0x02` | Invalid payload length or data |
| `ERR_CRC_MISMATCH` | `0x03` | CRC check failed |
| `ERR_CALIBRATION_FAILED` | `0x04` | Calibration failed |
| `ERR_SAVE_FAILED` | `0x05` | Failed to save to NVS |
| `ERR_NOT_CALIBRATED` | `0x06` | Device not calibrated |
| `ERR_OUT_OF_RANGE` | `0x07` | Parameter out of range |
| `ERR_NOT_SUPPORTED` | `0x08` | Command not supported on this target |

---

## Command Details

### CMD_PING (0x01)

**Request:** No payload  
**Response:** `RSP_ACK` with `ERR_OK`

Simple connectivity test.

---

### CMD_GET_FIRMWARE_VERSION (0x02)

**Request:** No payload  
**Response:** `RSP_FIRMWARE_VERSION`

**Response Payload Format:**
```
[version_major: uint8]
[version_minor: uint8]
[version_patch: uint8]
[build_date_len: uint8]
[build_date_str: string, build_date_len bytes]
[model_len: uint8]
[model_str: string, model_len bytes]
[idf_version_len: uint8]
[idf_version_str: string, idf_version_len bytes]
```

**Example:**
- Version: v1.2.3 → major=1, minor=2, patch=3
- Build date: "Dec 15 2024 10:30:45"
- Model: "gb_controller_lite" or "gb_controller_dual_throttle"
- IDF version: "v5.1.2"

---

### CMD_GET_CONFIG (0x03)

**Request:** No payload  
**Response:** `RSP_CONFIG`

**Response Payload Format:**
```
[flags: uint8]
[backlight: uint8]
[motor_poles: uint8]
[gear_ratio_x1000: uint16, little-endian]
[wheel_diameter_mm: uint16, little-endian]
[speed: int32, little-endian, signed]
[throttle_min: uint32, little-endian] (if calibrated)
[throttle_max: uint32, little-endian] (if calibrated)
[brake_min: uint32, little-endian] (if calibrated, dual throttle only)
[brake_max: uint32, little-endian] (if calibrated, dual throttle only)
```

**Flags Byte:**
- Bit 0: `speed_unit_mph` (1 = mph, 0 = km/h)
- Bit 1: `throttle_inverted` (1 = inverted, Lite mode only)
- Bit 2: `ble_connected` (1 = connected to VESC)
- Bit 3: `calibrated` (1 = throttle is calibrated)
- Bits 4-7: Reserved

**Notes:**
- `backlight`: 0-100 (percentage)
- `gear_ratio_x1000`: Stored as integer × 1000 (e.g., 2200 = 2.2)
- `speed`: Current speed in configured units (km/h or mph), only valid if BLE connected
- Throttle/brake calibration values are only included if device is calibrated

---

### CMD_GET_CALIBRATION (0x04)

**Request:** No payload  
**Response:** `RSP_CALIBRATION` or `RSP_ACK` with `ERR_NOT_CALIBRATED`

**Response Payload Format (if calibrated):**
```
[calibrated_flag: uint8] (always 1)
[throttle_min: uint32, little-endian]
[throttle_max: uint32, little-endian]
[brake_min: uint32, little-endian] (dual throttle only)
[brake_max: uint32, little-endian] (dual throttle only)
[current_value: uint32, little-endian]
```

**Current Value:**
- Dual throttle: Current BLE value (0-255)
- Lite mode: Current ADC value (mapped, 0-255)

---

### CMD_CALIBRATE_THROTTLE (0x05)

**Request:** No payload  
**Response:** `RSP_CALIBRATION` (on success) or `RSP_ACK` with `ERR_CALIBRATION_FAILED`

**Response Payload Format (on success):**
Same as `CMD_GET_CALIBRATION` response.

**Note:** This command triggers a calibration sequence. The device will collect samples over ~6 seconds.

---

### CMD_RESET_ODOMETER (0x06)

**Request:** No payload  
**Response:** `RSP_ACK` with `ERR_OK`

Resets the trip distance/odometer to zero.

---

### CMD_SET_SPEED_UNIT (0x07)

**Request Payload:**
```
[unit: uint8] (0 = km/h, 1 = mph)
```

**Response:** `RSP_ACK` with error code

---

### CMD_SET_BACKLIGHT (0x08)

**Request Payload:**
```
[brightness: uint8] (0-100, percentage)
```

**Response:** `RSP_ACK` with error code

---

### CMD_INVERT_THROTTLE (0x09)

**Request:** No payload  
**Response:** `RSP_ACK` with error code

**Note:** Only supported on Lite mode. On dual throttle, returns `ERR_NOT_SUPPORTED`.

---

### CMD_START_STREAMING (0x10)

**Request Payload (optional):**
```
[rate_hz_lsb: uint8]
[rate_hz_msb: uint8]
```

If no payload provided, defaults to 10 Hz.  
Valid range: 1-100 Hz

**Response:** `RSP_ACK` with `ERR_OK`

Starts streaming real-time data at the specified rate. The device will send `RSP_STREAM_DATA` packets at the configured rate.

---

### CMD_STOP_STREAMING (0x11)

**Request:** No payload  
**Response:** `RSP_ACK` with `ERR_OK`

Stops the real-time data streaming.

---

### CMD_SET_STREAM_RATE (0x12)

**Request Payload:**
```
[rate_hz_lsb: uint8]
[rate_hz_msb: uint8]
```

Valid range: 1-100 Hz

**Response:** `RSP_ACK` with error code

Changes the streaming rate without stopping the stream.

---

## Streaming Data Format

When streaming is enabled, the device sends `RSP_STREAM_DATA` (0x90) packets at the configured rate.

### RSP_STREAM_DATA (0x90) Payload Format

```
[timestamp_ms: uint32, little-endian]
[flags: uint8]
[throttle_raw: uint32, little-endian]
[brake_raw: uint32, little-endian]
[throttle_brake_ble: uint8]
```

**Total Payload Size:** 14 bytes

**Field Descriptions:**

1. **timestamp_ms** (4 bytes, uint32, little-endian)
   - Milliseconds since device boot
   - Used for timing analysis and synchronization

2. **flags** (1 byte, uint8)
   - Bit 0: `ble_connected` (1 = connected to VESC)
   - Bit 1: `calibrated` (1 = throttle is calibrated)
   - Bits 2-7: Reserved (always 0)

3. **throttle_raw** (4 bytes, uint32, little-endian)
   - Raw ADC reading from throttle input
   - Range: 0 to ADC maximum (typically 0-4095 for 12-bit ADC)
   - Negative values are clamped to 0

4. **brake_raw** (4 bytes, uint32, little-endian)
   - Raw ADC reading from brake input
   - Range: 0 to ADC maximum (typically 0-4095 for 12-bit ADC)
   - For Lite mode: Always 0 (brake not available)
   - For Dual Throttle: Actual brake ADC value
   - Negative values are clamped to 0

5. **throttle_brake_ble** (1 byte, uint8)
   - The combined throttle/brake value that is sent to BLE
   - Range: 0-255
   - **128 = Neutral** (VESC_NEUTRAL_VALUE)
   - **0 = Full brake** (brake at minimum)
   - **128-255 = Throttle range** (when brake is at maximum)
   - For Lite mode: Includes throttle inversion if configured
   - For Dual Throttle: Calculated combination of throttle and brake

**Streaming Behavior:**
- Packets are sent automatically at the configured rate (1-100 Hz)
- Each packet is a complete `RSP_STREAM_DATA` response with full packet structure (START + CMD + LENGTH + PAYLOAD + CRC)
- Streaming continues until `CMD_STOP_STREAMING` is received
- If the rate is changed with `CMD_SET_STREAM_RATE`, the new rate takes effect immediately

---

## Response Format Details

### RSP_ACK (0x80)

**Payload Format:**
```
[original_cmd: uint8]
[error_code: uint8]
```

Sent in response to most commands to indicate success or failure.

---

### RSP_ERROR (0x81)

**Payload Format:**
```
[error_code: uint8]
[message_len: uint8] (implicit, length - 1)
[message: string, variable length]
```

Sent when a command fails with a descriptive error message.

---

## Communication Flow

### Example: Ping Command

**Request:**
```
0xAA 0x01 0x00 0x00 [CRC_LSB] [CRC_MSB]
```

**Response:**
```
0xAA 0x80 0x02 0x00 0x01 0x00 [CRC_LSB] [CRC_MSB]
```
- `0x80` = RSP_ACK
- `0x01` = original command (CMD_PING)
- `0x00` = ERR_OK

### Example: Start Streaming at 20 Hz

**Request:**
```
0xAA 0x10 0x02 0x00 0x14 0x00 [CRC_LSB] [CRC_MSB]
```
- `0x10` = CMD_START_STREAMING
- `0x14 0x00` = 20 (little-endian uint16)

**Response:**
```
0xAA 0x80 0x02 0x00 0x10 0x00 [CRC_LSB] [CRC_MSB]
```

**Streaming Data (sent every 50ms at 20 Hz):**
```
0xAA 0x90 0x0E 0x00 [14 bytes payload] [CRC_LSB] [CRC_MSB]
```

### Example: Get Firmware Version

**Request:**
```
0xAA 0x02 0x00 0x00 [CRC_LSB] [CRC_MSB]
```

**Response:**
```
0xAA 0x82 [payload_length] [payload...] [CRC_LSB] [CRC_MSB]
```

---

## Implementation Notes

### Packet Parsing

1. Wait for START_BYTE (0xAA)
2. Read CMD_ID
3. Read LENGTH (2 bytes, little-endian)
4. Validate LENGTH (must be ≤ 512)
5. Read PAYLOAD (LENGTH bytes)
6. Read CRC (2 bytes, little-endian)
7. Calculate CRC over [CMD_ID][LENGTH][PAYLOAD]
8. Compare calculated CRC with received CRC
9. If match, process packet; otherwise, send error or ignore

### Timeouts

- Recommended timeout for command responses: **1 second**
- Streaming packets arrive continuously at the configured rate
- If no response received, resend command (with exponential backoff)

### Error Handling

- If CRC mismatch: Device sends `RSP_ACK` with `ERR_CRC_MISMATCH`
- If unknown command: Device sends `RSP_ACK` with `ERR_UNKNOWN_CMD`
- If invalid payload: Device sends `RSP_ACK` with `ERR_INVALID_PAYLOAD`

### Target-Specific Behavior

- **Lite Mode:** Single throttle, no brake, throttle inversion supported
- **Dual Throttle Mode:** Throttle + brake, no throttle inversion

Commands may return `ERR_NOT_SUPPORTED` if not available on the current target.

---

## Testing Checklist

When implementing the config tool, verify:

- [ ] Ping command works
- [ ] Firmware version retrieval works
- [ ] Config retrieval works (all fields parsed correctly)
- [ ] Calibration commands work
- [ ] Settings commands work (speed unit, backlight, etc.)
- [ ] Streaming starts/stops correctly
- [ ] Streaming data format is correct (14 bytes payload)
- [ ] CRC calculation matches device
- [ ] Error handling works (invalid commands, CRC errors, etc.)
- [ ] Works with both Lite and Dual Throttle targets

---

## Revision History

- **v1.0** (2024-12-XX): Initial protocol specification
  - Updated streaming format to include timestamp, flags, throttle_raw, brake_raw, and throttle_brake_ble

