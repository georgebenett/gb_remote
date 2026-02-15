#ifndef VESC_CONFIG_H
#define VESC_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

// NVS Storage keys (only user preferences are persisted)
#define VESC_NVS_NAMESPACE "vesc_cfg"
#define NVS_KEY_SPEED_UNIT "speed_unit"
#ifdef CONFIG_TARGET_LITE
#define NVS_KEY_INVERT_THROTTLE "inv_throttle"
#endif

#define VESC_NEUTRAL_VALUE 128

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
    bool speed_unit_mph;        // Speed unit: false = km/h, true = mph
#ifdef CONFIG_TARGET_LITE
    bool invert_throttle;       // Whether to invert throttle direction
#endif
} vesc_config_t;

esp_err_t vesc_config_init(void);
esp_err_t vesc_config_load(vesc_config_t *config);
esp_err_t vesc_config_save(const vesc_config_t *config);  // Only saves user preferences
int32_t vesc_config_get_speed(const vesc_config_t *config);

// Update motor config from VESC (called by BLE when data is received)
void vesc_config_update_motor(uint8_t motor_poles, uint16_t gear_ratio_x1000, uint16_t wheel_diameter_mm);

// Get current motor config (for display/debug)
void vesc_config_get_motor(vesc_motor_config_t *motor_config);

#endif // VESC_CONFIG_H