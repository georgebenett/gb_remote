#include "vesc_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "ble_spp_client.h"
#include <math.h>
static const char *TAG = "VESC_CONFIG";

// Default configuration values
static const vesc_config_t default_config = {
    .motor_pulley = 15,        // 15T motor pulley
    .wheel_pulley = 33,        // 33T wheel pulley
    .wheel_diameter_mm = 115,   // 115mm wheels
    .motor_poles = 14,         // 14 pole motor
    .invert_throttle = false   // Normal throttle direction
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

    uint8_t inv_throttle;
    err = nvs_get_u8(nvs_handle, NVS_KEY_INV_THROT, &inv_throttle);
    if (err != ESP_OK) goto cleanup;
    config->invert_throttle = (bool)inv_throttle;

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

    err = nvs_set_u8(nvs_handle, NVS_KEY_INV_THROT, (uint8_t)config->invert_throttle);
    if (err != ESP_OK) goto cleanup;

    err = nvs_commit(nvs_handle);

cleanup:
    nvs_close(nvs_handle);
    return err;
}


int32_t vesc_config_get_speed(const vesc_config_t *config) {
    int32_t erpm = get_latest_erpm();
    
    // Validate ERPM
    if (erpm > 100000 || erpm < -100000) {
        ESP_LOGW(TAG, "Invalid ERPM for speed calculation: %ld", erpm);
        return 0;
    }

    float rpm = (float)erpm/config->motor_poles;
    float gear_ratio = (float)config->wheel_pulley / (float)config->motor_pulley;
    float wheel_circumference_m = (float)config->wheel_diameter_mm / 1000.0f * 3.14159f;
    float wheel_RPM = rpm * gear_ratio;
    float speed = wheel_RPM * wheel_circumference_m * 60.0f / 1000.0f;

    // Validate final speed
    if (speed > 100.0f || speed < -100.0f) {
        ESP_LOGW(TAG, "Calculated speed out of range: %.2f", speed);
        return 0;
    }

    return (int32_t)fabsf(speed);
}
