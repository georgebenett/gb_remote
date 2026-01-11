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
#include "vesc_config.h"
#include "esp_task_wdt.h"

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

// Getter function for battery module to access ADC handle
adc_oneshot_unit_handle_t adc_get_handle(void) {
    return adc1_handle;
}

bool adc_is_initialized(void) {
    return adc_initialized;
}

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
    // Register with task watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    uint32_t last_value = 0;
    const uint32_t CHANGE_THRESHOLD = 2;

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
            latest_adc_value = VESC_NEUTRAL_VALUE;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        error_count = 0;  // Reset error count on successful read

#ifdef CONFIG_TARGET_DUAL_THROTTLE
        // Calculate combined throttle/brake BLE value (dual_throttle mode)
        uint8_t mapped_value = get_throttle_brake_ble_value();
#elif defined(CONFIG_TARGET_LITE)
        // Single throttle mapping (lite mode)
        uint8_t mapped_value = map_adc_value(adc_value);
        // Update latest_adc_value for BLE transmission
        latest_adc_value = mapped_value;
#endif

        if(!is_connect){
            // Only monitor value changes and reset timer when BLE is not connected
            if (abs((int32_t)mapped_value - (int32_t)last_value) > CHANGE_THRESHOLD) {
                power_reset_inactivity_timer();
                last_value = mapped_value;
            }
        }

        if (xQueueSend(adc_display_queue, &mapped_value, 0) != pdTRUE) {
            uint32_t dummy;
            xQueueReceive(adc_display_queue, &dummy, 0);
            xQueueSend(adc_display_queue, &mapped_value, 0);
        }

        // Reset watchdog before delay
        esp_task_wdt_reset();
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

bool throttle_calibrate(void) {
    ESP_LOGI(TAG, "Starting ADC calibration...");

    calibration_in_progress = true;

    // Save the previous calibration state so we can restore it if calibration fails
    bool had_previous_calibration = calibration_done;

    uint32_t throttle_min = UINT32_MAX;
    uint32_t throttle_max = 0;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    uint32_t brake_min = UINT32_MAX;
    uint32_t brake_max = 0;
#endif

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

        // Use a longer delay to ensure 6 seconds total
        vTaskDelay(pdMS_TO_TICKS(ADC_CALIBRATION_DELAY_MS));
    }

    // Clear calibration in progress flag
    calibration_in_progress = false;

    bool throttle_valid = (throttle_min != UINT32_MAX && throttle_max != 0);
    bool throttle_range_ok = false;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    bool brake_valid = (brake_min != UINT32_MAX && brake_max != 0);
    bool brake_range_ok = false;
#endif

    // Calibrate throttle
    if (throttle_valid) {
        uint32_t throttle_range = throttle_max - throttle_min;

        if (throttle_range < 150) {
            ESP_LOGW(TAG, "Throttle: insufficient range %lu (need 150+)", throttle_range);
        } else {
            throttle_range_ok = true;
            // Use measured values directly without margins
            adc_input_min_value = throttle_min;
            adc_input_max_value = throttle_max;
            ESP_LOGI(TAG, "Throttle calibrated: %lu - %lu", adc_input_min_value, adc_input_max_value);
        }
    } else {
        ESP_LOGE(TAG, "Throttle: no valid readings");
    }

#ifdef CONFIG_TARGET_DUAL_THROTTLE
    // Calibrate brake (dual_throttle only)
    if (brake_valid) {
        uint32_t brake_range = brake_max - brake_min;

        // Check if the range is sufficient (at least 150 ADC units)
        if (brake_range < 150) {
            ESP_LOGW(TAG, "Brake: insufficient range %lu (need 150+)", brake_range);
        } else {
            brake_range_ok = true;
            // Use measured values directly without margins
            brake_input_min_value = brake_min;
            brake_input_max_value = brake_max;
            ESP_LOGI(TAG, "Brake calibrated: %lu - %lu", brake_input_min_value, brake_input_max_value);
        }
    } else {
        ESP_LOGE(TAG, "Brake: no valid readings");
    }
#endif

#ifdef CONFIG_TARGET_DUAL_THROTTLE
    // For dual throttle: both throttle AND brake must have sufficient range
    bool calibration_passed = throttle_range_ok && brake_range_ok;
#else
    // For lite: only throttle needs sufficient range
    bool calibration_passed = throttle_range_ok;
#endif

    if (calibration_passed) {
        calibration_done = true;
        // Save calibration to NVS
        if (save_calibration_to_nvs() == ESP_OK) {
            ESP_LOGI(TAG, "Calibration saved to NVS");
        } else {
            ESP_LOGE(TAG, "Failed to save calibration to NVS");
        }
    } else {
        // Restore previous calibration state - don't lose working calibration on failed attempt
        calibration_done = had_previous_calibration;
        ESP_LOGW(TAG, "Calibration failed - %s",
                 had_previous_calibration ? "previous calibration active" : "no calibration");
    }

    return calibration_passed;
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

bool throttle_should_use_neutral(void) {
    return calibration_in_progress || !calibration_done;
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
        return VESC_NEUTRAL_VALUE;
    }

    // Read current throttle and brake values
    int32_t throttle_raw = throttle_read_value();
    int32_t brake_raw = brake_read_value();

    if (throttle_raw < 0 || brake_raw < 0) {
        return VESC_NEUTRAL_VALUE;  // Return neutral on error
    }

    // Constrain values to calibrated ranges
    if (throttle_raw < adc_input_min_value) throttle_raw = adc_input_min_value;
    if (throttle_raw > adc_input_max_value) throttle_raw = adc_input_max_value;
    if (brake_raw < brake_input_min_value) brake_raw = brake_input_min_value;
    if (brake_raw > brake_input_max_value) brake_raw = brake_input_max_value;

    uint32_t brake_range = brake_input_max_value - brake_input_min_value;
    uint32_t throttle_range = adc_input_max_value - adc_input_min_value;

    if (brake_range == 0 || throttle_range == 0) {
        return VESC_NEUTRAL_VALUE;  // Avoid division by zero
    }

    // Calculate brake factor
    float brake_factor = (float)(brake_raw - brake_input_min_value) / (float)brake_range;

    // Calculate throttle factor
    float throttle_factor = (float)(throttle_raw - adc_input_min_value) / (float)throttle_range;

    // Throttle mapping: throttle MAX (factor=1.0) = 255, throttle MIN (factor=0.0) = 128 (neutral)
    uint8_t throttle_ble_value = VESC_NEUTRAL_VALUE + (uint8_t)(throttle_factor * 127.0f);

    uint8_t ble_value = (uint8_t)(throttle_ble_value * (1.0f - brake_factor));

    return ble_value;
}
#endif

#ifdef CONFIG_TARGET_LITE
// Lite mode: single throttle mapping function
uint8_t map_adc_value(uint32_t adc_value) {
    return map_throttle_value(adc_value);
}
#endif
