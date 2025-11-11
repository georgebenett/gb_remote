#include "throttle.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdint.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "target_config.h"
#include "ble.h"
#include "power.h"

static const char *TAG = "ADC";
static adc_oneshot_unit_handle_t adc1_handle;
static adc_oneshot_unit_init_cfg_t init_config1;
static adc_oneshot_chan_cfg_t config;
static QueueHandle_t adc_display_queue = NULL;
static uint32_t latest_adc_value = 0;
static bool adc_initialized = false;
static int error_count = 0;
static const int MAX_ERRORS = 5;
static uint32_t adc_input_max_value = ADC_INITIAL_MAX_VALUE;
static uint32_t adc_input_min_value = ADC_INITIAL_MIN_VALUE;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
static uint32_t brake_input_max_value = ADC_INITIAL_MAX_VALUE;
static uint32_t brake_input_min_value = ADC_INITIAL_MIN_VALUE;
#endif
static bool calibration_done = false;
static bool calibration_in_progress = false;
static esp_err_t load_calibration_from_nvs(void);

void adc_deinit(void);

esp_err_t adc_init(void)
{
    if (adc_initialized) {
        ESP_LOGI(TAG, "ADC already initialized");
        return ESP_OK;
    }

    esp_err_t ret;

    // Create queue first
    adc_display_queue = xQueueCreate(10, sizeof(uint32_t));
    if (adc_display_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue");
        return ESP_FAIL;
    }

    // ADC1 init configuration
    init_config1.unit_id = ADC_UNIT_1;
    init_config1.ulp_mode = ADC_ULP_MODE_DISABLE;
    ret = adc_oneshot_new_unit(&init_config1, &adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit initialization failed");
        return ret;
    }

    // Channel configuration
    config.atten = ADC_ATTEN_DB_12;
    config.bitwidth = ADC_BITWIDTH_12;
    ret = adc_oneshot_config_channel(adc1_handle, THROTTLE_PIN, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel configuration failed");
        return ret;
    }

#ifdef CONFIG_TARGET_DUAL_THROTTLE
    // Configure brake channel (dual_throttle only)
    ret = adc_oneshot_config_channel(adc1_handle, BREAK_PIN, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Brake ADC channel configuration failed");
        return ret;
    }
#endif

    adc_initialized = true;
    return ESP_OK;
}

int32_t throttle_read_value(void)
{
    if (!adc_initialized || !adc1_handle) {
        ESP_LOGE(TAG, "ADC not properly initialized");
        return -1;
    }

    // Take multiple readings and average
    const int NUM_SAMPLES = 5;
    int32_t sum = 0;
    int valid_samples = 0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
        int adc_raw = 0;
        esp_err_t ret = adc_oneshot_read(adc1_handle, THROTTLE_PIN, &adc_raw);

        if (ret == ESP_OK) {
            sum += adc_raw;
            valid_samples++;
        }

        // Small delay between samples
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return valid_samples > 0 ? (sum / valid_samples) : -1;
}

#ifdef CONFIG_TARGET_DUAL_THROTTLE
int32_t brake_read_value(void)
{
    if (!adc_initialized || !adc1_handle) {
        ESP_LOGE(TAG, "ADC not properly initialized");
        return -1;
    }

    // Take multiple readings and average
    const int NUM_SAMPLES = 5;
    int32_t sum = 0;
    int valid_samples = 0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
        int adc_raw = 0;
        esp_err_t ret = adc_oneshot_read(adc1_handle, BREAK_PIN, &adc_raw);

        if (ret == ESP_OK) {
            sum += adc_raw;
            valid_samples++;
        }

        // Small delay between samples
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return valid_samples > 0 ? (sum / valid_samples) : -1;
}
#endif

static void adc_task(void *pvParameters) {
    uint32_t last_value = 0;
    const uint32_t CHANGE_THRESHOLD = 2; // Adjust this threshold as needed

    while (1) {
    int32_t adc_raw;
    adc_raw = throttle_read_value();

#ifdef CONFIG_TARGET_LITE
    uint32_t adc_value = (adc_raw >= 0) ? (uint32_t)adc_raw : 0;
#endif

        if (adc_raw < 0) {
            error_count++;
            if (error_count >= MAX_ERRORS) {
                ESP_LOGE(TAG, "Too many ADC errors, attempting re-initialization");
                adc_deinit();
                vTaskDelay(pdMS_TO_TICKS(100));
                if (adc_init() == ESP_OK) {
                    error_count = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));  // Wait before retry
            continue;
        }
        error_count = 0;  // Reset error count on successful read

#ifdef CONFIG_TARGET_DUAL_THROTTLE
        // Calculate combined throttle/brake BLE value (dual_throttle mode)
        uint8_t mapped_value = get_throttle_brake_ble_value();
#elif defined(CONFIG_TARGET_LITE)
        // Single throttle mapping (lite mode)
        uint8_t mapped_value = map_adc_value(adc_value);
#endif
        latest_adc_value = mapped_value;
        if(!is_connect){
            // Only monitor value changes and reset timer when BLE is not connected
            if (abs((int32_t)mapped_value - (int32_t)last_value) > CHANGE_THRESHOLD) {
                power_reset_inactivity_timer();
                last_value = mapped_value;
            }
        }

        xQueueSend(adc_display_queue, &mapped_value, 0);
        vTaskDelay(pdMS_TO_TICKS(ADC_SAMPLING_TICKS));
    }
}

void adc_start_task(void) {
    esp_err_t ret = adc_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC initialization failed, not starting task");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

#if CALIBRATE_THROTTLE
    ESP_LOGI(TAG, "Force calibration flag set, performing calibration");
    // Clear existing calibration
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_key(nvs_handle, NVS_KEY_CALIBRATED);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    throttle_calibrate();
#else
    // Only calibrate if no valid calibration exists
    if (load_calibration_from_nvs() != ESP_OK) {
        throttle_calibrate();
    }
#endif

    xTaskCreate(adc_task, "adc_task", 4096, NULL, 10, NULL);
}


uint32_t adc_get_latest_value(void) {
    return latest_adc_value;
}

void adc_deinit(void)
{
    if (!adc_initialized) {
        return;
    }

    if (adc1_handle) {
        adc_oneshot_del_unit(adc1_handle);
        adc1_handle = NULL;
    }

    if (adc_display_queue) {
        vQueueDelete(adc_display_queue);
        adc_display_queue = NULL;
    }

    adc_initialized = false;
}

static esp_err_t load_calibration_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    // Try to read calibration flag
    uint8_t is_calibrated = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY_CALIBRATED, &is_calibrated);
    if (err != ESP_OK || !is_calibrated) {
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }

    // Read throttle calibration values
    err = nvs_get_u32(nvs_handle, NVS_KEY_MIN, &adc_input_min_value);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_get_u32(nvs_handle, NVS_KEY_MAX, &adc_input_max_value);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

#ifdef CONFIG_TARGET_DUAL_THROTTLE
    // Read brake calibration values (if available, use defaults otherwise)
    err = nvs_get_u32(nvs_handle, NVS_KEY_BRAKE_MIN, &brake_input_min_value);
    if (err != ESP_OK) {
        brake_input_min_value = ADC_INITIAL_MIN_VALUE;
    }

    err = nvs_get_u32(nvs_handle, NVS_KEY_BRAKE_MAX, &brake_input_max_value);
    if (err != ESP_OK) {
        brake_input_max_value = ADC_INITIAL_MAX_VALUE;
    }
#endif

    nvs_close(nvs_handle);
    calibration_done = true;
    return ESP_OK;
}

static esp_err_t save_calibration_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    // Save throttle calibration values
    err = nvs_set_u32(nvs_handle, NVS_KEY_MIN, adc_input_min_value);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_u32(nvs_handle, NVS_KEY_MAX, adc_input_max_value);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

#ifdef CONFIG_TARGET_DUAL_THROTTLE
    // Save brake calibration values
    err = nvs_set_u32(nvs_handle, NVS_KEY_BRAKE_MIN, brake_input_min_value);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_u32(nvs_handle, NVS_KEY_BRAKE_MAX, brake_input_max_value);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
#endif

    // Set calibration flag
    err = nvs_set_u8(nvs_handle, NVS_KEY_CALIBRATED, 1);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return err;
}

void throttle_calibrate(void) {
    ESP_LOGI(TAG, "Starting ADC calibration...");
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    ESP_LOGI(TAG, "Please move throttle and brake through full range during the next 6 seconds");
#elif defined(CONFIG_TARGET_LITE)
    ESP_LOGI(TAG, "Please move throttle through full range during the next 6 seconds");
#endif

    // Set calibration in progress flag BEFORE any early returns
    calibration_in_progress = true;

    // Clear existing calibration from NVS to force new calibration
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_key(nvs_handle, NVS_KEY_CALIBRATED);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    uint32_t throttle_min = UINT32_MAX;
    uint32_t throttle_max = 0;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    uint32_t brake_min = UINT32_MAX;
    uint32_t brake_max = 0;
#endif
    int progress = 0;
    int last_reported_progress = -1;

    // Take multiple samples to find the actual range
    for (int i = 0; i < ADC_CALIBRATION_SAMPLES; i++) {
#ifdef CONFIG_TARGET_DUAL_THROTTLE
        int32_t throttle_value = throttle_read_value();
        int32_t brake_value = brake_read_value();

        if (throttle_value != -1) {  // Valid reading
            if (throttle_value < throttle_min) throttle_min = throttle_value;
            if (throttle_value > throttle_max) throttle_max = throttle_value;
        }

        if (brake_value != -1) {  // Valid reading
            if (brake_value < brake_min) brake_min = brake_value;
            if (brake_value > brake_max) brake_max = brake_value;
        }
#elif defined(CONFIG_TARGET_LITE)
        int32_t value = throttle_read_value();
        if (value != -1) {  // Valid reading
            if (value < throttle_min) throttle_min = value;
            if (value > throttle_max) throttle_max = value;
        }
#endif

        // Calculate and report progress every 10%
        progress = (i * 100) / ADC_CALIBRATION_SAMPLES;
        if (progress % 10 == 0 && progress != last_reported_progress) {
            ESP_LOGI(TAG, "Calibration progress: %d%%", progress);
            printf("Calibration progress: %d%%\n", progress);
            last_reported_progress = progress;
        }

        // Use a longer delay to ensure 6 seconds total
        vTaskDelay(pdMS_TO_TICKS(ADC_CALIBRATION_DELAY_MS));
    }

    // Clear calibration in progress flag
    calibration_in_progress = false;

    bool throttle_valid = (throttle_min != UINT32_MAX && throttle_max != 0);
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    bool brake_valid = (brake_min != UINT32_MAX && brake_max != 0);
#endif

    // Calibrate throttle
    if (throttle_valid) {
        uint32_t throttle_range = throttle_max - throttle_min;

        // Check if the range is sufficient (at least 150 ADC units)
        if (throttle_range < 150) {
            ESP_LOGE(TAG, "Throttle calibration failed - insufficient range: %lu (minimum required: 150)", throttle_range);
            printf("Throttle calibration failed - insufficient movement detected!\n");
        } else {
            // Add small margins to prevent edge cases (5% margin)
            adc_input_min_value = throttle_min + (throttle_range * 0.05);
            adc_input_max_value = throttle_max - (throttle_range * 0.05);

            ESP_LOGI(TAG, "Throttle calibration complete:");
            ESP_LOGI(TAG, "Raw min value: %lu", throttle_min);
            ESP_LOGI(TAG, "Raw max value: %lu", throttle_max);
            ESP_LOGI(TAG, "Calibrated min value: %lu", adc_input_min_value);
            ESP_LOGI(TAG, "Calibrated max value: %lu", adc_input_max_value);
        }
    } else {
        ESP_LOGE(TAG, "Throttle calibration failed - invalid readings");
    }

#ifdef CONFIG_TARGET_DUAL_THROTTLE
    // Calibrate brake (dual_throttle only)
    if (brake_valid) {
        uint32_t brake_range = brake_max - brake_min;

        // Check if the range is sufficient (at least 150 ADC units)
        if (brake_range < 150) {
            ESP_LOGE(TAG, "Brake calibration failed - insufficient range: %lu (minimum required: 150)", brake_range);
            printf("Brake calibration failed - insufficient movement detected!\n");
        } else {
            // Add small margins to prevent edge cases (5% margin)
            brake_input_min_value = brake_min + (brake_range * 0.05);
            brake_input_max_value = brake_max - (brake_range * 0.05);

            ESP_LOGI(TAG, "Brake calibration complete:");
            ESP_LOGI(TAG, "Raw min value: %lu", brake_min);
            ESP_LOGI(TAG, "Raw max value: %lu", brake_max);
            ESP_LOGI(TAG, "Calibrated min value: %lu", brake_input_min_value);
            ESP_LOGI(TAG, "Calibrated max value: %lu", brake_input_max_value);
        }
    } else {
        ESP_LOGE(TAG, "Brake calibration failed - invalid readings");
    }
#endif

    // Mark calibration as done if at least throttle is valid
    if (throttle_valid) {
        calibration_done = true;
        printf("Calibration complete!\n");
        if (throttle_valid) {
            printf("Throttle range: %lu - %lu\n", throttle_min, throttle_max);
        }
#ifdef CONFIG_TARGET_DUAL_THROTTLE
        if (brake_valid) {
            printf("Brake range: %lu - %lu\n", brake_min, brake_max);
        }
#endif
    } else {
        calibration_done = false;
        ESP_LOGE(TAG, "ADC calibration failed");
        printf("Calibration failed - no valid readings detected\n");
    }

    // Save calibration to NVS
    if (save_calibration_to_nvs() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save calibration to NVS");
        printf("Warning: Failed to save calibration to memory\n");
    }

    // After successful calibration, save to NVS
    if (calibration_done) {
        if (save_calibration_to_nvs() == ESP_OK) {
            ESP_LOGI(TAG, "Calibration saved to NVS");
            printf("Calibration saved to memory successfully\n");
        } else {
            ESP_LOGE(TAG, "Failed to save calibration to NVS");
            printf("Warning: Failed to save calibration to memory\n");
        }
    }
}

bool throttle_is_calibrated(void) {
    return calibration_done;
}

void throttle_get_calibration_values(uint32_t *min_val, uint32_t *max_val) {
    if (min_val) *min_val = adc_input_min_value;
    if (max_val) *max_val = adc_input_max_value;
}

#ifdef CONFIG_TARGET_DUAL_THROTTLE
void brake_get_calibration_values(uint32_t *min_val, uint32_t *max_val) {
    if (min_val) *min_val = brake_input_min_value;
    if (max_val) *max_val = brake_input_max_value;
}
#endif

bool adc_get_calibration_status(void) {
    return calibration_done;
}

bool adc_is_calibrating(void) {
    return calibration_in_progress;
}

uint8_t map_throttle_value(uint32_t adc_value) {
    // Constrain input value to the calibrated range
    if (adc_value < adc_input_min_value) {
        adc_value = adc_input_min_value;
    }
    if (adc_value > adc_input_max_value) {
        adc_value = adc_input_max_value;
    }

    // Perform the mapping
    uint8_t mapped = (uint8_t)((adc_value - adc_input_min_value) *
           (ADC_OUTPUT_MAX_VALUE - ADC_OUTPUT_MIN_VALUE) /
           (adc_input_max_value - adc_input_min_value) +
           ADC_OUTPUT_MIN_VALUE);

    return mapped;
}

#ifdef CONFIG_TARGET_DUAL_THROTTLE
uint8_t map_brake_value(uint32_t adc_value) {
    // Constrain input value to the calibrated range
    if (adc_value < brake_input_min_value) {
        adc_value = brake_input_min_value;
    }
    if (adc_value > brake_input_max_value) {
        adc_value = brake_input_max_value;
    }

    // Perform the mapping (no offset for brake)
    uint8_t mapped = (uint8_t)((adc_value - brake_input_min_value) *
           (ADC_OUTPUT_MAX_VALUE - ADC_OUTPUT_MIN_VALUE) /
           (brake_input_max_value - brake_input_min_value) +
           ADC_OUTPUT_MIN_VALUE);

    return mapped;
}
#endif

#ifdef CONFIG_TARGET_DUAL_THROTTLE
uint8_t get_throttle_brake_ble_value(void) {
    // Neutral value when not calibrated
    if (!calibration_done || calibration_in_progress) {
        return 127;
    }

    // Read current throttle and brake values
    int32_t throttle_raw = throttle_read_value();
    int32_t brake_raw = brake_read_value();

    if (throttle_raw < 0 || brake_raw < 0) {
        return 127;  // Return neutral on error
    }

    // Constrain values to calibrated ranges
    if (throttle_raw < adc_input_min_value) throttle_raw = adc_input_min_value;
    if (throttle_raw > adc_input_max_value) throttle_raw = adc_input_max_value;
    if (brake_raw < brake_input_min_value) brake_raw = brake_input_min_value;
    if (brake_raw > brake_input_max_value) brake_raw = brake_input_max_value;

    uint32_t brake_range = brake_input_max_value - brake_input_min_value;
    uint32_t throttle_range = adc_input_max_value - adc_input_min_value;

    if (brake_range == 0 || throttle_range == 0) {
        return 127;  // Avoid division by zero
    }

    // Calculate brake factor: 0.0 at MIN, 1.0 at MAX
    float brake_factor = (float)(brake_raw - brake_input_min_value) / (float)brake_range;

    // Brake overrides throttle: when brake is at MIN, BLE = 0
    // When brake is at MIN (factor=0): brake forces BLE to 0
    // When brake is at MAX (factor=1): brake allows throttle control
    if (brake_factor < 0.01f) {
        // Brake at MIN: brake overrides, BLE = 0
        return 0;
    }

    // Brake is at MAX (or close to MAX): throttle controls BLE value
    // When throttle at MAX: BLE = 127
    // When throttle at MIN: BLE = 255
    float throttle_factor = (float)(throttle_raw - adc_input_min_value) / (float)throttle_range;

    // Invert throttle mapping: throttle MAX (factor=1.0) = 127, throttle MIN (factor=0.0) = 255
    uint8_t ble_value = 127 + (uint8_t)((1.0f - throttle_factor) * 128.0f);

    // If brake is not fully at MAX, interpolate between brake override (0-127) and throttle control (127-255)
    if (brake_factor < 1.0f) {
        // When brake moves from MIN to MAX, it transitions from override (0) to allowing throttle
        // Brake at MAX: use throttle value (127-255)
        // Brake between MIN and MAX: interpolate between 0 and throttle value
        uint8_t brake_override_value = 0;  // When brake at MIN
        float interpolated = brake_override_value + (brake_factor * (float)ble_value);
        ble_value = (uint8_t)interpolated;
    }

    return ble_value;
}
#endif

#ifdef CONFIG_TARGET_LITE
// Lite mode: single throttle mapping function
uint8_t map_adc_value(uint32_t adc_value) {
    // Constrain input value to the calibrated range
    if (adc_value < adc_input_min_value) {
        adc_value = adc_input_min_value;
    }
    if (adc_value > adc_input_max_value) {
        adc_value = adc_input_max_value;
    }

    // Perform the mapping
    uint8_t mapped = (uint8_t)((adc_value - adc_input_min_value) *
           (ADC_OUTPUT_MAX_VALUE - ADC_OUTPUT_MIN_VALUE) /
           (adc_input_max_value - adc_input_min_value) +
           ADC_OUTPUT_MIN_VALUE);

    return mapped;
}
#endif

esp_err_t adc_battery_init(void) {
    if (!adc_initialized || !adc1_handle) {
        ESP_LOGE(TAG, "ADC not properly initialized");
        return ESP_FAIL;
    }

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

int32_t adc_read_battery_voltage(uint8_t channel) {
    if (!adc_initialized || !adc1_handle) {
        ESP_LOGE(TAG, "ADC not properly initialized");
        return -1;
    }

    // Take multiple readings and average
    const int NUM_SAMPLES = 10;
    int32_t sum = 0;
    int valid_samples = 0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
        int adc_raw = 0;
        esp_err_t ret = adc_oneshot_read(adc1_handle, channel, &adc_raw);

        if (ret == ESP_OK) {
            sum += adc_raw;
            valid_samples++;
        }

        // Small delay between samples
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return valid_samples > 0 ? (sum / valid_samples) : -1;
}