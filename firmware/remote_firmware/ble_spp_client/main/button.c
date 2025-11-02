#include "button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include <string.h>
#include <stdio.h>
#include "ui.h"
#include "lvgl.h"

#define TAG "BUTTON"
#define DEBOUNCE_TIME_MS 20
#define TASK_STACK_SIZE 4096
#define TASK_PRIORITY 3
#define MAX_CALLBACKS 4

typedef struct {
    button_callback_t callback;
    void* user_data;
    bool in_use;
} button_callback_entry_t;

typedef enum {
    SCREEN_HOME,
    SCREEN_SHUTDOWN,
    SCREEN_MAX
} screen_state_t;

static button_config_t button_cfg;
static button_state_t current_state = BUTTON_IDLE;
static TickType_t press_start_time = 0;
static TickType_t last_release_time = 0;
static bool first_press_registered = false;
static TaskHandle_t button_task_handle = NULL;
static button_callback_entry_t callbacks[MAX_CALLBACKS] = {0};
static void default_button_handler(button_event_t event, void* user_data);

static void notify_callbacks(button_event_t event) {
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (callbacks[i].in_use && callbacks[i].callback) {
            callbacks[i].callback(event, callbacks[i].user_data);
        }
    }
}

void button_register_callback(button_callback_t callback, void* user_data) {
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (!callbacks[i].in_use) {
            callbacks[i].callback = callback;
            callbacks[i].user_data = user_data;
            callbacks[i].in_use = true;
            return;
        }
    }
    ESP_LOGW(TAG, "No free callback slots available");
}

void button_unregister_callback(button_callback_t callback) {
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (callbacks[i].in_use && callbacks[i].callback == callback) {
            callbacks[i].in_use = false;
            return;
        }
    }
}

static void button_monitor_task(void* pvParameters) {
    bool last_reading = !button_cfg.active_low;
    bool button_pressed = false;
    bool long_press_sent = false;

    // On startup, if button is already pressed, wait for it to be released first
    // This prevents triggering events for buttons held during boot
    bool current_reading = gpio_get_level(button_cfg.gpio_num);
    if (button_cfg.active_low) {
        current_reading = !current_reading;
    }
    
    if (current_reading) {
        // Button is pressed at startup - wait for release
        ESP_LOGI(TAG, "Button pressed at startup, waiting for release");
        while (current_reading) {
            vTaskDelay(pdMS_TO_TICKS(50));
            current_reading = gpio_get_level(button_cfg.gpio_num);
            if (button_cfg.active_low) {
                current_reading = !current_reading;
            }
        }
        // Button was released - send RELEASED event so callbacks know button has been released
        // This allows the power module to set button_released_since_boot = true
        notify_callbacks(BUTTON_EVENT_RELEASED);
        // Now button is released, initialize last_reading to this state
        last_reading = current_reading;
        vTaskDelay(pdMS_TO_TICKS(100));  // Small delay after release
    } else {
        // Button is not pressed at startup - mark as released so first press works
        // Send RELEASED event to indicate button has been released (it was never pressed)
        notify_callbacks(BUTTON_EVENT_RELEASED);
    }

    while (1) {
        current_reading = gpio_get_level(button_cfg.gpio_num);
        if (button_cfg.active_low) {
            current_reading = !current_reading;
        }

        if (current_reading != last_reading) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME_MS));
            current_reading = gpio_get_level(button_cfg.gpio_num);
            if (button_cfg.active_low) {
                current_reading = !current_reading;
            }
        }

        if (current_reading && !button_pressed) {
            press_start_time = xTaskGetTickCount();
            button_pressed = true;
            long_press_sent = false;
            current_state = BUTTON_PRESSED;
            notify_callbacks(BUTTON_EVENT_PRESSED);
        } else if (current_reading && button_pressed) {
            // Check for long press while button is held
            uint32_t press_duration = (xTaskGetTickCount() - press_start_time) * portTICK_PERIOD_MS;
            if (!long_press_sent && press_duration >= button_cfg.long_press_time_ms) {
                current_state = BUTTON_LONG_PRESS;
                long_press_sent = true;
                notify_callbacks(BUTTON_EVENT_LONG_PRESS);
            }
        } else if (!current_reading && button_pressed) {
            button_pressed = false;
            if (!long_press_sent) {
                TickType_t current_time = xTaskGetTickCount();
                if (first_press_registered &&
                    (current_time - last_release_time) * portTICK_PERIOD_MS < button_cfg.double_press_time_ms) {
                    current_state = BUTTON_DOUBLE_PRESS;
                    first_press_registered = false;
                    notify_callbacks(BUTTON_EVENT_DOUBLE_PRESS);
                } else {
                    first_press_registered = true;
                    last_release_time = current_time;
                }
            }
            notify_callbacks(BUTTON_EVENT_RELEASED);
            current_state = BUTTON_IDLE;
        }

        last_reading = current_reading;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t button_init(const button_config_t* config) {
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&button_cfg, config, sizeof(button_config_t));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  // Pull-down for active-high button
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    button_register_callback(default_button_handler, NULL);

    return ESP_OK;
}

button_state_t button_get_state(void) {
    return current_state;
}

uint32_t button_get_press_duration_ms(void) {
    if (current_state == BUTTON_IDLE) {
        return 0;
    }
    return (xTaskGetTickCount() - press_start_time) * portTICK_PERIOD_MS;
}

void button_start_monitoring(void) {
    xTaskCreate(button_monitor_task, "button_monitor", TASK_STACK_SIZE,
                NULL, TASK_PRIORITY, &button_task_handle);
}

static void default_button_handler(button_event_t event, void* user_data) {
    // Default handler - no action needed, handlers should be registered
    // via button_register_callback for specific functionality
    (void)event;
    (void)user_data;
}

void switch_to_screen2_callback(button_event_t event, void* user_data) {
    if (event == BUTTON_EVENT_LONG_PRESS) {
        // Switch to Screen2
        lv_disp_load_scr(objects.shutdown_screen);
    }
}