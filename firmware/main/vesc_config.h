#ifndef VESC_CONFIG_H
#define VESC_CONFIG_H

#include "esp_err.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>

// NVS Storage keys (only user preferences are persisted)
#define VESC_NVS_NAMESPACE "vesc_cfg"
#define NVS_KEY_SPEED_UNIT "speed_unit"
#define NVS_KEY_DUAL_CONNECTION "dual_conn"
#define NVS_KEY_BATTERY_CELLS "batt_cells"
#define NVS_KEY_BATTERY_CELL_TYPE "batt_celltyp"
#define NVS_KEY_SMART_REVERSE "smart_rev"
#define NVS_KEY_ASSIST_PUSH "assist_push"
#define NVS_KEY_ASSIST_STRENGTH "assist_str"
#define NVS_KEY_ASSIST_DECAY "assist_dec"

// Assistive push tuning, clamped again by the receiver that runs it.
#define ASSIST_STRENGTH_DEFAULT 15 // percent of the VESC current limit
#define ASSIST_STRENGTH_MIN 5
#define ASSIST_STRENGTH_MAX 50
#define ASSIST_DECAY_DEFAULT 30 // motor rpm/s bled off the held speed
#define ASSIST_DECAY_MIN 10
#define ASSIST_DECAY_MAX 120
#ifdef CONFIG_TARGET_LITE
#define NVS_KEY_INVERT_THROTTLE "inv_throttle"
#endif

#define VESC_NEUTRAL_VALUE 128
#define THROTTLE_NEUTRAL_DEADBAND                                              \
  15 // ADC units around neutral (0-255 scale) that snap to exact neutral

// Motor configuration from VESC (received via BLE, NOT persisted)
typedef struct {
  uint8_t motor_poles;        // Number of motor poles (from VESC via BLE)
  uint16_t gear_ratio_x1000;  // Gear ratio * 1000 (from VESC via BLE)
  uint16_t wheel_diameter_mm; // Wheel diameter in mm (from VESC via BLE)
} vesc_motor_config_t;

// Full config including user preferences
typedef struct {
  // Motor config from VESC (volatile - not saved to NVS)
  uint8_t motor_poles;        // Number of motor poles (from VESC via BLE)
  uint16_t gear_ratio_x1000;  // Gear ratio * 1000 (from VESC via BLE)
  uint16_t wheel_diameter_mm; // Wheel diameter in mm (from VESC via BLE)
  // User preferences (persisted to NVS)
  bool speed_unit_mph;       // Speed unit: false = km/h, true = mph
  bool dual_connection;      // Allow connecting to two receivers simultaneously
  uint8_t battery_cells;     // Skate pack series count: 0 = unset (show pack
                             // voltage), else 5..20 (estimate percentage)
  uint8_t battery_cell_type; // Cell chemistry (battery_cell_type_t) picking
                             // which SoC curve the estimate uses
  bool smart_reverse;        // VESC-style smart reverse, run by the receiver
  bool assist_push;        // Assistive push (endless mode), run by the receiver
  uint8_t assist_strength; // Assistive push strength, percent of current limit
  uint8_t assist_decay;    // Assistive push decay, motor rpm/s
#ifdef CONFIG_TARGET_LITE
  bool invert_throttle; // Whether to invert throttle direction
#endif
} vesc_config_t;

esp_err_t vesc_config_init(void);
esp_err_t vesc_config_load(vesc_config_t *config);
esp_err_t
vesc_config_save(const vesc_config_t *config); // Only saves user preferences
int32_t vesc_config_get_speed(const vesc_config_t *config);

// Update motor config from VESC (called by BLE when data is received)
void vesc_config_update_motor(uint8_t motor_poles, uint16_t gear_ratio_x1000,
                              uint16_t wheel_diameter_mm);

#endif // VESC_CONFIG_H