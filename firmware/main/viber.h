#ifndef VIBER_H
#define VIBER_H

#include <stdint.h>
#include "esp_err.h"

// Vibration patterns
typedef enum {
    VIBER_PATTERN_VERY_SHORT,
    VIBER_PATTERN_SINGLE_SHORT,
    VIBER_PATTERN_SINGLE_LONG,
    VIBER_PATTERN_DOUBLE_SHORT,
    VIBER_PATTERN_SUCCESS,
    VIBER_PATTERN_ERROR,
    VIBER_PATTERN_ALERT
} viber_pattern_t;

esp_err_t viber_init(void);

esp_err_t viber_play_pattern(viber_pattern_t pattern);

esp_err_t viber_vibrate(uint32_t duration_ms);

esp_err_t viber_custom_pattern(const uint32_t* durations, uint8_t count);

esp_err_t viber_stop(void);

#endif // VIBER_H