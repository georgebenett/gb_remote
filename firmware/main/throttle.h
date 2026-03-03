#ifndef ADC_H
#define ADC_H

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hw_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "target_config.h"
#include <stdint.h>

// Timing constants
#define ADC_SAMPLE_MS 2               // Delay between ADC samples for settling
#define CALIBRATION_STEP_DELAY_MS 100 // Delay between calibration steps
#define CALIBRATE_THROTTLE 0
#define ADC_SAMPLING_TICKS 30
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
uint8_t map_throttle_value(uint32_t adc_value);
calibration_result_t throttle_calibrate(calibration_progress_cb_t progress_cb);
bool throttle_is_calibrated(void);
void adc_deinit(void);
void throttle_get_calibration_values(uint32_t *min_val, uint32_t *max_val);
bool throttle_should_use_neutral(void);

#ifdef CONFIG_TARGET_DUAL_THROTTLE
int32_t brake_read_value(void);
uint8_t map_brake_value(uint32_t adc_value);
uint8_t get_throttle_brake_ble_value(
    void); // Combined throttle/brake value for BLE (0-255, 128=neutral)
void brake_get_calibration_values(uint32_t *min_val, uint32_t *max_val);
#elif defined(CONFIG_TARGET_LITE)
uint8_t
map_adc_value(uint32_t adc_value); // Single throttle mapping for lite mode
#endif

// Throttle curve exponent: 1.0 = linear, higher = gentler low-end
float throttle_get_curve_exponent(void);
esp_err_t throttle_set_curve_exponent(float exponent);

// ADC handle accessors for other modules (e.g., battery)
adc_oneshot_unit_handle_t adc_get_handle(void);
bool adc_is_initialized(void);

#endif // ADC_H