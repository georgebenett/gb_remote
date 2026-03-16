#include "throttle.h"
#include "ble.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "power.h"
#include "target_config.h"
#include "vesc_config.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#define TAG "ADC"

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
static float throttle_curve_exponent = 4.0f;
static uint8_t throttle_curve_index = 3; // 0=Linear, 1=Gentle, 2=Medium, 3=Soft
static const float throttle_curve_presets[THROTTLE_CURVE_COUNT] = {1.0f, 2.0f,
                                                                   3.0f, 4.0f};
#ifdef CONFIG_TARGET_DUAL_THROTTLE
static float brake_curve_exponent = 4.0f;
static uint8_t brake_curve_index = 3;
#endif
static esp_err_t load_calibration_from_nvs(void);

void adc_deinit(void);

// Getter function for battery module to access ADC handle
adc_oneshot_unit_handle_t adc_get_handle(void) { return adc1_handle; }

bool adc_is_initialized(void) { return adc_initialized; }

esp_err_t adc_init(void) {
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
  ret = adc_oneshot_config_channel(adc1_handle, BRAKE_PIN, &config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Brake ADC channel configuration failed");
    return ret;
  }
#endif

  adc_initialized = true;
  return ESP_OK;
}

int32_t throttle_read_value(void) {
  if (!adc_initialized || !adc1_handle) {
    ESP_LOGE(TAG, "ADC not properly initialized");
    return -1;
  }

  // Take multiple readings and average
  const int NUM_SAMPLES = 5;
  const int MIN_VALID_SAMPLES = 3; // Require at least 3/5 valid samples
  int32_t samples[NUM_SAMPLES];
  int32_t sum = 0;
  int valid_samples = 0;

  for (int i = 0; i < NUM_SAMPLES; i++) {
    int adc_raw = 0;
    esp_err_t ret = adc_oneshot_read(adc1_handle, THROTTLE_PIN, &adc_raw);

    if (ret == ESP_OK && adc_raw >= 0 && adc_raw <= 4095) {
      samples[valid_samples] = adc_raw;
      sum += adc_raw;
      valid_samples++;
    }

    // Small delay between samples for ADC settling
    vTaskDelay(pdMS_TO_TICKS(ADC_SAMPLE_MS));
  }

  // Require minimum valid samples
  if (valid_samples < MIN_VALID_SAMPLES) {
    ESP_LOGW(TAG, "Insufficient valid ADC samples: %d/%d", valid_samples,
             NUM_SAMPLES);
    return -1;
  }

  int32_t average = sum / valid_samples;

  // Detect stuck ADC (all samples at extreme values)
  bool all_at_min = true;
  bool all_at_max = true;
  for (int i = 0; i < valid_samples; i++) {
    if (samples[i] > 10)
      all_at_min = false;
    if (samples[i] < 4085)
      all_at_max = false;
  }

  if (all_at_min || all_at_max) {
    ESP_LOGW(TAG, "ADC appears stuck at %s",
             all_at_min ? "minimum" : "maximum");
    // Still return the value but log warning - calibration may handle this
  }

  return average;
}

#ifdef CONFIG_TARGET_DUAL_THROTTLE
int32_t brake_read_value(void) {
  if (!adc_initialized || !adc1_handle) {
    ESP_LOGE(TAG, "ADC not properly initialized");
    return -1;
  }

  // Take multiple readings and average
  const int NUM_SAMPLES = 5;
  const int MIN_VALID_SAMPLES = 3; // Require at least 3/5 valid samples
  int32_t samples[NUM_SAMPLES];
  int32_t sum = 0;
  int valid_samples = 0;

  for (int i = 0; i < NUM_SAMPLES; i++) {
    int adc_raw = 0;
    esp_err_t ret = adc_oneshot_read(adc1_handle, BRAKE_PIN, &adc_raw);

    if (ret == ESP_OK && adc_raw >= 0 && adc_raw <= 4095) {
      samples[valid_samples] = adc_raw;
      sum += adc_raw;
      valid_samples++;
    }

    // Small delay between samples for ADC settling
    vTaskDelay(pdMS_TO_TICKS(ADC_SAMPLE_MS));
  }

  // Require minimum valid samples
  if (valid_samples < MIN_VALID_SAMPLES) {
    ESP_LOGW(TAG, "Insufficient valid brake ADC samples: %d/%d", valid_samples,
             NUM_SAMPLES);
    return -1;
  }

  int32_t average = sum / valid_samples;

  // Detect stuck ADC (all samples at extreme values)
  bool all_at_min = true;
  bool all_at_max = true;
  for (int i = 0; i < valid_samples; i++) {
    if (samples[i] > 10)
      all_at_min = false;
    if (samples[i] < 4085)
      all_at_max = false;
  }

  if (all_at_min || all_at_max) {
    ESP_LOGW(TAG, "Brake ADC appears stuck at %s",
             all_at_min ? "minimum" : "maximum");
  }

  return average;
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
    // On ADC error, use neutral value (not 0 which would be throttle)
    uint32_t adc_value =
        (adc_raw >= 0) ? (uint32_t)adc_raw : VESC_NEUTRAL_VALUE;
#endif

    if (adc_raw < 0) {
      error_count++;
      if (error_count >= MAX_ERRORS) {
        ESP_LOGE(TAG, "Too many ADC errors, attempting re-initialization");
        adc_deinit();
        vTaskDelay(pdMS_TO_TICKS(CALIBRATION_STEP_DELAY_MS));
        if (adc_init() == ESP_OK) {
          error_count = 0;
        }
      }
      latest_adc_value = VESC_NEUTRAL_VALUE;
      vTaskDelay(pdMS_TO_TICKS(CALIBRATION_STEP_DELAY_MS));
      continue;
    }
    error_count = 0; // Reset error count on successful read

#ifdef CONFIG_TARGET_DUAL_THROTTLE
    // Calculate combined throttle/brake BLE value (dual_throttle mode)
    uint8_t mapped_value = get_throttle_brake_ble_value();
#elif defined(CONFIG_TARGET_LITE)
    // Single throttle mapping (lite mode)
    uint8_t mapped_value = map_adc_value(adc_value);
    // Update latest_adc_value for BLE transmission
    latest_adc_value = mapped_value;
#endif

    if (!ble_is_connected()) {
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

  vTaskDelay(pdMS_TO_TICKS(CALIBRATION_STEP_DELAY_MS));

#if CALIBRATE_THROTTLE
  ESP_LOGI(TAG, "Force calibration flag set, performing calibration");
  // Clear existing calibration
  nvs_handle_t nvs_handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
    nvs_erase_key(nvs_handle, NVS_KEY_CALIBRATED);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
  }
  throttle_calibrate(NULL);
#else
  // Only calibrate if no valid calibration exists
  if (load_calibration_from_nvs() != ESP_OK) {
    throttle_calibrate(NULL);
  }
#endif

  xTaskCreate(adc_task, "adc_task", 4096, NULL, 10, NULL);
}

uint32_t adc_get_latest_value(void) { return latest_adc_value; }

void adc_deinit(void) {
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

static bool validate_calibration_values(uint32_t min_val, uint32_t max_val) {
  // Check values are within valid ADC range (12-bit = 0-4095)
  if (min_val > 4095 || max_val > 4095) {
    ESP_LOGW(TAG, "Calibration values out of ADC range");
    return false;
  }

  // Check min < max
  if (min_val >= max_val) {
    ESP_LOGW(TAG, "Calibration min >= max");
    return false;
  }

  // Check range is sufficient (at least 150 units)
  if ((max_val - min_val) < 150) {
    ESP_LOGW(TAG, "Calibration range too small: %lu", (max_val - min_val));
    return false;
  }

  return true;
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
  uint32_t temp_min, temp_max;
  err = nvs_get_u32(nvs_handle, NVS_KEY_MIN, &temp_min);
  if (err != ESP_OK) {
    nvs_close(nvs_handle);
    return err;
  }

  err = nvs_get_u32(nvs_handle, NVS_KEY_MAX, &temp_max);
  if (err != ESP_OK) {
    nvs_close(nvs_handle);
    return err;
  }

  // Validate throttle calibration
  if (!validate_calibration_values(temp_min, temp_max)) {
    ESP_LOGW(TAG, "Invalid throttle calibration in NVS, re-calibration needed");
    nvs_close(nvs_handle);
    return ESP_ERR_INVALID_STATE;
  }

  adc_input_min_value = temp_min;
  adc_input_max_value = temp_max;

#ifdef CONFIG_TARGET_DUAL_THROTTLE
  // Read brake calibration values
  uint32_t brake_temp_min, brake_temp_max;
  err = nvs_get_u32(nvs_handle, NVS_KEY_BRAKE_MIN, &brake_temp_min);
  if (err != ESP_OK) {
    brake_temp_min = ADC_INITIAL_MIN_VALUE;
  }

  err = nvs_get_u32(nvs_handle, NVS_KEY_BRAKE_MAX, &brake_temp_max);
  if (err != ESP_OK) {
    brake_temp_max = ADC_INITIAL_MAX_VALUE;
  }

  // Validate brake calibration
  if (validate_calibration_values(brake_temp_min, brake_temp_max)) {
    brake_input_min_value = brake_temp_min;
    brake_input_max_value = brake_temp_max;
  } else {
    ESP_LOGW(TAG, "Invalid brake calibration in NVS, using defaults");
    brake_input_min_value = ADC_INITIAL_MIN_VALUE;
    brake_input_max_value = ADC_INITIAL_MAX_VALUE;
  }
#endif

  // Load throttle curve: prefer curve index (0-3), else legacy curve_exp for
  // backward compat
  uint8_t stored_idx = THROTTLE_CURVE_DEFAULT_INDEX;
  if (nvs_get_u8(nvs_handle, NVS_KEY_CURVE_INDEX, &stored_idx) == ESP_OK &&
      stored_idx < THROTTLE_CURVE_COUNT) {
    throttle_curve_index = stored_idx;
    throttle_curve_exponent = throttle_curve_presets[stored_idx];
    ESP_LOGI(TAG, "Loaded throttle curve index %u (exponent %.2f)",
             (unsigned)stored_idx, throttle_curve_exponent);
  } else {
    uint16_t stored_exp = 100;
    if (nvs_get_u16(nvs_handle, NVS_KEY_CURVE_EXP, &stored_exp) == ESP_OK) {
      throttle_curve_exponent = (float)stored_exp / 100.0f;
      if (throttle_curve_exponent < 0.1f)
        throttle_curve_exponent = 0.1f;
      if (throttle_curve_exponent > 5.0f)
        throttle_curve_exponent = 5.0f;
      // Map to nearest preset index and persist for next time
      uint8_t nearest = 0;
      float best = fabsf(throttle_curve_exponent - throttle_curve_presets[0]);
      for (uint8_t i = 1; i < THROTTLE_CURVE_COUNT; i++) {
        float d = fabsf(throttle_curve_exponent - throttle_curve_presets[i]);
        if (d < best) {
          best = d;
          nearest = i;
        }
      }
      throttle_curve_index = nearest;
      throttle_curve_exponent = throttle_curve_presets[nearest];
      ESP_LOGI(TAG,
               "Loaded throttle curve exponent (legacy) -> index %u (%.2f)",
               (unsigned)nearest, throttle_curve_exponent);
    } else {
      throttle_curve_index = THROTTLE_CURVE_DEFAULT_INDEX;
      throttle_curve_exponent =
          throttle_curve_presets[THROTTLE_CURVE_DEFAULT_INDEX];
      ESP_LOGI(TAG, "Throttle curve default index %u (%.2f)",
               (unsigned)throttle_curve_index, throttle_curve_exponent);
    }
  }

#ifdef CONFIG_TARGET_DUAL_THROTTLE
  // Load brake curve index (dual throttle only)
  uint8_t brake_idx = THROTTLE_CURVE_DEFAULT_INDEX;
  if (nvs_get_u8(nvs_handle, NVS_KEY_BRAKE_CURVE_INDEX, &brake_idx) == ESP_OK &&
      brake_idx < THROTTLE_CURVE_COUNT) {
    brake_curve_index = brake_idx;
    brake_curve_exponent = throttle_curve_presets[brake_idx];
    ESP_LOGI(TAG, "Loaded brake curve index %u (exponent %.2f)",
             (unsigned)brake_idx, brake_curve_exponent);
  } else {
    brake_curve_index = THROTTLE_CURVE_DEFAULT_INDEX;
    brake_curve_exponent = throttle_curve_presets[THROTTLE_CURVE_DEFAULT_INDEX];
  }
#endif

  nvs_close(nvs_handle);
  calibration_done = true;
  ESP_LOGI(TAG, "Loaded calibration: throttle %lu-%lu", adc_input_min_value,
           adc_input_max_value);
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

calibration_result_t throttle_calibrate(calibration_progress_cb_t progress_cb) {
  ESP_LOGI(TAG, "Starting ADC calibration...");

  calibration_in_progress = true;

  // Save the previous calibration state so we can restore it if calibration
  // fails
  bool had_previous_calibration = calibration_done;

  uint32_t throttle_min = UINT32_MAX;
  uint32_t throttle_max = 0;
  uint32_t throttle_current = 0;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
  uint32_t brake_min = UINT32_MAX;
  uint32_t brake_max = 0;
  uint32_t brake_current = 0;
#endif

#define CAL_PROGRESS_INTERVAL 5 // Send progress update every N samples (~50ms)

  // Take multiple samples to find the actual range
  for (int i = 0; i < ADC_CALIBRATION_SAMPLES; i++) {
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    int32_t throttle_value = throttle_read_value();
    int32_t brake_value = brake_read_value();

    if (throttle_value != -1) { // Valid reading
      throttle_current = (uint32_t)throttle_value;
      if (throttle_min == UINT32_MAX || throttle_value < (int32_t)throttle_min)
        throttle_min = (uint32_t)throttle_value;
      if (throttle_value > (int32_t)throttle_max)
        throttle_max = (uint32_t)throttle_value;
    }

    if (brake_value != -1) { // Valid reading
      brake_current = (uint32_t)brake_value;
      if (brake_min == UINT32_MAX || brake_value < (int32_t)brake_min)
        brake_min = (uint32_t)brake_value;
      if (brake_value > (int32_t)brake_max)
        brake_max = (uint32_t)brake_value;
    }
#elif defined(CONFIG_TARGET_LITE)
    int32_t value = throttle_read_value();
    if (value != -1) { // Valid reading
      throttle_current = (uint32_t)value;
      if (throttle_min == UINT32_MAX || value < (int32_t)throttle_min)
        throttle_min = (uint32_t)value;
      if (value > (int32_t)throttle_max)
        throttle_max = (uint32_t)value;
    }
#endif

    // Fire progress callback every CAL_PROGRESS_INTERVAL samples (and on the
    // last sample)
    if (progress_cb && ((i % CAL_PROGRESS_INTERVAL == 0) ||
                        (i == ADC_CALIBRATION_SAMPLES - 1))) {
      uint32_t t_min_report = (throttle_min == UINT32_MAX) ? 0 : throttle_min;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
      uint32_t b_min_report = (brake_min == UINT32_MAX) ? 0 : brake_min;
      progress_cb((uint16_t)i, (uint16_t)ADC_CALIBRATION_SAMPLES,
                  throttle_current, t_min_report, throttle_max, brake_current,
                  b_min_report, brake_max);
#else
      progress_cb((uint16_t)i, (uint16_t)ADC_CALIBRATION_SAMPLES,
                  throttle_current, t_min_report, throttle_max, 0, 0, 0);
#endif
    }

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
      ESP_LOGW(TAG, "Throttle: insufficient range %lu (need 150+)",
               throttle_range);
    } else {
      throttle_range_ok = true;
      // Use measured values directly without margins
      adc_input_min_value = throttle_min;
      adc_input_max_value = throttle_max;
      ESP_LOGI(TAG, "Throttle calibrated: %lu - %lu", adc_input_min_value,
               adc_input_max_value);
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
      ESP_LOGI(TAG, "Brake calibrated: %lu - %lu", brake_input_min_value,
               brake_input_max_value);
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
    if (save_calibration_to_nvs() != ESP_OK) {
      ESP_LOGE(TAG, "Failed to save calibration to NVS");
      calibration_done = had_previous_calibration;
      return CAL_FAIL_SAVE;
    }
    ESP_LOGI(TAG, "Calibration saved to NVS");
    return CAL_OK;
  }

  // Restore previous calibration state - don't lose working calibration on
  // failed attempt
  calibration_done = had_previous_calibration;
  ESP_LOGW(TAG, "Calibration failed - %s",
           had_previous_calibration ? "previous calibration active"
                                    : "no calibration");

  // Return specific failure reason (throttle checked first, then brake)
  if (!throttle_valid) {
    return CAL_FAIL_THROTTLE_NO_READINGS;
  }
  if (!throttle_range_ok) {
    return CAL_FAIL_THROTTLE_RANGE;
  }
#ifdef CONFIG_TARGET_DUAL_THROTTLE
  if (!brake_valid) {
    return CAL_FAIL_BRAKE_NO_READINGS;
  }
  if (!brake_range_ok) {
    return CAL_FAIL_BRAKE_RANGE;
  }
#endif
  return CAL_FAIL_THROTTLE_RANGE; // Fallback (should not reach)
}

bool throttle_is_calibrated(void) { return calibration_done; }

void throttle_get_calibration_values(uint32_t *min_val, uint32_t *max_val) {
  if (min_val)
    *min_val = adc_input_min_value;
  if (max_val)
    *max_val = adc_input_max_value;
}

#ifdef CONFIG_TARGET_DUAL_THROTTLE
void brake_get_calibration_values(uint32_t *min_val, uint32_t *max_val) {
  if (min_val)
    *min_val = brake_input_min_value;
  if (max_val)
    *max_val = brake_input_max_value;
}
#endif

bool throttle_should_use_neutral(void) {
  return calibration_in_progress || !calibration_done;
}

// Apply non-linear throttle curve. Neutral (128) is kept fixed; curve applies
// only to the throttle half (128..255). So: output = 128 + (input-128)^exp
// scaled to 127 range. exponent=1.0 is linear, higher = gentler start.
static uint8_t apply_throttle_curve(uint8_t linear_value) {
  if (throttle_curve_exponent == 1.0f) {
    return linear_value; // Fast path: no curve
  }
  if (linear_value <= VESC_NEUTRAL_VALUE) {
    return linear_value; // Below/at neutral: no curve, keep as-is
  }
  // Throttle half: map (128..255) -> 0..1, apply curve, map back to 128..255
  float throttle_amount = (float)(linear_value - VESC_NEUTRAL_VALUE) / 127.0f;
  float curved = powf(throttle_amount, throttle_curve_exponent);
  int32_t result = VESC_NEUTRAL_VALUE + (int32_t)(curved * 127.0f + 0.5f);
  if (result > 255)
    result = 255;
  return (uint8_t)result;
}

uint8_t map_throttle_value(uint32_t adc_value) {
  // Protection against invalid calibration
  uint32_t range = adc_input_max_value - adc_input_min_value;
  if (range == 0) {
    return VESC_NEUTRAL_VALUE; // Return neutral on invalid calibration
  }

  // Constrain input value to the calibrated range
  if (adc_value < adc_input_min_value) {
    adc_value = adc_input_min_value;
  }
  if (adc_value > adc_input_max_value) {
    adc_value = adc_input_max_value;
  }

  // Perform the linear mapping
  uint8_t mapped =
      (uint8_t)((adc_value - adc_input_min_value) *
                    (ADC_OUTPUT_MAX_VALUE - ADC_OUTPUT_MIN_VALUE) / range +
                ADC_OUTPUT_MIN_VALUE);

  // Apply non-linear throttle curve
  mapped = apply_throttle_curve(mapped);

  // Apply dead zone around neutral (±3 units)
  if (mapped >= (VESC_NEUTRAL_VALUE - 3) &&
      mapped <= (VESC_NEUTRAL_VALUE + 3)) {
    mapped = VESC_NEUTRAL_VALUE;
  }

  return mapped;
}

#ifdef CONFIG_TARGET_DUAL_THROTTLE
uint8_t map_brake_value(uint32_t adc_value) {
  // Protection against invalid calibration
  uint32_t range = brake_input_max_value - brake_input_min_value;
  if (range == 0) {
    return ADC_OUTPUT_MIN_VALUE; // Return minimum brake on invalid calibration
  }

  // Constrain input value to the calibrated range
  if (adc_value < brake_input_min_value) {
    adc_value = brake_input_min_value;
  }
  if (adc_value > brake_input_max_value) {
    adc_value = brake_input_max_value;
  }

  // Perform the mapping (no offset for brake)
  uint8_t mapped =
      (uint8_t)((adc_value - brake_input_min_value) *
                    (ADC_OUTPUT_MAX_VALUE - ADC_OUTPUT_MIN_VALUE) / range +
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
    return VESC_NEUTRAL_VALUE; // Return neutral on error
  }

  // Constrain values to calibrated ranges
  if (throttle_raw < adc_input_min_value)
    throttle_raw = adc_input_min_value;
  if (throttle_raw > adc_input_max_value)
    throttle_raw = adc_input_max_value;
  if (brake_raw < brake_input_min_value)
    brake_raw = brake_input_min_value;
  if (brake_raw > brake_input_max_value)
    brake_raw = brake_input_max_value;

  uint32_t brake_range = brake_input_max_value - brake_input_min_value;
  uint32_t throttle_range = adc_input_max_value - adc_input_min_value;

  if (brake_range == 0 || throttle_range == 0) {
    return VESC_NEUTRAL_VALUE; // Avoid division by zero
  }

  // Calculate brake factor
  float brake_factor =
      (float)(brake_raw - brake_input_min_value) / (float)brake_range;

  // Apply non-linear brake curve (same presets as throttle)
  if (brake_curve_exponent != 1.0f) {
    brake_factor = powf(brake_factor, brake_curve_exponent);
  }

  // Calculate throttle factor
  float throttle_factor =
      (float)(throttle_raw - adc_input_min_value) / (float)throttle_range;

  // Apply non-linear throttle curve
  if (throttle_curve_exponent != 1.0f) {
    throttle_factor = powf(throttle_factor, throttle_curve_exponent);
  }

  // Throttle mapping: throttle MAX (factor=1.0) = 255, throttle MIN
  // (factor=0.0) = 128 (neutral)
  uint8_t throttle_ble_value =
      VESC_NEUTRAL_VALUE + (uint8_t)(throttle_factor * 127.0f);

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

// --- Throttle curve exponent API ---

float throttle_get_curve_exponent(void) { return throttle_curve_exponent; }

esp_err_t throttle_set_curve_exponent(float exponent) {
  // Clamp to sane range
  if (exponent < 0.1f)
    exponent = 0.1f;
  if (exponent > 5.0f)
    exponent = 5.0f;

  throttle_curve_exponent = exponent;

  // Persist to NVS
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS for curve exponent: %s",
             esp_err_to_name(err));
    return err;
  }

  // Store as integer: exponent * 100 (e.g. 1.50 -> 150)
  uint16_t stored = (uint16_t)(exponent * 100.0f + 0.5f);
  err = nvs_set_u16(nvs_handle, NVS_KEY_CURVE_EXP, stored);
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Throttle curve exponent set to %.2f", exponent);
  } else {
    ESP_LOGE(TAG, "Failed to save curve exponent: %s", esp_err_to_name(err));
  }
  return err;
}

uint8_t throttle_get_curve_index(void) { return throttle_curve_index; }

esp_err_t throttle_set_curve_index(uint8_t index) {
  if (index >= THROTTLE_CURVE_COUNT) {
    return ESP_ERR_INVALID_ARG;
  }
  throttle_curve_index = index;
  throttle_curve_exponent = throttle_curve_presets[index];

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS for curve index: %s",
             esp_err_to_name(err));
    return err;
  }
  err = nvs_set_u8(nvs_handle, NVS_KEY_CURVE_INDEX, throttle_curve_index);
  if (err == ESP_OK) {
    uint16_t stored_exp = (uint16_t)(throttle_curve_exponent * 100.0f + 0.5f);
    nvs_set_u16(nvs_handle, NVS_KEY_CURVE_EXP, stored_exp);
    err = nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Throttle curve set to index %u (exponent %.2f)",
             (unsigned)index, throttle_curve_exponent);
  } else {
    ESP_LOGE(TAG, "Failed to save curve index: %s", esp_err_to_name(err));
  }
  return err;
}

uint8_t throttle_get_brake_curve_index(void) {
#ifdef CONFIG_TARGET_DUAL_THROTTLE
  return brake_curve_index;
#else
  return 0;
#endif
}

esp_err_t throttle_set_brake_curve_index(uint8_t index) {
#ifdef CONFIG_TARGET_DUAL_THROTTLE
  if (index >= THROTTLE_CURVE_COUNT) {
    return ESP_ERR_INVALID_ARG;
  }
  brake_curve_index = index;
  brake_curve_exponent = throttle_curve_presets[index];

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS for brake curve index: %s",
             esp_err_to_name(err));
    return err;
  }
  err = nvs_set_u8(nvs_handle, NVS_KEY_BRAKE_CURVE_INDEX, brake_curve_index);
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Brake curve set to index %u (exponent %.2f)",
             (unsigned)index, brake_curve_exponent);
  } else {
    ESP_LOGE(TAG, "Failed to save brake curve index: %s", esp_err_to_name(err));
  }
  return err;
#else
  (void)index;
  return ESP_ERR_NOT_SUPPORTED;
#endif
}
