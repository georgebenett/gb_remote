/**
 * @file main.c
 * @brief Main entry point for the GB Remote firmware
 *
 * ERROR HANDLING STRATEGY:
 * ========================
 * This codebase uses a tiered error handling approach:
 *
 * 1. CRITICAL ERRORS (ESP_ERROR_CHECK / esp_restart):
 *    - NVS flash initialization failure
 *    - ADC initialization failure (throttle input is safety-critical)
 *    - Display buffer allocation failure
 *    - Button initialization failure (power control)
 *    - BLE stack initialization failure
 *    These errors prevent safe operation and require immediate abort/restart.
 *
 * 2. NON-CRITICAL ERRORS (log warning, continue with degraded functionality):
 *    - VESC config load failure -> use defaults
 *    - Viber/haptic init failure -> disable haptic feedback
 *    - NVS read failures for settings -> use defaults
 *    - UI update failures -> skip update, try again next cycle
 *    These errors allow continued operation with reduced features.
 *
 * 3. TRANSIENT ERRORS (retry or ignore):
 *    - ADC read failures -> retry with averaging
 *    - BLE write failures -> log and continue
 *    - Mutex timeout -> retry or skip update
 *    These errors are expected occasionally and handled gracefully.
 *
 * THREAD SAFETY:
 * ==============
 * - Shared state uses volatile for visibility across tasks
 * - LVGL operations protected with dedicated mutex
 * - UI updates use command queue for thread-safe cross-task communication
 * - On ESP32, 32-bit aligned reads/writes are atomic at hardware level
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "ble.h"
#include "throttle.h"
#include "lcd.h"
#include "power.h"
#include "button.h"
#include "ui.h"
#include "vesc_config.h"
#include "battery.h"
#include "ui_updater.h"
#include "usb_serial.h"
#include "version.h"
#include "viber.h"

#define TAG "MAIN"

static void initialize_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void initialize_system(void)
{
    initialize_nvs();

    if (vesc_config_init() != ESP_OK) {
        ESP_LOGW(TAG, "VESC config init failed, using defaults");
    }

    if (viber_init() != ESP_OK) {
        ESP_LOGW(TAG, "Viber init failed, haptic feedback disabled");
    }
}

static void initialize_hardware(void)
{
    ESP_ERROR_CHECK(adc_init());
    adc_start_task();
    lcd_init();

    while (!throttle_is_calibrated()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void initialize_communication(void)
{
    usb_serial_init();
    usb_serial_start_task();
    spp_client_demo_init();
    ESP_LOGI(TAG, "BLE initialization complete");
}

static void initialize_monitoring(void)
{
    ESP_ERROR_CHECK(battery_init());
    battery_start_monitoring();
    button_start_monitoring();
}

static void initialize_ui(void)
{
    ui_init();
    ui_create_aux_output_indicator();

    vesc_config_t config;
    esp_err_t err = vesc_config_load(&config);
    if (err == ESP_OK) {
        ui_update_speed_unit(config.speed_unit_mph);
        ESP_LOGI(TAG, "Speed unit set to: %s", config.speed_unit_mph ? "mph" : "km/h");
    } else {
        ESP_LOGW(TAG, "Failed to load speed unit config, using default km/h");
        ui_update_speed_unit(false);
    }
}

static void log_startup_info(void)
{
    ESP_LOGI(TAG, "Starting Application");
    ESP_LOGI(TAG, "Firmware version: %s", FW_VERSION);
    ESP_LOGI(TAG, "Build date: %s %s", BUILD_DATE, BUILD_TIME);
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());
}

void app_main(void)
{
    log_startup_info();

    // Power-on sequence (sleeps if button not held long enough)
    ESP_ERROR_CHECK(button_init_main());
    power_init();

    // Initialize all subsystems
    initialize_system();
    initialize_hardware();
    initialize_communication();
    initialize_monitoring();

    // Initialize and show UI
    initialize_ui();
    ui_show_splash_screen();
    lcd_fade_to_saved_brightness();

    // Main loop: monitor for inactivity
    while (1) {
        power_check_inactivity(ble_is_connected());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
