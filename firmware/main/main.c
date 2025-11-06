#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "ble.h"
#include "throttle.h"
#include "lcd.h"
#include "driver/gpio.h"
#include "power.h"
#include "button.h"
#include "ui.h"
#include "vesc_config.h"
#include "battery.h"
#include "esp_heap_caps.h"
#include "ui_updater.h"
#include "esp_timer.h"
#include "usb_serial_handler.h"
#include "level_assistant.h"
#include "version.h"
#include "target_config.h"
#include "viber.h"

#define TAG "MAIN"

extern bool is_connect;

static void splash_timer_cb(lv_timer_t * timer)
{
    lv_disp_load_scr(objects.home_screen);  // Switch to home screen after timeout
}

void app_main(void)
{

    ESP_LOGI(TAG, "Starting Application");

    ESP_LOGI(TAG, "Firmware version: %s", APP_VERSION_STRING);
    ESP_LOGI(TAG, "Build date: %s %s", BUILD_DATE, BUILD_TIME);
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    // Initialize power module
    power_init();

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize VESC configuration
    ESP_ERROR_CHECK(vesc_config_init());

    // Initialize level assistant
    ESP_ERROR_CHECK(level_assistant_init());

    // Initialize viber
    ESP_ERROR_CHECK(viber_init());

    // Initialize ADC and start tasks
    ESP_ERROR_CHECK(adc_init());
    adc_start_task();

    // Initialize LCD and LVGL
    lcd_init();

    // Wait for ADC calibration
    while (!throttle_is_calibrated()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Initialize USB Serial Handler FIRST (before BLE UART setup)
    // Note: This is optional - if it fails, the device will still work without USB commands
    ESP_LOGI(TAG, "Attempting to initialize USB Serial Handler...");

    // Initialize full USB serial handler
    usb_serial_init();
    usb_serial_start_task();

    // Initialize BLE
    spp_client_demo_init();
    ESP_LOGI(TAG, "BLE Initialization complete");

    ESP_ERROR_CHECK(battery_init());
    battery_start_monitoring();

    // Start power monitoring
    power_start_monitoring();

    // Initialize SquareLine Studio UI
    ui_init();

    // Set initial speed unit from saved configuration
    vesc_config_t config;
    esp_err_t err = vesc_config_load(&config);
    if (err == ESP_OK) {
        ui_update_speed_unit(config.speed_unit_mph);
        ESP_LOGI(TAG, "Initial speed unit set to: %s", config.speed_unit_mph ? "mi/h" : "km/h");
    } else {
        ESP_LOGW(TAG, "Failed to load speed unit configuration, using default km/h");
        ui_update_speed_unit(false); // Default to km/h
    }

    viber_play_pattern(VIBER_PATTERN_SINGLE_SHORT);
    lv_disp_load_scr(objects.splash_screen);  // Load splash screen first
    lv_timer_t * splash_timer = lv_timer_create(splash_timer_cb, 4000, NULL);  // Create timer for 3 seconds
    lv_timer_set_repeat_count(splash_timer, 1);  // Run only once
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Fade up the backlight smoothly
    lcd_fade_backlight(LCD_BACKLIGHT_MIN, LCD_BACKLIGHT_DEFAULT, LCD_BACKLIGHT_FADE_DURATION_MS);

    // Main task loop
    while (1) {
        power_check_inactivity(is_connect);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

