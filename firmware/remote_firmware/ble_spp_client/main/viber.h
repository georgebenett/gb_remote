#ifndef VIBER_H
#define VIBER_H

#include <stdint.h>
#include "esp_err.h"

// Vibration patterns
typedef enum {
    VIBER_PATTERN_VERY_SHORT,      // Very short vibration
    VIBER_PATTERN_SINGLE_SHORT,    // Single short vibration
    VIBER_PATTERN_SINGLE_LONG,     // Single long vibration
    VIBER_PATTERN_DOUBLE_SHORT,    // Two short vibrations
    VIBER_PATTERN_SUCCESS,         // Success pattern (short-long)
    VIBER_PATTERN_ERROR,           // Error pattern (three short)
    VIBER_PATTERN_ALERT            // Alert pattern (long-short-long)
} viber_pattern_t;

// Initialize vibration module
esp_err_t viber_init(void);

// Play a predefined vibration pattern
esp_err_t viber_play_pattern(viber_pattern_t pattern);

// Custom vibration with specific duration (in milliseconds)
esp_err_t viber_vibrate(uint32_t duration_ms);

// Custom pattern with multiple vibrations
// durations: array of vibration/pause durations in milliseconds
// count: number of durations in the array
esp_err_t viber_custom_pattern(const uint32_t* durations, uint8_t count);

// Stop any ongoing vibration
esp_err_t viber_stop(void);

#endif // VIBER_H 