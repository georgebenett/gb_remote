#include "sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd.h"
#include "ui/ui.h"
#include "lvgl.h"
#include "esp_sleep.h"
#include "ui_updater.h"
#include "esp_log_internal.h"
#include "esp_err.h"
#include "driver/rtc_io.h"

#define TAG "SLEEP"

static TickType_t last_activity_time;
static TickType_t last_reset_time = 0;
#define RESET_DEBOUNCE_TIME_MS 2000

static lv_anim_t arc_anim;
static bool arc_animation_active = false;

// Add a global flag to indicate when we're entering sleep mode
volatile bool entering_sleep_mode = false;

static void set_bar_value(void * obj, int32_t v)
{
    lv_bar_set_value(obj, v, LV_ANIM_OFF);

    // If we reach 100%, trigger sleep immediately
    if (v >= 100) {
        ESP_LOGI(TAG, "Bar filled - Entering deep sleep mode");

        // Set the flag to indicate we're entering sleep mode
        entering_sleep_mode = true;

        // Give UI tasks time to see the flag
        vTaskDelay(pdMS_TO_TICKS(500));
        sleep_enter_deep_sleep();
        
    }
}

static void sleep_button_callback(button_event_t event, void* user_data) {
    static bool long_press_triggered = false;

    switch(event) {
        case BUTTON_EVENT_PRESSED:
            long_press_triggered = false;
            break;

        case BUTTON_EVENT_RELEASED:
            if (arc_animation_active) {
                // If released before full, cancel sleep
                lv_anim_del(objects.shutting_down_bar, set_bar_value);
                lv_bar_set_value(objects.shutting_down_bar, 0, LV_ANIM_OFF);
                arc_animation_active = false;
                lv_disp_load_scr(objects.home_screen);
            }
            long_press_triggered = false;
            break;

        case BUTTON_EVENT_LONG_PRESS:
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

void sleep_init(void) {
    button_config_t config = {
        .gpio_num = MAIN_BUTTON_GPIO,
        .long_press_time_ms = BUTTON_LONG_PRESS_TIME_MS,
        .double_press_time_ms = BUTTON_DOUBLE_PRESS_TIME_MS,
        .active_low = false
    };

    ESP_ERROR_CHECK(button_init(&config));
    button_register_callback(sleep_button_callback, NULL);

    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
    last_activity_time = xTaskGetTickCount();
}

void sleep_start_monitoring(void) {
    button_start_monitoring();
}

void sleep_reset_inactivity_timer(void)
{
    TickType_t current_time = xTaskGetTickCount();

    // Only reset if enough time has passed since last reset
    if ((current_time - last_reset_time) * portTICK_PERIOD_MS >= RESET_DEBOUNCE_TIME_MS) {
        last_activity_time = current_time;
        last_reset_time = current_time;
    }
}

void sleep_check_inactivity(bool is_ble_connected)
{
    TickType_t current_time = xTaskGetTickCount();
    TickType_t elapsed_time = (current_time - last_activity_time) * portTICK_PERIOD_MS;

    // Check if we should go to sleep (if inactive and not connected)
    if (elapsed_time > INACTIVITY_TIMEOUT_MS && !is_ble_connected) {
        ESP_LOGI(TAG, "System inactive for %lu ms and no BLE connection. Entering deep sleep.",
                 elapsed_time);

        // Enter deep sleep
        //sleep_enter_deep_sleep();
    }
}

void sleep_enter_deep_sleep(void) {
    ESP_LOGI(TAG, "Preparing for deep sleep");

    // Disable UI updates by taking and holding the mutex
    if (take_lvgl_mutex()) {
        ESP_LOGI(TAG, "Acquired LVGL mutex for shutdown");

        // Save trip distance
        esp_err_t err = ui_save_trip_distance();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save trip distance: %s", esp_err_to_name(err));
            // Try one more time
            vTaskDelay(pdMS_TO_TICKS(100));
            err = ui_save_trip_distance();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save trip distance on second attempt");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(GPIO_NUM_4, 0);

    } else {
        ESP_LOGW(TAG, "Could not acquire LVGL mutex for shutdown, proceeding anyway");

        // Continue with normal shutdown
        ui_save_trip_distance();
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(GPIO_NUM_4, 0);
    }
}
