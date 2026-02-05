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
#include <stdlib.h>
#include "throttle.h"
#include "hw_config.h"
#include "power.h"
#include "viber.h"
#include "ui.h"
#include "ui_updater.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"

static const char *TAG = "BATTERY";
static bool battery_initialized = false;
static float latest_battery_voltage = 0.0f;
static bool low_voltage_alerted = false;
static bool low_voltage_shutdown_triggered = false;

// Timing constants
#define ADC_SAMPLE_SETTLING_MS      2       // Delay between ADC samples
#define TASK_STARTUP_DELAY_MS       100     // Delay for task initialization
#define BATTERY_MONITOR_INTERVAL_MS 500     // Battery monitoring poll rate
#define LOW_BATTERY_ALERT_DELAY_MS  500     // Haptic feedback delay
#define LOW_BATTERY_WARNING_MS      2000    // Show warning before shutdown
#define POWER_OFF_SETTLE_MS         100     // Delay after power pin toggle

// Battery state of charge lookup table
typedef struct {
    float voltage;
    float soc;   // state of charge in %
} soc_point_t;

static const soc_point_t soc_table[] = {
    {4.15, 100},
    {4.10,  90},
    {3.98,  80},
    {3.85,  70},
    {3.80,  60},
    {3.75,  50},
    {3.70,  40},
    {3.65,  30},
    {3.55,  20},
    {3.45,  10},
    {3.30,   5},
    {2.80,   0}
};

#define SOC_TABLE_SIZE (sizeof(soc_table)/sizeof(soc_table[0]))

// Convert voltage to state of charge using lookup table with interpolation
static float voltage_to_soc(float v) {
    if (v >= soc_table[0].voltage) return 100.0f;
    if (v <= soc_table[SOC_TABLE_SIZE-1].voltage) return 0.0f;

    for (int i = 0; i < SOC_TABLE_SIZE - 1; i++) {
        if (v <= soc_table[i].voltage && v >= soc_table[i+1].voltage) {

            float dv = soc_table[i].voltage - soc_table[i+1].voltage;
            float dsoc = soc_table[i].soc - soc_table[i+1].soc;

            float ratio = (v - soc_table[i+1].voltage) / dv;

            return soc_table[i+1].soc + ratio * dsoc;
        }
    }
    return 0.0f;
}

static float battery_voltage_samples[BATTERY_VOLTAGE_SAMPLES] = {0};
static int battery_sample_index = 0;
static bool battery_samples_filled = false;

static void battery_monitoring_task(void *pvParameters);
static void battery_low_voltage_shutdown(void);

float battery_read_voltage(void);

esp_err_t adc_battery_init(void) {
    if (!adc_is_initialized() || !adc_get_handle()) {
        ESP_LOGE(TAG, "ADC not properly initialized");
        return ESP_FAIL;
    }

    adc_oneshot_unit_handle_t adc1_handle = adc_get_handle();

    // Configure the battery ADC channel
    adc_oneshot_chan_cfg_t battery_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12
    };

    esp_err_t ret = adc_oneshot_config_channel(adc1_handle, BATTERY_VOLTAGE_PIN, &battery_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Battery ADC channel configuration failed");
        return ret;
    }

    ESP_LOGI(TAG, "Battery ADC initialized successfully on ADC1_CH%d", BATTERY_VOLTAGE_PIN);
    return ESP_OK;
}

// Simple comparison function for qsort
static int compare_int32(const void *a, const void *b) {
    int32_t va = *(const int32_t *)a;
    int32_t vb = *(const int32_t *)b;
    return (va > vb) - (va < vb);
}

int32_t adc_read_battery_voltage(uint8_t channel) {
    if (!adc_is_initialized() || !adc_get_handle()) {
        ESP_LOGE(TAG, "ADC not properly initialized");
        return -1;
    }

    adc_oneshot_unit_handle_t adc1_handle = adc_get_handle();

    // Take multiple readings with outlier rejection
    const int NUM_SAMPLES = 10;
    const int MIN_VALID_SAMPLES = 6;  // Require at least 6/10 valid samples
    const int TRIM_COUNT = 2;  // Remove 2 highest and 2 lowest values
    int32_t samples[NUM_SAMPLES];
    int valid_samples = 0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
        int adc_raw = 0;
        esp_err_t ret = adc_oneshot_read(adc1_handle, channel, &adc_raw);

        if (ret == ESP_OK && adc_raw >= 0 && adc_raw <= 4095) {
            samples[valid_samples] = adc_raw;
            valid_samples++;
        }

        // Small delay between samples for ADC settling
        vTaskDelay(pdMS_TO_TICKS(ADC_SAMPLE_SETTLING_MS));
    }

    // Require minimum valid samples
    if (valid_samples < MIN_VALID_SAMPLES) {
        ESP_LOGW(TAG, "Insufficient valid battery ADC samples: %d/%d", valid_samples, NUM_SAMPLES);
        return -1;
    }

    // Sort samples for outlier rejection (trimmed mean)
    qsort(samples, valid_samples, sizeof(int32_t), compare_int32);

    // Calculate trimmed mean (exclude TRIM_COUNT from each end if we have enough samples)
    int start_idx = (valid_samples > 2 * TRIM_COUNT) ? TRIM_COUNT : 0;
    int end_idx = (valid_samples > 2 * TRIM_COUNT) ? valid_samples - TRIM_COUNT : valid_samples;
    int32_t sum = 0;
    int count = 0;

    for (int i = start_idx; i < end_idx; i++) {
        sum += samples[i];
        count++;
    }

    return count > 0 ? (sum / count) : -1;
}

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

    // Initialize battery probe pin as OUTPUT
    gpio_config_t probe_conf = {
        .pin_bit_mask = (1ULL << BATTERY_PROBE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ret = gpio_config(&probe_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure battery probe pin: %s", esp_err_to_name(ret));
        return ret;
    }
    // Start with probe pin LOW (disabled)
    gpio_set_level(BATTERY_PROBE_PIN, 0);
    ESP_LOGI(TAG, "Battery probe pin GPIO %d initialized", BATTERY_PROBE_PIN);

    // Initialize battery charging status GPIO as INPUT
    gpio_config_t charging_conf = {
        .pin_bit_mask = (1ULL << BATTERY_IS_CHARGING_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ret = gpio_config(&charging_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure battery charging status GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Battery charging status GPIO %d initialized", BATTERY_IS_CHARGING_GPIO);

    esp_log_level_set("gpio", ESP_LOG_WARN);

    battery_initialized = true;
    ESP_LOGI(TAG, "Battery monitoring initialized successfully for ADC1_CH%d",
            BATTERY_VOLTAGE_PIN);
    return ESP_OK;
}

void battery_start_monitoring(void) {
    xTaskCreate(battery_monitoring_task, "battery_monitor", 4096, NULL, 5, NULL);
}

float battery_read_voltage(void) {
    gpio_set_level(BATTERY_PROBE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(TASK_STARTUP_DELAY_MS));

    int32_t adc_value = adc_read_battery_voltage(BATTERY_VOLTAGE_PIN);

    gpio_set_level(BATTERY_PROBE_PIN, 0);

    if (adc_value < 0) {
        ESP_LOGW(TAG, "No valid ADC samples obtained");
        return -1.0f;
    }

    float adc_voltage = ((float)adc_value / ADC_RESOLUTION) * ADC_REFERENCE_VOLTAGE;

    float divided_voltage = adc_voltage * VOLTAGE_DIVIDER_RATIO;

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
    // Register with task watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    TickType_t last_wake_time = xTaskGetTickCount();
    int low_voltage_count = 0;
    const int LOW_VOLTAGE_CONFIRM_COUNT = 3; // Require 3 consecutive low readings (1.5 seconds)

    while (1) {
        float voltage = battery_read_voltage();

        if (voltage > 0.0f) {
            latest_battery_voltage = voltage;

            battery_voltage_samples[battery_sample_index] = voltage;
            battery_sample_index = (battery_sample_index + 1) % BATTERY_VOLTAGE_SAMPLES;

            if (battery_sample_index == 0) {
                battery_samples_filled = true;
            }

            // Check for low voltage condition
            if (voltage < BATTERY_LOW_VOLTAGE_THRESHOLD) {
                low_voltage_count++;

                // Alert user on first detection
                if (!low_voltage_alerted) {
                    ESP_LOGW(TAG, "Battery voltage low: %.2fV (threshold: %.2fV)",
                             voltage, BATTERY_LOW_VOLTAGE_THRESHOLD);
                    viber_play_pattern(VIBER_PATTERN_ALERT);
                    low_voltage_alerted = true;
                }

                // Shutdown after confirming low voltage for multiple readings
                if (low_voltage_count >= LOW_VOLTAGE_CONFIRM_COUNT) {
                    battery_low_voltage_shutdown();
                    // Should not reach here, but break just in case
                    break;
                }
            } else {
                // Voltage recovered, reset counters
                if (low_voltage_count > 0) {
                    ESP_LOGI(TAG, "Battery voltage recovered: %.2fV", voltage);
                    low_voltage_count = 0;
                    low_voltage_alerted = false;
                }
            }
        } else {
            ESP_LOGW(TAG, "Invalid battery reading");
        }

        // Reset watchdog before delay
        esp_task_wdt_reset();
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(BATTERY_MONITOR_INTERVAL_MS));
    }

    vTaskDelete(NULL);
}

int battery_get_percentage(void) {
    float voltage = latest_battery_voltage;

    if (voltage <= 0.0f) {
        return -1; // Invalid reading
    }

    // Calculate percentage using lookup table interpolation
    float soc = voltage_to_soc(voltage);
    return (int)(soc + 0.5f); // Round to nearest integer
}

bool battery_is_low_voltage(void) {
    float voltage = battery_get_voltage();
    return (voltage > 0.0f && voltage < BATTERY_LOW_VOLTAGE_THRESHOLD);
}

static void battery_low_voltage_shutdown(void) {
    if (low_voltage_shutdown_triggered) {
        return; // Already triggered shutdown
    }

    low_voltage_shutdown_triggered = true;

    // Alert user with haptic feedback
    viber_play_pattern(VIBER_PATTERN_ALERT);
    vTaskDelay(pdMS_TO_TICKS(LOW_BATTERY_ALERT_DELAY_MS));

    // Show warning on shutdown screen if possible
    if (take_lvgl_mutex()) {
        if (objects.low_battery_screen != NULL) {
            lv_disp_load_scr(objects.low_battery_screen);
            lv_obj_invalidate(objects.low_battery_screen);
        }
        give_lvgl_mutex();
    }

    // Give user a moment to see the warning
    vTaskDelay(pdMS_TO_TICKS(LOW_BATTERY_WARNING_MS));

    // Turn off power hold pin to prevent over-discharge
    ESP_LOGI(TAG, "Turning off power hold pin to prevent battery over-discharge");
    gpio_set_level(POWER_HOLD_GPIO, 0);

    // Small delay to ensure power is cut
    vTaskDelay(pdMS_TO_TICKS(POWER_OFF_SETTLE_MS));

    // Force deep sleep as final safety measure
    esp_deep_sleep_start();
}
