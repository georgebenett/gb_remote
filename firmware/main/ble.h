#ifndef SPP_CLIENT_DEMO_H
#define SPP_CLIENT_DEMO_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define DEVICE_NAME "GS-THUMB"
#define GATTC_TAG "GATTC_SPP_DEMO"

#define AUX_NVS_NAMESPACE "aux_cfg"
#define AUX_NVS_KEY_STATE "aux_state"
#define BLE_TRIM_NVS_NAMESPACE "ble_cfg"
#define BLE_TRIM_NVS_KEY_OFFSET "trim_offset"
#define BLE_PAIRED_NVS_NAMESPACE "ble_paired"
#define BLE_PAIRED_NVS_KEY_MAC "server_mac"
#define BLE_PAIRED_NVS_KEY_VALID "mac_valid"

#define RECONNECT_TIMEOUT_MS 5000 // 5 seconds to find stored server
#define RECONNECT_SCAN_INTERVAL_MS                                             \
  1000 // Scan interval when looking for stored server

// Task timing constants
#define ADC_SEND_INTERVAL_MS 75    // Throttle data send rate
#define RSSI_READ_INTERVAL_MS 1000 // RSSI polling rate

// BLE Security Configuration
#define BLE_PASSKEY 483265 // Fixed passkey for pairing (must match server)

#define PROFILE_NUM 1
#define PROFILE_APP_ID 0
#define BT_BD_ADDR_STR "%02x:%02x:%02x:%02x:%02x:%02x"
#define BT_BD_ADDR_HEX(addr)                                                   \
  addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]
#define ESP_GATT_SPP_SERVICE_UUID 0xABF0
#define SCAN_ALL_THE_TIME 0

// Thread-safe connection status check
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

// Paired device management
bool ble_has_paired_device(void);
esp_err_t ble_clear_paired_device(void);

#endif // SPP_CLIENT_DEMO_H