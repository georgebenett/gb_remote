#include "usb_serial.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "esp_vfs_dev.h"
#include "fcntl.h"
#include "ble.h"
#include "vesc_config.h"
#include "ui_updater.h"
#include "throttle.h"
#include "version.h"
#include "target_config.h"
#include "lcd.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "USB_SERIAL"

// NVS key for backlight brightness
#define NVS_NAMESPACE_LCD "lcd_cfg"
#define NVS_KEY_BACKLIGHT "backlight"

// Binary protocol state machine variables
static TaskHandle_t usb_task_handle = NULL;
static packet_state_t rx_state = STATE_WAIT_START;
static binary_packet_t rx_packet;
static uint16_t rx_payload_index = 0;
static uint16_t rx_crc_calculated = 0;

// Streaming configuration
static stream_config_t stream_config = {
    .enabled = false,
    .rate_hz = 10,  // Default 10Hz
    .last_send_ms = 0
};

// Configuration storage using vesc_config_t structure
static vesc_config_t hand_controller_config;

// Forward declarations - binary protocol handlers
static void usb_serial_task(void *pvParameters);
static void handle_cmd_ping(const binary_packet_t* packet);
static void handle_cmd_get_firmware_version(const binary_packet_t* packet);
static void handle_cmd_get_config(const binary_packet_t* packet);
static void handle_cmd_get_calibration(const binary_packet_t* packet);
static void handle_cmd_calibrate_throttle(const binary_packet_t* packet);
static void handle_cmd_reset_odometer(const binary_packet_t* packet);
static void handle_cmd_set_speed_unit(const binary_packet_t* packet);
static void handle_cmd_set_backlight(const binary_packet_t* packet);
static void handle_cmd_invert_throttle(const binary_packet_t* packet);
static void handle_cmd_start_streaming(const binary_packet_t* packet);
static void handle_cmd_stop_streaming(const binary_packet_t* packet);
static void handle_cmd_set_stream_rate(const binary_packet_t* packet);
static void handle_cmd_increase_ble_trim(const binary_packet_t* packet);
static void handle_cmd_decrease_ble_trim(const binary_packet_t* packet);
static void handle_cmd_get_ble_trim(const binary_packet_t* packet);

// CRC-16-CCITT calculation (polynomial: 0x1021)
uint16_t calculate_crc16(const uint8_t* data, uint16_t length) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

void usb_serial_init(void)
{
    ESP_LOGI(TAG, "Initializing USB Serial Handler");
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "USB CDC Enabled: %d", USB_CDC_ENABLED);

    if (!USB_CDC_ENABLED) {
        ESP_LOGW(TAG, "USB CDC not enabled for this target");
        return;
    }

    // Add target-specific initialization delay
    vTaskDelay(pdMS_TO_TICKS(USB_CDC_INIT_DELAY_MS));

    usb_serial_init_esp32s3();

    // Initialize packet state machine
    rx_state = STATE_WAIT_START;
    rx_payload_index = 0;
    memset(&rx_packet, 0, sizeof(rx_packet));

    // Load configuration from NVS
    esp_err_t err = vesc_config_load(&hand_controller_config);
    if (err != ESP_OK) {
        // Initialize with default values if loading fails
        hand_controller_config.motor_poles = 14;
        hand_controller_config.gear_ratio_x1000 = 2200;  // 2.2 gear ratio
        hand_controller_config.wheel_diameter_mm = 115;
        hand_controller_config.speed_unit_mph = false; // Default to km/h
    }

    ESP_LOGI(TAG, "Binary Protocol v1.0 - Packet-based communication ready");
    ESP_LOGI(TAG, "Max payload size: %d bytes", PACKET_MAX_PAYLOAD_SIZE);
}

void usb_serial_start_task(void)
{
    if (!USB_CDC_ENABLED) {
        ESP_LOGW(TAG, "USB CDC not enabled, skipping task creation");
        return;
    }

    if (usb_task_handle == NULL) {
        xTaskCreate(usb_serial_task, "usb_serial_task", 4096, NULL, 5, &usb_task_handle);
    }
}

void usb_serial_init_esp32s3(void)
{
    ESP_LOGI(TAG, "Setting up USB Serial JTAG interface for ESP32-S3");

    /* Disable buffering on stdin */
    setvbuf(stdin, NULL, _IONBF, 0);

    /* Binary mode - no line ending conversion */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);

    /* Enable non-blocking mode on stdin and stdout */
    fcntl(fileno(stdout), F_SETFL, O_NONBLOCK);
    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

    usb_serial_jtag_driver_config_t usb_serial_jtag_config;
    usb_serial_jtag_config.rx_buffer_size = USB_CDC_BUFFER_SIZE;
    usb_serial_jtag_config.tx_buffer_size = USB_CDC_BUFFER_SIZE;

    esp_err_t ret = ESP_OK;
    /* Install USB-SERIAL-JTAG driver for interrupt-driven reads and writes */
    ret = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB serial driver: %s", esp_err_to_name(ret));
        return;
    }

    /* Tell vfs to use usb-serial-jtag driver */
    usb_serial_jtag_vfs_use_driver();

    ESP_LOGI(TAG, "USB Serial JTAG (Binary Mode) initialized successfully");
}

static void usb_serial_task(void *pvParameters)
{
    ESP_LOGI(TAG, "USB Serial task started");

    uint8_t temp_crc_buffer[PACKET_MAX_PAYLOAD_SIZE + 4]; // CMD + LEN(2) + PAYLOAD

    for (;;) {
        // Handle streaming if enabled
        if (stream_config.enabled) {
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t interval_ms = 1000 / stream_config.rate_hz;
            if ((now_ms - stream_config.last_send_ms) >= interval_ms) {
                usb_serial_send_stream_data();
                stream_config.last_send_ms = now_ms;
            }
        }

        // Process incoming bytes
        int byte = fgetc(stdin);
        if (byte == EOF || byte == 0xFF) {
            vTaskDelay(USB_CDC_TASK_DELAY_MS / portTICK_PERIOD_MS);
            continue;
        }

        uint8_t data = (uint8_t)byte;

        switch (rx_state) {
            case STATE_WAIT_START:
                if (data == PACKET_START_BYTE) {
                    rx_state = STATE_WAIT_CMD;
                    rx_payload_index = 0;
                    memset(&rx_packet, 0, sizeof(rx_packet));
                }
                break;

            case STATE_WAIT_CMD:
                rx_packet.cmd_id = data;
                rx_state = STATE_WAIT_LEN_LSB;
                break;

            case STATE_WAIT_LEN_LSB:
                rx_packet.payload_length = data;
                rx_state = STATE_WAIT_LEN_MSB;
                break;

            case STATE_WAIT_LEN_MSB:
                rx_packet.payload_length |= ((uint16_t)data << 8);

                // Validate payload length
                if (rx_packet.payload_length > PACKET_MAX_PAYLOAD_SIZE) {
                    ESP_LOGW(TAG, "Invalid payload length: %d", rx_packet.payload_length);
                    rx_state = STATE_WAIT_START;
                } else if (rx_packet.payload_length == 0) {
                    // No payload, go directly to CRC
                    rx_state = STATE_WAIT_CRC_LSB;
                } else {
                    rx_state = STATE_WAIT_PAYLOAD;
                    rx_payload_index = 0;
                }
                break;

            case STATE_WAIT_PAYLOAD:
                rx_packet.payload[rx_payload_index++] = data;
                if (rx_payload_index >= rx_packet.payload_length) {
                    rx_state = STATE_WAIT_CRC_LSB;
                }
                break;

            case STATE_WAIT_CRC_LSB:
                rx_packet.crc = data;
                rx_state = STATE_WAIT_CRC_MSB;
                break;

            case STATE_WAIT_CRC_MSB:
                rx_packet.crc |= ((uint16_t)data << 8);

                // Calculate CRC over [CMD][LEN_LSB][LEN_MSB][PAYLOAD]
                temp_crc_buffer[0] = rx_packet.cmd_id;
                temp_crc_buffer[1] = rx_packet.payload_length & 0xFF;
                temp_crc_buffer[2] = (rx_packet.payload_length >> 8) & 0xFF;
                memcpy(&temp_crc_buffer[3], rx_packet.payload, rx_packet.payload_length);

                rx_crc_calculated = calculate_crc16(temp_crc_buffer, 3 + rx_packet.payload_length);

                if (rx_crc_calculated == rx_packet.crc) {
                    // Valid packet received, process it
                    ESP_LOGD(TAG, "Valid packet: CMD=0x%02X, LEN=%d",
                             rx_packet.cmd_id, rx_packet.payload_length);
                    usb_serial_process_packet(&rx_packet);
                } else {
                    ESP_LOGW(TAG, "CRC mismatch: expected 0x%04X, got 0x%04X",
                             rx_crc_calculated, rx_packet.crc);
                    usb_serial_send_ack(rx_packet.cmd_id, ERR_CRC_MISMATCH);
                }

                rx_state = STATE_WAIT_START;
                break;

            default:
                rx_state = STATE_WAIT_START;
                break;
        }

        vTaskDelay(USB_CDC_TASK_DELAY_MS / portTICK_PERIOD_MS);
    }
}

// Send a binary response packet
void usb_serial_send_response(uint8_t cmd_id, const uint8_t* payload, uint16_t length) {
    if (length > PACKET_MAX_PAYLOAD_SIZE) {
        ESP_LOGE(TAG, "Payload too large: %d bytes", length);
        return;
    }

    // Build packet: [START][CMD][LEN_LSB][LEN_MSB][PAYLOAD][CRC_LSB][CRC_MSB]
    uint8_t packet[PACKET_MAX_PAYLOAD_SIZE + 10];
    uint16_t idx = 0;

    packet[idx++] = PACKET_START_BYTE;
    packet[idx++] = cmd_id;
    packet[idx++] = length & 0xFF;
    packet[idx++] = (length >> 8) & 0xFF;

    if (length > 0 && payload != NULL) {
        memcpy(&packet[idx], payload, length);
        idx += length;
    }

    // Calculate CRC over [CMD][LEN][PAYLOAD]
    uint16_t crc = calculate_crc16(&packet[1], 3 + length);
    packet[idx++] = crc & 0xFF;
    packet[idx++] = (crc >> 8) & 0xFF;

    // Send packet
    usb_serial_jtag_write_bytes((const char*)packet, idx, pdMS_TO_TICKS(100));

    ESP_LOGD(TAG, "Sent response: CMD=0x%02X, LEN=%d, CRC=0x%04X", cmd_id, length, crc);
}

// Send ACK/NACK response
void usb_serial_send_ack(uint8_t original_cmd, error_code_t error_code) {
    uint8_t payload[2];
    payload[0] = original_cmd;
    payload[1] = (uint8_t)error_code;
    usb_serial_send_response(RSP_ACK, payload, 2);
}

// Send error response with message
void usb_serial_send_error(error_code_t error_code, const char* message) {
    uint8_t payload[256];
    payload[0] = (uint8_t)error_code;

    uint16_t msg_len = 0;
    if (message != NULL) {
        msg_len = strlen(message);
        if (msg_len > 255) msg_len = 255;
        memcpy(&payload[1], message, msg_len);
    }

    usb_serial_send_response(RSP_ERROR, payload, 1 + msg_len);
}

// Process received binary packet
void usb_serial_process_packet(const binary_packet_t* packet) {
    switch (packet->cmd_id) {
        case CMD_PING:
            handle_cmd_ping(packet);
            break;
        case CMD_GET_FIRMWARE_VERSION:
            handle_cmd_get_firmware_version(packet);
            break;
        case CMD_GET_CONFIG:
            handle_cmd_get_config(packet);
            break;
        case CMD_GET_CALIBRATION:
            handle_cmd_get_calibration(packet);
            break;
        case CMD_CALIBRATE_THROTTLE:
            handle_cmd_calibrate_throttle(packet);
            break;
        case CMD_RESET_ODOMETER:
            handle_cmd_reset_odometer(packet);
            break;
        case CMD_SET_SPEED_UNIT:
            handle_cmd_set_speed_unit(packet);
            break;
        case CMD_SET_BACKLIGHT:
            handle_cmd_set_backlight(packet);
            break;
        case CMD_INVERT_THROTTLE:
            handle_cmd_invert_throttle(packet);
            break;
        case CMD_START_STREAMING:
            handle_cmd_start_streaming(packet);
            break;
        case CMD_STOP_STREAMING:
            handle_cmd_stop_streaming(packet);
            break;
        case CMD_SET_STREAM_RATE:
            handle_cmd_set_stream_rate(packet);
            break;
        case CMD_INCREASE_BLE_TRIM:
            handle_cmd_increase_ble_trim(packet);
            break;
        case CMD_DECREASE_BLE_TRIM:
            handle_cmd_decrease_ble_trim(packet);
            break;
        case CMD_GET_BLE_TRIM:
            handle_cmd_get_ble_trim(packet);
            break;
        default:
            ESP_LOGW(TAG, "Unknown command: 0x%02X", packet->cmd_id);
            usb_serial_send_ack(packet->cmd_id, ERR_UNKNOWN_CMD);
            break;
    }
}

// ========== BINARY PROTOCOL COMMAND HANDLERS ==========

static void handle_cmd_ping(const binary_packet_t* packet) {
    // Simple ping response - just ACK
    usb_serial_send_ack(CMD_PING, ERR_OK);
}

static void handle_cmd_get_firmware_version(const binary_packet_t* packet) {
    uint8_t payload[256];
    uint16_t idx = 0;

    // Parse version string to get individual components
    uint8_t major = 0, minor = 0, patch = 0;
    sscanf(FW_VERSION, "%hhu.%hhu.%hhu", &major, &minor, &patch);
    payload[idx++] = major;
    payload[idx++] = minor;
    payload[idx++] = patch;

    // Build date string
    char build_str[64];
    snprintf(build_str, sizeof(build_str), "%s %s", BUILD_DATE, BUILD_TIME);
    uint8_t build_len = strlen(build_str);
    payload[idx++] = build_len;
    memcpy(&payload[idx], build_str, build_len);
    idx += build_len;

    // Model name
    uint8_t model_len = strlen(TARGET_NAME);
    payload[idx++] = model_len;
    memcpy(&payload[idx], TARGET_NAME, model_len);
    idx += model_len;

    // IDF version
    const char* idf_ver = esp_get_idf_version();
    uint8_t idf_len = strlen(idf_ver);
    payload[idx++] = idf_len;
    memcpy(&payload[idx], idf_ver, idf_len);
    idx += idf_len;

    usb_serial_send_response(RSP_FIRMWARE_VERSION, payload, idx);
}

static void handle_cmd_get_config(const binary_packet_t* packet) {
    // Reload configuration to ensure we have the latest settings
    esp_err_t err = vesc_config_load(&hand_controller_config);
    if (err != ESP_OK) {
        usb_serial_send_ack(CMD_GET_CONFIG, ERR_SAVE_FAILED);
        return;
    }

    uint8_t payload[256];
    uint16_t idx = 0;

    uint8_t flags = 0;
    if (hand_controller_config.speed_unit_mph) flags |= 0x01;
#ifdef CONFIG_TARGET_LITE
    if (hand_controller_config.invert_throttle) flags |= 0x02;
#endif
    if (is_connect) flags |= 0x04;
    if (throttle_is_calibrated()) flags |= 0x08;
    payload[idx++] = flags;

    // Get backlight brightness
    nvs_handle_t nvs_handle;
    uint8_t brightness = LCD_BACKLIGHT_DEFAULT;
    err = nvs_open(NVS_NAMESPACE_LCD, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        nvs_get_u8(nvs_handle, NVS_KEY_BACKLIGHT, &brightness);
        nvs_close(nvs_handle);
    }
    payload[idx++] = brightness;

    // Motor configuration (2 bytes each, little-endian)
    payload[idx++] = hand_controller_config.motor_poles;
    payload[idx++] = (hand_controller_config.gear_ratio_x1000 >> 0) & 0xFF;
    payload[idx++] = (hand_controller_config.gear_ratio_x1000 >> 8) & 0xFF;
    payload[idx++] = (hand_controller_config.wheel_diameter_mm >> 0) & 0xFF;
    payload[idx++] = (hand_controller_config.wheel_diameter_mm >> 8) & 0xFF;

    // Current speed (4 bytes, little-endian, signed)
    int32_t speed = 0;
    if (is_connect) {
        speed = vesc_config_get_speed(&hand_controller_config);
    }
    payload[idx++] = (speed >> 0) & 0xFF;
    payload[idx++] = (speed >> 8) & 0xFF;
    payload[idx++] = (speed >> 16) & 0xFF;
    payload[idx++] = (speed >> 24) & 0xFF;

    // BLE trim offset (1 byte, signed)
    int8_t trim_offset = ble_get_trim_offset();
    payload[idx++] = (uint8_t)trim_offset;  // Cast to uint8_t for transmission (will be interpreted as int8_t on receive)

    // Throttle calibration (4 bytes each, little-endian)
    if (throttle_is_calibrated()) {
        uint32_t min_val, max_val;
        throttle_get_calibration_values(&min_val, &max_val);

        payload[idx++] = (min_val >> 0) & 0xFF;
        payload[idx++] = (min_val >> 8) & 0xFF;
        payload[idx++] = (min_val >> 16) & 0xFF;
        payload[idx++] = (min_val >> 24) & 0xFF;

        payload[idx++] = (max_val >> 0) & 0xFF;
        payload[idx++] = (max_val >> 8) & 0xFF;
        payload[idx++] = (max_val >> 16) & 0xFF;
        payload[idx++] = (max_val >> 24) & 0xFF;

#ifdef CONFIG_TARGET_DUAL_THROTTLE
        uint32_t brake_min, brake_max;
        brake_get_calibration_values(&brake_min, &brake_max);

        payload[idx++] = (brake_min >> 0) & 0xFF;
        payload[idx++] = (brake_min >> 8) & 0xFF;
        payload[idx++] = (brake_min >> 16) & 0xFF;
        payload[idx++] = (brake_min >> 24) & 0xFF;

        payload[idx++] = (brake_max >> 0) & 0xFF;
        payload[idx++] = (brake_max >> 8) & 0xFF;
        payload[idx++] = (brake_max >> 16) & 0xFF;
        payload[idx++] = (brake_max >> 24) & 0xFF;
#endif
    }

    usb_serial_send_response(RSP_CONFIG, payload, idx);
}

static void handle_cmd_reset_odometer(const binary_packet_t* packet) {
    ui_reset_trip_distance();
    usb_serial_send_ack(CMD_RESET_ODOMETER, ERR_OK);
}

static void handle_cmd_calibrate_throttle(const binary_packet_t* packet) {
    ESP_LOGI(TAG, "Starting throttle calibration...");

    // Trigger the throttle calibration - returns true if successful
    bool calibration_succeeded = throttle_calibrate();

    if (calibration_succeeded) {
        // Build calibration response
        uint8_t payload[32];
        uint16_t idx = 0;

        payload[idx++] = 1; // Success flag

        uint32_t throttle_min, throttle_max;
        throttle_get_calibration_values(&throttle_min, &throttle_max);

        payload[idx++] = (throttle_min >> 0) & 0xFF;
        payload[idx++] = (throttle_min >> 8) & 0xFF;
        payload[idx++] = (throttle_min >> 16) & 0xFF;
        payload[idx++] = (throttle_min >> 24) & 0xFF;

        payload[idx++] = (throttle_max >> 0) & 0xFF;
        payload[idx++] = (throttle_max >> 8) & 0xFF;
        payload[idx++] = (throttle_max >> 16) & 0xFF;
        payload[idx++] = (throttle_max >> 24) & 0xFF;

#ifdef CONFIG_TARGET_DUAL_THROTTLE
        uint32_t brake_min, brake_max;
        brake_get_calibration_values(&brake_min, &brake_max);

        payload[idx++] = (brake_min >> 0) & 0xFF;
        payload[idx++] = (brake_min >> 8) & 0xFF;
        payload[idx++] = (brake_min >> 16) & 0xFF;
        payload[idx++] = (brake_min >> 24) & 0xFF;

        payload[idx++] = (brake_max >> 0) & 0xFF;
        payload[idx++] = (brake_max >> 8) & 0xFF;
        payload[idx++] = (brake_max >> 16) & 0xFF;
        payload[idx++] = (brake_max >> 24) & 0xFF;
#endif

        usb_serial_send_response(RSP_CALIBRATION, payload, idx);
    } else {
        usb_serial_send_ack(CMD_CALIBRATE_THROTTLE, ERR_CALIBRATION_FAILED);
    }
}

static void handle_cmd_get_calibration(const binary_packet_t* packet) {
    bool is_calibrated = throttle_is_calibrated();

    if (!is_calibrated) {
        usb_serial_send_ack(CMD_GET_CALIBRATION, ERR_NOT_CALIBRATED);
        return;
    }

    // Build calibration response
    uint8_t payload[64];
    uint16_t idx = 0;

    payload[idx++] = 1; // Calibrated flag

    uint32_t throttle_min, throttle_max;
    throttle_get_calibration_values(&throttle_min, &throttle_max);

    payload[idx++] = (throttle_min >> 0) & 0xFF;
    payload[idx++] = (throttle_min >> 8) & 0xFF;
    payload[idx++] = (throttle_min >> 16) & 0xFF;
    payload[idx++] = (throttle_min >> 24) & 0xFF;

    payload[idx++] = (throttle_max >> 0) & 0xFF;
    payload[idx++] = (throttle_max >> 8) & 0xFF;
    payload[idx++] = (throttle_max >> 16) & 0xFF;
    payload[idx++] = (throttle_max >> 24) & 0xFF;

#ifdef CONFIG_TARGET_DUAL_THROTTLE
    uint32_t brake_min, brake_max;
    brake_get_calibration_values(&brake_min, &brake_max);

    payload[idx++] = (brake_min >> 0) & 0xFF;
    payload[idx++] = (brake_min >> 8) & 0xFF;
    payload[idx++] = (brake_min >> 16) & 0xFF;
    payload[idx++] = (brake_min >> 24) & 0xFF;

    payload[idx++] = (brake_max >> 0) & 0xFF;
    payload[idx++] = (brake_max >> 8) & 0xFF;
    payload[idx++] = (brake_max >> 16) & 0xFF;
    payload[idx++] = (brake_max >> 24) & 0xFF;

    // Current BLE value
    int32_t ble_val = get_throttle_brake_ble_value();
    payload[idx++] = (ble_val >> 0) & 0xFF;
    payload[idx++] = (ble_val >> 8) & 0xFF;
    payload[idx++] = (ble_val >> 16) & 0xFF;
    payload[idx++] = (ble_val >> 24) & 0xFF;
#else
    // Current ADC value
    uint32_t adc_val = adc_get_latest_value();
    payload[idx++] = (adc_val >> 0) & 0xFF;
    payload[idx++] = (adc_val >> 8) & 0xFF;
    payload[idx++] = (adc_val >> 16) & 0xFF;
    payload[idx++] = (adc_val >> 24) & 0xFF;
#endif

    usb_serial_send_response(RSP_CALIBRATION, payload, idx);
}

static void handle_cmd_set_speed_unit(const binary_packet_t* packet) {
    // Payload: [0=kmh, 1=mph]
    if (packet->payload_length != 1) {
        usb_serial_send_ack(CMD_SET_SPEED_UNIT, ERR_INVALID_PAYLOAD);
        return;
    }

    uint8_t unit = packet->payload[0];
    if (unit > 1) {
        usb_serial_send_ack(CMD_SET_SPEED_UNIT, ERR_OUT_OF_RANGE);
        return;
    }

    hand_controller_config.speed_unit_mph = (unit == 1);
    esp_err_t err = vesc_config_save(&hand_controller_config);
    ui_update_speed_unit(hand_controller_config.speed_unit_mph);
    ui_force_config_reload();

    if (err == ESP_OK) {
        usb_serial_send_ack(CMD_SET_SPEED_UNIT, ERR_OK);
    } else {
        usb_serial_send_ack(CMD_SET_SPEED_UNIT, ERR_SAVE_FAILED);
    }
}

static void handle_cmd_set_backlight(const binary_packet_t* packet) {
    // Payload: [brightness 0-100]
    if (packet->payload_length != 1) {
        usb_serial_send_ack(CMD_SET_BACKLIGHT, ERR_INVALID_PAYLOAD);
        return;
    }

    uint8_t brightness = packet->payload[0];
    if (brightness > LCD_BACKLIGHT_MAX) {
        usb_serial_send_ack(CMD_SET_BACKLIGHT, ERR_OUT_OF_RANGE);
        return;
    }

    // Map percentage (0-100) to PWM duty (0-255)
    uint8_t pwm_value = (brightness * 255) / 100;
    lcd_set_backlight(pwm_value);

    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_LCD, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_KEY_BACKLIGHT, brightness);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }

    if (err == ESP_OK) {
        usb_serial_send_ack(CMD_SET_BACKLIGHT, ERR_OK);
    } else {
        usb_serial_send_ack(CMD_SET_BACKLIGHT, ERR_SAVE_FAILED);
    }
}

static void handle_cmd_invert_throttle(const binary_packet_t* packet) {
#ifdef CONFIG_TARGET_LITE
    hand_controller_config.invert_throttle = !hand_controller_config.invert_throttle;
    esp_err_t err = vesc_config_save(&hand_controller_config);
    ui_force_config_reload();

    if (err == ESP_OK) {
        usb_serial_send_ack(CMD_INVERT_THROTTLE, ERR_OK);
    } else {
        usb_serial_send_ack(CMD_INVERT_THROTTLE, ERR_SAVE_FAILED);
    }
#else
    usb_serial_send_ack(CMD_INVERT_THROTTLE, ERR_NOT_SUPPORTED);
#endif
}

// ========== STREAMING FUNCTIONS ==========

// Apply trim offset with range compensation to maintain full 0-255 span
static uint8_t apply_trim_with_compensation(uint8_t adc_value, int8_t trim_offset) {
    int32_t new_center = VESC_NEUTRAL_VALUE + trim_offset;

    // Clamp new center to valid range
    if (new_center < 0) new_center = 0;
    if (new_center > 255) new_center = 255;

    uint8_t final_value;

    if (adc_value <= VESC_NEUTRAL_VALUE) {
        // Scale lower half (0-128) to map to 0 to new_center, preserving proportion
        if (VESC_NEUTRAL_VALUE > 0) {
            int32_t scaled = (int32_t)((float)adc_value * (float)new_center / (float)VESC_NEUTRAL_VALUE + 0.5f);
            final_value = (uint8_t)(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
        } else {
            final_value = 0;
        }
    } else {
        // Scale upper half (129-255) to map from new_center to 255, preserving proportion
        int32_t upper_output_range = 255 - new_center;
        int32_t upper_input_range = 255 - VESC_NEUTRAL_VALUE;
        if (upper_input_range > 0) {
            int32_t scaled = new_center + (int32_t)((float)(adc_value - VESC_NEUTRAL_VALUE) * (float)upper_output_range / (float)upper_input_range + 0.5f);
            final_value = (uint8_t)(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
        } else {
            final_value = (uint8_t)new_center;
        }
    }

    return final_value;
}

static void handle_cmd_start_streaming(const binary_packet_t* packet) {
    uint16_t rate_hz = 10; // Default 10Hz

    if (packet->payload_length >= 2) {
        // Payload: [rate_hz_lsb][rate_hz_msb]
        rate_hz = packet->payload[0] | (packet->payload[1] << 8);
    }

    // Validate rate (1Hz - 100Hz)
    if (rate_hz < 1) rate_hz = 1;
    if (rate_hz > 100) rate_hz = 100;

    usb_serial_start_streaming(rate_hz);

    ESP_LOGI(TAG, "Streaming started at %d Hz", rate_hz);
    usb_serial_send_ack(CMD_START_STREAMING, ERR_OK);
}

static void handle_cmd_stop_streaming(const binary_packet_t* packet) {
    usb_serial_stop_streaming();
    ESP_LOGI(TAG, "Streaming stopped");
    usb_serial_send_ack(CMD_STOP_STREAMING, ERR_OK);
}

static void handle_cmd_set_stream_rate(const binary_packet_t* packet) {
    if (packet->payload_length != 2) {
        usb_serial_send_ack(CMD_SET_STREAM_RATE, ERR_INVALID_PAYLOAD);
        return;
    }

    uint16_t rate_hz = packet->payload[0] | (packet->payload[1] << 8);

    // Validate rate (1Hz - 100Hz)
    if (rate_hz < 1 || rate_hz > 100) {
        usb_serial_send_ack(CMD_SET_STREAM_RATE, ERR_OUT_OF_RANGE);
        return;
    }

    stream_config.rate_hz = rate_hz;

    ESP_LOGI(TAG, "Streaming rate changed to %d Hz", rate_hz);
    usb_serial_send_ack(CMD_SET_STREAM_RATE, ERR_OK);
}

void usb_serial_start_streaming(uint16_t rate_hz) {
    stream_config.enabled = true;
    stream_config.rate_hz = rate_hz;
    stream_config.last_send_ms = 0;
}

void usb_serial_stop_streaming(void) {
    stream_config.enabled = false;
}

void usb_serial_send_stream_data(void) {
    if (!stream_config.enabled) {
        return;
    }

    uint8_t payload[32];
    uint16_t idx = 0;

    uint32_t timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    payload[idx++] = (timestamp_ms >> 0) & 0xFF;
    payload[idx++] = (timestamp_ms >> 8) & 0xFF;
    payload[idx++] = (timestamp_ms >> 16) & 0xFF;
    payload[idx++] = (timestamp_ms >> 24) & 0xFF;

    uint8_t flags = 0;
    if (is_connect) flags |= 0x01;
    if (throttle_is_calibrated()) flags |= 0x02;
    payload[idx++] = flags;

    // Throttle raw value (4 bytes, signed)
    int32_t throttle_raw = throttle_read_value();
    if (throttle_raw < 0) throttle_raw = 0;  // Clamp negative values to 0
    uint32_t throttle_raw_uint = (uint32_t)throttle_raw;
    payload[idx++] = (throttle_raw_uint >> 0) & 0xFF;
    payload[idx++] = (throttle_raw_uint >> 8) & 0xFF;
    payload[idx++] = (throttle_raw_uint >> 16) & 0xFF;
    payload[idx++] = (throttle_raw_uint >> 24) & 0xFF;

    // Brake raw value (4 bytes, signed)
    uint32_t brake_raw_uint = 0;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    int32_t brake_raw = brake_read_value();
    if (brake_raw < 0) brake_raw = 0;  // Clamp negative values to 0
    brake_raw_uint = (uint32_t)brake_raw;
#endif
    payload[idx++] = (brake_raw_uint >> 0) & 0xFF;
    payload[idx++] = (brake_raw_uint >> 8) & 0xFF;
    payload[idx++] = (brake_raw_uint >> 16) & 0xFF;
    payload[idx++] = (brake_raw_uint >> 24) & 0xFF;

    // Throttle/brake combination value sent to BLE (1 byte, 0-255)
    uint8_t throttle_brake_ble = 0;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    throttle_brake_ble = get_throttle_brake_ble_value();
    // Apply trim offset with range compensation to match what's actually sent via BLE
    throttle_brake_ble = apply_trim_with_compensation(throttle_brake_ble, ble_get_trim_offset());
#elif defined(CONFIG_TARGET_LITE)
    // Get the value that would be sent to BLE (with inversion applied if configured)
    uint32_t adc_value = adc_get_latest_value();
    if (throttle_should_use_neutral()) {
        throttle_brake_ble = VESC_NEUTRAL_VALUE;
    } else {
        throttle_brake_ble = (uint8_t)adc_value;

        // Apply throttle inversion if configured
        vesc_config_t config;
        esp_err_t err = vesc_config_load(&config);
        if (err == ESP_OK && config.invert_throttle) {
            throttle_brake_ble = 255 - throttle_brake_ble;
        }
    }

    // Apply trim offset with range compensation to match what's actually sent via BLE (for lite mode)
    throttle_brake_ble = apply_trim_with_compensation(throttle_brake_ble, ble_get_trim_offset());
#endif

    payload[idx++] = throttle_brake_ble;

    usb_serial_send_response(RSP_STREAM_DATA, payload, idx);
}

static void handle_cmd_increase_ble_trim(const binary_packet_t* packet) {
    esp_err_t err = ble_increase_trim_offset();
    if (err == ESP_OK) {
        usb_serial_send_ack(CMD_INCREASE_BLE_TRIM, ERR_OK);
    } else if (err == ESP_ERR_INVALID_ARG) {
        usb_serial_send_ack(CMD_INCREASE_BLE_TRIM, ERR_OUT_OF_RANGE);
    } else {
        usb_serial_send_ack(CMD_INCREASE_BLE_TRIM, ERR_SAVE_FAILED);
    }
}

static void handle_cmd_decrease_ble_trim(const binary_packet_t* packet) {
    esp_err_t err = ble_decrease_trim_offset();
    if (err == ESP_OK) {
        usb_serial_send_ack(CMD_DECREASE_BLE_TRIM, ERR_OK);
    } else if (err == ESP_ERR_INVALID_ARG) {
        usb_serial_send_ack(CMD_DECREASE_BLE_TRIM, ERR_OUT_OF_RANGE);
    } else {
        usb_serial_send_ack(CMD_DECREASE_BLE_TRIM, ERR_SAVE_FAILED);
    }
}

static void handle_cmd_get_ble_trim(const binary_packet_t* packet) {
    int8_t trim_offset = ble_get_trim_offset();
    uint8_t payload[1];
    payload[0] = (uint8_t)trim_offset;  // Cast to uint8_t for transmission (interpret as int8_t on receive)
    usb_serial_send_response(RSP_BLE_TRIM, payload, 1);
}