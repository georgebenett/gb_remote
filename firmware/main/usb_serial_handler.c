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

static TaskHandle_t usb_task_handle = NULL;
static char command_buffer[MAX_COMMAND_LENGTH];
static int command_buffer_pos = 0;

// Command strings
static const char* CMD_STRINGS[] = {
    "reset_odometer",
    "set_gear_ratio",
    "set_wheel_size",
    "set_motor_poles",
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
static void handle_set_gear_ratio(const char* command);
static void handle_set_wheel_size(const char* command);
static void handle_set_motor_poles(const char* command);
static void handle_get_config(const char* command);
static void handle_calibrate_throttle(const char* command);
static void handle_get_calibration(const char* command);
static void handle_get_firmware_version(const char* command);
static void handle_set_speed_unit_kmh(const char* command);
static void handle_set_speed_unit_mph(const char* command);
static void handle_invert_throttle(const char* command);
static void handle_set_backlight(const char* command);

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
        case CMD_SET_GEAR_RATIO:
            handle_set_gear_ratio(command);
            break;
        case CMD_SET_WHEEL_SIZE:
            handle_set_wheel_size(command);
            break;
        case CMD_SET_MOTOR_POLES:
            handle_set_motor_poles(command);
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
        case CMD_UNKNOWN:
        default:
            printf("Unknown command: %s\n", command);
            printf("Type 'help' for available commands\n");
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
    printf("Odometer reset successfully\n");
}

static void handle_set_gear_ratio(const char* command)
{
    const char* value_str = strchr(command, ' ');
    if (value_str) {
        value_str++; // Skip the space
        float ratio = atof(value_str);
        if (ratio > 0.0f && ratio <= 65.535f) {
            hand_controller_config.gear_ratio_x1000 = (uint16_t)(ratio * 1000.0f);
            printf("Gear ratio set to: %.3f\n", ratio);
            // Save configuration to NVS
            esp_err_t err = vesc_config_save(&hand_controller_config);
            if (err != ESP_OK) {
                printf("Warning: Failed to save setting to memory\n");
            }
        } else {
            printf("Error: Invalid gear ratio value. Must be between 0.001 and 65.535\n");
        }
    } else {
        printf("Error: No value provided\n");
        printf("Usage: set_gear_ratio <ratio>\n");
        printf("Example: set_gear_ratio 2.2\n");
    }
    ui_force_config_reload(); // Force UI to reload config
}

static void handle_set_wheel_size(const char* command)
{
    const char* value_str = strchr(command, ' ');
    if (value_str) {
        value_str++; // Skip the space
        int size_mm = atoi(value_str);
        if (size_mm > 0 && size_mm <= 65535) {
            hand_controller_config.wheel_diameter_mm = (uint16_t)size_mm;
            printf("Wheel diameter set to: %d mm\n", size_mm);
            // Save configuration to NVS
            esp_err_t err = vesc_config_save(&hand_controller_config);
            if (err != ESP_OK) {
                printf("Warning: Failed to save setting to memory\n");
            }
        } else {
            printf("Error: Invalid wheel size value. Must be between 1 and 65535 mm\n");
        }
    } else {
        printf("Error: No value provided\n");
        printf("Usage: set_wheel_size <mm>\n");
        printf("Example: set_wheel_size 115\n");
    }
    ui_force_config_reload(); // Force UI to reload config
}

static void handle_set_motor_poles(const char* command)
{
    const char* value_str = strchr(command, ' ');
    if (value_str) {
        value_str++; // Skip the space
        int poles = atoi(value_str);
        if (poles > 0 && poles <= 255) {
            hand_controller_config.motor_poles = (uint8_t)poles;
            printf("Motor poles set to: %d\n", poles);
            // Save configuration to NVS
            esp_err_t err = vesc_config_save(&hand_controller_config);
            if (err != ESP_OK) {
                printf("Warning: Failed to save setting to memory\n");
            }
        } else {
            printf("Error: Invalid motor poles value. Must be between 1 and 255\n");
        }
    } else {
        printf("Error: No value provided\n");
        printf("Usage: set_motor_poles <poles>\n");
        printf("Example: set_motor_poles 14\n");
    }
    ui_force_config_reload(); // Force UI to reload config
}

static void handle_get_config(const char* command)
{
    // Reload configuration to ensure we have the latest settings
    esp_err_t err = vesc_config_load(&hand_controller_config);
    if (err != ESP_OK) {
        printf("Warning: Failed to reload configuration\n");
    }

    printf("\n=== Current Configuration ===\n");
    printf("GB Remote Model: %s\n", TARGET_NAME);
    printf("Firmware Version: %s\n", APP_VERSION_STRING);
    printf("Build Date: %s %s\n", BUILD_DATE, BUILD_TIME);
    printf("Speed Unit: %s\n",
           hand_controller_config.speed_unit_mph ? "mi/h" : "km/h");
    printf("Motor Poles: %d\n", hand_controller_config.motor_poles);
    printf("Gear Ratio: %.3f\n", hand_controller_config.gear_ratio_x1000 / 1000.0f);
    printf("Wheel Diameter: %d mm\n", hand_controller_config.wheel_diameter_mm);
#ifdef CONFIG_TARGET_LITE
    printf("Throttle Inversion: %s\n", hand_controller_config.invert_throttle ? "Enabled" : "Disabled");
#endif

    // Display backlight brightness
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_NAMESPACE_LCD, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        uint8_t brightness;
        err = nvs_get_u8(nvs_handle, NVS_KEY_BACKLIGHT, &brightness);
        if (err == ESP_OK) {
            printf("Backlight Brightness: %d%%\n", brightness);
        } else {
            printf("Backlight Brightness: %d%% (default)\n", LCD_BACKLIGHT_DEFAULT);
        }
        nvs_close(nvs_handle);
    } else {
        printf("Backlight Brightness: %d%% (default)\n", LCD_BACKLIGHT_DEFAULT);
    }

    printf("BLE Connected: %s\n", is_connect ? "Yes" : "No");

    // Calculate and display current speed if connected
    if (is_connect) {
        int32_t speed = vesc_config_get_speed(&hand_controller_config);
        printf("Current Speed: %ld %s\n", speed,
               hand_controller_config.speed_unit_mph ? "mi/h" : "km/h");
    }
    printf("\n");
}

static void handle_calibrate_throttle(const char* command)
{
    printf("\n=== Throttle Calibration ===\n");
    printf("Starting manual throttle calibration...\n");
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    printf("Please move the throttle AND brake through their full range during the next 6 seconds.\n");
#else
    printf("Please move the throttle through its full range during the next 6 seconds.\n");
#endif
    printf("Progress: ");

    // Trigger the throttle calibration
    throttle_calibrate();

    // Check if calibration was successful
    if (throttle_is_calibrated()) {
        printf("Throttle calibration completed successfully!\n");
        printf("Calibration values have been saved to memory.\n");
        printf("Throttle signals were set to neutral during calibration.\n");
#ifdef CONFIG_TARGET_DUAL_THROTTLE
        // Display brake calibration values for dual throttle
        uint32_t brake_min, brake_max;
        brake_get_calibration_values(&brake_min, &brake_max);
        printf("Brake calibration values: Min=%lu, Max=%lu, Range=%lu\n",
               brake_min, brake_max, brake_max - brake_min);
        // Display current BLE value being sent
        uint8_t ble_value = get_throttle_brake_ble_value();
        printf("Current BLE value being sent: %d\n", ble_value);
#else
        // Display current BLE value being sent (lite mode)
        uint32_t current_ble_value = adc_get_latest_value();
        printf("Current BLE value being sent: %lu\n", current_ble_value);
#endif
    } else {
        printf("\n✗ Throttle calibration failed!\n");
#ifdef CONFIG_TARGET_DUAL_THROTTLE
        printf("This usually means the throttle or brake wasn't moved through its full range.\n");
        printf("Please ensure you move both the throttle and brake from minimum to maximum position\n");
#else
        printf("This usually means the throttle wasn't moved through its full range.\n");
        printf("Please ensure you move the throttle from minimum to maximum position\n");
#endif
        printf("and try the calibration again.\n");
    }
    printf("\n");
}

static void handle_get_calibration(const char* command)
{
    printf("\n=== Throttle Calibration Status ===\n");

    bool is_calibrated = throttle_is_calibrated();
    printf("Calibration Status: %s\n", is_calibrated ? "Calibrated" : "Not Calibrated");

    if (is_calibrated) {
        uint32_t min_val, max_val;
        throttle_get_calibration_values(&min_val, &max_val);
        printf("Throttle - Calibrated Min Value: %lu\n", min_val);
        printf("Throttle - Calibrated Max Value: %lu\n", max_val);

#ifdef CONFIG_TARGET_DUAL_THROTTLE
        // Display brake calibration values for dual throttle
        uint32_t brake_min, brake_max;
        brake_get_calibration_values(&brake_min, &brake_max);
        printf("Brake - Calibrated Min Value: %lu\n", brake_min);
        printf("Brake - Calibrated Max Value: %lu\n", brake_max);

        uint8_t ble_value = get_throttle_brake_ble_value();
        printf("Current BLE value being sent: %d\n", ble_value);
#else
        // Display current BLE value being sent (lite mode)
        uint32_t current_ble_value = adc_get_latest_value();
        printf("Current BLE value being sent: %lu\n", current_ble_value);
#endif
    } else {
        printf("No calibration data available.\n");
        printf("Use 'calibrate_throttle' to perform calibration.\n");
    }
    printf("\n");
}

static void handle_get_firmware_version(const char* command)
{
    printf("Firmware version: %s\n", APP_VERSION_STRING);
    printf("Build date: %s %s\n", BUILD_DATE, BUILD_TIME);
    printf("Product Model: %s\n", TARGET_NAME);
    printf("IDF version: %s\n", esp_get_idf_version());
}

static void handle_set_speed_unit_kmh(const char* command)
{
    hand_controller_config.speed_unit_mph = false;
    printf("Speed unit set to: km/h\n");

    // Save configuration to NVS
    esp_err_t err = vesc_config_save(&hand_controller_config);
    if (err != ESP_OK) {
        printf("Warning: Failed to save setting to memory\n");
    }

    // Immediately update the speed unit label in the UI
    ui_update_speed_unit(hand_controller_config.speed_unit_mph);

    ui_force_config_reload(); // Force UI to reload config
}

static void handle_set_speed_unit_mph(const char* command)
{
    hand_controller_config.speed_unit_mph = true;
    printf("Speed unit set to: mi/h\n");

    // Save configuration to NVS
    esp_err_t err = vesc_config_save(&hand_controller_config);
    if (err != ESP_OK) {
        printf("Warning: Failed to save setting to memory\n");
    }

    // Immediately update the speed unit label in the UI
    ui_update_speed_unit(hand_controller_config.speed_unit_mph);

    ui_force_config_reload(); // Force UI to reload config
}

static void handle_invert_throttle(const char* command)
{
#ifdef CONFIG_TARGET_LITE
    // Toggle the invert_throttle setting
    hand_controller_config.invert_throttle = !hand_controller_config.invert_throttle;
    printf("Throttle inversion %s\n", hand_controller_config.invert_throttle ? "enabled" : "disabled");

    // Save configuration to NVS
    esp_err_t err = vesc_config_save(&hand_controller_config);
    if (err != ESP_OK) {
        printf("Warning: Failed to save setting to memory\n");
    } else {
        printf("Setting saved successfully\n");
    }

    ui_force_config_reload(); // Force UI to reload config
#else
    printf("Error: invert_throttle command is only available for lite target\n");
    printf("Current target: %s\n", TARGET_NAME);
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
            printf("Backlight brightness set to: %d%%\n", brightness);

            // Save to NVS
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open(NVS_NAMESPACE_LCD, NVS_READWRITE, &nvs_handle);
            if (err == ESP_OK) {
                err = nvs_set_u8(nvs_handle, NVS_KEY_BACKLIGHT, (uint8_t)brightness);
                if (err == ESP_OK) {
                    err = nvs_commit(nvs_handle);
                }
                nvs_close(nvs_handle);

                if (err != ESP_OK) {
                    printf("Warning: Failed to save setting to memory\n");
                }
            } else {
                printf("Warning: Failed to save setting to memory\n");
            }
        } else {
            printf("Error: Invalid brightness value. Must be between %d and %d\n",
                   LCD_BACKLIGHT_MIN, LCD_BACKLIGHT_MAX);
        }
    } else {
        printf("Error: No value provided\n");
        printf("Usage: set_backlight <brightness>\n");
        printf("Example: set_backlight 50\n");
        printf("Valid range: %d-%d (default: %d)\n",
               LCD_BACKLIGHT_MIN, LCD_BACKLIGHT_MAX, LCD_BACKLIGHT_DEFAULT);
    }
}