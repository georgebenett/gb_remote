#include "power.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd.h"
#include "ui.h"
#include "lvgl.h"
#include "ui_updater.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "button.h"
#include "viber.h"

#define TAG "POWER"

static TickType_t last_activity_time;
static TickType_t last_reset_time = 0;
#define RESET_DEBOUNCE_TIME_MS 2000

static lv_anim_t arc_anim;
static bool arc_animation_active = false;

// Add a global flag to indicate when we're entering power off mode
volatile bool entering_power_off_mode = false;

// Flag to track if button has been released since boot
// This prevents shutdown from triggering if button is held during boot
static bool button_released_since_boot = false;

static void set_bar_value(void * obj, int32_t v)
{
    lv_bar_set_value(obj, v, LV_ANIM_OFF);

    // If we reach 100%, trigger shutdown immediately
    if (v >= 100) {
        ESP_LOGI(TAG, "Bar filled - Shutting down");

        // Set the flag to indicate we're entering power off mode
        entering_power_off_mode = true;

        // Give UI tasks time to see the flag
        vTaskDelay(pdMS_TO_TICKS(500));
        power_shutdown();
    }
}

static void power_button_callback(button_event_t event, void* user_data) {
    static bool long_press_triggered = false;

    switch(event) {
        case BUTTON_EVENT_PRESSED:
            long_press_triggered = false;
            break;

        case BUTTON_EVENT_RELEASED:
            // Mark that button has been released since boot
            button_released_since_boot = true;

            if (arc_animation_active) {
                // If released before full, cancel shutdown
                lv_anim_del(objects.shutting_down_bar, set_bar_value);
                lv_bar_set_value(objects.shutting_down_bar, 0, LV_ANIM_OFF);
                arc_animation_active = false;
                lv_disp_load_scr(objects.home_screen);
            }
            long_press_triggered = false;
            break;

        case BUTTON_EVENT_LONG_PRESS:
            // Only allow shutdown sequence if button has been released since boot
            // This prevents shutdown from triggering if button is held during boot
            if (!button_released_since_boot) {
                ESP_LOGI(TAG, "Long press ignored - button must be released first after boot");
                break;
            }

            if (!long_press_triggered) {
                long_press_triggered = true;
                // Switch to shutdown screen
                lv_disp_load_scr(objects.shutdown_screen);

                // Start bar animation
                lv_anim_init(&arc_anim);
                lv_anim_set_var(&arc_anim, objects.shutting_down_bar);
                lv_anim_set_exec_cb(&arc_anim, set_bar_value);
                lv_anim_set_time(&arc_anim, 2000);  // 2 seconds to fill
                lv_anim_set_values(&arc_anim, 0, 100);
                lv_anim_start(&arc_anim);
                arc_animation_active = true;
            }
            break;

        case BUTTON_EVENT_DOUBLE_PRESS:
            break;
    }
}

void power_init(void) {
    button_config_t config = {
        .gpio_num = MAIN_BUTTON_GPIO,
        .long_press_time_ms = BUTTON_LONG_PRESS_TIME_MS,
        .double_press_time_ms = BUTTON_DOUBLE_PRESS_TIME_MS,
        .active_low = false
    };

    // Set POWER_OFF_GPIO to HIGH as first action
    gpio_config_t gpio4_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_4),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&gpio4_conf));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_4, 1));

    ESP_ERROR_CHECK(button_init(&config));
    button_register_callback(power_button_callback, NULL);

    last_activity_time = xTaskGetTickCount();
}

void power_start_monitoring(void) {
    button_start_monitoring();
}

void power_reset_inactivity_timer(void)
{
    TickType_t current_time = xTaskGetTickCount();

    // Only reset if enough time has passed since last reset
    if ((current_time - last_reset_time) * portTICK_PERIOD_MS >= RESET_DEBOUNCE_TIME_MS) {
        last_activity_time = current_time;
        last_reset_time = current_time;
    }
}

void power_check_inactivity(bool is_ble_connected)
{
    TickType_t current_time = xTaskGetTickCount();
    TickType_t elapsed_time = (current_time - last_activity_time) * portTICK_PERIOD_MS;

    // Check if we should shut down (if inactive and not connected)
    // Note: Inactivity timeout removed - power control is manual via button only
    // This function can be used for future inactivity-based shutdown if needed
    (void)elapsed_time;
    (void)is_ble_connected;
}

void power_shutdown(void) {
    ESP_LOGI(TAG, "Preparing for shutdown");
    viber_play_pattern(VIBER_PATTERN_DOUBLE_SHORT);
    lcd_fade_backlight(100, 0, 1000);  // Fade from 100 to 0 over 1 second
    // Save trip distance
    esp_err_t err = ui_save_trip_distance();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save trip distance: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    // Shut down by setting GPIO 4 to LOW
    gpio_set_level(POWER_OFF_GPIO, 0);
}

