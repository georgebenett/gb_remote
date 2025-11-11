#include "viber.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hw_config.h"

#define TAG "VIBER"

// Predefined pattern durations (in ms)
#define VERY_SHORT_DURATION 30
#define SHORT_DURATION 60
#define LONG_DURATION  300
#define PAUSE_DURATION 100

static bool viber_initialized = false;

// Task parameters for vibration execution
typedef struct {
    const uint32_t* durations;
    uint8_t count;
    volatile bool is_running;
} viber_task_params_t;

static viber_task_params_t task_params = {0};

static void viber_task(void *pvParameters);

esp_err_t viber_init(void) {
    if (viber_initialized) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << VIBER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(VIBER_PIN, 0);

    task_params.is_running = false;
    task_params.durations = NULL;
    task_params.count = 0;

    xTaskCreate(viber_task, "viber_task", 2048, NULL, 2, NULL);

    viber_initialized = true;
    ESP_LOGI(TAG, "Viber initialized on GPIO %d", VIBER_PIN);
    return ESP_OK;
}

esp_err_t viber_play_pattern(viber_pattern_t pattern) {
    static const uint32_t pattern_very_short[] = {VERY_SHORT_DURATION};
    static const uint32_t pattern_single_short[] = {SHORT_DURATION};
    static const uint32_t pattern_single_long[] = {LONG_DURATION};
    static const uint32_t pattern_double_short[] = {SHORT_DURATION, PAUSE_DURATION, SHORT_DURATION};
    static const uint32_t pattern_success[] = {SHORT_DURATION, PAUSE_DURATION, LONG_DURATION};
    static const uint32_t pattern_error[] = {SHORT_DURATION, PAUSE_DURATION, SHORT_DURATION,
                                           PAUSE_DURATION, SHORT_DURATION};
    static const uint32_t pattern_alert[] = {LONG_DURATION, PAUSE_DURATION, SHORT_DURATION,
                                           PAUSE_DURATION, LONG_DURATION};

    const uint32_t* durations;
    uint8_t count;

    switch (pattern) {
        case VIBER_PATTERN_VERY_SHORT:
            durations = pattern_very_short;
            count = sizeof(pattern_very_short) / sizeof(pattern_very_short[0]);
            break;
        case VIBER_PATTERN_SINGLE_SHORT:
            durations = pattern_single_short;
            count = sizeof(pattern_single_short) / sizeof(pattern_single_short[0]);
            break;
        case VIBER_PATTERN_SINGLE_LONG:
            durations = pattern_single_long;
            count = sizeof(pattern_single_long) / sizeof(pattern_single_long[0]);
            break;
        case VIBER_PATTERN_DOUBLE_SHORT:
            durations = pattern_double_short;
            count = sizeof(pattern_double_short) / sizeof(pattern_double_short[0]);
            break;
        case VIBER_PATTERN_SUCCESS:
            durations = pattern_success;
            count = sizeof(pattern_success) / sizeof(pattern_success[0]);
            break;
        case VIBER_PATTERN_ERROR:
            durations = pattern_error;
            count = sizeof(pattern_error) / sizeof(pattern_error[0]);
            break;
        case VIBER_PATTERN_ALERT:
            durations = pattern_alert;
            count = sizeof(pattern_alert) / sizeof(pattern_alert[0]);
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return viber_custom_pattern(durations, count);
}

esp_err_t viber_vibrate(uint32_t duration_ms) {
    return viber_custom_pattern(&duration_ms, 1);
}

esp_err_t viber_custom_pattern(const uint32_t* durations, uint8_t count) {
    if (!viber_initialized || !durations || count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    viber_stop();
    vTaskDelay(pdMS_TO_TICKS(10));  // Small delay to ensure previous pattern is stopped

    task_params.durations = durations;
    task_params.count = count;
    task_params.is_running = true;

    return ESP_OK;
}

esp_err_t viber_stop(void) {
    if (!viber_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    task_params.is_running = false;
    gpio_set_level(VIBER_PIN, 0);
    return ESP_OK;
}

static void viber_task(void *pvParameters) {
    while (1) {
        if (task_params.is_running && task_params.durations && task_params.count > 0) {
            for (uint8_t i = 0; i < task_params.count; i++) {
                if (!task_params.is_running) {
                    break;
                }

                if (i % 2 == 0) {
                    // Even indices: vibration
                    gpio_set_level(VIBER_PIN, 1);
                    vTaskDelay(pdMS_TO_TICKS(task_params.durations[i]));
                    gpio_set_level(VIBER_PIN, 0);
                } else {
                    // Odd indices: pause
                    vTaskDelay(pdMS_TO_TICKS(task_params.durations[i]));
                }
            }
            task_params.is_running = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}