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
#include "nvs.h"
#include "vesc_config.h"
#include "ble.h"

#define TAG "POWER"

static TickType_t last_activity_time;
static TickType_t last_reset_time = 0;

static lv_anim_t arc_anim;
static bool arc_animation_active = false;

volatile bool entering_power_off_mode = false;

static bool button_released_since_boot = false;

static void set_bar_value(void * obj, int32_t v)
{
    lv_bar_set_value(obj, v, LV_ANIM_OFF);

    // If we reach 100%, trigger shutdown immediately
    if (v >= 100) {
        ESP_LOGI(TAG, "Bar filled - Shutting down");
        viber_play_pattern(VIBER_PATTERN_DOUBLE_SHORT);
        // Set the flag to indicate we're entering power off mode
        entering_power_off_mode = true;
        // Give UI tasks time to see the flag
        vTaskDelay(pdMS_TO_TICKS(100));
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
                // Force full screen redraw to ensure no artifacts from previous screen
                lv_obj_invalidate(objects.home_screen);
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

            // Prevent shutdown if vehicle is moving
            {
                vesc_config_t config;
                if (vesc_config_load(&config) == ESP_OK) {
                    int32_t speed = vesc_config_get_speed(&config);
                    if (speed > 0) {
                        //ESP_LOGW(TAG, "Long press ignored - vehicle is moving (speed: %ld)", (long)speed);
                        break;
                    }
                }
            }

            if (!long_press_triggered) {
                long_press_triggered = true;
                // Switch to shutdown screen
                lv_disp_load_scr(objects.shutdown_screen);
                // Force full screen redraw to ensure no artifacts from previous screen
                lv_obj_invalidate(objects.shutdown_screen);
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
            // Toggle auxiliary output on double press (only if BLE connected)
            if (is_connect) {
                ble_toggle_aux_output();
                ui_update_aux_output_indicator();
                ESP_LOGI(TAG, "Aux output toggled: %s", ble_get_aux_output_state() ? "ON" : "OFF");
                viber_play_pattern(VIBER_PATTERN_SINGLE_SHORT);
            }
            break;
    }
}

void power_init(void) {
    // Set POWER_HOLD_GPIO to HIGH as first action
    gpio_config_t POWER_HOLD_GPIO_conf = {
        .pin_bit_mask = (1ULL << POWER_HOLD_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&POWER_HOLD_GPIO_conf));
    ESP_ERROR_CHECK(gpio_set_level(POWER_HOLD_GPIO, 1));

    // Register power button callback (button should already be initialized)
    button_register_callback(power_button_callback, NULL);

    last_activity_time = xTaskGetTickCount();
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

    if (!is_ble_connected &&
        button_released_since_boot &&
        elapsed_time >= INACTIVITY_TIMEOUT_MS) {
        ESP_LOGI(TAG, "Inactivity timeout reached (%u ms) - shutting down", (unsigned int)elapsed_time);
        power_shutdown();
    }
}

void power_shutdown(void) {
    ESP_LOGI(TAG, "Preparing for shutdown");

    // Load saved backlight brightness or use default
    uint8_t current_brightness = LCD_BACKLIGHT_DEFAULT;
    nvs_handle_t nvs_handle;
    if (nvs_open("lcd_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
        uint8_t saved_brightness;
        if (nvs_get_u8(nvs_handle, "backlight", &saved_brightness) == ESP_OK) {
            current_brightness = saved_brightness;
        }
        nvs_close(nvs_handle);
    }

    // Fade out from current brightness to minimum
    // Map percentage (1-100) to PWM duty (0-255)
    uint8_t current_pwm = (current_brightness * 255) / 100;
    uint8_t min_pwm = (LCD_BACKLIGHT_MIN * 255) / 100;
    lcd_fade_backlight(current_pwm, min_pwm, LCD_BACKLIGHT_FADE_DURATION_MS);

    // Save trip distance
    esp_err_t err = ui_save_trip_distance();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save trip distance: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    // Shut down by setting GPIO 4 to LOW
    gpio_set_level(POWER_HOLD_GPIO, 0);
}

