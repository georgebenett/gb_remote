#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

// Pin definitions
#define UART1_VESC_TX_PIN         GPIO_NUM_5
#define UART1_VESC_RX_PIN         GPIO_NUM_18
#define LED_PIN                   GPIO_NUM_12

#define BMS_UART_TX_PIN           GPIO_NUM_17
#define BMS_UART_RX_PIN           GPIO_NUM_16

// UART configurations
#define UART1_VESC_BAUD_RATE      115200
#define UART1_VESC_BUF_SIZE       256

#define BMS_UART_PORT             UART_NUM_2


