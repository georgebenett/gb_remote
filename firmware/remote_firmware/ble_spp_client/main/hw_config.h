#ifndef HW_CONFIG_H
#define HW_CONFIG_H

#include "driver/gpio.h"

// ADC channel definitions
#define THROTTLE_PIN ADC_CHANNEL_2


// Battery ADC channel
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3
#define BATTERY_PIN  ADC_CHANNEL_3

// Button GPIO definitions
#define MAIN_BUTTON_GPIO GPIO_NUM_10

// TFT display GPIO definitions
#define TFT_MOSI_PIN GPIO_NUM_17
#define TFT_SCLK_PIN GPIO_NUM_16
#define TFT_CS_PIN   GPIO_NUM_15
#define TFT_DC_PIN   GPIO_NUM_14
#define TFT_RST_PIN  GPIO_NUM_18
#define TFT_BL_PIN   GPIO_NUM_13

#endif // HW_CONFIG_H