/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Changes since: Copyright (c) George Benett
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include <stdatomic.h>
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
#include "gb_protocol.h"
#include "rx_ota.h"
#include "settings.h"
#include "target_config.h"
#include "throttle.h"
#include "ui_updater.h"

enum {
  SPP_IDX_SVC,
  SPP_IDX_SPP_DATA_RECV_VAL,
  SPP_IDX_SPP_DATA_NTY_VAL,
  SPP_IDX_SPP_DATA_NTF_CFG,
  SPP_IDX_SPP_COMMAND_VAL,
  SPP_IDX_SPP_STATUS_VAL,
  SPP_IDX_SPP_STATUS_CFG,
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
static TaskHandle_t adc_send_task_handle = NULL;
static void read_rssi_cb(void *arg);
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

static _Atomic int64_t pairing_until_us;
// Scratch for the bond list; only the BTC task and ble_forget_receivers use it.
static esp_ble_bond_dev_t bond_list[CONFIG_BT_SMP_MAX_BONDS];

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
  bool fw_seen;   // Receiver answered GB_CMD_GET_FW_VERSION
  bool encrypted; // SMP done; the receiver ignores writes until then
  float vesc_voltage;
  float bms_total_voltage;
  float bms_remaining_capacity;
  float bms_nominal_capacity;
  float trip_km;
} receiver_link_t;

static receiver_link_t links[MAX_RECEIVER_LINKS];

static receiver_link_t *link_by_conn_id(uint16_t conn_id);
static receiver_link_t *link_by_bda(const esp_bd_addr_t bda);

static atomic_bool is_connect; // True while at least one link is connected
static const char device_name[] = GB_RECEIVER_ADV_NAME;
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
            .uuid16 = GB_UUID_SERVICE,
        },
};

static float latest_voltage = 0.0f;
static int32_t latest_erpm = 0;

static float bms_total_voltage = 0.0f;
static float bms_remaining_capacity = 0.0f;
static float bms_nominal_capacity = 0.0f;

#define OTA_CONN_INTERVAL 6 // 7.5 ms during receiver updates; tune on hardware

static bool aux_output_state = false;
static bool receiver_aux_output_state = false;
static float latest_trip_km = 0.0f;

/** When true, BLE is suspended (not on home screen). Starts true;
 *  resumed only when the home screen is reached. */
static atomic_bool ble_suspended = true;

float ble_get_latest_trip_km(void) { return latest_trip_km; }

/** Smart reverse preference (set via the USB config tool). The receiver runs
 *  the logic; it learns the setting from us on every connect. */
static bool smart_reverse_enabled = false;

/** Assistive push preference (set via the USB config tool). The receiver runs
 *  the logic; it learns the setting from us on every connect. */
static bool assist_push_enabled = false;
static uint8_t assist_strength = ASSIST_STRENGTH_DEFAULT;
static uint8_t assist_decay = ASSIST_DECAY_DEFAULT;

/** Speed limit cap in km/h (0 = off) and the ride profile's drive current
 *  share. The receiver enforces both; we resend them on every connect. */
static uint8_t speed_limit_cap_kmh = 0;
static uint8_t current_scale_pct = 100;

/** Reverse lockout (USB config tool). Sent to the receiver on connect. */
static bool no_reverse_enabled = false;

/* Write to a link's command characteristic. ESP_ERR_INVALID_STATE until
 * discovery found a writable one. */
static esp_err_t link_write_cmd(const receiver_link_t *link, const uint8_t *buf,
                                uint16_t len) {
  if (link == NULL || !link->ready || !link->encrypted ||
      !(link->db[SPP_IDX_SPP_COMMAND_VAL].properties &
        (ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE)))
    return ESP_ERR_INVALID_STATE;
  return esp_ble_gattc_write_char(
      spp_gattc_if, link->conn_id,
      link->db[SPP_IDX_SPP_COMMAND_VAL].attribute_handle, len, (uint8_t *)buf,
      ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
}

/* ESP_OK if at least one receiver took it. */
static esp_err_t write_cmd_all(const uint8_t *buf, uint16_t len) {
  esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
  for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
    if (link_write_cmd(&links[i], buf, len) == ESP_OK)
      ret = ESP_OK;
  }
  return ret;
}

static bool is_bonded(const esp_bd_addr_t bda) {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0 || n > CONFIG_BT_SMP_MAX_BONDS ||
      esp_ble_get_bond_device_list(&n, bond_list) != ESP_OK)
    return false;
  for (int i = 0; i < n; i++) {
    if (memcmp(bond_list[i].bd_addr, bda, sizeof(esp_bd_addr_t)) == 0)
      return true;
  }
  return false;
}

bool ble_is_pairing(void) {
  return esp_timer_get_time() < pairing_until_us ||
         esp_ble_get_bond_device_num() == 0;
}

void ble_start_pairing(uint32_t seconds) {
  pairing_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
  ESP_LOGI(GATTC_TAG, "Pairing open for %lu s", (unsigned long)seconds);
  resume_scan_if_needed();
}

void ble_forget_receivers(void) {
  int n = CONFIG_BT_SMP_MAX_BONDS;
  if (esp_ble_get_bond_device_list(&n, bond_list) == ESP_OK) {
    for (int i = 0; i < n; i++) {
      esp_ble_remove_bond_device(bond_list[i].bd_addr);
    }
  }
  if (spp_gattc_if != ESP_GATT_IF_NONE) {
    for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
      if (links[i].in_use) {
        esp_ble_gattc_close(spp_gattc_if, links[i].conn_id);
      }
    }
  }
  ESP_LOGI(GATTC_TAG, "Forgot %d receiver(s)", n);
}

static void cmd_no_reverse(uint8_t out[2]) {
  out[0] = GB_CMD_SET_NO_REVERSE;
  out[1] = no_reverse_enabled;
}
static void cmd_smart_reverse(uint8_t out[2]) {
  out[0] = GB_CMD_SET_SMART_REVERSE;
  out[1] = smart_reverse_enabled;
}
static void cmd_assist(uint8_t out[4]) {
  out[0] = GB_CMD_SET_ASSIST_PUSH;
  out[1] = assist_push_enabled;
  out[2] = assist_strength;
  out[3] = assist_decay;
}

/* The receiver resets these to off on every connect until we send ours. */
static void link_send_settings(const receiver_link_t *link) {
  uint8_t cmd[4];
  cmd_smart_reverse(cmd);
  link_write_cmd(link, cmd, 2);
  cmd_no_reverse(cmd);
  link_write_cmd(link, cmd, 2);
  cmd_assist(cmd);
  link_write_cmd(link, cmd, 4);
  link_write_cmd(link, (uint8_t[]){GB_CMD_SET_SPEED_LIMIT, speed_limit_cap_kmh},
                 2);
  link_write_cmd(link, (uint8_t[]){GB_CMD_SET_CURRENT_SCALE, current_scale_pct},
                 2);
}

/* Settings and the version request go out once the link is both discovered
 * and encrypted, whichever finishes last. */
static void link_on_secure(receiver_link_t *link) {
  if (!link->ready || !link->encrypted) {
    return;
  }
  link_send_settings(link);
  uint8_t req = GB_CMD_GET_FW_VERSION;
  link_write_cmd(link, &req, 1);
}

esp_err_t ble_send_reset_odometer(void) {
  if (!ble_is_connected()) {
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t err = write_cmd_all((uint8_t[]){GB_CMD_RESET_ODOMETER}, 1);
  if (err == ESP_OK) {
    ESP_LOGI(GATTC_TAG, "Reset odometer command sent to receiver(s)");
  }
  return err;
}

esp_err_t ble_send_shutdown(void) {
  if (!ble_is_connected()) {
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t err = write_cmd_all((uint8_t[]){GB_CMD_SHUTDOWN}, 1);
  if (err == ESP_OK) {
    ESP_LOGI(GATTC_TAG, "Shutdown command sent to receiver(s)");
  }
  return err;
}

static void aux_output_save_state(void) {
  settings_t cfg;
  settings_get(&cfg);
  cfg.aux_on = aux_output_state;
  settings_set(&cfg);
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

static void notify_event_handler(esp_ble_gattc_cb_param_t *p_data) {
  uint16_t handle = 0;

  if (p_data->notify.is_notify == true) {
    ESP_LOGD(GATTC_TAG, "+NOTIFY:handle = %d,length = %d ",
             p_data->notify.handle, p_data->notify.value_len);
  } else {
    ESP_LOGD(GATTC_TAG, "+INDICATE:handle = %d,length = %d ",
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
    const uint8_t *v = p_data->notify.value;
    if (p_data->notify.value_len >= 10 && v[0] == GB_CMD_GET_FW_VERSION) {
      char fw[12];
      link->fw_seen = true;
      snprintf(fw, sizeof(fw), "%u.%u.%u", v[1], v[2], v[3]);
      ui_set_receiver_fw(fw);
      rx_ota_on_version(link_idx, v[1], v[2], v[3]);
      return;
    }
    if (p_data->notify.value_len >= 1 && v[0] >= GB_CMD_OTA_BEGIN &&
        v[0] <= GB_CMD_OTA_END) {
      rx_ota_on_reply(v, p_data->notify.value_len);
      return;
    }
    if (p_data->notify.value_len >= 2 &&
        p_data->notify.value[0] == GB_CMD_RESET_ODOMETER) {
      if (p_data->notify.value[1] == 0x00) {
        latest_trip_km = 0.0f;
        link->trip_km = 0.0f;
        ui_update_trip_distance(link_idx, 0.0f);
        ESP_LOGI(GATTC_TAG, "Odometer reset ACK received from receiver");
        printf("#>DATA odometer_reset=ok\n");
      }
    }
    if (!link->fw_seen) {
      // The first request lands before SMP completes and the receiver drops
      // it; its once-a-second status frame paces the retry.
      uint8_t req = GB_CMD_GET_FW_VERSION;
      ble_receiver_write_cmd(link_idx, &req, sizeof(req));
    }
    return;
  }

  if (handle == db[SPP_IDX_SPP_DATA_NTY_VAL].attribute_handle) {
    gb_telemetry_t t;
    if (!gb_telemetry_decode(p_data->notify.value, p_data->notify.value_len,
                             &t)) {
      ESP_LOGW(GATTC_TAG, "Telemetry too short: %d bytes",
               p_data->notify.value_len);
      return;
    }
    latest_erpm = t.erpm;
    float decoded_voltage = t.v_in / 100.0f;
    if (decoded_voltage > 1.0f) {
      latest_voltage = decoded_voltage;
      link->vesc_voltage = decoded_voltage;
    }
    bms_total_voltage = t.bms_total_voltage / 100.0f;
    link->bms_total_voltage = bms_total_voltage;
    bms_remaining_capacity = t.bms_remaining_ah / 100.0f;
    link->bms_remaining_capacity = bms_remaining_capacity;
    bms_nominal_capacity = t.bms_nominal_ah / 100.0f;
    link->bms_nominal_capacity = bms_nominal_capacity;

    // Motor config from the VESC, kept in RAM only
    settings_update_motor(t.motor_poles, t.gear_ratio_x1000,
                          t.wheel_diameter_mm);

    receiver_aux_output_state = t.aux_on != 0;
    ui_update_aux_output_indicator();

    latest_trip_km = t.trip_km_x100 / 100.0f;
    link->trip_km = latest_trip_km;
    ui_update_trip_distance(link_idx, link->trip_km);

    ESP_LOGD(GATTC_TAG, "VESC %.2f V, %ld erpm; BMS %.2f V, %.2f Ah, %d cells",
             latest_voltage, (long)latest_erpm, bms_total_voltage,
             bms_remaining_capacity, t.bms_cells);
  }
}

static void pairing_adv_apply(void) {
  bool want = !ble_suspended && !rx_ota_active() &&
              link_count_connected() < desired_link_count();
  if (want && !pairing_adv_active) {
    esp_ble_gap_start_advertising(&pairing_adv_params);
    pairing_adv_active = true;
  } else if (!want && pairing_adv_active) {
    esp_ble_gap_stop_advertising();
    pairing_adv_active = false;
  }
}

static void set_is_connect(bool value) { is_connect = value; }

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
  if (ble_suspended || scan_active || connect_pending || rx_ota_active())
    return;
  if (link_count_connected() >= desired_link_count())
    return;
  esp_ble_gap_start_scanning(0); // until stopped
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

      if (gb_adv_name_matches(adv_name, adv_name_len, device_name) &&
          (ble_is_pairing() || is_bonded(scan_result->scan_rst.bda))) {
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
  // WARN, only during receiver updates: GATTC_TAG is filtered to WARN and
  // these are what the update speed depends on.
  case ESP_GAP_BLE_PHY_UPDATE_COMPLETE_EVT:
    if (rx_ota_active()) {
      ESP_LOGW(GATTC_TAG, "PHY tx %d rx %d (status %d)",
               param->phy_update.tx_phy, param->phy_update.rx_phy,
               param->phy_update.status);
    }
    break;
  case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
    if (rx_ota_active()) {
      ESP_LOGW(GATTC_TAG, "Conn interval %u x 1.25 ms (status %d)",
               param->update_conn_params.conn_int,
               param->update_conn_params.status);
    }
    break;
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
    esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true,
                          CONFIG_GB_BLE_PASSKEY);
    break;

  case ESP_GAP_BLE_NC_REQ_EVT:
    // Only a peer we chose to connect to (bonded, or in a pairing window)
    // gets this far.
    esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
    break;

  case ESP_GAP_BLE_AUTH_CMPL_EVT:
    if (param->ble_security.auth_cmpl.success) {
      ESP_LOGI(GATTC_TAG, "Receiver " BT_BD_ADDR_STR " encrypted",
               BT_BD_ADDR_HEX(param->ble_security.auth_cmpl.bd_addr));
      receiver_link_t *link =
          link_by_bda(param->ble_security.auth_cmpl.bd_addr);
      if (link != NULL) {
        link->encrypted = true;
        link_on_secure(link);
      }
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
  ESP_LOGD(GATTC_TAG, "EVT %d, gattc if %d", event, gattc_if);

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
    ESP_LOGD(GATTC_TAG, "ESP_GATTC_NOTIFY_EVT");
    notify_event_handler(p_data);
    break;
  case ESP_GATTC_READ_CHAR_EVT:
    ESP_LOGD(GATTC_TAG, "ESP_GATTC_READ_CHAR_EVT");
    break;
  case ESP_GATTC_WRITE_CHAR_EVT:
    ESP_LOGD(GATTC_TAG, "ESP_GATTC_WRITE_CHAR_EVT:status = %d,handle = %d",
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
    ESP_LOGD(GATTC_TAG, "ESP_GATTC_WRITE_DESCR_EVT: status =%d,handle = %d",
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
    link_on_secure(link);

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

static void spp_client_reg_task(void *arg) {
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
      ESP_LOGD(GATTC_TAG, "Index = %d,UUID = 0x%04x, handle = %d",
               work.attr_idx, link->db[work.attr_idx].uuid.uuid.uuid16,
               link->db[work.attr_idx].attribute_handle);
      esp_ble_gattc_register_for_notify(
          spp_gattc_if, link->bda, link->db[work.attr_idx].attribute_handle);
      // Enable notifications on the server; the CCCD is the next attribute.
      // WRITE_TYPE_RSP, not NO_RSP: the spec requires a Write Request for a
      // CCCD, and a rejected Write Command reports no error - the subscription
      // just silently never happens.
      uint16_t notify_en = 1;
      esp_ble_gattc_write_char_descr(
          spp_gattc_if, link->conn_id,
          link->db[work.attr_idx + 1].attribute_handle, sizeof(notify_en),
          (uint8_t *)&notify_en, ESP_GATT_WRITE_TYPE_RSP,
          ESP_GATT_AUTH_REQ_NONE);
      if (work.attr_idx == SPP_IDX_SPP_STATUS_VAL) {
        // After the CCCD write, so the reply is not notified into the void.
        uint8_t req = GB_CMD_GET_FW_VERSION;
        ble_receiver_write_cmd(work.link_idx, &req, sizeof(req));
      }
    }
  }
}

static void ble_client_appRegister(void) {
  esp_err_t status;
  char err_msg[20];

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

  // The receiver offers 500; receiver updates send MTU-sized chunks.
  esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
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
  uint32_t passkey = CONFIG_GB_BLE_PASSKEY;
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

  cmd_reg_queue = xQueueCreate(10, sizeof(reg_work_t));
  ESP_ERROR_CHECK(xTaskCreate(spp_client_reg_task, "spp_client_reg_task", 2048,
                              NULL, 10, NULL) == pdPASS
                      ? ESP_OK
                      : ESP_ERR_NO_MEM);
}

void ble_init(void) {
  esp_err_t ret;

  esp_log_level_set(GATTC_TAG, ESP_LOG_WARN);

  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

  {
    settings_t cfg;
    settings_get(&cfg);
    aux_output_state = cfg.aux_on;
    dual_connection_enabled = cfg.dual_connection;
    smart_reverse_enabled = cfg.smart_reverse;
    no_reverse_enabled = cfg.no_reverse;
    assist_push_enabled = cfg.assist_push;
    assist_strength = cfg.assist_strength;
    assist_decay = cfg.assist_decay;
    speed_limit_cap_kmh = cfg.speed_limit_kmh;
    current_scale_pct = settings_ride_profile_current_pct(cfg.ride_profile);
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
  ESP_ERROR_CHECK(xTaskCreate(adc_send_task, "adc_send_task", 4096, NULL, 8,
                              &adc_send_task_handle) == pdPASS
                      ? ESP_OK
                      : ESP_ERR_NO_MEM);
  const esp_timer_create_args_t rssi_timer_args = {.callback = read_rssi_cb,
                                                   .name = "ble_rssi"};
  esp_timer_handle_t rssi_timer;
  ESP_ERROR_CHECK(esp_timer_create(&rssi_timer_args, &rssi_timer));
  ESP_ERROR_CHECK(
      esp_timer_start_periodic(rssi_timer, RSSI_READ_INTERVAL_MS * 1000));
}

static void adc_send_task(void *pvParameters) {
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  uint8_t data_buffer[3]; // throttle (2) + aux state (1)

  while (1) {
    esp_task_wdt_reset(); // Reset every iteration so watchdog is fed when not
                          // connected

    if (ble_suspended) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (ble_is_connected() && link_any_ready()) {
      /* Paced by the sampler: our own tick would drift against it and re-send
       * unchanged values. A timeout means it stalled - send nothing, so a
       * frozen reading is never held on the motors. */
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ADC_SEND_TIMEOUT_MS)) == 0) {
        ESP_LOGW(GATTC_TAG, "No throttle sample in %d ms, not sending",
                 ADC_SEND_TIMEOUT_MS);
        continue;
      }

      throttle_cmd_t cmd;
      throttle_get_command(&cmd);

      uint32_t now_ms = esp_timer_get_time() / 1000;
      for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
        receiver_link_t *link = &links[i];
        if (!link->ready || !link->encrypted ||
            !(link->db[SPP_IDX_SPP_DATA_RECV_VAL].properties &
              (ESP_GATT_CHAR_PROP_BIT_WRITE_NR |
               ESP_GATT_CHAR_PROP_BIT_WRITE))) {
          continue;
        }
        // Hold neutral for the first NEUTRAL_HOLD_MS after this link connects,
        // and while a receiver takes a firmware update.
        uint8_t out_value =
            (now_ms - link->connect_ms < NEUTRAL_HOLD_MS || rx_ota_active())
                ? cmd.neutral
                : cmd.value;
        data_buffer[0] = out_value;
        data_buffer[1] = 0;
        data_buffer[2] = aux_output_state ? 1 : 0;

        esp_err_t ret = esp_ble_gattc_write_char(
            spp_gattc_if, link->conn_id,
            link->db[SPP_IDX_SPP_DATA_RECV_VAL].attribute_handle,
            sizeof(data_buffer), // 3 bytes
            data_buffer, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        static int64_t last_fail_us;
        int64_t now_us = esp_timer_get_time();
        if (ret != ESP_OK && now_us - last_fail_us > 1000000) {
          last_fail_us = now_us;
          ESP_LOGW(GATTC_TAG, "Failed to send throttle value: %s",
                   esp_err_to_name(ret));
        }
      }

      esp_task_wdt_reset();
    } else {
      vTaskDelay(
          pdMS_TO_TICKS(50)); // Yield when not connected, avoid tight loop
    }
  }
}

float ble_get_vesc_voltage(void) { return latest_voltage; }

int32_t ble_get_erpm(void) { return latest_erpm; }

float ble_get_bms_total_voltage(void) { return bms_total_voltage; }

static void read_rssi_cb(void *arg) {
  if (ble_suspended || !is_connect || spp_gattc_if == ESP_GATT_IF_NONE) {
    return;
  }
  for (int i = 0; i < MAX_RECEIVER_LINKS; i++) {
    if (links[i].in_use && esp_ble_gap_read_rssi(links[i].bda) != ESP_OK) {
      ESP_LOGE(GATTC_TAG, "Read RSSI failed");
    }
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

int ble_get_bms_battery_percentage(void) {
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

esp_err_t ble_receiver_write_cmd(int idx, const uint8_t *data, uint16_t len) {
  return link_write_cmd(link_for_idx(idx), data, len);
}

bool ble_receiver_can_send(int idx) {
  const receiver_link_t *link = link_for_idx(idx);
  return link != NULL && link->ready &&
         esp_ble_get_cur_sendable_packets_num(link->conn_id) > 0;
}

void ble_receiver_ota_link(int idx, bool on) {
  receiver_link_t *link = (receiver_link_t *)link_for_idx(idx);
  if (link != NULL && link->ready) {
    // 2M halves airtime but costs range, so it is only for updates.
    esp_ble_gap_phy_mask_t phy =
        on ? ESP_BLE_GAP_PHY_2M_PREF_MASK : ESP_BLE_GAP_PHY_1M_PREF_MASK;
    esp_ble_gap_set_preferred_phy(link->bda, 0, phy, phy,
                                  ESP_BLE_GAP_PHY_OPTIONS_NO_PREF);
    esp_ble_conn_update_params_t p = {
        .min_int = on ? OTA_CONN_INTERVAL : 16, // 16 = the usual 20 ms
        .max_int = on ? OTA_CONN_INTERVAL : 16,
        .latency = 0,
        .timeout = 400,
    };
    memcpy(p.bda, link->bda, sizeof(p.bda));
    esp_ble_gap_update_conn_params(&p);
  }
  // Scan and pairing advert share the radio with the link; rx_ota_active()
  // holds both off while an update runs.
  if (on && scan_active && !connect_pending) {
    esp_ble_gap_stop_scanning();
  }
  pairing_adv_apply();
  resume_scan_if_needed();
}

uint16_t ble_receiver_mtu(int idx) {
  const receiver_link_t *link = link_for_idx(idx);
  return (link != NULL && link->mtu >= 23) ? link->mtu : 23;
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
    // The second receiver is not bonded yet: let it pair.
    ble_start_pairing(BLE_PAIRING_WINDOW_S);
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

void ble_notify_throttle_sample(void) {
  if (adc_send_task_handle) {
    xTaskNotifyGive(adc_send_task_handle);
  }
}

void ble_set_smart_reverse(bool enabled) {
  smart_reverse_enabled = enabled;
  ESP_LOGI(GATTC_TAG, "Smart reverse %s", enabled ? "enabled" : "disabled");
  uint8_t cmd[2];
  cmd_smart_reverse(cmd);
  write_cmd_all(cmd, sizeof(cmd));
}

void ble_set_no_reverse(bool enabled) {
  no_reverse_enabled = enabled;
  ESP_LOGI(GATTC_TAG, "Reverse %s", enabled ? "disabled" : "enabled");
  uint8_t cmd[2];
  cmd_no_reverse(cmd);
  write_cmd_all(cmd, sizeof(cmd));
}

void ble_set_assist_push(bool enabled) {
  assist_push_enabled = enabled;
  ESP_LOGI(GATTC_TAG, "Assistive push %s", enabled ? "enabled" : "disabled");
  uint8_t cmd[4];
  cmd_assist(cmd);
  write_cmd_all(cmd, sizeof(cmd));
}

void ble_set_assist_params(uint8_t strength_pct, uint8_t decay_rpm_s) {
  assist_strength = strength_pct;
  assist_decay = decay_rpm_s;
  ESP_LOGI(GATTC_TAG, "Assist params: strength %d%%, decay %d rpm/s",
           strength_pct, decay_rpm_s);
  uint8_t cmd[4];
  cmd_assist(cmd);
  write_cmd_all(cmd, sizeof(cmd));
}

void ble_set_speed_limit(uint8_t cap_kmh) {
  speed_limit_cap_kmh = cap_kmh;
  ESP_LOGI(GATTC_TAG, "Speed limit: %u km/h", cap_kmh);
  write_cmd_all((uint8_t[]){GB_CMD_SET_SPEED_LIMIT, cap_kmh}, 2);
}

void ble_set_current_scale(uint8_t pct) {
  current_scale_pct = pct;
  ESP_LOGI(GATTC_TAG, "Drive current scale: %u%%", pct);
  write_cmd_all((uint8_t[]){GB_CMD_SET_CURRENT_SCALE, pct}, 2);
}

bool ble_dual_connection_is_enabled(void) { return dual_connection_enabled; }

bool ble_is_connected(void) { return !ble_suspended && is_connect; }
