#include "battery.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdint.h>
#include "adc.h" // Include ADC header to access its functions

static const char *TAG = "BATTERY";
static bool battery_initialized = false;
static float latest_battery_voltage = 0.0f;

static float battery_voltage_samples[BATTERY_VOLTAGE_SAMPLES] = {0};
static int battery_sample_index = 0;
static bool battery_samples_filled = false;

// Battery monitoring task
static void battery_monitoring_task(void *pvParameters);

// Function to read battery voltage using the existing ADC
float battery_read_voltage(void);

esp_err_t battery_init(void) {
    if (battery_initialized) {
        ESP_LOGI(TAG, "Battery monitoring already initialized");
        return ESP_OK;
    }

    // Initialize the battery ADC
    esp_err_t ret = adc_battery_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize battery ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    // Reduce GPIO log verbosity to avoid seeing configuration messages
    esp_log_level_set("gpio", ESP_LOG_WARN);

    battery_initialized = true;
    ESP_LOGI(TAG, "Battery monitoring initialized successfully for ADC1_CH%d",
            BATTERY_ADC_CHANNEL);
    return ESP_OK;
}

void battery_start_monitoring(void) {
    xTaskCreate(battery_monitoring_task, "battery_monitor", 4096, NULL, 5, NULL);
}

float battery_read_voltage(void) {
    int32_t adc_value = adc_read_battery_voltage(BATTERY_ADC_CHANNEL);

    if (adc_value < 0) {
        ESP_LOGW(TAG, "No valid ADC samples obtained");
        return -1.0f;
    }

    // Calculate intermediate values for debugging
    float adc_voltage = ((float)adc_value / ADC_RESOLUTION) * ADC_REFERENCE_VOLTAGE;

    // For a 10K-10K voltage divider, the actual battery voltage is 2x the measured voltage
    float divided_voltage = adc_voltage * VOLTAGE_DIVIDER_RATIO;

    // Apply calibration factor from battery.h
    float calibrated_voltage = divided_voltage * BATTERY_VOLTAGE_SCALE + BATTERY_VOLTAGE_OFFSET;

    return calibrated_voltage;
}

float battery_get_voltage(void) {
    if (!battery_samples_filled && battery_sample_index == 0) {
        return latest_battery_voltage;
    }

    // Calculate average of samples
    float sum = 0.0f;
    int count = battery_samples_filled ? BATTERY_VOLTAGE_SAMPLES : battery_sample_index;

    for (int i = 0; i < count; i++) {
        sum += battery_voltage_samples[i];
    }

    return sum / count;
}

static void battery_monitoring_task(void *pvParameters) {
    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) {
        float voltage = battery_read_voltage();

        if (voltage > 0.0f) {
            latest_battery_voltage = voltage;

            // Add to rolling average
            battery_voltage_samples[battery_sample_index] = voltage;
            battery_sample_index = (battery_sample_index + 1) % BATTERY_VOLTAGE_SAMPLES;

            if (battery_sample_index == 0) {
                battery_samples_filled = true;
            }
        } else {
            ESP_LOGW(TAG, "Invalid battery reading");
        }

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(500));
    }

    vTaskDelete(NULL);
}

int battery_get_percentage(void) {
    float voltage = latest_battery_voltage;

    if (voltage <= 0.0f) {
        return -1; // Invalid reading
    }

    // Calculate percentage based on voltage range
    if (voltage >= BATTERY_MAX_VOLTAGE) {
        return 100;
    } else if (voltage <= BATTERY_MIN_VOLTAGE) {
        return 0;
    } else {
        // Linear mapping from voltage to percentage
        return (int)((voltage - BATTERY_MIN_VOLTAGE) /
                     (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE) * 100.0f);
    }
}
