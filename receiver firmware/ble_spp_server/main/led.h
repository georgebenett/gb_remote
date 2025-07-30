#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// PWM configuration
#define LED_PWM_FREQ         5000        // 5 kHz
#define LED_PWM_RESOLUTION   8           // 8-bit resolution (0-255)
#define LED_PWM_TIMER       LEDC_TIMER_0
#define LED_PWM_CHANNEL     LEDC_CHANNEL_0

// LED brightness levels (0-255)
#define LED_PWM_DISCONNECTED 2    // Dim when disconnected
#define LED_PWM_CONNECTED    80   // Brighter when connected

// Transition configuration
#define LED_TRANSITION_STEP_MS   10   // Time between brightness steps (faster updates)
#define LED_TRANSITION_STEPS     20   // More steps for smoother transition

// Function declarations
esp_err_t led_init(void);
void led_set_duty(uint8_t duty);
void led_set_connection_state(bool connected); 