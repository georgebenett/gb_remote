#ifndef LEVEL_ASSISTANT_H
#define LEVEL_ASSISTANT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Level assistant configuration
#define LEVEL_ASSIST_NEUTRAL_THRESHOLD 5    // ADC units around neutral position (127) to consider "neutral"
#define LEVEL_ASSIST_ERPM_THRESHOLD 5       // ERPM threshold for level assist activation
#define LEVEL_ASSIST_MAX_THROTTLE 200       // Maximum throttle value level assistant can apply (127-255)
#define LEVEL_ASSIST_NEUTRAL_CENTER 127     // Center neutral position
#define LEVEL_ASSIST_ADC_CHANGE_THRESHOLD 3 // ADC change threshold to detect manual input
#define LEVEL_ASSIST_MANUAL_TIMEOUT_MS 500  // Time to consider throttle in manual mode after movement
#define ADAPT_INTERVAL_MS 200
#define SETPOINT_RPM 0
// Remove the slow update rate - run every time for smooth control
// #define LEVEL_ASSIST_UPDATE_RATE_MS 50      // How often to update assist (smooth continuous control)

// PID Controller parameters - Reduced gains for stability
#define LEVEL_ASSIST_PID_KP 0.3f            // Proportional gain (reduced from 0.8f)
#define LEVEL_ASSIST_PID_KI 0.1f            // Integral gain (reduced from 0.5f)
#define LEVEL_ASSIST_PID_KD 0.02f           // Derivative gain (reduced from 0.05f)
#define LEVEL_ASSIST_PID_SETPOINT 0.0f      // Target ERPM (0 = no rolling)
#define LEVEL_ASSIST_PID_OUTPUT_MAX 48.0f   // Max PID output (throttle units from neutral)

// Add deadband for ERPM to prevent jitter
#define LEVEL_ASSIST_ERPM_DEADBAND 3        // Don't react to ERPM changes smaller than this

// Add adaptive PID configuration
#define LEVEL_ASSIST_ADAPTIVE_ENABLED 1         // Enable adaptive PID
#define LEVEL_ASSIST_LEARNING_RATE 0.01f        // How fast to adapt (0.001-0.1)
#define LEVEL_ASSIST_PERFORMANCE_WINDOW 50      // Number of samples to evaluate performance
#define LEVEL_ASSIST_MAX_ERROR_THRESHOLD 10.0f  // Max acceptable error for good performance
#define LEVEL_ASSIST_OSCILLATION_THRESHOLD 3    // Number of sign changes to detect oscillation

typedef struct {
    bool enabled;                   // Is level assistant enabled
    bool was_at_zero_erpm;         // Was ERPM at zero in previous cycle
    bool throttle_was_neutral;     // Was throttle neutral in previous cycle
    bool is_manual_mode;           // Is throttle currently in manual mode (user input detected)
    int32_t previous_erpm;         // Previous ERPM reading
    uint32_t previous_throttle;    // Previous throttle value for change detection
    uint32_t last_assist_time_ms;  // Last time assist was applied (for debouncing)
    uint32_t last_manual_time_ms;  // Last time manual input was detected
    
    // PID Controller state
    float pid_integral;            // Integral accumulator
    float pid_previous_error;      // Previous error for derivative calculation
    float pid_output;              // Current PID output
    uint32_t pid_last_time_ms;     // Last time PID was calculated
    
    // Adaptive PID state
    float error_history[LEVEL_ASSIST_PERFORMANCE_WINDOW];  // Recent error history
    float output_history[LEVEL_ASSIST_PERFORMANCE_WINDOW]; // Recent output history
    int history_index;             // Current position in circular buffer
    int samples_collected;         // Number of samples collected so far
    float avg_error;               // Average absolute error over window
    float error_variance;          // Error variance (measures stability)
    int oscillation_count;         // Count of output sign changes (detects oscillation)
    float last_output_sign;        // Sign of last output for oscillation detection
    uint32_t last_adaptation_ms;   // Last time parameters were adapted
} level_assistant_state_t;

/**
 * Initialize the level assistant system
 */
esp_err_t level_assistant_init(void);

/**
 * Process level assistant logic and potentially modify throttle value
 * @param throttle_value Original throttle value (0-255)
 * @param current_erpm Current ERPM reading
 * @param is_enabled Whether level assistant is enabled in config
 * @return Modified throttle value with level assist applied if needed
 */
uint32_t level_assistant_process(uint32_t throttle_value, int32_t current_erpm, bool is_enabled);

/**
 * Reset the level assistant state (e.g., when throttle position changes significantly)
 */
void level_assistant_reset_state(void);

/**
 * Get current level assistant state for debugging
 */
level_assistant_state_t level_assistant_get_state(void);

/**
 * Set PID parameters for level assistant
 */
void level_assistant_set_pid_kp(float kp);
void level_assistant_set_pid_ki(float ki);
void level_assistant_set_pid_kd(float kd);
void level_assistant_set_pid_output_max(float output_max);

/**
 * Get PID parameters from level assistant
 */
float level_assistant_get_pid_kp(void);
float level_assistant_get_pid_ki(void);
float level_assistant_get_pid_kd(void);
float level_assistant_get_pid_output_max(void);

/**
 * Save/Load learned PID parameters to/from NVS
 */
esp_err_t level_assistant_save_pid_to_nvs(void);
esp_err_t level_assistant_load_pid_from_nvs(void);
esp_err_t level_assistant_reset_pid_to_defaults(void);

#endif // LEVEL_ASSISTANT_H
