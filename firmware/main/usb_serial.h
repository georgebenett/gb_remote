#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


// ESP32-S3 specific USB CDC settings
#define USB_CDC_ENABLED 1
#define USB_CDC_USE_PRIMARY_CONSOLE 1
#define USB_CDC_USE_SECONDARY_CONSOLE 0
#define USB_CDC_INIT_DELAY_MS 100
#define USB_CDC_TASK_DELAY_MS 20
#define USB_CDC_BUFFER_SIZE 2048  // Increased for binary protocol

// Binary Protocol Configuration
#define PACKET_START_BYTE       0xAA
#define PACKET_MAX_PAYLOAD_SIZE 512
#define PACKET_HEADER_SIZE      5    // START(1) + CMD(1) + LEN(2) + CRC(2) = 6, but -CRC = 4, +payload = varies
#define PACKET_MIN_SIZE         6    // START + CMD + LEN(2) + CRC(2)

// Binary Protocol Packet Structure:
// [START_BYTE] [CMD_ID] [LENGTH_LSB] [LENGTH_MSB] [PAYLOAD...] [CRC16_LSB] [CRC16_MSB]
// - START_BYTE: 0xAA (synchronization marker)
// - CMD_ID: Command identifier (1 byte)
// - LENGTH: Payload length in bytes (2 bytes, little-endian, does NOT include header/CRC)
// - PAYLOAD: Variable length data (0-512 bytes)
// - CRC16: CRC-16-CCITT over [CMD_ID] [LENGTH] [PAYLOAD] (2 bytes, little-endian)

// Command IDs - Request (Host -> Device)
typedef enum {
    CMD_PING                   = 0x01,  // Ping device
    CMD_GET_FIRMWARE_VERSION   = 0x02,  // Get firmware info
    CMD_GET_CONFIG             = 0x03,  // Get all configuration
    CMD_GET_CALIBRATION        = 0x04,  // Get throttle calibration
    CMD_CALIBRATE_THROTTLE     = 0x05,  // Start throttle calibration
    CMD_RESET_ODOMETER         = 0x06,  // Reset trip distance
    CMD_SET_SPEED_UNIT         = 0x07,  // Set speed unit (payload: 0=kmh, 1=mph)
    CMD_SET_BACKLIGHT          = 0x08,  // Set backlight (payload: uint8 0-100)
    CMD_INVERT_THROTTLE        = 0x09,  // Toggle throttle inversion
    CMD_START_STREAMING        = 0x10,  // Start real-time data streaming
    CMD_STOP_STREAMING         = 0x11,  // Stop real-time data streaming
    CMD_SET_STREAM_RATE        = 0x12,  // Set streaming rate in Hz (payload: uint16)
    CMD_INCREASE_BLE_TRIM      = 0x13,  // Increase BLE output trim offset by 1
    CMD_DECREASE_BLE_TRIM      = 0x14,  // Decrease BLE output trim offset by 1
    CMD_GET_BLE_TRIM           = 0x15,  // Get current BLE trim offset value
    CMD_CHECK_COREDUMP         = 0x16,  // Check if coredump exists
    CMD_GET_COREDUMP           = 0x17,  // Get coredump data (payload: chunk_offset uint16)

    // Response IDs (Device -> Host)
    RSP_ACK                    = 0x80,  // Acknowledge with result code
    RSP_ERROR                  = 0x81,  // Error response
    RSP_FIRMWARE_VERSION       = 0x82,  // Firmware version data
    RSP_CONFIG                 = 0x83,  // Configuration data
    RSP_CALIBRATION            = 0x84,  // Calibration data
    RSP_BLE_TRIM               = 0x85,  // BLE trim offset data
    RSP_COREDUMP_INFO          = 0x86,  // Coredump info (exists flag, size)
    RSP_COREDUMP_CHUNK         = 0x87,  // Coredump data chunk
    RSP_CALIBRATION_PROGRESS   = 0x88,  // Real-time calibration progress update
    RSP_STREAM_DATA            = 0x90,  // Real-time streaming data
} packet_command_t;

// Response/Error codes
typedef enum {
    ERR_OK                     = 0x00,  // Success
    ERR_UNKNOWN_CMD            = 0x01,  // Unknown command
    ERR_INVALID_PAYLOAD        = 0x02,  // Invalid payload length or data
    ERR_CRC_MISMATCH           = 0x03,  // CRC check failed
    ERR_CALIBRATION_FAILED     = 0x04,  // Calibration failed (generic, legacy)
    ERR_SAVE_FAILED            = 0x05,  // Failed to save to NVS
    ERR_NOT_CALIBRATED         = 0x06,  // Device not calibrated
    ERR_OUT_OF_RANGE           = 0x07,  // Parameter out of range
    ERR_NOT_SUPPORTED          = 0x08,  // Command not supported on this target
    ERR_NO_COREDUMP            = 0x09,  // No coredump available
    ERR_READ_FAILED            = 0x0A,  // Failed to read data
    // Calibration-specific failure reasons
    ERR_CAL_THROTTLE_RANGE     = 0x0B,  // Throttle range too small (< 150 ADC units)
    ERR_CAL_THROTTLE_NO_READINGS = 0x0C,  // No valid throttle readings
    ERR_CAL_BRAKE_RANGE        = 0x0D,  // Brake range too small (dual throttle)
    ERR_CAL_BRAKE_NO_READINGS  = 0x0E,  // No valid brake readings (dual throttle)
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
void usb_serial_process_packet(const binary_packet_t* packet);
void usb_serial_send_response(uint8_t cmd_id, const uint8_t* payload, uint16_t length);
void usb_serial_send_ack(uint8_t original_cmd, error_code_t error_code);
void usb_serial_send_error(error_code_t error_code, const char* message);

// Streaming functions
void usb_serial_start_streaming(uint16_t rate_hz);
void usb_serial_stop_streaming(void);
void usb_serial_send_stream_data(void);

// Utility functions
uint16_t calculate_crc16(const uint8_t* data, uint16_t length);
