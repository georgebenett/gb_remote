// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef THROTTLE_H
#define THROTTLE_H

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hw_config.h"
#include "target_config.h"
#include "throttle_math.h"
#include <stdint.h>

// Timing constants
#define CALIBRATION_STEP_DELAY_MS 100 // Delay between calibration steps
/* One fresh sample per BLE connection event: match ADC_SEND_INTERVAL_MS in
 * ble.h, and keep the sampling burst well inside it. */
#define ADC_SAMPLE_PERIOD_MS 20
// Initial values that will be updated by calibration
#define ADC_INITIAL_MAX_VALUE 4095 // 12-bit ADC max
#define ADC_INITIAL_MIN_VALUE 0

// Calibration settings
// About 6 s: the reads between the delays take microseconds.
#define ADC_CALIBRATION_SAMPLES 600
#define ADC_CALIBRATION_DELAY_MS 10
// Progress callback invoked periodically during calibration.
// Parameters: sample index, total samples, throttle current/min/max, brake
// current/min/max. Brake values are only meaningful for
// CONFIG_TARGET_DUAL_THROTTLE builds.
typedef void (*calibration_progress_cb_t)(
    uint16_t sample, uint16_t total, uint32_t throttle_current,
    uint32_t throttle_min, uint32_t throttle_max, uint32_t brake_current,
    uint32_t brake_min, uint32_t brake_max);

// Calibration result: success or specific failure reason
typedef enum {
  CAL_OK = 0,
  CAL_FAIL_THROTTLE_RANGE, // Throttle range < 150 ADC units
  CAL_FAIL_THROTTLE_NO_READINGS,
  CAL_FAIL_BRAKE_RANGE, // Brake range < 150 (dual throttle)
  CAL_FAIL_BRAKE_NO_READINGS,
  CAL_FAIL_SAVE, // NVS save failed after calibration passed
} calibration_result_t;

esp_err_t adc_init(void);
int32_t throttle_read_value(void);
void adc_start_task(void);
uint32_t adc_get_latest_value(void);
calibration_result_t throttle_calibrate(calibration_progress_cb_t progress_cb);
bool throttle_is_calibrated(void);
void throttle_get_calibration_values(uint32_t *min_val, uint32_t *max_val);
bool throttle_should_use_neutral(void);

/** What the radio sends this sample. */
typedef struct {
  uint8_t value;   // oriented, curved, trimmed, ramped, low-battery clamped
  uint8_t neutral; // a centred lever after trim: the post-connect hold value
  int32_t throttle_raw, brake_raw; // last ADC averages, -1 = read error
  bool low_battery;
} throttle_cmd_t;

/** The command for the latest sample. The sampler builds one per sample and
 *  then wakes the sender (ble_notify_throttle_sample). */
void throttle_get_command(throttle_cmd_t *out);

/** Output trim, -127..127, saved in settings. */
int8_t throttle_get_trim(void);
/** Step the trim by +-1. ESP_ERR_INVALID_ARG at the limit. */
esp_err_t throttle_step_trim(int delta);
/** LITE: flip the lever direction (live, not only at boot). */
void throttle_set_invert(bool inverted);

/** Ride profile ramp: ms to go from neutral to full throttle, 0 = instant.
 *  Reloaded at adc_init() and on every config-tool change. */
void throttle_set_ride_profile(uint16_t ramp_ms);

/** Throttle curve, owned by the config tool and reloaded at adc_init(). */
void throttle_set_curve(int8_t acc, int8_t brake, uint8_t mode);

#ifdef CONFIG_TARGET_DUAL_THROTTLE
int32_t brake_read_value(void);
void brake_get_calibration_values(uint32_t *min_val, uint32_t *max_val);
#endif

// ADC handle and mutex accessors for other modules (e.g., battery)
adc_oneshot_unit_handle_t adc_get_handle(void);
SemaphoreHandle_t adc_get_mutex(void);
bool adc_is_initialized(void);

#endif // THROTTLE_H