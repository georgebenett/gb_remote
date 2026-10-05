#ifndef ADC_H
#define ADC_H

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hw_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "target_config.h"
#include "throttle_math.h"
#include <stdint.h>

// Timing constants
#define ADC_SAMPLE_MS 1               // Delay between ADC samples for settling
#define CALIBRATION_STEP_DELAY_MS 100 // Delay between calibration steps
#define CALIBRATE_THROTTLE 0
/* One fresh sample per BLE connection event: match ADC_SEND_INTERVAL_MS in
 * ble.h, and keep the sampling burst well inside it. */
#define ADC_SAMPLE_PERIOD_MS 20
// Initial values that will be updated by calibration
#define ADC_INITIAL_MAX_VALUE 4095 // 12-bit ADC max
#define ADC_INITIAL_MIN_VALUE 0

#define ADC_OUTPUT_MAX_VALUE 255
#define ADC_OUTPUT_MIN_VALUE 0

// Calibration settings
#define ADC_CALIBRATION_SAMPLES                                                \
  600 // 600 samples over 6 seconds = 1 sample every 10ms
#define ADC_CALIBRATION_DELAY_MS                                               \
  10 // 10ms between samples for more accurate timing

#define NVS_NAMESPACE "adc_cal"
#define NVS_KEY_MIN "min_val"
#define NVS_KEY_MAX "max_val"
#define NVS_KEY_BRAKE_MIN "brake_min_val"
#define NVS_KEY_BRAKE_MAX "brake_max_val"
#define NVS_KEY_CALIBRATED "cal_done"
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

/** Curve then trim, for everything that leaves the remote. */
uint8_t throttle_shape_output(uint8_t value, int8_t trim);

/** Ride profile ramp: ms to go from neutral to full throttle, 0 = instant.
 *  Reloaded at adc_init() and on every config-tool change. */
void throttle_set_ride_profile(uint16_t ramp_ms);

/** Eases `value` up toward full throttle at the profile's rate. Stateful: call
 *  once per sample, with the (trimmed) neutral. Braking, release and neutral
 *  pass through instantly. */
uint8_t throttle_apply_ramp(uint8_t value, uint8_t neutral);

/** Throttle curve, owned by the config tool and reloaded at adc_init(). */
void throttle_set_curve(int8_t acc, int8_t brake, uint8_t mode);

#ifdef CONFIG_TARGET_DUAL_THROTTLE
int32_t brake_read_value(void);
uint8_t get_throttle_brake_ble_value(
    void); // Combined throttle/brake value for BLE (0-255, 128=neutral)
void brake_get_calibration_values(uint32_t *min_val, uint32_t *max_val);
#endif

// ADC handle and mutex accessors for other modules (e.g., battery)
adc_oneshot_unit_handle_t adc_get_handle(void);
SemaphoreHandle_t adc_get_mutex(void);
bool adc_is_initialized(void);

#endif // ADC_H