/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "ble.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "target_config.h"
#include "throttle.h"
#include "ui_updater.h"
#include "vesc_config.h"

enum {
  SPP_IDX_SVC,
  SPP_IDX_SPP_DATA_RECV_VAL,
  SPP_IDX_SPP_DATA_NTY_VAL,
  SPP_IDX_SPP_DATA_NTF_CFG,
  SPP_IDX_SPP_COMMAND_VAL,
  SPP_IDX_SPP_STATUS_VAL,
  SPP_IDX_SPP_STATUS_CFG,
#ifdef SUPPORT_HEARTBEAT
  SPP_IDX_SPP_HEARTBEAT_VAL,
  SPP_IDX_SPP_HEARTBEAT_CFG,
#endif
  SPP_IDX_NB,
};

static void esp_gap_cb(esp_gap_ble_cb_event_t event,
                       esp_ble_gap_cb_param_t *param);
static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                         esp_ble_gattc_cb_param_t *param);
static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param);
static void adc_send_task(void *pvParameters);
static void log_rssi_task(void *pvParameters);
static int link_count_connected(void);
static int desired_link_count(void);
static void resume_scan_if_needed(void);

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE};

// Pairing advert: receiver scans for this so the user can pick a remote by MAC.
static const uint8_t pairing_adv_data[11] = {
    0x02, 0x01, 0x06, 0x07, 0x09, 'G', 'S', '-', 'R', 'E', 'M',
};

static esp_ble_adv_params_t pairing_adv_params = {
    .adv_int_min = 0x40,
    .adv_int_max = 0x80,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static bool pairing_adv_active = false;

static void pairing_adv_apply(void);

// One link per receiver. Only the first slot is used unless the
// dual_connection user preference is enabled.
#define MAX_RECEIVER_LINKS BLE_MAX_RECEIVERS

typedef struct {
  bool in_use; // Slot claimed (connection opening or open)
  bool ready;  // Service discovery done, db[] valid, throttle may be sent
  uint16_t conn_id;
  esp_bd_addr_t bda;
  uint16_t srv_start_handle;
  uint16_t srv_end_handle;
  uint16_t mtu;
  esp_gattc_db_elem_t db[SPP_IDX_NB];
  uint32_t connect_ms; // Connection timestamp for the neutral hold period
  int8_t rssi;
  bool rssi_valid;
  // Telemetry as reported by this receiver. Cleared with the slot on
  // disconnect. The dual home screen shows one column per slot.
  float vesc_voltage;
  float bms_total_voltage;
  float bms_remaining_capacity;
  float bms_nominal_capacity;
  float trip_km;
} receiver_link_t;

static receiver_link_t links[MAX_RECEIVER_LINKS];

static receiver_link_t *link_by_conn_id(uint16_t conn_id);
static receiver_link_t *link_by_bda(const esp_bd_addr_t bda);

static bool is_connect = false; // True while at least one link is connected
static SemaphoreHandle_t is_connect_mutex = NULL;
static const char device_name[] = DEVICE_NAME;
/* Interface handed to us by ESP_GATTC_REG_EVT; ESP_GATT_IF_NONE until then. */
static esp_gatt_if_t spp_gattc_if = ESP_GATT_IF_NONE;
static bool dual_connection_enabled = false;
static bool scan_active = false;
// Stop-scan-then-open handshake: set when a matching receiver is found,
// consumed in ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT.
static bool connect_pending = false;
static esp_bd_addr_t pending_connect_bda;
static esp_ble_addr_type_t pending_connect_addr_type;
static QueueHandle_t cmd_reg_queue = NULL;

// Notify-registration work item processed by spp_client_reg_task
typedef struct {
  uint8_t link_idx;
  uint16_t attr_idx; // SPP_IDX_SPP_DATA_NTY_VAL or SPP_IDX_SPP_STATUS_VAL
} reg_work_t;

static esp_bt_uuid_t spp_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid =
        {
            .uuid16 = ESP_GATT_SPP_SERVICE_UUID,
        },
};

static float latest_voltage = 0.0f;
static int32_t latest_erpm = 0;

static float bms_total_voltage = 0.0f;
static float bms_remaining_capacity = 0.0f;
static float bms_nominal_capacity = 0.0f;

#define BLE_CMD_RESET_ODOMETER 0x01

static bool aux_output_state = false;
static bool receiver_aux_output_state = false;
static int8_t ble_trim_offset = 0; // Trim offset for BLE output (-127 to +127)
static float latest_trip_km = 0.0f;

/** When true, BLE is suspended (not on home screen). Starts true;
 *  resumed only when the home screen is reached. */
static volatile bool ble_suspended = true;

float ble_get_latest_trip_km(void) { return latest_trip_km; }

esp_err_t ble_send_reset_odometer(void) {
  if (!ble_is_connected()) {
    return ESP_ERR_INVALID_STATE;
  }
  uint8_t cmd[1] = {BLE_CMD_RESET_ODOMETER};
  bool sent = false;
  for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
    receiver_link_t *link = &links[i];
    if (!link->ready)
      continue;
    if (!(link->db[SPP_IDX_SPP_COMMAND_VAL].properties &
          (ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE)))
      continue;
    if (esp_ble_gattc_write_char(
            spp_gattc_if, link->conn_id,
            link->db[SPP_IDX_SPP_COMMAND_VAL].attribute_handle, sizeof(cmd),
            cmd, ESP_GATT_WRITE_TYPE_NO_RSP,
            ESP_GATT_AUTH_REQ_NONE) == ESP_OK) {
      sent = true;
    }
  }
  if (!sent) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  ESP_LOGI(GATTC_TAG, "Reset odometer command sent to receiver(s)");
  return ESP_OK;
}

static void aux_output_save_state(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(AUX_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    nvs_set_u8(nvs_handle, AUX_NVS_KEY_STATE, aux_output_state ? 1 : 0);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
  }
}

static void aux_output_load_state(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(AUX_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err == ESP_OK) {
    uint8_t state = 0;
    if (nvs_get_u8(nvs_handle, AUX_NVS_KEY_STATE, &state) == ESP_OK) {
      aux_output_state = (state != 0);
      ESP_LOGI(GATTC_TAG, "Aux output state loaded from NVS: %s",
               aux_output_state ? "ON" : "OFF");
    }
    nvs_close(nvs_handle);
  }
}

static void ble_trim_load_offset(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(BLE_TRIM_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err == ESP_OK) {
    int8_t offset = 0;
    if (nvs_get_i8(nvs_handle, BLE_TRIM_NVS_KEY_OFFSET, &offset) == ESP_OK) {
      if (offset < -127)
        offset = -127;
      ble_trim_offset = offset;
      ESP_LOGI(GATTC_TAG, "BLE trim offset loaded from NVS: %d",
               ble_trim_offset);
    }
    nvs_close(nvs_handle);
  }
}

static esp_err_t ble_trim_save_offset(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(BLE_TRIM_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_i8(nvs_handle, BLE_TRIM_NVS_KEY_OFFSET, ble_trim_offset);
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);
  return err;
}

int8_t ble_get_trim_offset(void) { return ble_trim_offset; }

esp_err_t ble_increase_trim_offset(void) {
  if (ble_trim_offset < 127) {
    ble_trim_offset++;
    esp_err_t err = ble_trim_save_offset();
    if (err == ESP_OK) {
      ESP_LOGI(GATTC_TAG, "BLE trim offset increased to: %d", ble_trim_offset);
    }
    return err;
  }
  return ESP_ERR_INVALID_ARG; // Already at maximum
}

esp_err_t ble_decrease_trim_offset(void) {
  if (ble_trim_offset > -127) {
    ble_trim_offset--;
    esp_err_t err = ble_trim_save_offset();
    if (err == ESP_OK) {
      ESP_LOGI(GATTC_TAG, "BLE trim offset decreased to: %d", ble_trim_offset);
    }
    return err;
  }
  return ESP_ERR_INVALID_ARG; // Already at minimum
}

void ble_toggle_aux_output(void) {
  aux_output_state = !aux_output_state;
  aux_output_save_state();
  ESP_LOGI(GATTC_TAG, "Aux output toggled: %s",
           aux_output_state ? "ON" : "OFF");
}

bool ble_get_aux_output_state(void) { return aux_output_state; }

bool ble_get_receiver_aux_output_state(void) {
  return receiver_aux_output_state;
}

// Telemetry is stored twice: per link (used by the dual home screen, which
// shows one battery arc and odometer per receiver) and in the aggregate
// globals below (last-write-wins), which feed the single-receiver home
// screen and the USB telemetry stream.
static void notify_event_handler(esp_ble_gattc_cb_param_t *p_data) {
  uint8_t handle = 0;

  if (p_data->notify.is_notify == true) {
    ESP_LOGI(GATTC_TAG, "+NOTIFY:handle = %d,length = %d ",
             p_data->notify.handle, p_data->notify.value_len);
  } else {
    ESP_LOGI(GATTC_TAG, "+INDICATE:handle = %d,length = %d ",
             p_data->notify.handle, p_data->notify.value_len);
  }

  handle = p_data->notify.handle;
  receiver_link_t *link = link_by_conn_id(p_data->notify.conn_id);
  if (link == NULL || !link->ready) {
    ESP_LOGE(GATTC_TAG, " %s no ready link for conn_id %d", __func__,
             p_data->notify.conn_id);
    return;
  }
  const esp_gattc_db_elem_t *db = link->db;

  int link_idx = (int)(link - links);

  if (handle == db[SPP_IDX_SPP_STATUS_VAL].attribute_handle) {
    if (p_data->notify.value_len >= 2 &&
        p_data->notify.value[0] == BLE_CMD_RESET_ODOMETER) {
      if (p_data->notify.value[1] == 0x00) {
        latest_trip_km = 0.0f;
        link->trip_km = 0.0f;
        ui_update_trip_distance(link_idx, 0.0f);
        ESP_LOGI(GATTC_TAG, "Odometer reset ACK received from receiver");
        printf("#>DATA odometer_reset=ok\n");
      }
    }
    return;
  }

  if (handle == db[SPP_IDX_SPP_DATA_NTY_VAL].attribute_handle) {
    if (p_data->notify.value_len ==
        65) { // VESC + BMS + motor config + aux state + trip_km
      // All values are little-endian (LSB first, MSB second). Temperatures and
      // currents are decoded for the log only — nothing displays them.
      int16_t temp_mos =
          p_data->notify.value[0] | ((int16_t)p_data->notify.value[1] << 8);
      int16_t temp_motor =
          p_data->notify.value[2] | ((int16_t)p_data->notify.value[3] << 8);
      int16_t current_motor =
          p_data->notify.value[4] | ((int16_t)p_data->notify.value[5] << 8);
      int16_t current_in =
          p_data->notify.value[6] | ((int16_t)p_data->notify.value[7] << 8);

      // rpm (bytes 8-11) - little-endian
      int32_t rpm_raw = ((int32_t)p_data->notify.value[8]) |
                        ((int32_t)p_data->notify.value[9] << 8) |
                        ((int32_t)p_data->notify.value[10] << 16) |
                        ((int32_t)p_data->notify.value[11] << 24);

      latest_erpm = rpm_raw;

      // voltage (bytes 12-13)
      int16_t voltage =
          p_data->notify.value[12] | ((int16_t)p_data->notify.value[13] << 8);
      float decoded_voltage = voltage / 100.0f;
      if (decoded_voltage > 1.0f) {
        latest_voltage = decoded_voltage;
        link->vesc_voltage = decoded_voltage;
      }

      // total_voltage (bytes 14-15)
      int16_t total_voltage =
          p_data->notify.value[14] | ((int16_t)p_data->notify.value[15] << 8);
      bms_total_voltage = total_voltage / 100.0f;
      link->bms_total_voltage = bms_total_voltage;

      // current (bytes 16-17) - log only
      int16_t bms_current_raw =
          p_data->notify.value[16] | ((int16_t)p_data->notify.value[17] << 8);

      // remaining_capacity (bytes 18-19)
      int16_t remaining_cap =
          p_data->notify.value[18] | ((int16_t)p_data->notify.value[19] << 8);
      bms_remaining_capacity = remaining_cap / 100.0f;
      link->bms_remaining_capacity = bms_remaining_capacity;

      // nominal_capacity (bytes 20-21)
      int16_t nominal_cap =
          p_data->notify.value[20] | ((int16_t)p_data->notify.value[21] << 8);
      bms_nominal_capacity = nominal_cap / 100.0f;
      link->bms_nominal_capacity = bms_nominal_capacity;

      // num_cells (byte 22) - log only. Bytes 23-54 hold the 16 per-cell
      // voltages; nothing on the remote shows them, so they are not decoded.
      uint8_t bms_num_cells = p_data->notify.value[22];

      // motor_poles (byte 55)
      uint8_t motor_poles = p_data->notify.value[55];

      // gear_ratio (bytes 56-57, uint16_t, scale ÷1000) - little-endian
      uint16_t gear_ratio_x1000 =
          p_data->notify.value[56] | ((uint16_t)p_data->notify.value[57] << 8);

      // wheel_diameter (bytes 58-59, uint16_t in mm, scale ÷1000 = meters) -
      // little-endian
      uint16_t wheel_diameter_mm =
          p_data->notify.value[58] | ((uint16_t)p_data->notify.value[59] << 8);

      // Update motor config from VESC (not saved to NVS, only kept in memory)
      vesc_config_update_motor(motor_poles, gear_ratio_x1000,
                               wheel_diameter_mm);

      // aux output state (byte 60)
      receiver_aux_output_state = (p_data->notify.value[60] != 0);
      ui_update_aux_output_indicator();

      // trip_km (bytes 61-64, int32 * 100)
      int32_t trip_km_x100 = ((int32_t)p_data->notify.value[61]) |
                             ((int32_t)p_data->notify.value[62] << 8) |
                             ((int32_t)p_data->notify.value[63] << 16) |
                             ((int32_t)p_data->notify.value[64] << 24);
      latest_trip_km = trip_km_x100 / 100.0f;
      link->trip_km = latest_trip_km;
      ui_update_trip_distance(link_idx, link->trip_km);

      ESP_LOGI(GATTC_TAG, "Combined Data Received:");
      ESP_LOGI(GATTC_TAG,
               "VESC: V=%.2fV, RPM=%ld, Motor=%.2fA, In=%.2fA, TempMos=%.2f°C, "
               "TempMotor=%.2f°C",
               latest_voltage, latest_erpm, current_motor / 100.0f,
               current_in / 100.0f, temp_mos / 100.0f, temp_motor / 100.0f);
      ESP_LOGI(GATTC_TAG,
               "BMS: Total V=%.2fV, Current=%.2fA, Remaining=%.2fAh, Cells=%d",
               bms_total_voltage, bms_current_raw / 100.0f,
               bms_remaining_capacity, bms_num_cells);
    } else {
      ESP_LOGW(GATTC_TAG, "Unexpected data length: %d (expected 65)",
               p_data->notify.value_len);
    }
  }
}

// Advertise while we still have receiver capacity (so a second receiver can
// pair while the first is connected), stop when full or suspended.
static void pairing_adv_apply(void) {
  bool want = !ble_suspended && link_count_connected() < desired_link_count();
  if (want && !pairing_adv_active) {
    esp_ble_gap_start_advertising(&pairing_adv_params);
    pairing_adv_active = true;
  } else if (!want && pairing_adv_active) {
    esp_ble_gap_stop_advertising();
    pairing_adv_active = false;
  }
}

static void set_is_connect(bool value) {
  if (is_connect_mutex != NULL &&
      xSemaphoreTake(is_connect_mutex, portMAX_DELAY) == pdTRUE) {
    is_connect = value;
    xSemaphoreGive(is_connect_mutex);
  } else {
    is_connect = value;
  }
}

static int link_count_connected(void) {
  int n = 0;
  for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
    if (links[i].in_use)
      n++;
  }
  return n;
}

static bool link_any_ready(void) {
  for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
    if (links[i].ready)
      return true;
  }
  return false;
}

static receiver_link_t *link_by_conn_id(uint16_t conn_id) {
  for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
    if (links[i].in_use && links[i].conn_id == conn_id)
      return &links[i];
  }
  return NULL;
}

static receiver_link_t *link_by_bda(const esp_bd_addr_t bda) {
  for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
    if (links[i].in_use &&
        memcmp(links[i].bda, bda, sizeof(esp_bd_addr_t)) == 0)
      return &links[i];
  }
  return NULL;
}

static receiver_link_t *link_alloc(const esp_bd_addr_t bda) {
  for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
    if (!links[i].in_use) {
      memset(&links[i], 0, sizeof(links[i]));
      links[i].in_use = true;
      memcpy(links[i].bda, bda, sizeof(esp_bd_addr_t));
      return &links[i];
    }
  }
  return NULL;
}

static void link_free(receiver_link_t *link) { memset(link, 0, sizeof(*link)); }

static int desired_link_count(void) { return dual_connection_enabled ? 2 : 1; }

/** Start scanning when below the desired receiver count and no
 *  stop-scan-then-open handshake is in flight. */
static void resume_scan_if_needed(void) {
  if (ble_suspended || scan_active || connect_pending)
    return;
  if (link_count_connected() >= desired_link_count())
    return;
  esp_ble_gap_start_scanning(SCAN_ALL_THE_TIME);
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event,
                       esp_ble_gap_cb_param_t *param) {
  uint8_t *adv_name = NULL;
  uint8_t adv_name_len = 0;
  esp_err_t err;

  switch (event) {
  case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
    if ((err = param->scan_param_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(GATTC_TAG, "Scan param set failed: %s", esp_err_to_name(err));
      break;
    }
    if (ble_suspended) {
      break;
    }
    // the unit of the duration is second
    uint32_t duration = 0xFFFF;
    ESP_LOGI(GATTC_TAG, "Enable Ble Scan:during time %04" PRIx32 " minutes.",
             duration);
    esp_ble_gap_start_scanning(duration);
    break;
  }
  case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
    if ((err = param->scan_start_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
      scan_active = false;
      ESP_LOGE(GATTC_TAG, "Scan start failed: %s", esp_err_to_name(err));
      break;
    }
    scan_active = true;
    ESP_LOGI(GATTC_TAG, "Scan start successfully");
    break;
  case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
    scan_active = false;
    if ((err = param->scan_stop_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(GATTC_TAG, "Scan stop failed: %s", esp_err_to_name(err));
      break;
    }
    ESP_LOGI(GATTC_TAG, "Scan stop successfully");
    if (ble_suspended) {
      connect_pending = false;
      break;
    }
    if (connect_pending) {
      connect_pending = false;
      ESP_LOGI(GATTC_TAG, "Connect to the remote device.");
      esp_ble_gattc_open(spp_gattc_if, pending_connect_bda,
                         pending_connect_addr_type, true);
    }
    break;
  case ESP_GAP_BLE_SCAN_RESULT_EVT: {
    esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
    switch (scan_result->scan_rst.search_evt) {
    case ESP_GAP_SEARCH_INQ_RES_EVT:
      adv_name =
          esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
                                   ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);

      if (adv_name != NULL &&
          strncmp((char *)adv_name, device_name, adv_name_len) == 0) {
        // Ignore while a connect is in flight, when the receiver is already
        // connected, or when we have all the links we want
        if (connect_pending || link_by_bda(scan_result->scan_rst.bda) != NULL ||
            link_count_connected() >= desired_link_count()) {
          break;
        }
        ESP_LOGI(GATTC_TAG, "Found device %s (" BT_BD_ADDR_STR "), RSSI: %d",
                 device_name, BT_BD_ADDR_HEX(scan_result->scan_rst.bda),
                 scan_result->scan_rst.rssi);
        memcpy(pending_connect_bda, scan_result->scan_rst.bda,
               sizeof(esp_bd_addr_t));
        pending_connect_addr_type = scan_result->scan_rst.ble_addr_type;
        connect_pending = true;
        esp_ble_gap_stop_scanning();
      }
      break;
    case ESP_GAP_SEARCH_INQ_CMPL_EVT:
      break;
    default:
      break;
    }
    break;
  }
  case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
    if ((err = param->adv_stop_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(GATTC_TAG, "Adv stop failed: %s", esp_err_to_name(err));
    } else {
      ESP_LOGI(GATTC_TAG, "Stop adv successfully");
    }
    pairing_adv_active = false;
    break;
  case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
    if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
      receiver_link_t *link = link_by_bda(param->read_rssi_cmpl.remote_addr);
      if (link != NULL) {
        link->rssi = param->read_rssi_cmpl.rssi;
        link->rssi_valid = true;
      }
      // Icons are per receiver and re-read each link's RSSI themselves.
      ui_update_connection_icon();
    } else {
      ESP_LOGE(GATTC_TAG, "RSSI read failed: %d", param->read_rssi_cmpl.status);
    }
    break;

  // Security events
  case ESP_GAP_BLE_SEC_REQ_EVT:
    ESP_LOGI(GATTC_TAG, "ESP_GAP_BLE_SEC_REQ_EVT");
    esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
    break;

  case ESP_GAP_BLE_PASSKEY_REQ_EVT:
    ESP_LOGI(GATTC_TAG, "ESP_GAP_BLE_PASSKEY_REQ_EVT - entering passkey: %06lu",
             (unsigned long)BLE_PASSKEY);
    esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true,
                          BLE_PASSKEY);
    break;

  case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
    ESP_LOGI(GATTC_TAG, "ESP_GAP_BLE_PASSKEY_NOTIF_EVT - passkey: %06lu",
             (unsigned long)param->ble_security.key_notif.passkey);
    break;

  case ESP_GAP_BLE_NC_REQ_EVT:
    // Numeric comparison - accept automatically
    ESP_LOGI(GATTC_TAG, "ESP_GAP_BLE_NC_REQ_EVT - numeric comparison: %06lu",
             (unsigned long)param->ble_security.key_notif.passkey);
    esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
    break;

  case ESP_GAP_BLE_AUTH_CMPL_EVT:
    if (param->ble_security.auth_cmpl.success) {
      ESP_LOGI(GATTC_TAG,
               "Authentication SUCCESS, addr_type: %d, auth_mode: %d",
               param->ble_security.auth_cmpl.addr_type,
               param->ble_security.auth_cmpl.auth_mode);
    } else {
      ESP_LOGW(GATTC_TAG, "Authentication FAILED, reason: 0x%x",
               param->ble_security.auth_cmpl.fail_reason);
    }
    break;

  case ESP_GAP_BLE_KEY_EVT:
    ESP_LOGI(GATTC_TAG, "ESP_GAP_BLE_KEY_EVT, key_type: %d",
             param->ble_security.ble_key.key_type);
    break;

  case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
    pairing_adv_apply();
    break;

  case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
    if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(GATTC_TAG, "Pairing adv start failed: %d",
               param->adv_start_cmpl.status);
      pairing_adv_active = false;
    }
    break;

  default:
    break;
  }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                         esp_ble_gattc_cb_param_t *param) {
  ESP_LOGI(GATTC_TAG, "EVT %d, gattc if %d", event, gattc_if);

  if (event == ESP_GATTC_REG_EVT) {
    if (param->reg.status != ESP_GATT_OK) {
      ESP_LOGI(GATTC_TAG, "Reg app failed, app_id %04x, status %d",
               param->reg.app_id, param->reg.status);
      return;
    }
    spp_gattc_if = gattc_if;
  }
  gattc_profile_event_handler(event, gattc_if, param);
}

static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
  esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

  switch (event) {
  case ESP_GATTC_REG_EVT:
    ESP_LOGI(GATTC_TAG, "REG EVT, set scan params");
    ESP_LOGI(GATTC_TAG, "Scanning for any %s device...", device_name);
    esp_ble_gap_set_scan_params(&ble_scan_params);
    break;
  case ESP_GATTC_CONNECT_EVT: {
    ESP_LOGI(GATTC_TAG, "ESP_GATTC_CONNECT_EVT: conn_id=%d, gatt_if = %d",
             p_data->connect.conn_id, gattc_if);
    ESP_LOGI(GATTC_TAG, "REMOTE BDA: " BT_BD_ADDR_STR,
             BT_BD_ADDR_HEX(p_data->connect.remote_bda));
    receiver_link_t *link = link_by_bda(p_data->connect.remote_bda);
    if (link == NULL) {
      link = link_alloc(p_data->connect.remote_bda);
    }
    if (link == NULL) {
      ESP_LOGW(GATTC_TAG, "No free receiver link slot, closing connection");
      esp_ble_gattc_close(gattc_if, p_data->connect.conn_id);
      break;
    }
    spp_gattc_if = gattc_if;
    link->conn_id = p_data->connect.conn_id;
    link->connect_ms = esp_timer_get_time() / 1000;
    set_is_connect(true);
    pairing_adv_apply();

    ESP_LOGI(GATTC_TAG, "Initiating BLE encryption...");
    esp_ble_set_encryption(p_data->connect.remote_bda,
                           ESP_BLE_SEC_ENCRYPT_MITM);

    esp_ble_gattc_search_service(spp_gattc_if, link->conn_id,
                                 &spp_service_uuid);

    // Request 20 ms connection interval so throttle packets are delivered
    // at the same rate we send them.
    esp_ble_conn_update_params_t conn_params = {
        .min_int = 16, // 16 * 1.25 ms = 20 ms
        .max_int = 16,
        .latency = 0,
        .timeout = 400, // 400 * 10 ms = 4 s supervision timeout
    };
    memcpy(conn_params.bda, p_data->connect.remote_bda, sizeof(esp_bd_addr_t));
    esp_ble_gap_update_conn_params(&conn_params);

    // Keep hunting for another receiver while dual connection wants one
    resume_scan_if_needed();

    // Send serial notification for config tool
    printf("#>DATA ble_status=connected\n");
    break;
  }
  case ESP_GATTC_DISCONNECT_EVT: {
    receiver_link_t *link = link_by_conn_id(p_data->disconnect.conn_id);
    if (link == NULL) {
      link = link_by_bda(p_data->disconnect.remote_bda);
    }
    ESP_LOGI(GATTC_TAG, "disconnect (conn_id=%d)", p_data->disconnect.conn_id);
    if (link != NULL) {
      int link_idx = (int)(link - links);
      link_free(link); // Also clears this receiver's telemetry
      ui_reset_skate_display(link_idx);
    }

    if (link_count_connected() == 0) {
      set_is_connect(false);

      // Send serial notification for config tool
      printf("#>DATA ble_status=disconnected\n");

      latest_erpm = 0;
      latest_voltage = 0.0f;
      latest_trip_km = 0.0f;
      bms_total_voltage = 0.0f;
      bms_remaining_capacity = 0.0f;
      bms_nominal_capacity = 0.0f;

      ESP_LOGI(GATTC_TAG,
               "Speed and battery values reset to 0 due to disconnection");

      ui_update_speed(0);
    }

    pairing_adv_apply();

    // Restart scanning for a receiver (skip when BLE is suspended)
    resume_scan_if_needed();
    break;
  }
  case ESP_GATTC_SEARCH_RES_EVT: {
    ESP_LOGI(GATTC_TAG,
             "ESP_GATTC_SEARCH_RES_EVT: start_handle = %d, end_handle = %d, "
             "UUID:0x%04x",
             p_data->search_res.start_handle, p_data->search_res.end_handle,
             p_data->search_res.srvc_id.uuid.uuid.uuid16);
    receiver_link_t *link = link_by_conn_id(p_data->search_res.conn_id);
    if (link != NULL) {
      link->srv_start_handle = p_data->search_res.start_handle;
      link->srv_end_handle = p_data->search_res.end_handle;
    }
    break;
  }
  case ESP_GATTC_SEARCH_CMPL_EVT:
    ESP_LOGI(GATTC_TAG, "SEARCH_CMPL: conn_id = %x, status %d",
             p_data->search_cmpl.conn_id, p_data->search_cmpl.status);
    esp_ble_gattc_send_mtu_req(gattc_if, p_data->search_cmpl.conn_id);
    break;
  case ESP_GATTC_REG_FOR_NOTIFY_EVT:
    // CCCD writes are issued directly by spp_client_reg_task; this event
    // only confirms the local registration.
    if (p_data->reg_for_notify.status != ESP_GATT_OK) {
      ESP_LOGE(GATTC_TAG, "ESP_GATTC_REG_FOR_NOTIFY_EVT, status = %d",
               p_data->reg_for_notify.status);
    }
    break;
  case ESP_GATTC_NOTIFY_EVT:
    ESP_LOGI(GATTC_TAG, "ESP_GATTC_NOTIFY_EVT");
    notify_event_handler(p_data);
    break;
  case ESP_GATTC_READ_CHAR_EVT:
    ESP_LOGI(GATTC_TAG, "ESP_GATTC_READ_CHAR_EVT");
    break;
  case ESP_GATTC_WRITE_CHAR_EVT:
    ESP_LOGI(GATTC_TAG, "ESP_GATTC_WRITE_CHAR_EVT:status = %d,handle = %d",
             param->write.status, param->write.handle);
    if (param->write.status != ESP_GATT_OK) {
      ESP_LOGE(GATTC_TAG, "ESP_GATTC_WRITE_CHAR_EVT, error status = %d",
               p_data->write.status);
      break;
    }
    break;
  case ESP_GATTC_PREP_WRITE_EVT:
    break;
  case ESP_GATTC_EXEC_EVT:
    break;
  case ESP_GATTC_WRITE_DESCR_EVT:
    ESP_LOGI(GATTC_TAG, "ESP_GATTC_WRITE_DESCR_EVT: status =%d,handle = %d",
             p_data->write.status, p_data->write.handle);
    if (p_data->write.status != ESP_GATT_OK) {
      ESP_LOGE(GATTC_TAG, "ESP_GATTC_WRITE_DESCR_EVT, error status = %d",
               p_data->write.status);
    }
    break;
  case ESP_GATTC_CFG_MTU_EVT: {
    if (p_data->cfg_mtu.status != ESP_OK) {
      break;
    }
    ESP_LOGI(GATTC_TAG, "+MTU:%d", p_data->cfg_mtu.mtu);
    receiver_link_t *link = link_by_conn_id(p_data->cfg_mtu.conn_id);
    if (link == NULL) {
      break;
    }
    link->mtu = p_data->cfg_mtu.mtu;

    uint16_t count = SPP_IDX_NB;
    if (esp_ble_gattc_get_db(gattc_if, link->conn_id, link->srv_start_handle,
                             link->srv_end_handle, link->db,
                             &count) != ESP_GATT_OK) {
      ESP_LOGE(GATTC_TAG, "%s:get db failed", __func__);
      break;
    }
    if (count != SPP_IDX_NB) {
      ESP_LOGE(GATTC_TAG,
               "%s:get db count != SPP_IDX_NB, count = %d, SPP_IDX_NB = %d",
               __func__, count, SPP_IDX_NB);
      break;
    }
    for (int i = 0; i < SPP_IDX_NB; i++) {
      ESP_LOGI(GATTC_TAG,
               "db[%d]: type=%d,attribute_handle=%d,start_handle=%d,"
               "end_handle=%d,properties=0x%x,uuid=0x%04x",
               i, link->db[i].type, link->db[i].attribute_handle,
               link->db[i].start_handle, link->db[i].end_handle,
               link->db[i].properties, link->db[i].uuid.uuid.uuid16);
    }
    link->ready = true;

    reg_work_t work = {.link_idx = (uint8_t)(link - links),
                       .attr_idx = SPP_IDX_SPP_DATA_NTY_VAL};
    xQueueSend(cmd_reg_queue, &work, 10 / portTICK_PERIOD_MS);
    work.attr_idx = SPP_IDX_SPP_STATUS_VAL;
    xQueueSend(cmd_reg_queue, &work, 10 / portTICK_PERIOD_MS);
    break;
  }
  case ESP_GATTC_SRVC_CHG_EVT:
    break;
  default:
    break;
  }
}

void spp_client_reg_task(void *arg) {
  reg_work_t work;
  for (;;) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if (xQueueReceive(cmd_reg_queue, &work, portMAX_DELAY)) {
      if (work.link_idx >= MAX_RECEIVER_LINKS ||
          (work.attr_idx != SPP_IDX_SPP_DATA_NTY_VAL &&
           work.attr_idx != SPP_IDX_SPP_STATUS_VAL)) {
        continue;
      }
      receiver_link_t *link = &links[work.link_idx];
      if (!link->ready) {
        continue; // Link dropped before registration ran
      }
      ESP_LOGI(GATTC_TAG, "Index = %d,UUID = 0x%04x, handle = %d",
               work.attr_idx, link->db[work.attr_idx].uuid.uuid.uuid16,
               link->db[work.attr_idx].attribute_handle);
      esp_ble_gattc_register_for_notify(
          spp_gattc_if, link->bda, link->db[work.attr_idx].attribute_handle);
      // Enable notifications on the server; the CCCD is the next attribute
      uint16_t notify_en = 1;
      esp_ble_gattc_write_char_descr(
          spp_gattc_if, link->conn_id,
          link->db[work.attr_idx + 1].attribute_handle, sizeof(notify_en),
          (uint8_t *)&notify_en, ESP_GATT_WRITE_TYPE_NO_RSP,
          ESP_GATT_AUTH_REQ_NONE);
    }
  }
}

void ble_client_appRegister(void) {
  esp_err_t status;
  char err_msg[20];

  if (is_connect_mutex == NULL) {
    is_connect_mutex = xSemaphoreCreateMutex();
    if (is_connect_mutex == NULL) {
      ESP_LOGE(GATTC_TAG, "Failed to create is_connect mutex");
    }
  }

  ESP_LOGI(GATTC_TAG, "register callback");

  if ((status = esp_ble_gap_register_callback(esp_gap_cb)) != ESP_OK) {
    ESP_LOGE(GATTC_TAG, "gap register error: %s",
             esp_err_to_name_r(status, err_msg, sizeof(err_msg)));
    return;
  }
  if ((status = esp_ble_gattc_register_callback(esp_gattc_cb)) != ESP_OK) {
    ESP_LOGE(GATTC_TAG, "gattc register error: %s",
             esp_err_to_name_r(status, err_msg, sizeof(err_msg)));
    return;
  }
  esp_ble_gattc_app_register(PROFILE_APP_ID);

  esp_ble_gap_config_adv_data_raw((uint8_t *)pairing_adv_data,
                                  sizeof(pairing_adv_data));

  esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(200);
  if (local_mtu_ret) {
    ESP_LOGE(GATTC_TAG, "set local  MTU failed: %s",
             esp_err_to_name_r(local_mtu_ret, err_msg, sizeof(err_msg)));
  }

  esp_ble_auth_req_t auth_req =
      ESP_LE_AUTH_REQ_SC_MITM_BOND; // Secure Connections, MITM protection,
                                    // Bonding
  esp_ble_io_cap_t io_cap =
      ESP_IO_CAP_KBDISP; // Keyboard + display (client enters passkey)
  uint8_t key_size = 16;
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint32_t passkey = BLE_PASSKEY;
  uint8_t oob_support = ESP_BLE_OOB_DISABLE;

  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey,
                                 sizeof(uint32_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key,
                                 sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support,
                                 sizeof(uint8_t));

  ESP_LOGI(GATTC_TAG, "BLE Security configured with passkey: %06lu",
           (unsigned long)passkey);

  cmd_reg_queue = xQueueCreate(10, sizeof(reg_work_t));
  xTaskCreate(spp_client_reg_task, "spp_client_reg_task", 2048, NULL, 10, NULL);
}

void spp_client_demo_init(void) {
  esp_err_t ret;

  esp_log_level_set(GATTC_TAG, ESP_LOG_WARN);

  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

  nvs_flash_init();

  aux_output_load_state();

  ble_trim_load_offset();

  // Load dual connection preference (set via the USB config tool)
  {
    vesc_config_t cfg;
    if (vesc_config_load(&cfg) == ESP_OK) {
      dual_connection_enabled = cfg.dual_connection;
    }
    ESP_LOGI(GATTC_TAG, "Dual connection %s",
             dual_connection_enabled ? "enabled" : "disabled");
  }

  ret = esp_bt_controller_init(&bt_cfg);
  if (ret) {
    ESP_LOGE(GATTC_TAG, "%s enable controller failed: %s", __func__,
             esp_err_to_name(ret));
    return;
  }

  ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret) {
    ESP_LOGE(GATTC_TAG, "%s enable controller failed: %s", __func__,
             esp_err_to_name(ret));
    return;
  }

  ESP_LOGI(GATTC_TAG, "%s init bluetooth", __func__);

  ret = esp_bluedroid_init();
  if (ret) {
    ESP_LOGE(GATTC_TAG, "%s init bluetooth failed: %s", __func__,
             esp_err_to_name(ret));
    return;
  }
  ret = esp_bluedroid_enable();
  if (ret) {
    ESP_LOGE(GATTC_TAG, "%s enable bluetooth failed: %s", __func__,
             esp_err_to_name(ret));
    return;
  }

  ble_client_appRegister();
  xTaskCreate(adc_send_task, "adc_send_task", 4096, NULL, 8, NULL);
  xTaskCreate(log_rssi_task, "log_rssi_task", 2048, NULL, 4, NULL);
}

static void adc_send_task(void *pvParameters) {
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  uint8_t data_buffer[3]; // throttle (2) + aux state (1)

#ifdef CONFIG_TARGET_LITE
  // Load once — invert_throttle only changes via USB config tool, never
  // mid-ride.
  bool invert_throttle = false;
  {
    vesc_config_t cfg;
    if (vesc_config_load(&cfg) == ESP_OK) {
      invert_throttle = cfg.invert_throttle;
    }
  }
#endif

  while (1) {
    esp_task_wdt_reset(); // Reset every iteration so watchdog is fed when not
                          // connected

    if (ble_suspended) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (ble_is_connected() && link_any_ready()) {
      uint32_t adc_value;
      bool throttle_inverted = false;

      // SAFETY: Block throttle on low battery - force neutral
      if (battery_is_low_voltage()) {
        adc_value = VESC_NEUTRAL_VALUE;
        ESP_LOGW(GATTC_TAG, "Low battery - throttle blocked, sending neutral");
      } else {
#ifdef CONFIG_TARGET_DUAL_THROTTLE
        adc_value = adc_get_latest_value();
#elif defined(CONFIG_TARGET_LITE)
        if (throttle_should_use_neutral()) {
          adc_value = VESC_NEUTRAL_VALUE;
        } else {
          adc_value = adc_get_latest_value();
        }

        if (invert_throttle) {
          adc_value = 255 - adc_value;
          throttle_inverted = true;
        }
#endif
      }

      int8_t effective_trim =
          throttle_inverted ? -ble_trim_offset : ble_trim_offset;
      uint8_t final_ble_value =
          throttle_apply_trim((uint8_t)adc_value, effective_trim);
      // Trimmed neutral: what the same mapping yields for a centered stick.
      // Sent to a link during its post-connect neutral hold period.
      uint8_t neutral_ble_value =
          throttle_apply_trim(VESC_NEUTRAL_VALUE, effective_trim);

      uint32_t now_ms = esp_timer_get_time() / 1000;
      for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
        receiver_link_t *link = &links[i];
        if (!link->ready || !(link->db[SPP_IDX_SPP_DATA_RECV_VAL].properties &
                              (ESP_GATT_CHAR_PROP_BIT_WRITE_NR |
                               ESP_GATT_CHAR_PROP_BIT_WRITE))) {
          continue;
        }
        // Hold neutral for the first NEUTRAL_HOLD_MS after this link connects
        uint8_t out_value = (now_ms - link->connect_ms < NEUTRAL_HOLD_MS)
                                ? neutral_ble_value
                                : final_ble_value;
        data_buffer[0] = out_value;
        data_buffer[1] = 0;
        data_buffer[2] = aux_output_state ? 1 : 0;

        esp_err_t ret = esp_ble_gattc_write_char(
            spp_gattc_if, link->conn_id,
            link->db[SPP_IDX_SPP_DATA_RECV_VAL].attribute_handle,
            sizeof(data_buffer), // 3 bytes
            data_buffer, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        if (ret != ESP_OK) {
          ESP_LOGW(GATTC_TAG, "Failed to send throttle value: %s",
                   esp_err_to_name(ret));
        }
      }

      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(ADC_SEND_INTERVAL_MS));
    } else {
      vTaskDelay(
          pdMS_TO_TICKS(50)); // Yield when not connected, avoid tight loop
    }
  }
}

float get_latest_voltage(void) { return latest_voltage; }

int32_t get_latest_erpm(void) { return latest_erpm; }

float get_bms_total_voltage(void) { return bms_total_voltage; }

static void log_rssi_task(void *pvParameters) {
  while (1) {
    if (ble_suspended) {
      vTaskDelay(pdMS_TO_TICKS(RSSI_READ_INTERVAL_MS));
      continue;
    }
    if (ble_is_connected() && spp_gattc_if != ESP_GATT_IF_NONE) {
      for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
        if (!links[i].in_use) {
          continue;
        }
        esp_err_t ret = esp_ble_gap_read_rssi(links[i].bda);
        if (ret != ESP_OK) {
          ESP_LOGE(GATTC_TAG, "Read RSSI failed: %s", esp_err_to_name(ret));
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(RSSI_READ_INTERVAL_MS));
  }
}

static int bms_capacity_to_percentage(float remaining, float nominal) {
  if (nominal <= 0.0f)
    return -1;

  float percentage = (remaining / nominal) * 100.0f;

  if (percentage > 100.0f)
    percentage = 100.0f;
  if (percentage < 0.0f)
    percentage = 0.0f;

  return (int)percentage;
}

int get_bms_battery_percentage(void) {
  return bms_capacity_to_percentage(bms_remaining_capacity,
                                    bms_nominal_capacity);
}

static const receiver_link_t *link_for_idx(int idx) {
  if (idx < 0 || idx >= MAX_RECEIVER_LINKS)
    return NULL;
  return &links[idx];
}

bool ble_receiver_is_connected(int idx) {
  const receiver_link_t *link = link_for_idx(idx);
  return !ble_suspended && link != NULL && link->ready;
}

bool ble_receiver_get_rssi(int idx, int8_t *out_rssi) {
  const receiver_link_t *link = link_for_idx(idx);
  if (link == NULL || !link->in_use || !link->rssi_valid)
    return false;
  if (out_rssi != NULL)
    *out_rssi = link->rssi;
  return true;
}

float ble_receiver_get_bms_total_voltage(int idx) {
  const receiver_link_t *link = link_for_idx(idx);
  return link != NULL ? link->bms_total_voltage : 0.0f;
}

int ble_receiver_get_bms_battery_percentage(int idx) {
  const receiver_link_t *link = link_for_idx(idx);
  if (link == NULL)
    return -1;
  return bms_capacity_to_percentage(link->bms_remaining_capacity,
                                    link->bms_nominal_capacity);
}

float ble_receiver_get_vesc_voltage(int idx) {
  const receiver_link_t *link = link_for_idx(idx);
  return link != NULL ? link->vesc_voltage : 0.0f;
}

float ble_receiver_get_trip_km(int idx) {
  const receiver_link_t *link = link_for_idx(idx);
  return link != NULL ? link->trip_km : 0.0f;
}

void ble_suspend(void) {
  if (ble_suspended) {
    return; // already suspended
  }
  ble_suspended = true;
  connect_pending = false;
  esp_ble_gap_stop_scanning();
  pairing_adv_apply();
  if (spp_gattc_if != ESP_GATT_IF_NONE) {
    for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
      if (links[i].in_use) {
        esp_ble_gattc_close(spp_gattc_if, links[i].conn_id);
      }
    }
  }
  ESP_LOGI(GATTC_TAG, "BLE suspended: stopped scan and connections");
}

void ble_resume(void) {
  if (!ble_suspended) {
    return; // already active
  }
  ble_suspended = false;
  ESP_LOGI(GATTC_TAG, "BLE resumed: starting scan");
  esp_ble_gap_set_scan_params(&ble_scan_params);
  pairing_adv_apply();
}

void ble_set_dual_connection(bool enabled) {
  if (dual_connection_enabled == enabled) {
    return;
  }
  dual_connection_enabled = enabled;
  ESP_LOGI(GATTC_TAG, "Dual connection %s", enabled ? "enabled" : "disabled");
  if (enabled) {
    // Start looking for a second receiver right away
    resume_scan_if_needed();
  } else if (spp_gattc_if != ESP_GATT_IF_NONE) {
    // Keep the first connected link, drop any extra
    bool kept = false;
    for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
      if (!links[i].in_use) {
        continue;
      }
      if (!kept) {
        kept = true;
        continue;
      }
      esp_ble_gattc_close(spp_gattc_if, links[i].conn_id);
    }
  }
  pairing_adv_apply();
}

bool ble_dual_connection_is_enabled(void) { return dual_connection_enabled; }

bool ble_is_connected(void) {
  if (ble_suspended) {
    return false;
  }
  bool result = false;
  if (is_connect_mutex != NULL &&
      xSemaphoreTake(is_connect_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    result = is_connect;
    xSemaphoreGive(is_connect_mutex);
  } else {
    result = is_connect;
  }
  return result;
}
