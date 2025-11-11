#include "level_assistant.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdlib.h>
#include <math.h>

// NVS Configuration for PID parameters
#define LEVEL_ASSIST_NVS_NAMESPACE "level_pid"
#define NVS_KEY_PID_KP "pid_kp"
#define NVS_KEY_PID_KI "pid_ki"
#define NVS_KEY_PID_KD "pid_kd"
#define NVS_KEY_PID_OUTPUT_MAX "pid_out_max"

static const char *TAG = "LEVEL_ASSIST";
static level_assistant_state_t state = {0};

// Runtime PID parameters (can be modified via serial)
static float pid_kp = LEVEL_ASSIST_PID_KP;
static float pid_ki = LEVEL_ASSIST_PID_KI;
static float pid_kd = LEVEL_ASSIST_PID_KD;
static float pid_output_max = LEVEL_ASSIST_PID_OUTPUT_MAX;

// Forward declarations for static functions

esp_err_t level_assistant_init(void) {
    // Initialize state
    state.enabled = false;
    state.was_at_zero_erpm = false;
    state.throttle_was_neutral = false;
    state.is_manual_mode = false;
    state.previous_erpm = 0;
    state.previous_throttle = LEVEL_ASSIST_NEUTRAL_CENTER;
    state.last_assist_time_ms = 0;
    state.last_manual_time_ms = 0;

    // Initialize PID state
    state.pid_integral = 0.0f;
    state.pid_previous_error = 0.0f;
    state.pid_output = 0.0f;
    state.pid_last_time_ms = 0;

    // Try to load PID parameters from NVS
    esp_err_t err = level_assistant_load_pid_from_nvs();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded PID parameters from NVS");
    } else {
        ESP_LOGI(TAG, "Using default PID parameters");
    }

    ESP_LOGI(TAG, "Level assistant initialized");
    return ESP_OK;
}

static bool is_throttle_neutral(uint32_t throttle_value) {
    return abs((int32_t)throttle_value - LEVEL_ASSIST_NEUTRAL_CENTER) <= LEVEL_ASSIST_NEUTRAL_THRESHOLD;
}

static uint32_t get_current_time_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

    // Main PID calculation
float calculate_pid_output(float setpoint_erpm, float current_erpm, uint32_t current_time_ms) {
    static float smoothed_output = 0.0f;

    float dt = (current_time_ms - state.pid_last_time_ms) / 1000.0f; // seconds
    if (dt <= 0.0f) dt = 0.001f; // prevent division by zero

    // --- PID error ---
    float error = setpoint_erpm - current_erpm;

    // --- Integral term ---
    state.pid_integral += error * dt;

    // --- Derivative term ---
    float derivative = (error - state.pid_previous_error) / dt;

    // --- Raw PID output ---
    float output = pid_kp * error + pid_ki * state.pid_integral + pid_kd * derivative;

    // --- Apply output limits ---
    if (output > pid_output_max) output = pid_output_max;
    if (output < -pid_output_max) output = -pid_output_max;

    // --- Smoothing ---
    smoothed_output = 0.7f * smoothed_output + 0.3f * output;

    // --- Update state ---
    state.pid_previous_error = error;
    state.pid_last_time_ms = current_time_ms;

    return smoothed_output;
}



uint32_t level_assistant_process(uint32_t throttle_value, int32_t current_erpm, bool is_enabled) {
    uint32_t current_time = get_current_time_ms();

    if (!is_enabled) {
        // Reset state when disabled
        state.enabled = false;
        state.is_manual_mode = false;
        state.pid_integral = 0.0f;
        state.pid_output = 0.0f;
        return throttle_value;
    }

    state.enabled = true;

    // Detect manual throttle input (ADC movement)
    uint32_t throttle_change = abs((int32_t)throttle_value - (int32_t)state.previous_throttle);
    if (throttle_change >= LEVEL_ASSIST_ADC_CHANGE_THRESHOLD) {
        state.is_manual_mode = true;
        state.last_manual_time_ms = current_time;
        // Reset PID when manual input detected
        state.pid_integral = 0.0f;
        state.pid_output = 0.0f;
    }

    // Check if we should exit manual mode (timeout)
    if (state.is_manual_mode && (current_time - state.last_manual_time_ms > LEVEL_ASSIST_MANUAL_TIMEOUT_MS)) {
        state.is_manual_mode = false;
        // ESP_LOGI(TAG, "Entering auto mode");
    }

    bool throttle_is_neutral = is_throttle_neutral(throttle_value);

    uint32_t modified_throttle = throttle_value;

    if (!state.is_manual_mode && throttle_is_neutral) {
        state.pid_output = calculate_pid_output(SETPOINT_RPM, (float)current_erpm, current_time);

        if (fabsf(state.pid_output) > 1.0f) {
            static float smoothed_output = 0.0f;
            smoothed_output = 0.3f * smoothed_output + 0.7f * state.pid_output;

            float throttle_correction = smoothed_output;

            if (throttle_correction > 0.0f) {
                modified_throttle = LEVEL_ASSIST_NEUTRAL_CENTER + (uint32_t)throttle_correction;

                // Ensure we don't exceed maximum throttle
                if (modified_throttle > LEVEL_ASSIST_MAX_THROTTLE) {
                    modified_throttle = LEVEL_ASSIST_MAX_THROTTLE;
                }
            }
        }
    } else {
        // Not in assist mode, gradually reset PID state to prevent windup
        state.pid_integral *= 0.95f; // Gradual decay
        state.pid_output *= 0.95f;   // Gradual output decay
    }

    // Update state for next iteration
    state.previous_throttle = throttle_value;

    return modified_throttle;
}

void level_assistant_reset_state(void) {
    // ESP_LOGI(TAG, "Level assistant state reset");
    state.is_manual_mode = false;
    state.previous_throttle = LEVEL_ASSIST_NEUTRAL_CENTER;
    state.last_assist_time_ms = 0;
    state.last_manual_time_ms = 0;

    // Reset PID state
    state.pid_integral = 0.0f;
    state.pid_previous_error = 0.0f;
    state.pid_output = 0.0f;
    state.pid_last_time_ms = 0;

}

level_assistant_state_t level_assistant_get_state(void) {
    return state;
}

// PID parameter setters
void level_assistant_set_pid_kp(float kp) {
    if (kp >= 0.0f && kp <= 10.0f) {  // Reasonable range
        pid_kp = kp;
        // Reset integral when changing gains to prevent windup
        state.pid_integral = 0.0f;
        // Save to NVS
        level_assistant_save_pid_to_nvs();
    }
}

void level_assistant_set_pid_ki(float ki) {
    if (ki >= 0.0f && ki <= 2.0f) {  // Reasonable range
        pid_ki = ki;
        // Reset integral when changing gains to prevent windup
        state.pid_integral = 0.0f;
        // Save to NVS
        level_assistant_save_pid_to_nvs();
    }
}

void level_assistant_set_pid_kd(float kd) {
    if (kd >= 0.0f && kd <= 1.0f) {  // Reasonable range
        pid_kd = kd;
        // Save to NVS
        level_assistant_save_pid_to_nvs();
    }
}

void level_assistant_set_pid_output_max(float output_max) {
    if (output_max >= 10.0f && output_max <= 100.0f) {  // Reasonable range
        pid_output_max = output_max;
        // Save to NVS
        level_assistant_save_pid_to_nvs();
    }
}

// PID parameter getters
float level_assistant_get_pid_kp(void) {
    return pid_kp;
}

float level_assistant_get_pid_ki(void) {
    return pid_ki;
}

float level_assistant_get_pid_kd(void) {
    return pid_kd;
}

float level_assistant_get_pid_output_max(void) {
    return pid_output_max;
}

esp_err_t level_assistant_save_pid_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS
    err = nvs_open(LEVEL_ASSIST_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    // Save PID parameters as blobs (4 bytes each for float)
    err = nvs_set_blob(nvs_handle, NVS_KEY_PID_KP, &pid_kp, sizeof(float));
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_blob(nvs_handle, NVS_KEY_PID_KI, &pid_ki, sizeof(float));
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_blob(nvs_handle, NVS_KEY_PID_KD, &pid_kd, sizeof(float));
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_blob(nvs_handle, NVS_KEY_PID_OUTPUT_MAX, &pid_output_max, sizeof(float));
    if (err != ESP_OK) goto cleanup;

    // Commit changes
    err = nvs_commit(nvs_handle);

cleanup:
    nvs_close(nvs_handle);
    return err;
}

esp_err_t level_assistant_load_pid_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS
    err = nvs_open(LEVEL_ASSIST_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    // Load PID parameters
    size_t required_size = sizeof(float);

    err = nvs_get_blob(nvs_handle, NVS_KEY_PID_KP, &pid_kp, &required_size);
    if (err != ESP_OK) goto cleanup;

    required_size = sizeof(float);
    err = nvs_get_blob(nvs_handle, NVS_KEY_PID_KI, &pid_ki, &required_size);
    if (err != ESP_OK) goto cleanup;

    required_size = sizeof(float);
    err = nvs_get_blob(nvs_handle, NVS_KEY_PID_KD, &pid_kd, &required_size);
    if (err != ESP_OK) goto cleanup;

    required_size = sizeof(float);
    err = nvs_get_blob(nvs_handle, NVS_KEY_PID_OUTPUT_MAX, &pid_output_max, &required_size);
    if (err != ESP_OK) goto cleanup;

cleanup:
    nvs_close(nvs_handle);
    return err;
}

esp_err_t level_assistant_reset_pid_to_defaults(void) {
    // Reset to default values from header
    pid_kp = LEVEL_ASSIST_PID_KP;
    pid_ki = LEVEL_ASSIST_PID_KI;
    pid_kd = LEVEL_ASSIST_PID_KD;
    pid_output_max = LEVEL_ASSIST_PID_OUTPUT_MAX;

    // Clear PID parameters from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(LEVEL_ASSIST_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    // Reset PID state to prevent windup
    state.pid_integral = 0.0f;
    state.pid_output = 0.0f;

    return ESP_OK;
}

