#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>

// ESP32-S3 specific USB CDC settings
#define USB_CDC_ENABLED 1
#define USB_CDC_USE_PRIMARY_CONSOLE 1
#define USB_CDC_USE_SECONDARY_CONSOLE 0
#define USB_CDC_INIT_DELAY_MS 100
#define USB_CDC_TASK_DELAY_MS 20
#define USB_CDC_BUFFER_SIZE 2048 // Increased for binary protocol

// Binary Protocol Configuration
#define PACKET_START_BYTE 0xAA
#define PACKET_MAX_PAYLOAD_SIZE 512
#define PACKET_HEADER_SIZE                                                     \
  5 // START(1) + CMD(1) + LEN(2) + CRC(2) = 6, but -CRC = 4, +payload = varies
#define PACKET_MIN_SIZE 6 // START + CMD + LEN(2) + CRC(2)

// Binary Protocol Packet Structure:
// [START_BYTE] [CMD_ID] [LENGTH_LSB] [LENGTH_MSB] [PAYLOAD...] [CRC16_LSB]
// [CRC16_MSB]
// - START_BYTE: 0xAA (synchronization marker)
// - CMD_ID: Command identifier (1 byte)
// - LENGTH: Payload length in bytes (2 bytes, little-endian, does NOT include
// header/CRC)
// - PAYLOAD: Variable length data (0-512 bytes)
// - CRC16: CRC-16-CCITT over [CMD_ID] [LENGTH] [PAYLOAD] (2 bytes,
// little-endian)

// Command IDs - Request (Host -> Device)
typedef enum {
  CMD_PING = 0x01,                 // Ping device
  CMD_GET_FIRMWARE_VERSION = 0x02, // Get firmware info
  CMD_GET_CONFIG = 0x03,           // Get all configuration
  CMD_GET_CALIBRATION = 0x04,      // Get throttle calibration
  CMD_CALIBRATE_THROTTLE = 0x05,   // Start throttle calibration
  CMD_RESET_ODOMETER = 0x06,       // Reset trip distance
  CMD_SET_SPEED_UNIT = 0x07,       // Set speed unit (payload: 0=kmh, 1=mph)
  CMD_SET_BACKLIGHT = 0x08,        // Set backlight (payload: uint8 0-100)
  CMD_INVERT_THROTTLE = 0x09,      // Toggle throttle inversion
  CMD_START_STREAMING = 0x10,      // Start real-time data streaming
  CMD_STOP_STREAMING = 0x11,       // Stop real-time data streaming
  CMD_SET_STREAM_RATE = 0x12,      // Set streaming rate in Hz (payload: uint16)
  CMD_INCREASE_BLE_TRIM = 0x13,    // Increase BLE output trim offset by 1
  CMD_DECREASE_BLE_TRIM = 0x14,    // Decrease BLE output trim offset by 1
  CMD_GET_BLE_TRIM = 0x15,         // Get current BLE trim offset value
  CMD_CHECK_COREDUMP = 0x16,       // Check if coredump exists
  CMD_GET_COREDUMP = 0x17,   // Get coredump data (payload: chunk_offset uint16)
  CMD_BOOT_FULL_MODE = 0x1E, // Boot into full mode from charging screen
  CMD_SET_HAPTIC_INTENSITY =
      0x1F, // Set haptic feedback intensity (payload: uint8 0-100)
  // 0x20-0x23 are reserved: lite_v1 legacy motor config commands
  CMD_TOGGLE_DUAL_CONNECTION = 0x24, // Toggle dual receiver connection mode
  CMD_SET_BATTERY_CELLS = 0x25,      // Set skate pack series count and cell
                                // chemistry (payload: [cells 0=unset or 5-20]
                                // or [cells, cell_type])
  CMD_TOGGLE_SMART_REVERSE = 0x26, // Toggle VESC-style smart reverse
  CMD_TOGGLE_ASSIST_PUSH = 0x27,   // Toggle assistive push (endless mode)
  CMD_SET_ASSIST_PARAMS =
      0x28, // Assist tuning (payload: [strength%, decay rpm/s])
  CMD_GET_ASSIST_PARAMS = 0x29,  // Read assist tuning back
  CMD_TOGGLE_NO_REVERSE = 0x2A,  // Toggle reverse lockout (brake only)
  CMD_SET_THROTTLE_CURVE = 0x2B, // Curve (payload: [mode, acc, brake], the two
                                 // curves signed tenths)
  CMD_GET_THROTTLE_CURVE = 0x2C, // Read the curve back
  CMD_SET_RIDE_SETTINGS = 0x2D,  // Payload: [ride profile 0-3 (Beginner..
                                 // Rocket), speed limit mode 0-4 (Off, 20/25
                                 // km/h, 20/25 mph)]
  CMD_GET_ALL = 0x2F,            // Everything the config tool shows, in one
                                 // tagged answer (see ALL_TAG_*)

  // Response IDs (Device -> Host)
  RSP_ACK = 0x80,                  // Acknowledge with result code
  RSP_ERROR = 0x81,                // Error response
  RSP_FIRMWARE_VERSION = 0x82,     // Firmware version data
  RSP_CONFIG = 0x83,               // Configuration data
  RSP_CALIBRATION = 0x84,          // Calibration data
  RSP_BLE_TRIM = 0x85,             // BLE trim offset data
  RSP_COREDUMP_INFO = 0x86,        // Coredump info (exists flag, size)
  RSP_COREDUMP_CHUNK = 0x87,       // Coredump data chunk
  RSP_CALIBRATION_PROGRESS = 0x88, // Real-time calibration progress update
  RSP_STREAM_DATA = 0x90,          // Real-time streaming data
  RSP_ASSIST_PARAMS = 0x91,        // Assist tuning: [strength%, decay rpm/s]
  RSP_THROTTLE_CURVE = 0x92,       // Curve: [mode, acc, brake]
  RSP_ALL = 0x94,                  // Tagged config snapshot, see ALL_TAG_*
} packet_command_t;

// GET_ALL answer: RSP_ALL = [format version][entries...], each entry
// [tag][len][value, little-endian]. A tool skips tags it does not know and a
// device omits what it does not have, so fields can be added without breaking
// either side. Format version 1.
#define ALL_FORMAT_VERSION 1
#define ALL_TAG_FLAGS                                                          \
  0x01                         // 1: bit0 mph, bit1 inverted, bit2 receiver
                               // connected, bit3 calibrated, bit4 dual
                               // connection, bit5 smart reverse, bit6 assist
                               // push, bit7 reverse disabled
#define ALL_TAG_BACKLIGHT 0x02 // 1: percent
#define ALL_TAG_MOTOR 0x03     // 5: poles, gear ratio x1000 u16, wheel mm u16
#define ALL_TAG_SPEED 0x04     // 4: current speed, i32
#define ALL_TAG_BLE_TRIM 0x05  // 1: i8
#define ALL_TAG_HAPTIC 0x06    // 1: percent
#define ALL_TAG_BATTERY 0x07   // 2: series cells (0 unset), cell type
#define ALL_TAG_CALIBRATION                                                    \
  0x08                      // 9 or 17: [1, throttle min u32, max u32,
                            // (brake min u32, max u32)]; omitted if
                            // not calibrated
#define ALL_TAG_ASSIST 0x09 // 2: strength %, decay rpm/s
#define ALL_TAG_CURVE 0x0A  // 3: mode, acc, brake (signed tenths)
#define ALL_TAG_RIDE 0x0B   // 2: ride profile 0-3, speed limit mode 0-4

// Response/Error codes
typedef enum {
  ERR_OK = 0x00,                 // Success
  ERR_UNKNOWN_CMD = 0x01,        // Unknown command
  ERR_INVALID_PAYLOAD = 0x02,    // Invalid payload length or data
  ERR_CRC_MISMATCH = 0x03,       // CRC check failed
  ERR_CALIBRATION_FAILED = 0x04, // Calibration failed (generic, legacy)
  ERR_SAVE_FAILED = 0x05,        // Failed to save to NVS
  ERR_NOT_CALIBRATED = 0x06,     // Device not calibrated
  ERR_OUT_OF_RANGE = 0x07,       // Parameter out of range
  ERR_NOT_SUPPORTED = 0x08,      // Command not supported on this target
  ERR_NO_COREDUMP = 0x09,        // No coredump available
  ERR_READ_FAILED = 0x0A,        // Failed to read data
  // Calibration-specific failure reasons
  ERR_CAL_THROTTLE_RANGE = 0x0B, // Throttle range too small (< 150 ADC units)
  ERR_CAL_THROTTLE_NO_READINGS = 0x0C, // No valid throttle readings
  ERR_CAL_BRAKE_RANGE = 0x0D,          // Brake range too small (dual throttle)
  ERR_CAL_BRAKE_NO_READINGS = 0x0E, // No valid brake readings (dual throttle)
  ERR_NO_RECEIVER = 0x0F,           // BLE link to receiver is not connected
} error_code_t;

// Packet state machine
typedef enum {
  STATE_WAIT_START,
  STATE_WAIT_CMD,
  STATE_WAIT_LEN_LSB,
  STATE_WAIT_LEN_MSB,
  STATE_WAIT_PAYLOAD,
  STATE_WAIT_CRC_LSB,
  STATE_WAIT_CRC_MSB,
} packet_state_t;

// Packet structure for internal use
typedef struct {
  uint8_t cmd_id;
  uint16_t payload_length;
  uint8_t payload[PACKET_MAX_PAYLOAD_SIZE];
  uint16_t crc;
} binary_packet_t;

// Streaming configuration
typedef struct {
  bool enabled;
  uint16_t rate_hz;
  uint32_t last_send_ms;
} stream_config_t;

// Function prototypes
void usb_serial_init(void);
void usb_serial_start_task(void);
void usb_serial_init_esp32s3(void);

// Binary protocol functions
void usb_serial_process_packet(const binary_packet_t *packet);
void usb_serial_send_response(uint8_t cmd_id, const uint8_t *payload,
                              uint16_t length);
void usb_serial_send_ack(uint8_t original_cmd, error_code_t error_code);

// Streaming functions
void usb_serial_start_streaming(uint16_t rate_hz);
void usb_serial_stop_streaming(void);
void usb_serial_send_stream_data(void);

// Utility functions
uint16_t calculate_crc16(const uint8_t *data, uint16_t length);
