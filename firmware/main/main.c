#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_sleep.h"
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
#include "hw_config.h"

#define TAG "MAIN"

extern bool is_connect;

static void splash_timer_cb(lv_timer_t *timer)
{
    lv_disp_load_scr(objects.home_screen);
}

static uint8_t load_backlight_brightness(void)
{
    uint8_t brightness = LCD_BACKLIGHT_DEFAULT;
    nvs_handle_t nvs_handle;

    if (nvs_open("lcd_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
        uint8_t saved_brightness;
        if (nvs_get_u8(nvs_handle, "backlight", &saved_brightness) == ESP_OK) {
            brightness = saved_brightness;
            ESP_LOGI(TAG, "Loaded saved backlight brightness: %d%%", saved_brightness);
        }
        nvs_close(nvs_handle);
    }

    return brightness;
}

static void initialize_system(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize VESC configuration
    ESP_ERROR_CHECK(vesc_config_init());

    // Initialize viber
    ESP_ERROR_CHECK(viber_init());
}

static void initialize_hardware(void)
{
    // Initialize ADC and start tasks
    ESP_ERROR_CHECK(adc_init());
    adc_start_task();

    // Initialize LCD and LVGL
    lcd_init();

    // Wait for ADC calibration
    while (!throttle_is_calibrated()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void initialize_communication(void)
{
    usb_serial_init();
    usb_serial_start_task();
    spp_client_demo_init();
    ESP_LOGI(TAG, "BLE Initialization complete");
}

static void initialize_ui(void)
{
    ui_init();

    // Create aux output indicator and set initial visibility
    ui_create_aux_output_indicator();
    ui_update_aux_output_indicator();

    // Set initial speed unit from saved configuration
    vesc_config_t config;
    esp_err_t err = vesc_config_load(&config);
    if (err == ESP_OK) {
        ui_update_speed_unit(config.speed_unit_mph);
        ESP_LOGI(TAG, "Initial speed unit set to: %s", config.speed_unit_mph ? "mi/h" : "km/h");
    } else {
        ESP_LOGW(TAG, "Failed to load speed unit configuration, using default km/h");
        ui_update_speed_unit(false);
    }
}

static void show_splash_screen(void)
{
    viber_play_pattern(VIBER_PATTERN_SINGLE_SHORT);
    lv_disp_load_scr(objects.splash_screen);

    // Create timer to switch to home screen after 4 seconds
    lv_timer_t *splash_timer = lv_timer_create(splash_timer_cb, 4000, NULL);
    lv_timer_set_repeat_count(splash_timer, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void initialize_backlight(void)
{
    uint8_t target_brightness = load_backlight_brightness();

    // Fade up the backlight smoothly to saved/default brightness
    // Map percentage (1-100) to PWM duty (0-255)
    uint8_t target_pwm = (target_brightness * 255) / 100;
    uint8_t min_pwm = (LCD_BACKLIGHT_MIN * 255) / 100;
    lcd_fade_backlight(min_pwm, target_pwm, LCD_BACKLIGHT_FADE_DURATION_MS);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Application");
    ESP_LOGI(TAG, "Firmware version: %s", APP_VERSION_STRING);
    ESP_LOGI(TAG, "Build date: %s %s", BUILD_DATE, BUILD_TIME);
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    // Initialize main button early to check wake-up
    ESP_ERROR_CHECK(button_init_main());

    // Initialize power module (sets POWER_HOLD_GPIO high)
    power_init();

    // Check if we woke from sleep and if button long press is required
    if (!power_check_wake_from_sleep()) {
        // Button not held long enough or not pressed - go back to sleep immediately
        power_sleep_immediate();
        return; // Should not reach here, but just in case
    }

    // Initialize system components
    initialize_system();
    initialize_hardware();
    initialize_communication();

    // Initialize battery and button monitoring
    ESP_ERROR_CHECK(battery_init());
    battery_start_monitoring();
    button_start_monitoring();

    // Initialize UI
    initialize_ui();
    show_splash_screen();
    initialize_backlight();

    // Main task loop
    while (1) {
        power_check_inactivity(is_connect);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

