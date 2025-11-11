#ifndef LEVEL_ASSISTANT_H
#define LEVEL_ASSISTANT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define LEVEL_ASSIST_NEUTRAL_THRESHOLD 10
#define LEVEL_ASSIST_ERPM_THRESHOLD 5
#define LEVEL_ASSIST_MAX_THROTTLE 160
#define LEVEL_ASSIST_NEUTRAL_CENTER 127
#define LEVEL_ASSIST_ADC_CHANGE_THRESHOLD 10
#define LEVEL_ASSIST_MANUAL_TIMEOUT_MS 500
#define SETPOINT_RPM 0
#define LEVEL_ASSIST_PID_KP 0.05f
#define LEVEL_ASSIST_PID_KI 0.005f
#define LEVEL_ASSIST_PID_KD 0.001f
#define LEVEL_ASSIST_PID_SETPOINT 0.0f
#define LEVEL_ASSIST_PID_OUTPUT_MAX 48.0f
#define LEVEL_ASSIST_ERPM_DEADBAND 3

typedef struct {
    bool enabled;
    bool was_at_zero_erpm;
    bool throttle_was_neutral;
    bool is_manual_mode;
    int32_t previous_erpm;
    uint32_t previous_throttle;
    uint32_t last_assist_time_ms;
    uint32_t last_manual_time_ms;

    float pid_integral;
    float pid_previous_error;
    float pid_output;
    uint32_t pid_last_time_ms;

} level_assistant_state_t;

esp_err_t level_assistant_init(void);

uint32_t level_assistant_process(uint32_t throttle_value, int32_t current_erpm, bool is_enabled);

void level_assistant_reset_state(void);

level_assistant_state_t level_assistant_get_state(void);

void level_assistant_set_pid_kp(float kp);
void level_assistant_set_pid_ki(float ki);
void level_assistant_set_pid_kd(float kd);
void level_assistant_set_pid_output_max(float output_max);

float level_assistant_get_pid_kp(void);
float level_assistant_get_pid_ki(void);
float level_assistant_get_pid_kd(void);
float level_assistant_get_pid_output_max(void);

esp_err_t level_assistant_save_pid_to_nvs(void);
esp_err_t level_assistant_load_pid_from_nvs(void);
esp_err_t level_assistant_reset_pid_to_defaults(void);

#endif // LEVEL_ASSISTANT_H
