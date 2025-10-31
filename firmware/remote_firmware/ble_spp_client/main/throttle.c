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
#include "sleep.h"
#include "ble_spp_client.h"

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
static bool calibration_done = false;
static bool calibration_in_progress = false;
static esp_err_t load_calibration_from_nvs(void);

// Add this function prototype
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

    adc_initialized = true;
    return ESP_OK;
}

int32_t adc_read_value(void)
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

static void adc_task(void *pvParameters) {
    uint32_t last_value = 0;
    const uint32_t CHANGE_THRESHOLD = 2; // Adjust this threshold as needed

    while (1) {
        uint32_t adc_value = adc_read_value();
        if (adc_value == -1) {
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

        uint8_t mapped_value = map_adc_value(adc_value);
        latest_adc_value = mapped_value;
        if(!is_connect){
            // Only monitor value changes and reset timer when BLE is not connected
            if (abs((int32_t)mapped_value - (int32_t)last_value) > CHANGE_THRESHOLD) {
                sleep_reset_inactivity_timer();
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

    // Add delay after initialization
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


// Add this function to get the latest ADC value
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

    // Read calibration values
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

    // Save calibration values
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
    ESP_LOGI(TAG, "Please move throttle through full range during the next 6 seconds");

    // Set calibration in progress flag BEFORE any early returns
    calibration_in_progress = true;

    // Clear existing calibration from NVS to force new calibration
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_key(nvs_handle, NVS_KEY_CALIBRATED);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    uint32_t min_value = UINT32_MAX;
    uint32_t max_value = 0;
    int progress = 0;
    int last_reported_progress = -1;

    // Take multiple samples to find the actual range
    for (int i = 0; i < ADC_CALIBRATION_SAMPLES; i++) {
        int32_t value = adc_read_value();
        if (value != -1) {  // Valid reading
            if (value < min_value) min_value = value;
            if (value > max_value) max_value = value;
        }

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

    // Only update if we got valid readings
    if (min_value != UINT32_MAX && max_value != 0) {
        uint32_t range = max_value - min_value;
        
        // Check if the range is sufficient (at least 150 ADC units)
        if (range < 150) {
            ESP_LOGE(TAG, "ADC calibration failed - insufficient range: %lu (minimum required: 150)", range);
            printf("Calibration failed - insufficient throttle movement detected!\n");
            printf("Range detected: %lu ADC units (minimum required: 150)\n", range);
            printf("Please move the throttle through its FULL range and try again.\n");
            calibration_done = false;
        } else {
            // Add small margins to prevent edge cases (5% margin)
            adc_input_min_value = min_value + (range * 0.05);
            adc_input_max_value = max_value - (range * 0.05);

            calibration_done = true;

            ESP_LOGI(TAG, "ADC calibration complete:");
            ESP_LOGI(TAG, "Raw min value: %lu", min_value);
            ESP_LOGI(TAG, "Raw max value: %lu", max_value); 
            ESP_LOGI(TAG, "Calibrated min value: %lu", adc_input_min_value);
            ESP_LOGI(TAG, "Calibrated max value: %lu", adc_input_max_value);
            
            printf("Calibration complete!\n");
            printf("Raw range: %lu - %lu\n", min_value, max_value);
            printf("Calibrated range: %lu - %lu\n", adc_input_min_value, adc_input_max_value);
        }
    } else {
        ESP_LOGE(TAG, "ADC calibration failed - invalid readings");
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

void adc_get_calibration_values(uint32_t *min_val, uint32_t *max_val) {
    if (min_val) *min_val = adc_input_min_value;
    if (max_val) *max_val = adc_input_max_value;
}

bool adc_get_calibration_status(void) {
    return calibration_done;
}

bool adc_is_calibrating(void) {
    return calibration_in_progress;
}

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
           (ADC_OUTPUT_MAX_VALUE - ADC_OUTPUT_MIN_VALUE - ADC_THROTTLE_OFFSET) /
           (adc_input_max_value - adc_input_min_value) +
           ADC_OUTPUT_MIN_VALUE);

    // Add offset only to non-zero values to maintain 0 at minimum
    if (mapped > 0) {
        mapped += ADC_THROTTLE_OFFSET;
    }
    
    return mapped;
}

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

    esp_err_t ret = adc_oneshot_config_channel(adc1_handle, BATTERY_PIN, &battery_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Battery ADC channel configuration failed");
        return ret;
    }

    ESP_LOGI(TAG, "Battery ADC initialized successfully on ADC1_CH%d", BATTERY_PIN);
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