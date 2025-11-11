#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include "esp_err.h"
#include "throttle.h"


#define BATTERY_VOLTAGE_OFFSET 0.0f
#define BATTERY_VOLTAGE_SCALE 1.062f

#define VOLTAGE_DIVIDER_RATIO 2.0f
#define ADC_REFERENCE_VOLTAGE 3.3f
#define ADC_RESOLUTION        4095
#define BATTERY_MAX_VOLTAGE 4.15f
#define BATTERY_MIN_VOLTAGE 3.3f
#define BATTERY_VOLTAGE_SAMPLES 10


esp_err_t battery_init(void);
void battery_start_monitoring(void);
float battery_get_voltage(void);
int battery_get_percentage(void);

#endif // BATTERY_H