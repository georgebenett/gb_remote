#ifndef SPP_CLIENT_DEMO_H
#define SPP_CLIENT_DEMO_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

extern bool is_connect;
bool ble_is_connected(void);

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

// Auxiliary output control
void ble_toggle_aux_output(void);
bool ble_get_aux_output_state(void);

// BLE trim offset control
int8_t ble_get_trim_offset(void);
esp_err_t ble_increase_trim_offset(void);
esp_err_t ble_decrease_trim_offset(void);

#endif // SPP_CLIENT_DEMO_H