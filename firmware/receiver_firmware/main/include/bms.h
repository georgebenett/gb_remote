// jiabaida_driver.h

#pragma once

#include "esp_err.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_config.h"
#include "datatypes.h"  // Add this for mc_values struct
#include <stdint.h>
#include <stddef.h>

#define BMS_TAG "BMS"
#define BMS_BAUD_RATE 9600
#define BMS_BUF_SIZE 512

// Add BMS values structure definition
typedef struct {
    float total_voltage;
    float current;
    float remaining_capacity;
    float nominal_capacity;
    uint8_t num_cells;
    float cell_voltages[16];  // Assuming maximum 16 cells
} bms_values_t;

// Function declarations
esp_err_t bms_uart_init(void);
esp_err_t bms_read_basic_info(uint8_t *response, size_t *response_len);
esp_err_t bms_read_cell_voltages(uint8_t *response, size_t *response_len);
esp_err_t bms_read_bms_version(uint8_t *response, size_t *response_len);

// Add declaration for get_stored_vesc_values
mc_values* get_stored_vesc_values(void);
void send_telemetry_data(const mc_values* vesc_data, const bms_values_t* bms_data);


// MOS control types
typedef enum {
    MOS_RELEASE = 0x00,
    MOS_CHARGE_OFF = 0x01,
    MOS_DISCHARGE_OFF = 0x02,
    MOS_BOTH_OFF = 0x03
} mos_control_t;

esp_err_t bms_control_mos(mos_control_t mode);
