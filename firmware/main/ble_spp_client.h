#ifndef SPP_CLIENT_DEMO_H
#define SPP_CLIENT_DEMO_H

#include <stdint.h>
#include "esp_err.h"

extern bool is_connect;

void spp_client_demo_init(void);
float get_latest_voltage(void);
int32_t get_latest_erpm(void);
float get_latest_current_motor(void);
float get_latest_current_in(void);
float get_bms_total_voltage(void);
float get_bms_current(void);
float get_bms_remaining_capacity(void);
float get_bms_nominal_capacity(void);
uint8_t get_bms_num_cells(void);
float get_bms_cell_voltage(uint8_t cell_index);
float get_latest_temp_mos(void);
float get_latest_temp_motor(void);
int get_bms_battery_percentage(void);

// BLE trim offset functions for fine-tuning throttle neutral position
// Range: -20 to +20 (applied to 0-255 BLE value, scaled for 12-bit ADC)
int8_t ble_get_trim_offset(void);
esp_err_t ble_increase_trim_offset(void);
esp_err_t ble_decrease_trim_offset(void);

#endif // SPP_CLIENT_DEMO_H