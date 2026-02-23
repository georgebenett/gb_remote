#ifndef BATTERY_H
#define BATTERY_H

#include "esp_err.h"
#include "throttle.h"
#include <stdbool.h>

#define BATTERY_VOLTAGE_OFFSET 0.0f
#define BATTERY_VOLTAGE_SCALE 1.075f

#define VOLTAGE_DIVIDER_RATIO 2.0f
#define ADC_REFERENCE_VOLTAGE 3.3f
#define ADC_RESOLUTION 4095
#define BATTERY_MAX_VOLTAGE 4.15f
#define BATTERY_LOW_VOLTAGE_THRESHOLD 2.95f
#define BATTERY_VOLTAGE_SAMPLES 10

// Timing constants
#define ADC_SAMPLE_MS 2                 // Delay between ADC samples
#define TASK_STARTUP_DELAY_MS 100       // Delay for task initialization
#define BATTERY_MONITOR_INTERVAL_MS 500 // Battery monitoring poll rate
#define LOW_BATTERY_ALERT_DELAY_MS 500  // Haptic feedback delay
#define LOW_BATTERY_WARNING_MS 2000     // Show warning before shutdown
#define POWER_OFF_SETTLE_MS 100         // Delay after power pin toggle

esp_err_t battery_init(void);
void battery_start_monitoring(void);
float battery_get_voltage(void);
int battery_get_percentage(void);
bool battery_is_low_voltage(void);

// Battery ADC functions (internal use)
esp_err_t adc_battery_init(void);
int32_t adc_read_battery_voltage(uint8_t channel);

#endif // BATTERY_H