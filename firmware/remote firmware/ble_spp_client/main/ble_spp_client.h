#ifndef SPP_CLIENT_DEMO_H
#define SPP_CLIENT_DEMO_H

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

#endif // SPP_CLIENT_DEMO_H