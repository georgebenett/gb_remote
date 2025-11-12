#include "vesc_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "ble.h"
#include <math.h>
static const char *TAG = "VESC_CONFIG";

// Default configuration values
static const vesc_config_t default_config = {
    .motor_pulley = 15,        // 15T motor pulley
    .wheel_pulley = 33,        // 33T wheel pulley
    .wheel_diameter_mm = 115,   // 115mm wheels
    .motor_poles = 14,         // 14 pole motor
    .speed_unit_mph = false,   // Speed unit: km/h by default
#ifdef CONFIG_TARGET_LITE
    .invert_throttle = false   // Throttle inversion disabled by default
#endif
};

esp_err_t vesc_config_init(void) {
    // Try to load config, if it fails (first time) save defaults
    vesc_config_t config;
    esp_err_t err = vesc_config_load(&config);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No configuration found, saving defaults");
        return vesc_config_save(&default_config);
    }

    return err;
}

esp_err_t vesc_config_load(vesc_config_t *config) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(VESC_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return err;

    // Load each value, with error checking
    err = nvs_get_u8(nvs_handle, NVS_KEY_MOTOR_PULLEY, &config->motor_pulley);
    if (err != ESP_OK) goto cleanup;

    err = nvs_get_u8(nvs_handle, NVS_KEY_WHEEL_PULLEY, &config->wheel_pulley);
    if (err != ESP_OK) goto cleanup;

    err = nvs_get_u8(nvs_handle, NVS_KEY_WHEEL_DIAM, &config->wheel_diameter_mm);
    if (err != ESP_OK) goto cleanup;

    err = nvs_get_u8(nvs_handle, NVS_KEY_MOTOR_POLES, &config->motor_poles);
    if (err != ESP_OK) goto cleanup;

    uint8_t speed_unit;
    err = nvs_get_u8(nvs_handle, NVS_KEY_SPEED_UNIT, &speed_unit);
    if (err == ESP_OK) {
        config->speed_unit_mph = (bool)speed_unit;
    } else {
        // Default to km/h if key doesn't exist (backward compatibility)
        config->speed_unit_mph = false;
        err = ESP_OK; // Don't fail the entire load for missing speed unit setting
    }

#ifdef CONFIG_TARGET_LITE
    uint8_t invert_throttle;
    err = nvs_get_u8(nvs_handle, NVS_KEY_INVERT_THROTTLE, &invert_throttle);
    if (err == ESP_OK) {
        config->invert_throttle = (bool)invert_throttle;
    } else {
        // Default to false if key doesn't exist (backward compatibility)
        config->invert_throttle = false;
        err = ESP_OK; // Don't fail the entire load for missing invert throttle setting
    }
#endif

cleanup:
    nvs_close(nvs_handle);
    return err;
}

esp_err_t vesc_config_save(const vesc_config_t *config) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(VESC_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    // Save each value
    err = nvs_set_u8(nvs_handle, NVS_KEY_MOTOR_PULLEY, config->motor_pulley);
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_u8(nvs_handle, NVS_KEY_WHEEL_PULLEY, config->wheel_pulley);
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_u8(nvs_handle, NVS_KEY_WHEEL_DIAM, config->wheel_diameter_mm);
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_u8(nvs_handle, NVS_KEY_MOTOR_POLES, config->motor_poles);
    if (err != ESP_OK) goto cleanup;

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

int32_t vesc_config_get_speed(const vesc_config_t *config) {
    // Validate config and motor poles
    if (config == NULL || config->motor_poles == 0 || config->motor_pulley == 0) {
        return 0;
    }

    int32_t erpm = get_latest_erpm();

    float rpm = (float)erpm / (float)config->motor_poles;  // Convert to float early
    float gear_ratio = (float)config->wheel_pulley / (float)config->motor_pulley;

    // Check for division by zero in gear ratio
    if (gear_ratio == 0.0f) {
        return 0;
    }

    float wheel_circumference_m = (float)config->wheel_diameter_mm / 1000.0f * M_PI;
    float wheel_RPM = rpm / gear_ratio;  // Changed multiplication to division for gear reduction
    float speed_kmh = wheel_RPM * wheel_circumference_m * 60.0f / 1000.0f;

    if (speed_kmh < 0){
        speed_kmh *= -1;
    }

    // Convert to mph if needed (1 km/h = 0.621371 mph)
    if (config->speed_unit_mph) {
        speed_kmh *= 0.621371f;
    }

    return (int32_t)speed_kmh;
}