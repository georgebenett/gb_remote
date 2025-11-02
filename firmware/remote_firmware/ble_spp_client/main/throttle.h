#ifndef ADC_H
#define ADC_H

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_adc/adc_oneshot.h"
#include "hw_config.h"

#define CALIBRATE_THROTTLE 0

#define ADC_SAMPLING_TICKS 20

// Initial values that will be updated by calibration
#define ADC_INITIAL_MAX_VALUE 4095  // 12-bit ADC max
#define ADC_INITIAL_MIN_VALUE 0

#define ADC_OUTPUT_MAX_VALUE 255
#define ADC_OUTPUT_MIN_VALUE 0

// Calibration settings
#define ADC_CALIBRATION_SAMPLES 600  // 600 samples over 6 seconds = 1 sample every 10ms
#define ADC_CALIBRATION_DELAY_MS 10  // 10ms between samples for more accurate timing

#define NVS_NAMESPACE "adc_cal"
#define NVS_KEY_MIN "min_val"
#define NVS_KEY_MAX "max_val"
#define NVS_KEY_BRAKE_MIN "brake_min_val"
#define NVS_KEY_BRAKE_MAX "brake_max_val"
#define NVS_KEY_CALIBRATED "cal_done"

#define ADC_THROTTLE_OFFSET 0

esp_err_t adc_init(void);
int32_t throttle_read_value(void);
int32_t brake_read_value(void);
void adc_start_task(void);
uint32_t adc_get_latest_value(void);  // Deprecated, use get_throttle_brake_ble_value instead
uint8_t map_throttle_value(uint32_t adc_value);
uint8_t map_brake_value(uint32_t adc_value);
uint8_t get_throttle_brake_ble_value(void);  // Combined throttle/brake value for BLE (0-255, 127=neutral)
void throttle_calibrate(void);
bool throttle_is_calibrated(void);
void adc_deinit(void);
void throttle_get_calibration_values(uint32_t *min_val, uint32_t *max_val);
void brake_get_calibration_values(uint32_t *min_val, uint32_t *max_val);
bool adc_get_calibration_status(void);
bool adc_is_calibrating(void);

// Add these function declarations for battery ADC
esp_err_t adc_battery_init(void);
int32_t adc_read_battery_voltage(uint8_t channel);

#endif // ADC_H