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
// Task timing constants
/* BLE connection interval: the fastest a throttle packet can leave. Sends are
 * driven by the ADC task; ADC_SAMPLE_PERIOD_MS (throttle.h) matches this. */
#define ADC_SEND_INTERVAL_MS 20 // 50 Hz
/* Over ADC_SAMPLE_PERIOD_MS so jitter is not a stall, under the receiver's
 * 200 ms THROTTLE_TIMEOUT_MS so a real stall reaches its failsafe. */
#define ADC_SEND_TIMEOUT_MS 100
#define RSSI_READ_INTERVAL_MS 1000 // RSSI polling rate
#define NEUTRAL_HOLD_MS 1000       // Hold neutral after connection

// BLE Security Configuration
#define BLE_PASSKEY 483265 // Fixed passkey for pairing (must match server)

// Maximum simultaneous receiver links. Only slot 0 is used unless the
// dual connection preference is enabled.
#define BLE_MAX_RECEIVERS 2

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
float get_bms_total_voltage(void);
int get_bms_battery_percentage(void);

// Auxiliary output control
void ble_toggle_aux_output(void);
bool ble_get_aux_output_state(void);
bool ble_get_receiver_aux_output_state(void);

// Trip odometer (stored on receiver)
float ble_get_latest_trip_km(void);

// Send reset-odometer command to receiver over BLE
esp_err_t ble_send_reset_odometer(void);

// Send shutdown command to receiver over BLE (skips its failsafe on disconnect)
esp_err_t ble_send_shutdown(void);

// BLE trim offset control
int8_t ble_get_trim_offset(void);
esp_err_t ble_increase_trim_offset(void);
esp_err_t ble_decrease_trim_offset(void);

/** Apply the dual connection preference at runtime (also loaded from NVS at
 *  BLE init). Enabling starts scanning for a second receiver; disabling
 *  drops any link beyond the first. */
void ble_set_dual_connection(bool enabled);

/** Apply the smart reverse preference at runtime (also loaded from NVS at BLE
 *  init) and send it to every connected receiver, which runs the logic. */
/** Wakes the sender on a new sample, so it transmits once per sample. */
void ble_notify_throttle_sample(void);

/** Speed limit cap in km/h, 0 = off. The receiver enforces it on its own ERPM.
 */
void ble_set_speed_limit(uint8_t cap_kmh);

/** Percent of the receiver's max motor current the ride profile allows. */
void ble_set_current_scale(uint8_t pct);

void ble_set_smart_reverse(bool enabled);

/** Reverse lockout, sent to every receiver: they brake instead of reversing. */
void ble_set_no_reverse(bool enabled);

/** Throttle inversion (lite). Must be applied live: the USB stream reloads it
 *  per packet, so a cached copy here would disagree with the config tool. */
void ble_set_invert_throttle(bool enabled);

/** Apply the assistive push preference at runtime (also loaded from NVS at BLE
 *  init) and send it to every connected receiver, which runs the logic. */
void ble_set_assist_push(bool enabled);

/** Apply the assistive push tuning at runtime and send it to every connected
 *  receiver. Strength is a percent of the VESC current limit; decay is in
 *  motor rpm/s. The receiver clamps both again. */
void ble_set_assist_params(uint8_t strength_pct, uint8_t decay_rpm_s);

/** True while the dual connection preference is active. */
bool ble_dual_connection_is_enabled(void);

/* Per-receiver telemetry. `idx` is a receiver slot in
 * [0, BLE_MAX_RECEIVERS); out-of-range or unconnected slots read as
 * "no data" (0.0f / -1 / false). The aggregate getters above keep reporting
 * whichever receiver notified last and are what the single-receiver home
 * screen uses. */
bool ble_receiver_is_connected(int idx);
/** Latest RSSI for `idx`. Returns false when the slot has no valid reading. */
bool ble_receiver_get_rssi(int idx, int8_t *out_rssi);
float ble_receiver_get_bms_total_voltage(int idx);
int ble_receiver_get_bms_battery_percentage(int idx);
float ble_receiver_get_vesc_voltage(int idx);
float ble_receiver_get_trip_km(int idx);

/** Suspend BLE: disconnect and stop scanning.
 *  Called when leaving home screen (charging, snake, splash, etc.). */
void ble_suspend(void);
/** Resume BLE: restart scanning.
 *  Called when arriving at home screen. */
void ble_resume(void);

#endif // SPP_CLIENT_DEMO_H