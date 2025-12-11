#include "usb_serial_handler.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
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
#define MAX_COMMAND_LENGTH 256

// NVS key for backlight brightness
#define NVS_NAMESPACE_LCD "lcd_cfg"
#define NVS_KEY_BACKLIGHT "backlight"

// Response prefixes for config tool parsing
// All command responses use these prefixes so tools can filter out ESP-IDF log messages
#define RSP_PREFIX    "#> "      // Normal response line
#define RSP_OK        "#>OK "    // Success response
#define RSP_ERR       "#>ERR "   // Error response
#define RSP_DATA      "#>DATA "  // Data/value response (for parsing)
#define RSP_PROGRESS  "#>PROG "  // Progress indicator

// Helper macros for formatted output
#define rsp_print(fmt, ...)      printf(RSP_PREFIX fmt "\n", ##__VA_ARGS__)
#define rsp_ok(fmt, ...)         printf(RSP_OK fmt "\n", ##__VA_ARGS__)
#define rsp_err(fmt, ...)        printf(RSP_ERR fmt "\n", ##__VA_ARGS__)
#define rsp_data(key, fmt, ...)  printf(RSP_DATA "%s=" fmt "\n", key, ##__VA_ARGS__)

static TaskHandle_t usb_task_handle = NULL;
static char command_buffer[MAX_COMMAND_LENGTH];
static int command_buffer_pos = 0;

// Command strings
static const char* CMD_STRINGS[] = {
    "reset_odometer",
    "get_config",
    "calibrate_throttle",
    "get_calibration",
    "get_firmware_version",
    "set_speed_unit_kmh",
    "set_speed_unit_mph",
    "invert_throttle",
    "set_backlight",
    "help"
};

// Configuration storage using vesc_config_t structure
static vesc_config_t hand_controller_config;

static void usb_serial_task(void *pvParameters);
static usb_command_t parse_command(const char* input);
static void handle_reset_odometer(const char* command);
static void handle_get_config(const char* command);
static void handle_calibrate_throttle(const char* command);
static void handle_get_calibration(const char* command);
static void handle_get_firmware_version(const char* command);
static void handle_set_speed_unit_kmh(const char* command);
static void handle_set_speed_unit_mph(const char* command);
static void handle_invert_throttle(const char* command);
static void handle_set_backlight(const char* command);
static void handle_help(const char* command);

void usb_serial_init(void)
{
    ESP_LOGI(TAG, "Initializing USB Serial Handler for Hand Controller");
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "USB CDC Enabled: %d", USB_CDC_ENABLED);

    if (!USB_CDC_ENABLED) {
        ESP_LOGW(TAG, "USB CDC not enabled for this target");
        return;
    }

    // Add target-specific initialization delay
    vTaskDelay(pdMS_TO_TICKS(USB_CDC_INIT_DELAY_MS));


    usb_serial_init_esp32s3();

    // Load configuration from NVS
    esp_err_t err = vesc_config_load(&hand_controller_config);
    if (err != ESP_OK) {
        // Initialize with default values if loading fails
        hand_controller_config.motor_poles = 14;
        hand_controller_config.gear_ratio_x1000 = 2200;  // 2.2 gear ratio
        hand_controller_config.wheel_diameter_mm = 115;
        hand_controller_config.speed_unit_mph = false; // Default to km/h
    }

    ESP_LOGI(TAG, "USB Serial Handler initialization complete");
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

    /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
    esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    /* Move the caret to the beginning of the next line on '\n' */
    esp_vfs_dev_usb_serial_jtag_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    /* Enable non-blocking mode on stdin and stdout */
    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);

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
    esp_vfs_usb_serial_jtag_use_driver();

    ESP_LOGI(TAG, "USB Serial JTAG initialized successfully for ESP32-S3");
}

static void usb_serial_task(void *pvParameters)
{
    ESP_LOGI(TAG, "USB Serial task started");

    for (;;) {
        int ch = fgetc(stdin);

        if (ch != EOF && ch != 0xFF) {
            ESP_LOGD(TAG, "Received character: 0x%02X (%c)", ch, (ch >= 32 && ch <= 126) ? ch : '?');

            if (ch == '\r' || ch == '\n') {
                // End of command, process it
                if (command_buffer_pos > 0) {
                    command_buffer[command_buffer_pos] = '\0';
                    usb_serial_process_command(command_buffer);
                    command_buffer_pos = 0;
                }
                printf("\n> ");
            } else if (ch == '\b' || ch == 127) {
                // Backspace
                if (command_buffer_pos > 0) {
                    command_buffer_pos--;
                    printf("\b \b");
                }
            } else if (command_buffer_pos < MAX_COMMAND_LENGTH - 1) {
                // Add character to buffer
                command_buffer[command_buffer_pos++] = ch;
            }
        }

        vTaskDelay(USB_CDC_TASK_DELAY_MS / portTICK_PERIOD_MS);
    }
}

void usb_serial_process_command(const char* command)
{
    usb_command_t cmd = parse_command(command);

    switch (cmd) {
        case CMD_RESET_ODOMETER:
            handle_reset_odometer(command);
            break;
        case CMD_GET_CONFIG:
            handle_get_config(command);
            break;
        case CMD_CALIBRATE_THROTTLE:
            handle_calibrate_throttle(command);
            break;
        case CMD_GET_CALIBRATION:
            handle_get_calibration(command);
            break;
        case CMD_GET_FIRMWARE_VERSION:
            handle_get_firmware_version(command);
            break;
        case CMD_SET_SPEED_UNIT_KMH:
            handle_set_speed_unit_kmh(command);
            break;
        case CMD_SET_SPEED_UNIT_MPH:
            handle_set_speed_unit_mph(command);
            break;
        case CMD_INVERT_THROTTLE:
            handle_invert_throttle(command);
            break;
        case CMD_SET_BACKLIGHT:
            handle_set_backlight(command);
            break;
        case CMD_HELP:
            handle_help(command);
            break;
        case CMD_UNKNOWN:
        default:
            rsp_err("Unknown command: %s", command);
            break;
    }
}

static usb_command_t parse_command(const char* input)
{
    // Skip leading whitespace
    while (*input == ' ' || *input == '\t') input++;

    // Find the first word (command)
    char command[64];
    int i = 0;
    while (input[i] && input[i] != ' ' && input[i] != '\t' && i < 63) {
        command[i] = input[i];
        i++;
    }
    command[i] = '\0';

    // Convert to lowercase for case-insensitive comparison
    for (int j = 0; j < i; j++) {
        if (command[j] >= 'A' && command[j] <= 'Z') {
            command[j] = command[j] + 32;
        }
    }

    // Compare with known commands
    for (int cmd = 0; cmd < sizeof(CMD_STRINGS) / sizeof(CMD_STRINGS[0]); cmd++) {
        if (strcmp(command, CMD_STRINGS[cmd]) == 0) {
            return (usb_command_t)cmd;
        }
    }

    return CMD_UNKNOWN;
}

static void handle_reset_odometer(const char* command)
{
    // Reset the local trip distance display
    ui_reset_trip_distance();
    rsp_ok("Odometer reset");
}

static void handle_get_config(const char* command)
{
    // Reload configuration to ensure we have the latest settings
    esp_err_t err = vesc_config_load(&hand_controller_config);
    if (err != ESP_OK) {
        rsp_err("Failed to load config");
        return;
    }

    // Device info
    rsp_data("model", "%s", TARGET_NAME);
    rsp_data("firmware_version", "%s", APP_VERSION_STRING);
    rsp_data("build_date", "%s %s", BUILD_DATE, BUILD_TIME);

    // User preferences (saved locally)
    rsp_data("speed_unit", "%s", hand_controller_config.speed_unit_mph ? "mph" : "kmh");
#ifdef CONFIG_TARGET_LITE
    rsp_data("throttle_inverted", "%d", hand_controller_config.invert_throttle ? 1 : 0);
#endif

    // Get backlight brightness
    nvs_handle_t nvs_handle;
    uint8_t brightness = LCD_BACKLIGHT_DEFAULT;
    err = nvs_open(NVS_NAMESPACE_LCD, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        nvs_get_u8(nvs_handle, NVS_KEY_BACKLIGHT, &brightness);
        nvs_close(nvs_handle);
    }
    rsp_data("backlight", "%d", brightness);

    // Connection status
    rsp_data("ble_connected", "%d", is_connect ? 1 : 0);

    // Motor config from VESC (read-only, received via BLE)
    rsp_data("vesc_motor_poles", "%d", hand_controller_config.motor_poles);
    rsp_data("vesc_gear_ratio", "%.3f", hand_controller_config.gear_ratio_x1000 / 1000.0f);
    rsp_data("vesc_wheel_diameter_mm", "%d", hand_controller_config.wheel_diameter_mm);

    if (is_connect) {
        int32_t speed = vesc_config_get_speed(&hand_controller_config);
        rsp_data("current_speed", "%ld", speed);
    }

    rsp_ok("config");
}

static void handle_calibrate_throttle(const char* command)
{
    rsp_print("Starting calibration (6 seconds)...");

    // Trigger the throttle calibration - returns true if successful
    bool calibration_succeeded = throttle_calibrate();

    if (calibration_succeeded) {
#ifdef CONFIG_TARGET_DUAL_THROTTLE
        uint32_t throttle_min, throttle_max;
        throttle_get_calibration_values(&throttle_min, &throttle_max);
        rsp_data("throttle_min", "%lu", throttle_min);
        rsp_data("throttle_max", "%lu", throttle_max);

        uint32_t brake_min, brake_max;
        brake_get_calibration_values(&brake_min, &brake_max);
        rsp_data("brake_min", "%lu", brake_min);
        rsp_data("brake_max", "%lu", brake_max);
#else
        uint32_t throttle_min, throttle_max;
        throttle_get_calibration_values(&throttle_min, &throttle_max);
        rsp_data("throttle_min", "%lu", throttle_min);
        rsp_data("throttle_max", "%lu", throttle_max);
#endif
        rsp_ok("calibration");
    } else {
        rsp_data("previous_calibration_active", "%d", throttle_is_calibrated() ? 1 : 0);
        rsp_err("Calibration failed - insufficient movement");
    }
}

static void handle_get_calibration(const char* command)
{
    bool is_calibrated = throttle_is_calibrated();
    rsp_data("calibrated", "%d", is_calibrated ? 1 : 0);

    if (is_calibrated) {
        uint32_t min_val, max_val;
        throttle_get_calibration_values(&min_val, &max_val);
        rsp_data("throttle_min", "%lu", min_val);
        rsp_data("throttle_max", "%lu", max_val);

#ifdef CONFIG_TARGET_DUAL_THROTTLE
        uint32_t brake_min, brake_max;
        brake_get_calibration_values(&brake_min, &brake_max);
        rsp_data("brake_min", "%lu", brake_min);
        rsp_data("brake_max", "%lu", brake_max);
        rsp_data("ble_value", "%d", get_throttle_brake_ble_value());
#else
        rsp_data("ble_value", "%lu", adc_get_latest_value());
#endif
        rsp_ok("calibration");
    } else {
        rsp_err("Not calibrated");
    }
}

static void handle_get_firmware_version(const char* command)
{
    rsp_data("firmware_version", "%s", APP_VERSION_STRING);
    rsp_data("build_date", "%s %s", BUILD_DATE, BUILD_TIME);
    rsp_data("model", "%s", TARGET_NAME);
    rsp_data("idf_version", "%s", esp_get_idf_version());
    rsp_ok("version");
}

static void handle_set_speed_unit_kmh(const char* command)
{
    hand_controller_config.speed_unit_mph = false;
    esp_err_t err = vesc_config_save(&hand_controller_config);
    ui_update_speed_unit(hand_controller_config.speed_unit_mph);
    ui_force_config_reload();

    if (err == ESP_OK) {
        rsp_ok("speed_unit=kmh");
    } else {
        rsp_err("Failed to save speed_unit");
    }
}

static void handle_set_speed_unit_mph(const char* command)
{
    hand_controller_config.speed_unit_mph = true;
    esp_err_t err = vesc_config_save(&hand_controller_config);
    ui_update_speed_unit(hand_controller_config.speed_unit_mph);
    ui_force_config_reload();

    if (err == ESP_OK) {
        rsp_ok("speed_unit=mph");
    } else {
        rsp_err("Failed to save speed_unit");
    }
}

static void handle_invert_throttle(const char* command)
{
#ifdef CONFIG_TARGET_LITE
    hand_controller_config.invert_throttle = !hand_controller_config.invert_throttle;
    esp_err_t err = vesc_config_save(&hand_controller_config);
    ui_force_config_reload();

    if (err == ESP_OK) {
        rsp_ok("throttle_inverted=%d", hand_controller_config.invert_throttle ? 1 : 0);
    } else {
        rsp_err("Failed to save throttle_inverted");
    }
#else
    rsp_err("Command only available for lite target");
#endif
}

static void handle_set_backlight(const char* command)
{
    const char* value_str = strchr(command, ' ');
    if (value_str) {
        value_str++; // Skip the space
        int brightness = atoi(value_str);
        if (brightness >= LCD_BACKLIGHT_MIN && brightness <= LCD_BACKLIGHT_MAX) {
            // Map percentage (0-100) to PWM duty (0-255)
            uint8_t pwm_value = (brightness * 255) / 100;
            lcd_set_backlight(pwm_value);

            // Save to NVS
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open(NVS_NAMESPACE_LCD, NVS_READWRITE, &nvs_handle);
            if (err == ESP_OK) {
                err = nvs_set_u8(nvs_handle, NVS_KEY_BACKLIGHT, (uint8_t)brightness);
                if (err == ESP_OK) {
                    err = nvs_commit(nvs_handle);
                }
                nvs_close(nvs_handle);
            }

            if (err == ESP_OK) {
                rsp_ok("backlight=%d", brightness);
            } else {
                rsp_err("Failed to save backlight");
            }
        } else {
            rsp_err("Invalid backlight (range: %d-%d)", LCD_BACKLIGHT_MIN, LCD_BACKLIGHT_MAX);
        }
    } else {
        rsp_err("Usage: set_backlight <0-100>");
    }
}

static void handle_help(const char* command)
{
    rsp_print("Available commands:");
    rsp_print("  get_config           - Get all configuration");
    rsp_print("  get_firmware_version - Get firmware version");
    rsp_print("  get_calibration      - Get throttle calibration status");
    rsp_print("  calibrate_throttle   - Start throttle calibration");
    rsp_print("  set_speed_unit_kmh   - Set speed unit to km/h");
    rsp_print("  set_speed_unit_mph   - Set speed unit to mi/h");
    rsp_print("  set_backlight <0-100>- Set backlight brightness");
    rsp_print("  reset_odometer       - Reset trip distance");
#ifdef CONFIG_TARGET_LITE
    rsp_print("  invert_throttle      - Toggle throttle inversion");
#endif
    rsp_print("");
    rsp_print("Note: Motor config (poles, gear ratio, wheel size)");
    rsp_print("      is received from VESC via BLE (read-only).");
    rsp_print("");
    rsp_print("Response format:");
    rsp_print("  #>OK   - Success");
    rsp_print("  #>ERR  - Error");
    rsp_print("  #>DATA - Key=value data");
    rsp_ok("help");
}