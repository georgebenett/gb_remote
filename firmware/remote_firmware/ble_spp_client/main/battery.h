#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include "esp_err.h"
#include "adc.h"

// Function declarations
esp_err_t battery_init(void);
void battery_start_monitoring(void);
float battery_get_voltage(void);
int battery_get_percentage(void);

// Add to battery.h
#define BATTERY_VOLTAGE_OFFSET 0.0f
#define BATTERY_VOLTAGE_SCALE 0.873f

// Voltage divider parameters (two 100K resistors)
#define VOLTAGE_DIVIDER_RATIO 2.0f  // For two equal resistors
#define ADC_REFERENCE_VOLTAGE 3.3f  // ESP32 ADC reference voltage
#define ADC_RESOLUTION        4095  // 12-bit ADC resolution
#define BATTERY_VOLTAGE_CALIBRATION_FACTOR 1.034f  // Adjusted based on multimeter reading (3.972V vs 4.00V)
#define BATTERY_MAX_VOLTAGE 4.0f  // Show 100% when battery is above 4V
#define BATTERY_MIN_VOLTAGE 3.3f  // Minimum safe LiPo cell voltage
#define BATTERY_VOLTAGE_SAMPLES 10


#endif // BATTERY_H