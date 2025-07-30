#ifndef VESC_CONFIG_H
#define VESC_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// NVS Storage keys
#define VESC_NVS_NAMESPACE "vesc_cfg"
#define NVS_KEY_MOTOR_PULLEY "mot_pulley"
#define NVS_KEY_WHEEL_PULLEY "wheel_pulley"
#define NVS_KEY_WHEEL_DIAM "wheel_diam"
#define NVS_KEY_MOTOR_POLES "motor_poles"
#define NVS_KEY_INV_THROT "inv_throttle"

typedef struct {
    uint8_t motor_pulley;      // Number of teeth on motor pulley
    uint8_t wheel_pulley;      // Number of teeth on wheel pulley
    uint8_t wheel_diameter_mm; // Wheel diameter in millimeters
    uint8_t motor_poles;      // Number of motor poles
    bool invert_throttle;     // Whether to invert the throttle direction
} vesc_config_t;

esp_err_t vesc_config_init(void);
esp_err_t vesc_config_load(vesc_config_t *config);
esp_err_t vesc_config_save(const vesc_config_t *config);
int32_t vesc_config_get_speed(const vesc_config_t *config);

#endif // VESC_CONFIG_H 