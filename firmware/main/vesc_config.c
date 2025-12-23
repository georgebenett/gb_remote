#include "vesc_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "ble.h"
#include <math.h>
static const char *TAG = "VESC_CONFIG";

// Current motor config from VESC (volatile, updated via BLE)
static vesc_motor_config_t current_motor_config = {
    .motor_poles = 10,          // Default until VESC sends config
    .gear_ratio_x1000 = 1000,   // Default until VESC sends config
    .wheel_diameter_mm = 100,   // Default until VESC sends config
};

// Default user preferences (saved to NVS)
static const vesc_config_t default_config = {
    .motor_poles = 11,          // Will be overwritten by VESC
    .gear_ratio_x1000 = 1001,   // Will be overwritten by VESC
    .wheel_diameter_mm = 101,   // Will be overwritten by VESC
    .speed_unit_mph = false,    // Speed unit: km/h by default
#ifdef CONFIG_TARGET_LITE
    .invert_throttle = false    // Throttle inversion disabled by default
#endif
};

esp_err_t vesc_config_init(void) {
    // Try to load user preferences, if it fails (first time) save defaults
    vesc_config_t config;
    esp_err_t err = vesc_config_load(&config);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No user preferences found, saving defaults");
        return vesc_config_save(&default_config);
    }

    return err;
}

esp_err_t vesc_config_load(vesc_config_t *config) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Start with current motor config from VESC (or defaults if not yet received)
    config->motor_poles = current_motor_config.motor_poles;
    config->gear_ratio_x1000 = current_motor_config.gear_ratio_x1000;
    config->wheel_diameter_mm = current_motor_config.wheel_diameter_mm;

    // Set default user preferences
    config->speed_unit_mph = false;
#ifdef CONFIG_TARGET_LITE
    config->invert_throttle = false;
#endif

    // Load user preferences from NVS
    err = nvs_open(VESC_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        // No NVS data, use defaults - this is OK
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }

    uint8_t speed_unit;
    err = nvs_get_u8(nvs_handle, NVS_KEY_SPEED_UNIT, &speed_unit);
    if (err == ESP_OK) {
        config->speed_unit_mph = (bool)speed_unit;
    }

#ifdef CONFIG_TARGET_LITE
    uint8_t invert_throttle;
    err = nvs_get_u8(nvs_handle, NVS_KEY_INVERT_THROTTLE, &invert_throttle);
    if (err == ESP_OK) {
        config->invert_throttle = (bool)invert_throttle;
    }
#endif

    nvs_close(nvs_handle);
    return ESP_OK;
}

esp_err_t vesc_config_save(const vesc_config_t *config) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Only save user preferences to NVS (NOT motor config from VESC)
    err = nvs_open(VESC_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(nvs_handle, NVS_KEY_SPEED_UNIT, (uint8_t)config->speed_unit_mph);
    if (err != ESP_OK) goto cleanup;

#ifdef CONFIG_TARGET_LITE
    err = nvs_set_u8(nvs_handle, NVS_KEY_INVERT_THROTTLE, (uint8_t)config->invert_throttle);
    if (err != ESP_OK) goto cleanup;
#endif

    err = nvs_commit(nvs_handle);

cleanup:
    nvs_close(nvs_handle);
    return err;
}

void vesc_config_update_motor(uint8_t motor_poles, uint16_t gear_ratio_x1000, uint16_t wheel_diameter_mm) {
    if (motor_poles != current_motor_config.motor_poles ||
        gear_ratio_x1000 != current_motor_config.gear_ratio_x1000 ||
        wheel_diameter_mm != current_motor_config.wheel_diameter_mm) {

        current_motor_config.motor_poles = motor_poles;
        current_motor_config.gear_ratio_x1000 = gear_ratio_x1000;
        current_motor_config.wheel_diameter_mm = wheel_diameter_mm;

        ESP_LOGI(TAG, "Motor config from VESC: poles=%d, gear_ratio=%.3f, wheel_diam=%dmm",
                 motor_poles, gear_ratio_x1000 / 1000.0f, wheel_diameter_mm);
    }
}

void vesc_config_get_motor(vesc_motor_config_t *motor_config) {
    if (motor_config != NULL) {
        motor_config->motor_poles = current_motor_config.motor_poles;
        motor_config->gear_ratio_x1000 = current_motor_config.gear_ratio_x1000;
        motor_config->wheel_diameter_mm = current_motor_config.wheel_diameter_mm;
    }
}

int32_t vesc_config_get_speed(const vesc_config_t *config) {
    // Validate config and motor poles
    if (config == NULL || config->motor_poles == 0 || config->gear_ratio_x1000 == 0) {
        return 0;
    }

    int32_t erpm = get_latest_erpm();

    // Convert ERPM to mechanical RPM: ERPM / pole_pairs = ERPM / (motor_poles / 2)
    float pole_pairs = (float)config->motor_poles / 2.0f;
    float rpm = (float)erpm / pole_pairs;
    float gear_ratio = (float)config->gear_ratio_x1000 / 1000.0f;  // gear_ratio from BLE (already scaled)

    float wheel_circumference_m = (float)config->wheel_diameter_mm / 1000.0f * M_PI;
    float wheel_RPM = rpm / gear_ratio;  // Wheel RPM after gear reduction
    float speed_kmh = wheel_RPM * wheel_circumference_m * 60.0f / 1000.0f;

    if (speed_kmh < 0) {
        speed_kmh *= -1;
    }

    // Convert to mph if needed (1 km/h = 0.621371 mph)
    if (config->speed_unit_mph) {
        speed_kmh *= 0.621371f;
    }

    return (int32_t)speed_kmh;
}