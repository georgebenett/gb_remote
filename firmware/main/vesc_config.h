#ifndef VESC_CONFIG_H
#define VESC_CONFIG_H

#include "esp_err.h"
#include "sdkconfig.h"
#include "throttle_math.h"
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
#define NVS_KEY_NO_REVERSE "no_reverse"
#define NVS_KEY_CURVE_ACC "curve_acc"
#define NVS_KEY_CURVE_BRAKE "curve_brk"
#define NVS_KEY_CURVE_MODE "curve_mode"
#define NVS_KEY_SPEED_LIMIT "speed_limit"
#define NVS_KEY_RIDE_PROFILE "ride_profile"

// Speed limit modes: Off, 20 km/h, 25 km/h, 20 mph, 25 mph.
#define SPEED_LIMIT_MODE_COUNT 5

// Ride profiles: Beginner, Eco, Medium, Rocket.
#define RIDE_PROFILE_COUNT 4
#define RIDE_PROFILE_ROCKET (RIDE_PROFILE_COUNT - 1) // instant, no cap

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
  bool no_reverse;           // Below neutral only brakes, run by the receiver
  int8_t throttle_curve_acc; // Throttle curve, tenths (see throttle_math.h)
  int8_t throttle_curve_brake; // Brake curve, tenths
  uint8_t throttle_curve_mode; // throttle_curve_mode_t
  bool assist_push;        // Assistive push (endless mode), run by the receiver
  uint8_t assist_strength; // Assistive push strength, percent of current limit
  uint8_t assist_decay;    // Assistive push decay, motor rpm/s
  uint8_t speed_limit_mode; // 0 off, 1 20km/h, 2 25km/h, 3 20mph, 4 25mph;
                            // enforced by the receiver
  uint8_t ride_profile;     // 0 Beginner .. 3 Rocket: eased throttle ramp here,
                            // drive current cap on the receiver. Rocket = off.
#ifdef CONFIG_TARGET_LITE
  bool invert_throttle; // Whether to invert throttle direction
#endif
} vesc_config_t;

esp_err_t vesc_config_init(void);
esp_err_t vesc_config_load(vesc_config_t *config);
esp_err_t
vesc_config_save(const vesc_config_t *config); // Only saves user preferences
int32_t vesc_config_get_speed(const vesc_config_t *config);

// The cap a speed_limit_mode enforces, in km/h; 0 means no limit.
float vesc_config_speed_limit_kmh(uint8_t speed_limit_mode);

// Time (ms) a profile takes to ramp neutral -> full throttle; 0 = instant.
// Braking is never ramped.
uint16_t vesc_config_ride_profile_ramp_ms(uint8_t ride_profile);

// Percent of max motor current the profile may drive; enforced by the receiver.
uint8_t vesc_config_ride_profile_current_pct(uint8_t ride_profile);

// Update motor config from VESC (called by BLE when data is received)
void vesc_config_update_motor(uint8_t motor_poles, uint16_t gear_ratio_x1000,
                              uint16_t wheel_diameter_mm);

#endif // VESC_CONFIG_H