#include "throttle.h"
#include "battery.h"
#include "ble.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power.h"
#include "settings.h"
#include "target_config.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define TAG "ADC"

static adc_oneshot_unit_handle_t adc1_handle;
static adc_oneshot_unit_init_cfg_t init_config1;
static adc_oneshot_chan_cfg_t config;
static SemaphoreHandle_t adc_mutex = NULL;
/* Neutral, not 0: 0 is full brake, and the radio sends this if the task dies.
 */
static uint32_t latest_adc_value = VESC_NEUTRAL_VALUE;

/* Owned by the config tool, loaded at adc_init(). Zero is linear, which is
 * also what a failed load leaves. */
static int8_t curve_acc = 0;
static int8_t curve_brake = 0;
static uint8_t curve_mode = THROTTLE_CURVE_EXPO;
static atomic_int trim_offset;
static atomic_bool invert_throttle;
static atomic_int last_throttle_raw = -1, last_brake_raw = -1;
static throttle_cmd_t last_cmd = {.value = VESC_NEUTRAL_VALUE,
                                  .neutral = VESC_NEUTRAL_VALUE};
static portMUX_TYPE cmd_lock = portMUX_INITIALIZER_UNLOCKED;
static bool adc_initialized = false;
static int error_count = 0;
static const int MAX_ERRORS = 5;
/* The active calibration. Replaced whole under cal_lock, so the sampler never
 * maps with a half-updated min/max pair. */
static settings_cal_t cal = {.throttle_max = ADC_INITIAL_MAX_VALUE,
                             .brake_max = ADC_INITIAL_MAX_VALUE};
static portMUX_TYPE cal_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool calibration_in_progress;
static void load_calibration(void);
static void update_command(void);
static void adc_deinit(void);

static settings_cal_t cal_snapshot(void) {
  taskENTER_CRITICAL(&cal_lock);
  settings_cal_t c = cal;
  taskEXIT_CRITICAL(&cal_lock);
  return c;
}

static void cal_publish(const settings_cal_t *c) {
  taskENTER_CRITICAL(&cal_lock);
  cal = *c;
  taskEXIT_CRITICAL(&cal_lock);
}

// Getter function for battery module to access ADC handle
adc_oneshot_unit_handle_t adc_get_handle(void) { return adc1_handle; }

SemaphoreHandle_t adc_get_mutex(void) { return adc_mutex; }

bool adc_is_initialized(void) { return adc_initialized; }

esp_err_t adc_init(void) {
  if (adc_initialized) {
    ESP_LOGI(TAG, "ADC already initialized");
    return ESP_OK;
  }

  esp_err_t ret;

  // Create mutex to protect concurrent adc_oneshot_read calls
  if (adc_mutex == NULL) {
    adc_mutex = xSemaphoreCreateMutex();
    if (adc_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create ADC mutex");
      return ESP_FAIL;
    }
  }

  init_config1.unit_id = ADC_UNIT_1;
  init_config1.ulp_mode = ADC_ULP_MODE_DISABLE;
  ret = adc_oneshot_new_unit(&init_config1, &adc1_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "ADC unit initialization failed");
    return ret;
  }

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

  {
    settings_t cfg;
    settings_get(&cfg);
    trim_offset = cfg.ble_trim;
    invert_throttle = cfg.invert_throttle;
    throttle_set_curve(cfg.throttle_curve_acc, cfg.throttle_curve_brake,
                       cfg.throttle_curve_mode);
    throttle_set_ride_profile(settings_ride_profile_ramp_ms(cfg.ride_profile));
  }

  adc_initialized = true;
  return ESP_OK;
}

/* At 50 Hz a persistent fault would flood the console; once a second is
 * plenty. */
static bool log_due(int64_t *last_us) {
  int64_t now = esp_timer_get_time();
  if (now - *last_us < 1000000) {
    return false;
  }
  *last_us = now;
  return true;
}

/** Averaged read of one ADC channel. Returns -1 when too few samples come back
 *  valid; a stuck rail is logged but still returned (calibration copes). */
static int32_t adc_read_avg(adc_channel_t channel, const char *what) {
  if (!adc_initialized || !adc1_handle) {
    ESP_LOGE(TAG, "ADC not properly initialized");
    return -1;
  }

  const int NUM_SAMPLES = 5;
  const int MIN_VALID_SAMPLES = 3;
  int32_t samples[NUM_SAMPLES];
  int32_t sum = 0;
  int valid_samples = 0;

  for (int i = 0; i < NUM_SAMPLES; i++) {
    int adc_raw = 0;
    if (adc_mutex && xSemaphoreTake(adc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      esp_err_t ret = adc_oneshot_read(adc1_handle, channel, &adc_raw);
      xSemaphoreGive(adc_mutex);
      if (ret == ESP_OK && adc_raw >= 0 && adc_raw <= 4095) {
        samples[valid_samples] = adc_raw;
        sum += adc_raw;
        valid_samples++;
      }
    }
  }

  static int64_t last_log_us;
  if (valid_samples < MIN_VALID_SAMPLES) {
    if (log_due(&last_log_us))
      ESP_LOGW(TAG, "Insufficient valid %s ADC samples: %d/%d", what,
               valid_samples, NUM_SAMPLES);
    return -1;
  }

  bool all_at_min = true;
  bool all_at_max = true;
  for (int i = 0; i < valid_samples; i++) {
    if (samples[i] > 10)
      all_at_min = false;
    if (samples[i] < 4085)
      all_at_max = false;
  }
  if ((all_at_min || all_at_max) && log_due(&last_log_us)) {
    ESP_LOGW(TAG, "%s ADC appears stuck at %s", what,
             all_at_min ? "minimum" : "maximum");
  }

  return sum / valid_samples;
}

int32_t throttle_read_value(void) {
  return adc_read_avg(THROTTLE_PIN, "throttle");
}

#ifdef CONFIG_TARGET_DUAL_THROTTLE
int32_t brake_read_value(void) { return adc_read_avg(BRAKE_PIN, "brake"); }
#endif

static void adc_task(void *pvParameters) {
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  uint32_t last_value = VESC_NEUTRAL_VALUE;
  const uint32_t CHANGE_THRESHOLD = 2;
  TickType_t last_wake = xTaskGetTickCount();

  while (1) {
    // Both channels read once per pass and mapped from the raws here.
    int32_t adc_raw = throttle_read_value();
    last_throttle_raw = adc_raw;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    int32_t brake_raw = brake_read_value();
    last_brake_raw = brake_raw;
#endif

#ifdef CONFIG_TARGET_LITE
    // On ADC error, use neutral value (not 0 which would be throttle)
    uint32_t adc_value =
        (adc_raw >= 0) ? (uint32_t)adc_raw : VESC_NEUTRAL_VALUE;
#endif

#ifdef CONFIG_TARGET_DUAL_THROTTLE
    if (adc_raw < 0 || brake_raw < 0) {
#else
    if (adc_raw < 0) {
#endif
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
      esp_task_wdt_reset(); // skips the feed at the bottom of the loop
      vTaskDelay(pdMS_TO_TICKS(CALIBRATION_STEP_DELAY_MS));
      last_wake = xTaskGetTickCount(); // missed its slot, re-baseline
      continue;
    }
    error_count = 0; // Reset error count on successful read

    settings_cal_t c = cal_snapshot();
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    uint8_t mapped_value =
        throttle_should_use_neutral()
            ? VESC_NEUTRAL_VALUE
            : throttle_map_ble_value(adc_raw, brake_raw, c.throttle_min,
                                     c.throttle_max, c.brake_min, c.brake_max);
#elif defined(CONFIG_TARGET_LITE)
    uint8_t mapped_value =
        throttle_map_single(adc_value, c.throttle_min, c.throttle_max);
#endif
    // Update latest_adc_value so modules like snake can read current input
    latest_adc_value = mapped_value;
    update_command();
    ble_notify_throttle_sample(); // one sample in, one packet out

    if (!ble_is_connected()) {
      if (abs((int32_t)mapped_value - (int32_t)last_value) > CHANGE_THRESHOLD) {
        power_reset_inactivity_timer();
        last_value = mapped_value;
      }
    }

    esp_task_wdt_reset();
    /* Fixed cadence so a variable sampling burst does not stack onto the
     * period. On overrun take the next slot fresh: catching up would burst
     * back-to-back passes at priority 10, above everything else. */
    TickType_t period = pdMS_TO_TICKS(ADC_SAMPLE_PERIOD_MS);
    if ((xTaskGetTickCount() - last_wake) >= period) {
      last_wake = xTaskGetTickCount();
    } else {
      vTaskDelayUntil(&last_wake, period);
    }
  }
}

void adc_start_task(void) {
  esp_err_t ret = adc_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "ADC initialization failed, not starting task");
    return;
  }

  vTaskDelay(pdMS_TO_TICKS(CALIBRATION_STEP_DELAY_MS));

  // Uncalibrated (first flash) is reported over the binary protocol and
  // sends neutral until the config tool calibrates.
  load_calibration();

  ESP_ERROR_CHECK(xTaskCreate(adc_task, "adc_task", 4096, NULL, 10, NULL) ==
                          pdPASS
                      ? ESP_OK
                      : ESP_ERR_NO_MEM);
}

uint32_t adc_get_latest_value(void) { return latest_adc_value; }

static void adc_deinit(void) {
  if (!adc_initialized) {
    return;
  }

  if (adc1_handle) {
    adc_oneshot_del_unit(adc1_handle);
    adc1_handle = NULL;
  }

  if (adc_mutex) {
    vSemaphoreDelete(adc_mutex);
    adc_mutex = NULL;
  }

  adc_initialized = false;
}

static bool validate_calibration_values(uint32_t min_val, uint32_t max_val) {
  // Check values are within valid ADC range (12-bit = 0-4095)
  if (min_val > 4095 || max_val > 4095) {
    ESP_LOGW(TAG, "Calibration values out of ADC range");
    return false;
  }

  if (min_val >= max_val) {
    ESP_LOGW(TAG, "Calibration min >= max");
    return false;
  }

  if ((max_val - min_val) < 150) {
    ESP_LOGW(TAG, "Calibration range too small: %lu", (max_val - min_val));
    return false;
  }

  return true;
}

static void load_calibration(void) {
  settings_cal_t c;
  settings_get_cal(&c);
  if (!c.done) {
    ESP_LOGI(TAG, "No calibration data found, starting uncalibrated");
    return;
  }
  if (!validate_calibration_values(c.throttle_min, c.throttle_max)) {
    ESP_LOGW(TAG, "Invalid throttle calibration in NVS, re-calibration needed");
    return;
  }
#ifdef CONFIG_TARGET_DUAL_THROTTLE
  if (!validate_calibration_values(c.brake_min, c.brake_max)) {
    ESP_LOGW(TAG, "Invalid brake calibration in NVS, using defaults");
    c.brake_min = ADC_INITIAL_MIN_VALUE;
    c.brake_max = ADC_INITIAL_MAX_VALUE;
  }
#endif
  cal_publish(&c);
  ESP_LOGI(TAG, "Loaded calibration: throttle %lu-%lu", c.throttle_min,
           c.throttle_max);
}

calibration_result_t throttle_calibrate(calibration_progress_cb_t progress_cb) {
  ESP_LOGI(TAG, "Starting ADC calibration...");

  // Held until the new range is committed, so the sampler sends neutral
  // instead of mapping with a half-written one.
  calibration_in_progress = true;
  settings_cal_t next = cal_snapshot();

  uint32_t throttle_min = UINT32_MAX;
  uint32_t throttle_max = 0;
  uint32_t throttle_current = 0;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
  uint32_t brake_min = UINT32_MAX;
  uint32_t brake_max = 0;
  uint32_t brake_current = 0;
#endif

#define CAL_PROGRESS_INTERVAL 5 // Send progress update every N samples (~50ms)

  for (int i = 0; i < ADC_CALIBRATION_SAMPLES; i++) {
#ifdef CONFIG_TARGET_DUAL_THROTTLE
    int32_t throttle_value = throttle_read_value();
    int32_t brake_value = brake_read_value();

    if (throttle_value != -1) {
      throttle_current = (uint32_t)throttle_value;
      if (throttle_min == UINT32_MAX || throttle_value < (int32_t)throttle_min)
        throttle_min = (uint32_t)throttle_value;
      if (throttle_value > (int32_t)throttle_max)
        throttle_max = (uint32_t)throttle_value;
    }

    if (brake_value != -1) {
      brake_current = (uint32_t)brake_value;
      if (brake_min == UINT32_MAX || brake_value < (int32_t)brake_min)
        brake_min = (uint32_t)brake_value;
      if (brake_value > (int32_t)brake_max)
        brake_max = (uint32_t)brake_value;
    }
#elif defined(CONFIG_TARGET_LITE)
    int32_t value = throttle_read_value();
    if (value != -1) {
      throttle_current = (uint32_t)value;
      if (throttle_min == UINT32_MAX || value < (int32_t)throttle_min)
        throttle_min = (uint32_t)value;
      if (value > (int32_t)throttle_max)
        throttle_max = (uint32_t)value;
    }
#endif

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

    vTaskDelay(pdMS_TO_TICKS(ADC_CALIBRATION_DELAY_MS));
  }

  bool throttle_valid = (throttle_min != UINT32_MAX && throttle_max != 0);
  bool throttle_range_ok = false;
#ifdef CONFIG_TARGET_DUAL_THROTTLE
  bool brake_valid = (brake_min != UINT32_MAX && brake_max != 0);
  bool brake_range_ok = false;
#endif

  if (throttle_valid) {
    uint32_t throttle_range = throttle_max - throttle_min;

    if (throttle_range < 150) {
      ESP_LOGW(TAG, "Throttle: insufficient range %lu (need 150+)",
               throttle_range);
    } else {
      throttle_range_ok = true;
      next.throttle_min = throttle_min;
      next.throttle_max = throttle_max;
      ESP_LOGI(TAG, "Throttle calibrated: %lu - %lu", throttle_min,
               throttle_max);
    }
  } else {
    ESP_LOGE(TAG, "Throttle: no valid readings");
  }

#ifdef CONFIG_TARGET_DUAL_THROTTLE
  // Calibrate brake (dual_throttle only)
  if (brake_valid) {
    uint32_t brake_range = brake_max - brake_min;

    if (brake_range < 150) {
      ESP_LOGW(TAG, "Brake: insufficient range %lu (need 150+)", brake_range);
    } else {
      brake_range_ok = true;
      next.brake_min = brake_min;
      next.brake_max = brake_max;
      ESP_LOGI(TAG, "Brake calibrated: %lu - %lu", brake_min, brake_max);
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
    next.done = true;
    if (settings_set_cal(&next) != ESP_OK) {
      calibration_in_progress = false;
      return CAL_FAIL_SAVE;
    }
    cal_publish(&next);
    calibration_in_progress = false;
    ESP_LOGI(TAG, "Calibration saved");
    return CAL_OK;
  }

  // A failed attempt keeps whatever calibration was active.
  calibration_in_progress = false;
  ESP_LOGW(TAG, "Calibration failed - %s",
           cal_snapshot().done ? "previous calibration active"
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

bool throttle_is_calibrated(void) { return cal_snapshot().done; }

void throttle_get_calibration_values(uint32_t *min_val, uint32_t *max_val) {
  settings_cal_t c = cal_snapshot();
  if (min_val)
    *min_val = c.throttle_min;
  if (max_val)
    *max_val = c.throttle_max;
}

#ifdef CONFIG_TARGET_DUAL_THROTTLE
void brake_get_calibration_values(uint32_t *min_val, uint32_t *max_val) {
  settings_cal_t c = cal_snapshot();
  if (min_val)
    *min_val = c.brake_min;
  if (max_val)
    *max_val = c.brake_max;
}
#endif

bool throttle_should_use_neutral(void) {
  return calibration_in_progress || !cal_snapshot().done;
}

/* Counts per sample, averaged over the ramp; 0 = off (Rocket). ramp_value keeps
 * the slewed position between samples. */
static float ramp_ms_total = 0.0f;
static float ramp_value = VESC_NEUTRAL_VALUE;

void throttle_set_ride_profile(uint16_t ramp_ms) {
  ramp_ms_total = ramp_ms;
  ESP_LOGI(TAG, "Ride profile ramp: %u ms", ramp_ms);
}

static uint8_t throttle_apply_ramp(uint8_t value, uint8_t neutral) {
  if (value <= neutral) {
    ramp_value = value;
    return value;
  }
  if (ramp_value < neutral) {
    ramp_value = neutral;
  }
  if (ramp_ms_total <= 0.0f || value <= ramp_value) {
    ramp_value = value;
    return value;
  }
  float span = 255.0f - neutral;
  float step = span * ADC_SAMPLE_PERIOD_MS / ramp_ms_total;
  // Ease-in: step grows with sqrt of progress so position follows t^2; the
  // floor lets it leave neutral.
  float ease = 2.0f * sqrtf((ramp_value - neutral) / span);
  ramp_value += step * (ease < 0.25f ? 0.25f : ease);
  if (ramp_value > value) {
    ramp_value = value;
  }
  return (uint8_t)(ramp_value + 0.5f);
}

void throttle_set_curve(int8_t acc, int8_t brake, uint8_t mode) {
  curve_acc = acc;
  curve_brake = brake;
  curve_mode = mode;
  ESP_LOGI(TAG, "Throttle curve: mode %d, acc %d, brake %d", (int)mode,
           (int)acc, (int)brake);
}

static void update_command(void) {
  static bool low_batt_logged = false;
  uint8_t v = adc_get_latest_value();
  int8_t trim = trim_offset;
#ifdef CONFIG_TARGET_LITE
  if (throttle_should_use_neutral()) {
    v = VESC_NEUTRAL_VALUE;
  }
  /* Invert before the curve so the accel curve shapes the rider's accelerate
   * direction; the trim flips with it. */
  if (invert_throttle) {
    v = 255 - v;
    trim = -trim;
  }
#endif
  throttle_cmd_t c = {
      .neutral = throttle_apply_trim(VESC_NEUTRAL_VALUE, trim),
      .throttle_raw = last_throttle_raw,
      .brake_raw = last_brake_raw,
      .low_battery = battery_is_low_voltage(),
  };
  c.value = throttle_apply_ramp(
      throttle_apply_trim(
          throttle_apply_curve(v, curve_acc, curve_brake, curve_mode), trim),
      c.neutral);
  // Last, so nothing after it can undo it: low battery keeps the brake only.
  if (c.low_battery) {
    c.value = throttle_brake_only(c.value, c.neutral);
  }
  if (c.low_battery != low_batt_logged) {
    low_batt_logged = c.low_battery;
    if (c.low_battery) {
      ESP_LOGW(TAG, "Low battery - throttle blocked, brake only");
    }
  }
  taskENTER_CRITICAL(&cmd_lock);
  last_cmd = c;
  taskEXIT_CRITICAL(&cmd_lock);
}

void throttle_get_command(throttle_cmd_t *out) {
  taskENTER_CRITICAL(&cmd_lock);
  *out = last_cmd;
  taskEXIT_CRITICAL(&cmd_lock);
}

int8_t throttle_get_trim(void) { return trim_offset; }

esp_err_t throttle_step_trim(int delta) {
  int next = trim_offset + delta;
  if (next < -127 || next > 127) {
    return ESP_ERR_INVALID_ARG;
  }
  trim_offset = next;
  settings_t cfg;
  settings_get(&cfg);
  cfg.ble_trim = (int8_t)next;
  ESP_LOGI(TAG, "Trim offset: %d", next);
  return settings_set(&cfg);
}

void throttle_set_invert(bool inverted) {
  invert_throttle = inverted;
  ESP_LOGI(TAG, "Throttle inversion %s", inverted ? "enabled" : "disabled");
}
