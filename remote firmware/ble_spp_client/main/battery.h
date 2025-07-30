#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include "esp_err.h"

// Function declarations
esp_err_t battery_init(void);
void battery_start_monitoring(void);
float battery_get_voltage(void);
int battery_get_percentage(void);

// Add to battery.h
#define BATTERY_VOLTAGE_OFFSET 0.0f
#define BATTERY_VOLTAGE_SCALE 0.873f

#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3

#endif // BATTERY_H