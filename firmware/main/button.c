#include "button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include <string.h>
#include <stdio.h>
#include "ui.h"
#include "lvgl.h"
#include "ui_updater.h"
#include "hw_config.h"
#include "esp_task_wdt.h"

static const char *TAG = "BUTTON";

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
static volatile bool isr_triggered = false;

// ISR handler - runs in IRAM, minimal work
static void IRAM_ATTR button_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    isr_triggered = true;
    if (button_task_handle != NULL) {
        vTaskNotifyGiveFromISR(button_task_handle, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

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

static bool read_button_state(void) {
    bool state = gpio_get_level(button_cfg.gpio_num);
    return button_cfg.active_low ? !state : state;
}

static void button_monitor_task(void* pvParameters) {
    // Register with task watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    bool button_pressed = false;
    bool long_press_sent = false;

    // On startup, if button is already pressed, wait for it to be released first
    if (read_button_state()) {
        while (read_button_state()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_task_wdt_reset();  // Keep watchdog happy while waiting
        }
        notify_callbacks(BUTTON_EVENT_RELEASED);
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        notify_callbacks(BUTTON_EVENT_RELEASED);
    }

    // Install GPIO ISR service and add handler
    gpio_install_isr_service(0);
    gpio_isr_handler_add(button_cfg.gpio_num, button_isr_handler, NULL);

    ESP_LOGI(TAG, "Button interrupt handler installed on GPIO %d", button_cfg.gpio_num);

    while (1) {
        // Wait for interrupt or timeout (for long press detection while held)
        // Use WDT_RESET_INTERVAL_MS when idle to ensure watchdog gets reset periodically
        TickType_t wait_time = button_pressed ? pdMS_TO_TICKS(LONG_PRESS_CHECK_MS) : pdMS_TO_TICKS(WDT_RESET_INTERVAL_MS);
        uint32_t notification = ulTaskNotifyTake(pdTRUE, wait_time);

        // Reset watchdog
        esp_task_wdt_reset();

        if (notification > 0 || isr_triggered) {
            // Interrupt triggered - debounce
            isr_triggered = false;
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME_MS));

            // Clear any notifications that came during debounce
            ulTaskNotifyTake(pdTRUE, 0);
            isr_triggered = false;
        }

        bool current_state_pressed = read_button_state();

        if (current_state_pressed && !button_pressed) {
            // Button just pressed
            press_start_time = xTaskGetTickCount();
            button_pressed = true;
            long_press_sent = false;
            current_state = BUTTON_PRESSED;
            notify_callbacks(BUTTON_EVENT_PRESSED);

        } else if (current_state_pressed && button_pressed) {
            // Button still held - check for long press
            uint32_t press_duration = (xTaskGetTickCount() - press_start_time) * portTICK_PERIOD_MS;
            if (!long_press_sent && press_duration >= button_cfg.long_press_time_ms) {
                current_state = BUTTON_LONG_PRESS;
                long_press_sent = true;
                notify_callbacks(BUTTON_EVENT_LONG_PRESS);
            }

        } else if (!current_state_pressed && button_pressed) {
            // Button just released
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
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE  // Interrupt on both press and release
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIO %d: %s", config->gpio_num, esp_err_to_name(ret));
        return ret;
    }

    button_register_callback(default_button_handler, NULL);

    ESP_LOGI(TAG, "Button initialized on GPIO %d (interrupt-based)", config->gpio_num);

    return ESP_OK;
}

esp_err_t button_init_main(void) {
    button_config_t config = {
        .gpio_num = MAIN_BUTTON_GPIO,
        .long_press_time_ms = BUTTON_LONG_PRESS_TIME_MS,
        .double_press_time_ms = BUTTON_DOUBLE_PRESS_TIME_MS,
        .active_low = true
    };

    return button_init(&config);
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
    (void)event;
    (void)user_data;
}

void switch_to_screen2_callback(button_event_t event, void* user_data) {
    if (event == BUTTON_EVENT_LONG_PRESS) {
        if (take_lvgl_mutex()) {
            lv_disp_load_scr(objects.shutdown_screen);
            lv_obj_invalidate(objects.shutdown_screen);
            give_lvgl_mutex();
        }
    }
}