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
#define NVS_KEY_LEARNED_FLAG "learned"

static const char *TAG = "LEVEL_ASSIST";
static level_assistant_state_t state = {0};

// Runtime PID parameters (can be modified via serial)
static float pid_kp = LEVEL_ASSIST_PID_KP;
static float pid_ki = LEVEL_ASSIST_PID_KI;
static float pid_kd = LEVEL_ASSIST_PID_KD;
static float pid_output_max = LEVEL_ASSIST_PID_OUTPUT_MAX;

// Forward declarations for static functions
#if LEVEL_ASSIST_ADAPTIVE_ENABLED
static void update_performance_metrics(float error, float output);
static void adapt_pid_parameters(void);
static bool is_system_oscillating(void);
static bool is_system_stable(void);
#endif

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

    // Try to load learned PID parameters from NVS
    esp_err_t err = level_assistant_load_pid_from_nvs();
    if (err == ESP_OK) {
        // ESP_LOGI(TAG, "Loaded learned PID parameters from NVS");
    } else {
        // ESP_LOGI(TAG, "Using default PID parameters");
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

typedef struct {
    float pid_previous_error;
    uint32_t pid_last_time_ms;
} pid_state_t;




// Main PID calculation
float calculate_pid_output(float setpoint_erpm, float current_erpm, uint32_t current_time_ms) {
    static float smoothed_output = 0.0f;
    static float Kp = 1.0f;  // example PID constants
    static float Ki = 0.01f;
    static float Kd = 0.1f;
    static float integral = 0.0f;

    float dt = (current_time_ms - state.pid_last_time_ms) / 1000.0f; // seconds
    if (dt <= 0.0f) dt = 0.001f; // prevent division by zero

    // --- PID error ---
    float error = setpoint_erpm - current_erpm;

    // --- Integral term ---
    integral += error * dt;

    // --- Derivative term ---
    float derivative = (error - state.pid_previous_error) / dt;

    // --- Raw PID output ---
    float output = Kp * error + Ki * integral + Kd * derivative;

    // --- Asymmetric smoothing ---
    if (output < smoothed_output) {
        smoothed_output = 0.3f * smoothed_output + 0.7f * output;
    } else {
        smoothed_output = 0.7f * smoothed_output + 0.3f * output;
    }

    // --- Adaptive PID (periodic) ---
#if LEVEL_ASSIST_ADAPTIVE_ENABLED
    static uint32_t last_adapt_ms = 0;
    if (current_time_ms - last_adapt_ms >= ADAPT_INTERVAL_MS) {
        update_performance_metrics(error, smoothed_output);
        adapt_pid_parameters();
        last_adapt_ms = current_time_ms;
    }
#endif

    // --- Update history ---
    state.pid_previous_error = error;
    state.pid_last_time_ms = current_time_ms;

    return smoothed_output;
}


#if LEVEL_ASSIST_ADAPTIVE_ENABLED
static void update_performance_metrics(float error, float output) {
    // Store error and output in circular buffer
    state.error_history[state.history_index] = fabsf(error);
    state.output_history[state.history_index] = output;

    // Detect oscillation (output sign changes)
    if (state.samples_collected > 0) {
        float current_sign = (output > 0) ? 1.0f : -1.0f;
        if (current_sign != state.last_output_sign && fabsf(output) > 1.0f) {
            state.oscillation_count++;
        }
        state.last_output_sign = current_sign;
    }

    // Update circular buffer index
    state.history_index = (state.history_index + 1) % LEVEL_ASSIST_PERFORMANCE_WINDOW;
    if (state.samples_collected < LEVEL_ASSIST_PERFORMANCE_WINDOW) {
        state.samples_collected++;
    }

    // Calculate average error and variance when we have enough samples
    if (state.samples_collected >= LEVEL_ASSIST_PERFORMANCE_WINDOW) {
        float sum = 0.0f;
        float sum_squares = 0.0f;

        for (int i = 0; i < LEVEL_ASSIST_PERFORMANCE_WINDOW; i++) {
            sum += state.error_history[i];
            sum_squares += state.error_history[i] * state.error_history[i];
        }

        state.avg_error = sum / LEVEL_ASSIST_PERFORMANCE_WINDOW;
        state.error_variance = (sum_squares / LEVEL_ASSIST_PERFORMANCE_WINDOW) - (state.avg_error * state.avg_error);
    }
}

static void adapt_pid_parameters(void) {
    // Only adapt if we have enough data
    if (state.samples_collected < LEVEL_ASSIST_PERFORMANCE_WINDOW) {
        return;
    }

    // Store original values to detect significant changes
    float orig_kp = pid_kp;
    float orig_ki = pid_ki;
    float orig_kd = pid_kd;

    bool system_oscillating = is_system_oscillating();
    bool system_stable = is_system_stable();

    // Adaptive rules based on system behavior
    if (system_oscillating) {
        // System is oscillating - reduce aggressive gains
        pid_kp *= (1.0f - LEVEL_ASSIST_LEARNING_RATE);
        pid_kd *= (1.0f - LEVEL_ASSIST_LEARNING_RATE * 0.5f);

        // Clamp to minimum values
        if (pid_kp < 0.05f) pid_kp = 0.05f;
        if (pid_kd < 0.001f) pid_kd = 0.001f;

        // Reset oscillation counter
        state.oscillation_count = 0;

    } else if (!system_stable && state.avg_error > LEVEL_ASSIST_MAX_ERROR_THRESHOLD) {
        // System has large steady-state error - increase integral gain
        pid_ki *= (1.0f + LEVEL_ASSIST_LEARNING_RATE);

        // If error is very large, also increase proportional gain
        if (state.avg_error > LEVEL_ASSIST_MAX_ERROR_THRESHOLD * 2.0f) {
            pid_kp *= (1.0f + LEVEL_ASSIST_LEARNING_RATE * 0.5f);
        }

        // Clamp to maximum values
        if (pid_ki > 1.0f) pid_ki = 1.0f;
        if (pid_kp > 2.0f) pid_kp = 2.0f;

    } else if (system_stable && state.avg_error < 2.0f) {
        // System is performing well - fine-tune for better response
        if (state.error_variance > 1.0f) {
            // High variance suggests need for more damping
            pid_kd *= (1.0f + LEVEL_ASSIST_LEARNING_RATE * 0.5f);
            if (pid_kd > 0.2f) pid_kd = 0.2f;
        }
    }

    // Ensure parameters stay within reasonable bounds
    if (pid_kp < 0.05f) pid_kp = 0.05f;
    if (pid_kp > 2.0f) pid_kp = 2.0f;
    if (pid_ki < 0.01f) pid_ki = 0.01f;
    if (pid_ki > 1.0f) pid_ki = 1.0f;
    if (pid_kd < 0.001f) pid_kd = 0.001f;
    if (pid_kd > 0.2f) pid_kd = 0.2f;

    // Save to NVS if parameters changed significantly (> 5%)
    float kp_change = fabsf(pid_kp - orig_kp) / orig_kp;
    float ki_change = fabsf(pid_ki - orig_ki) / orig_ki;
    float kd_change = fabsf(pid_kd - orig_kd) / orig_kd;

    if (kp_change > 0.05f || ki_change > 0.05f || kd_change > 0.05f) {
        esp_err_t err = level_assistant_save_pid_to_nvs();
        if (err != ESP_OK) {
            // ESP_LOGW(TAG, "Failed to save learned PID parameters: %s", esp_err_to_name(err));
        }
    }
}

static bool is_system_oscillating(void) {
    // Oscillation detected if output changes sign too frequently
    return (state.oscillation_count > LEVEL_ASSIST_OSCILLATION_THRESHOLD);
}

static bool is_system_stable(void) {
    // System is stable if error is small and variance is low
    return (state.avg_error < LEVEL_ASSIST_MAX_ERROR_THRESHOLD &&
            state.error_variance < 5.0f);
}
#endif

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
    // Remove unused erpm_is_negative variable
    // bool erpm_is_negative = current_erpm < -LEVEL_ASSIST_ERPM_THRESHOLD;

    uint32_t modified_throttle = throttle_value;

    // Level assist only works in auto mode (no manual input)
    if (!state.is_manual_mode && throttle_is_neutral) {
        // Always calculate PID output for continuous fast control (no artificial delay)
        state.pid_output = calculate_pid_output(SETPOINT_RPM, (float)current_erpm, current_time);

        // Apply PID output to throttle with smoothing
        if (fabsf(state.pid_output) > 1.0f) {  // Reduced threshold for faster response
            // Apply lighter smoothing for faster response
            static float smoothed_output = 0.0f;
            smoothed_output = 0.3f * smoothed_output + 0.7f * state.pid_output;  // More responsive

            float throttle_correction = smoothed_output;

            // Only apply positive corrections (no reverse throttle for level assist)
            if (throttle_correction > 0.0f) {
                modified_throttle = LEVEL_ASSIST_NEUTRAL_CENTER + (uint32_t)throttle_correction;

                // Ensure we don't exceed maximum throttle
                if (modified_throttle > LEVEL_ASSIST_MAX_THROTTLE) {
                    modified_throttle = LEVEL_ASSIST_MAX_THROTTLE;
                }
            }
            // If correction is negative, stay at neutral (no braking)
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

    #if LEVEL_ASSIST_ADAPTIVE_ENABLED
    // Reset adaptive learning state
    state.history_index = 0;
    state.samples_collected = 0;
    state.oscillation_count = 0;
    state.last_output_sign = 0.0f;
    state.last_adaptation_ms = 0;
    for (int i = 0; i < LEVEL_ASSIST_PERFORMANCE_WINDOW; i++) {
        state.error_history[i] = 0.0f;
        state.output_history[i] = 0.0f;
    }
    #endif
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
    }
}

void level_assistant_set_pid_ki(float ki) {
    if (ki >= 0.0f && ki <= 2.0f) {  // Reasonable range
        pid_ki = ki;
        // Reset integral when changing gains to prevent windup
        state.pid_integral = 0.0f;
    }
}

void level_assistant_set_pid_kd(float kd) {
    if (kd >= 0.0f && kd <= 1.0f) {  // Reasonable range
        pid_kd = kd;
    }
}

void level_assistant_set_pid_output_max(float output_max) {
    if (output_max >= 10.0f && output_max <= 100.0f) {  // Reasonable range
        pid_output_max = output_max;
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

    // Set learned flag to indicate these are adaptive values
    uint8_t learned_flag = 1;
    err = nvs_set_u8(nvs_handle, NVS_KEY_LEARNED_FLAG, learned_flag);
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

    // Check if learned parameters exist
    uint8_t learned_flag = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY_LEARNED_FLAG, &learned_flag);
    if (err != ESP_OK || !learned_flag) {
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
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

    // Clear learned parameters from NVS
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
