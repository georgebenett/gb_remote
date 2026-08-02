#ifndef BATTERY_H
#define BATTERY_H

#include "esp_err.h"
#include "throttle.h"
#include <stdbool.h>
#include <stdint.h>

#define BATTERY_VOLTAGE_OFFSET 0.0f
#define BATTERY_VOLTAGE_SCALE 1.075f

#define VOLTAGE_DIVIDER_RATIO 2.0f
#define ADC_REFERENCE_VOLTAGE 3.3f
#define ADC_RESOLUTION 4095
#define BATTERY_LOW_VOLTAGE_THRESHOLD 2.95f
#define BATTERY_VOLTAGE_SAMPLES 10

// Timing constants (ADC_SAMPLE_MS comes from throttle.h)
#define BATTERY_TASK_STARTUP_DELAY_MS 100 // Delay for task initialization
#define BATTERY_MONITOR_INTERVAL_MS 500   // Battery monitoring poll rate
#define LOW_BATTERY_ALERT_DELAY_MS 500    // Haptic feedback delay
#define LOW_BATTERY_WARNING_MS 2000       // Show warning before shutdown
#define POWER_OFF_SETTLE_MS 100           // Delay after power pin toggle

// Cell chemistry families the skate pack SoC estimate can use. Cells listed for
// each entry sit within ~1-2% SoC of one another at the same resting voltage.
typedef enum {
  BATTERY_CELL_LI_ION_HIGH_DRAIN =
      0, // Molicel P42A/P45B, Samsung 30Q/40T, VTC6
  BATTERY_CELL_LI_ION_HIGH_CAPACITY = 1, // Molicel P50B, Samsung 50S, LG M50LT
  BATTERY_CELL_LIFEPO4 = 2,
  BATTERY_CELL_LIPO = 3,
  BATTERY_CELL_TYPE_COUNT
} battery_cell_type_t;

esp_err_t battery_init(void);
void battery_start_monitoring(void);
float battery_get_voltage(void);
int battery_get_percentage(void);
// Estimate a pack's state of charge from its total voltage, series cell count
// and cell chemistry. Returns -1 when cells is 0 or the voltage is invalid.
int battery_pack_percentage(float pack_voltage, uint8_t cells,
                            uint8_t cell_type);
bool battery_is_low_voltage(void);

// Battery ADC functions (internal use)
esp_err_t adc_battery_init(void);
int32_t adc_read_battery_voltage(uint8_t channel);

#endif // BATTERY_H