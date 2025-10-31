#include "usb_serial_handler.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "esp_vfs_dev.h"
#include "fcntl.h"
#include "ble_spp_client.h"
#include "vesc_config.h"
#include "ui_updater.h"
#include "throttle.h"
#include "level_assistant.h"
#include "version.h"

#define TAG "USB_SERIAL"
#define MAX_COMMAND_LENGTH 256

static TaskHandle_t usb_task_handle = NULL;
static char command_buffer[MAX_COMMAND_LENGTH];
static int command_buffer_pos = 0;

// Command strings
static const char* CMD_STRINGS[] = {
    "invert_throttle",
    "level_assistant",
    "reset_odometer",
    "set_motor_pulley",
    "set_wheel_pulley",
    "set_wheel_size",
    "set_motor_poles",
    "get_config",
    "calibrate_throttle",
    "get_calibration",
    "get_firmware_version",
    "set_speed_unit_kmh",
    "set_speed_unit_mph",
    "help"
};

// Configuration storage using vesc_config_t structure
static vesc_config_t hand_controller_config;

static void usb_serial_init_uart(void);
static void usb_serial_task(void *pvParameters);
static usb_command_t parse_command(const char* input);
static void print_help(void);
static void handle_invert_throttle(const char* command);
static void handle_level_assistant(const char* command);
static void handle_reset_odometer(const char* command);
static void handle_set_motor_pulley(const char* command);
static void handle_set_wheel_pulley(const char* command);
static void handle_set_wheel_size(const char* command);
static void handle_set_motor_poles(const char* command);
static void handle_get_config(const char* command);
static void handle_calibrate_throttle(const char* command);
static void handle_get_calibration(const char* command);
static void handle_get_firmware_version(const char* command);
static void handle_set_speed_unit_kmh(const char* command);
static void handle_set_speed_unit_mph(const char* command);

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

    usb_serial_init_uart();

    // Load configuration from NVS
    esp_err_t err = vesc_config_load(&hand_controller_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load configuration, using defaults");
        // Initialize with default values if loading fails
        hand_controller_config.motor_pulley = 15;
        hand_controller_config.wheel_pulley = 33;
        hand_controller_config.wheel_diameter_mm = 115;
        hand_controller_config.motor_poles = 14;
        hand_controller_config.invert_throttle = false;
        hand_controller_config.level_assistant = false;
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

#if defined(CONFIG_IDF_TARGET_ESP32C3)
void usb_serial_init_esp32c3(void)
{
    ESP_LOGI(TAG, "Setting up USB Serial JTAG interface for ESP32-C3");

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

    // ESP32-C3 specific: Additional delay for USB enumeration
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "USB Serial JTAG initialized successfully for ESP32-C3");
}
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
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
#endif

static void usb_serial_init_uart(void)
{
    ESP_LOGI(TAG, "Setting up USB Serial JTAG interface (generic)");

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

    ESP_LOGI(TAG, "USB Serial JTAG initialized successfully");
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
                    ESP_LOGI(TAG, "Processing command: %s", command_buffer);
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
    ESP_LOGI(TAG, "Processing command: '%s' (length: %d)", command, strlen(command));
    usb_command_t cmd = parse_command(command);
    ESP_LOGI(TAG, "Parsed command type: %d", cmd);

    switch (cmd) {
        case CMD_INVERT_THROTTLE:
            handle_invert_throttle(command);
            break;
        case CMD_LEVEL_ASSISTANT:
            handle_level_assistant(command);
            break;
        case CMD_RESET_ODOMETER:
            handle_reset_odometer(command);
            break;
        case CMD_SET_MOTOR_PULLEY:
            handle_set_motor_pulley(command);
            break;
        case CMD_SET_WHEEL_PULLEY:
            handle_set_wheel_pulley(command);
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

static void print_help(void)
{
    printf("\n=== Hand Controller Configuration Interface ===\n");
    printf("Available commands:\n");
    printf("  invert_throttle          - Toggle throttle inversion\n");
    printf("  level_assistant          - Toggle level assistant\n");
    printf("  set_speed_unit_kmh       - Set speed unit to km/h\n");
    printf("  set_speed_unit_mph       - Set speed unit to mi/h\n");
    printf("  reset_odometer           - Reset trip odometer\n");
    printf("  set_motor_pulley <teeth> - Set motor pulley teeth\n");
    printf("  set_wheel_pulley <teeth> - Set wheel pulley teeth\n");
    printf("  set_wheel_size <mm>      - Set wheel diameter in mm\n");
    printf("  set_motor_poles <poles>  - Set motor pole count\n");
    printf("  get_config               - Display current configuration\n");
    printf("  calibrate_throttle       - Start throttle calibration\n");
    printf("  get_calibration          - Get calibration status\n");
    printf("  get_firmware_version     - Get firmware version\n");
    printf("  help                     - Show this help\n");
    printf("\n");
}

static void handle_invert_throttle(const char* command)
{
    hand_controller_config.invert_throttle = !hand_controller_config.invert_throttle;
    printf("Throttle inversion: %s\n",
           hand_controller_config.invert_throttle ? "ENABLED" : "DISABLED");

    // Save configuration to NVS
    esp_err_t err = vesc_config_save(&hand_controller_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save throttle inversion setting: %s", esp_err_to_name(err));
        printf("Warning: Failed to save setting to memory\n");
    } else {
        ESP_LOGI(TAG, "Throttle inversion saved to NVS: %s",
                 hand_controller_config.invert_throttle ? "ENABLED" : "DISABLED");
    }
    ui_force_config_reload(); // Force UI to reload config
}

static void handle_level_assistant(const char* command)
{
    hand_controller_config.level_assistant = !hand_controller_config.level_assistant;
    printf("Level assistant: %s\n",
           hand_controller_config.level_assistant ? "ENABLED" : "DISABLED");

    // Save configuration to NVS
    esp_err_t err = vesc_config_save(&hand_controller_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save level assistant setting: %s", esp_err_to_name(err));
        printf("Warning: Failed to save setting to memory\n");
    } else {
        ESP_LOGI(TAG, "Level assistant saved to NVS: %s",
                 hand_controller_config.level_assistant ? "ENABLED" : "DISABLED");
    }
    ui_force_config_reload(); // Force UI to reload config
}

static void handle_reset_odometer(const char* command)
{
    printf("Odometer reset command received\n");

    // Reset the local trip distance display
    ui_reset_trip_distance();
    ESP_LOGI(TAG, "Local trip distance reset");


    printf("Odometer reset successfully\n");
}

static void handle_set_motor_pulley(const char* command)
{
    const char* value_str = strchr(command, ' ');
    if (value_str) {
        value_str++; // Skip the space
        int teeth = atoi(value_str);
        if (teeth > 0 && teeth <= 255) {
            hand_controller_config.motor_pulley = (uint8_t)teeth;
            printf("Motor pulley teeth set to: %d\n", teeth);
            ESP_LOGI(TAG, "Motor pulley teeth set to: %d", teeth);

            // Save configuration to NVS
            esp_err_t err = vesc_config_save(&hand_controller_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save motor pulley setting: %s", esp_err_to_name(err));
                printf("Warning: Failed to save setting to memory\n");
            }
        } else {
            printf("Error: Invalid pulley teeth value. Must be between 1 and 255\n");
        }
    } else {
        printf("Error: No value provided\n");
        printf("Usage: set_motor_pulley <teeth>\n");
        printf("Example: set_motor_pulley 15\n");
    }
    ui_force_config_reload(); // Force UI to reload config
}

static void handle_set_wheel_pulley(const char* command)
{
    const char* value_str = strchr(command, ' ');
    if (value_str) {
        value_str++; // Skip the space
        int teeth = atoi(value_str);
        if (teeth > 0 && teeth <= 255) {
            hand_controller_config.wheel_pulley = (uint8_t)teeth;
            printf("Wheel pulley teeth set to: %d\n", teeth);
            ESP_LOGI(TAG, "Wheel pulley teeth set to: %d", teeth);

            // Save configuration to NVS
            esp_err_t err = vesc_config_save(&hand_controller_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save wheel pulley setting: %s", esp_err_to_name(err));
                printf("Warning: Failed to save setting to memory\n");
            }
        } else {
            printf("Error: Invalid pulley teeth value. Must be between 1 and 255\n");
        }
    } else {
        printf("Error: No value provided\n");
        printf("Usage: set_wheel_pulley <teeth>\n");
        printf("Example: set_wheel_pulley 33\n");
    }
    ui_force_config_reload(); // Force UI to reload config
}

static void handle_set_wheel_size(const char* command)
{
    const char* value_str = strchr(command, ' ');
    if (value_str) {
        value_str++; // Skip the space
        int size_mm = atoi(value_str);
        if (size_mm > 0 && size_mm <= 255) {
            hand_controller_config.wheel_diameter_mm = (uint8_t)size_mm;
            printf("Wheel diameter set to: %d mm\n", size_mm);
            ESP_LOGI(TAG, "Wheel diameter set to: %d mm", size_mm);

            // Save configuration to NVS
            esp_err_t err = vesc_config_save(&hand_controller_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save wheel size setting: %s", esp_err_to_name(err));
                printf("Warning: Failed to save setting to memory\n");
            }
        } else {
            printf("Error: Invalid wheel size value. Must be between 1 and 255 mm\n");
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
            ESP_LOGI(TAG, "Motor poles set to: %d", poles);

            // Save configuration to NVS
            esp_err_t err = vesc_config_save(&hand_controller_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save motor poles setting: %s", esp_err_to_name(err));
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
        ESP_LOGW(TAG, "Failed to reload configuration: %s", esp_err_to_name(err));
        printf("Warning: Failed to reload configuration\n");
    }

    printf("\n=== Current Configuration ===\n");
    printf("Firmware Version: %s\n", APP_VERSION_STRING);
    printf("Throttle Inverted: %s\n",
           hand_controller_config.invert_throttle ? "Yes" : "No");
    printf("Level Assistant: %s\n",
           hand_controller_config.level_assistant ? "Yes" : "No");
    printf("Speed Unit: %s\n",
           hand_controller_config.speed_unit_mph ? "mi/h" : "km/h");
    printf("Motor Pulley Teeth: %d\n", hand_controller_config.motor_pulley);
    printf("Wheel Pulley Teeth: %d\n", hand_controller_config.wheel_pulley);
    printf("Wheel Diameter: %d mm\n", hand_controller_config.wheel_diameter_mm);
    printf("Motor Poles: %d\n", hand_controller_config.motor_poles);
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
    printf("Please move the throttle through its full range during the next 6 seconds.\n");
    printf("Progress: ");

    // Trigger the Throttle calibration
    throttle_calibrate();

    // Check if calibration was successful
    if (throttle_is_calibrated()) {
        printf("\n✓ Throttle calibration completed successfully!\n");
        printf("Calibration values have been saved to memory.\n");
        printf("Throttle signals were set to neutral during calibration.\n");
    } else {
        printf("\n✗ Throttle calibration failed!\n");
        printf("This usually means the throttle wasn't moved through its full range.\n");
        printf("Please ensure you move the throttle from minimum to maximum position\n");
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
        printf("Calibrated Min Value: %lu\n", min_val);
        printf("Calibrated Max Value: %lu\n", max_val);
        printf("Calibrated Range: %lu\n", max_val - min_val);

        // Show current ADC reading for reference
        int32_t current_throttle = throttle_read_value();
        if (current_throttle != -1) {
            uint8_t mapped_value = map_throttle_value(current_throttle);
            printf("Current ADC Reading: %ld\n", current_throttle);
            printf("Current Mapped Value: %d\n", mapped_value);
        }
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
    printf("Target: %s\n", CONFIG_IDF_TARGET);
    printf("IDF version: %s\n", esp_get_idf_version());
}

static void handle_set_speed_unit_kmh(const char* command)
{
    hand_controller_config.speed_unit_mph = false;
    printf("Speed unit set to: km/h\n");

    // Save configuration to NVS
    esp_err_t err = vesc_config_save(&hand_controller_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save speed unit setting: %s", esp_err_to_name(err));
        printf("Warning: Failed to save setting to memory\n");
    } else {
        ESP_LOGI(TAG, "Speed unit saved to NVS: km/h");
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
        ESP_LOGE(TAG, "Failed to save speed unit setting: %s", esp_err_to_name(err));
        printf("Warning: Failed to save setting to memory\n");
    } else {
        ESP_LOGI(TAG, "Speed unit saved to NVS: mi/h");
    }

    // Immediately update the speed unit label in the UI
    ui_update_speed_unit(hand_controller_config.speed_unit_mph);

    ui_force_config_reload(); // Force UI to reload config
}