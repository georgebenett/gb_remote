#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "datatypes.h"
#include "esp_err.h"

#define ADC_TIMEOUT_MS 200  // 200ms timeout
#define THROTTLE_NEUTRAL_VALUE 127
#define VESC_UPDATE_INTERVAL_MS 50

extern uint16_t current_adc_value;
extern TaskHandle_t adc_print_task_handle;

// Function declarations
esp_err_t adc_init(void);
void adc_update_value(uint16_t value);
void adc_reset_value(void);
void adc_reset_timeout(void);
void adc_start_timeout_monitor(void);
void adc_stop_timeout_monitor(void);
//void adc_print_task(void *pvParameters);

// External variable declarations
//extern TaskHandle_t adc_print_task_handle;
//extern mc_values* get_stored_vesc_values(void); 