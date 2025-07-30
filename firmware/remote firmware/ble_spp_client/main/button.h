#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

// Button GPIO definitions
#define MAIN_BUTTON_GPIO GPIO_NUM_4

// Button timing definitions
#define BUTTON_LONG_PRESS_TIME_MS 500
#define BUTTON_DOUBLE_PRESS_TIME_MS 300

// Button states
typedef enum {
    BUTTON_IDLE,
    BUTTON_PRESSED,
    BUTTON_LONG_PRESS,
    BUTTON_DOUBLE_PRESS
} button_state_t;

// Button configuration
typedef struct {
    gpio_num_t gpio_num;
    uint32_t long_press_time_ms;    // Time in ms to trigger long press
    uint32_t double_press_time_ms;  // Maximum time between presses to count as double press
    bool active_low;                // true if button is active low (pressed = 0)
} button_config_t;

// Button event types
typedef enum {
    BUTTON_EVENT_PRESSED,
    BUTTON_EVENT_RELEASED,
    BUTTON_EVENT_LONG_PRESS,
    BUTTON_EVENT_DOUBLE_PRESS
} button_event_t;

// Callback function type
typedef void (*button_callback_t)(button_event_t event, void* user_data);

// Initialize button with configuration
esp_err_t button_init(const button_config_t* config);

// Register callback for button events
void button_register_callback(button_callback_t callback, void* user_data);

// Get current button state
button_state_t button_get_state(void);

// Get current press duration in milliseconds
uint32_t button_get_press_duration_ms(void);

// Start button monitoring task
void button_start_monitoring(void);

// Add after other function declarations
void switch_to_screen2_callback(button_event_t event, void* user_data);

#endif // BUTTON_H