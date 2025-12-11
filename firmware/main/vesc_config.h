#ifndef VESC_CONFIG_H
#define VESC_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

// NVS Storage keys
#define VESC_NVS_NAMESPACE "vesc_cfg"
#define NVS_KEY_MOTOR_POLES "motor_poles"
#define NVS_KEY_GEAR_RATIO "gear_ratio"
#define NVS_KEY_WHEEL_DIAM "wheel_diam"
#define NVS_KEY_SPEED_UNIT "speed_unit"
#ifdef CONFIG_TARGET_LITE
#define NVS_KEY_INVERT_THROTTLE "inv_throttle"
#endif

typedef struct {
    uint8_t motor_poles;       // Number of motor poles (from BLE)
    uint16_t gear_ratio_x1000; // Gear ratio * 1000 (from BLE, divide by 1000 for actual ratio)
    uint16_t wheel_diameter_mm; // Wheel diameter in millimeters (from BLE)
    bool speed_unit_mph;       // Speed unit: false = km/h, true = mi/h
#ifdef CONFIG_TARGET_LITE
    bool invert_throttle;      // Whether to invert throttle direction (lite mode only)
#endif
} vesc_config_t;

esp_err_t vesc_config_init(void);
esp_err_t vesc_config_load(vesc_config_t *config);
esp_err_t vesc_config_save(const vesc_config_t *config);
int32_t vesc_config_get_speed(const vesc_config_t *config);

#endif // VESC_CONFIG_H