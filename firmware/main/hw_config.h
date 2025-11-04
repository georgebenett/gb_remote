#ifndef HW_CONFIG_H
#define HW_CONFIG_H

#include "driver/gpio.h"

// ADC channel definitions
#define THROTTLE_PIN ADC_CHANNEL_8
#define BREAK_PIN ADC_CHANNEL_7

// Battery ADC channel
#define BATTERY_VOLTAGE_PIN ADC_CHANNEL_0

// Button GPIO definitions
#define MAIN_BUTTON_GPIO GPIO_NUM_10

// Power off GPIO definition
#define POWER_OFF_GPIO GPIO_NUM_4

// Battery status GPIO
#define BATTERY_IS_CHARGING_GPIO GPIO_NUM_2
#define BATTERY_PROBE_PIN GPIO_NUM_7

// TFT display GPIO definitions
#define TFT_MOSI_PIN GPIO_NUM_17
#define TFT_SCLK_PIN GPIO_NUM_16
#define TFT_CS_PIN   GPIO_NUM_15
#define TFT_DC_PIN   GPIO_NUM_14
#define TFT_RST_PIN  GPIO_NUM_18
#define TFT_BL_PIN   GPIO_NUM_13

// Viber GPIO definition
#define VIBER_PIN GPIO_NUM_6

#endif // HW_CONFIG_H