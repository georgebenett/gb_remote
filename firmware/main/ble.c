/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/****************************************************************************
 *
 * This file is for ble spp client demo.
 *
 ****************************************************************************/

#include "driver/uart.h"
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

struct gattc_profile_inst {
  esp_gattc_cb_t gattc_cb;
  uint16_t gattc_if;
  uint16_t app_id;
  uint16_t conn_id;
  uint16_t service_start_handle;
  uint16_t service_end_handle;
  uint16_t char_handle;
  esp_bd_addr_t remote_bda;
};

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

/// Declare static functions
static void esp_gap_cb(esp_gap_ble_cb_event_t event,
                       esp_ble_gap_cb_param_t *param);
static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                         esp_ble_gattc_cb_param_t *param);
static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param);
static void adc_send_task(void *pvParameters);
static void log_rssi_task(void *pvParameters);

/* One gatt-based profile one app_id and one gattc_if, this array will store the
 * gattc_if returned by ESP_GATTS_REG_EVT */
static struct gattc_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_ID] =
        {
            .gattc_cb = gattc_profile_event_handler,
            .gattc_if = ESP_GATT_IF_NONE, /* Not get the gatt_if, so initial is
                                             ESP_GATT_IF_NONE */
        },
};

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE};

static bool is_connect = false;
static SemaphoreHandle_t is_connect_mutex = NULL;
static const char device_name[] = DEVICE_NAME;
static uint16_t spp_conn_id = 0;
static uint16_t spp_mtu_size = 23;
static uint16_t cmd = 0;
static uint16_t spp_srv_start_handle = 0;
static uint16_t spp_srv_end_handle = 0;
static uint16_t spp_gattc_if = 0xff;
static char *notify_value_p = NULL;
static int notify_value_offset = 0;
static int notify_value_count = 0;
static uint16_t count = SPP_IDX_NB;
static esp_gattc_db_elem_t *db = NULL;
static esp_ble_gap_cb_param_t scan_rst;
static QueueHandle_t cmd_reg_queue = NULL;
QueueHandle_t spp_uart_queue = NULL;

#ifdef SUPPORT_HEARTBEAT
static uint8_t heartbeat_s[9] = {'E', 's', 'p', 'r', 'e', 's', 's', 'i', 'f'};
static QueueHandle_t cmd_heartbeat_queue = NULL;
#endif

static esp_bt_uuid_t spp_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid =
        {
            .uuid16 = ESP_GATT_SPP_SERVICE_UUID,
        },
};

static float latest_voltage = 0.0f;
static int32_t latest_erpm = 0;
static float latest_current_motor = 0.0f;
static float latest_current_in = 0.0f;

static float bms_total_voltage = 0.0f;
static float bms_current = 0.0f;
static float bms_remaining_capacity = 0.0f;
static float bms_nominal_capacity = 0.0f;
static uint8_t bms_num_cells = 0;
static float bms_cell_voltages[16] = {0};

static float latest_temp_mos = 0.0f;
static float latest_temp_motor = 0.0f;

static bool aux_output_state = false;
static int8_t ble_trim_offset = 0; // Trim offset for BLE output (-127 to +127)

// Paired device management
static esp_bd_addr_t paired_server_mac = {0};
static bool has_paired_server = false;
static bool searching_for_paired = false;
static bool pending_scan_restart =
    false; // Flag to restart scan after stop completes
static int64_t reconnect_start_time = 0;
static TimerHandle_t reconnect_timer = NULL;
static TaskHandle_t reconnect_task_handle = NULL;

// Task to handle reconnect operations (avoids stack overflow in timer callback)
static void reconnect_handler_task(void *pvParameters);

float get_latest_temp_mos(void) { return latest_temp_mos; }

float get_latest_temp_motor(void) { return latest_temp_motor; }

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

static void ble_paired_load_mac(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(BLE_PAIRED_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err == ESP_OK) {
    uint8_t valid = 0;
    if (nvs_get_u8(nvs_handle, BLE_PAIRED_NVS_KEY_VALID, &valid) == ESP_OK &&
        valid == 1) {
      size_t mac_len = sizeof(esp_bd_addr_t);
      if (nvs_get_blob(nvs_handle, BLE_PAIRED_NVS_KEY_MAC, paired_server_mac,
                       &mac_len) == ESP_OK) {
        has_paired_server = true;
        ESP_LOGI(GATTC_TAG, "Loaded paired server MAC: " BT_BD_ADDR_STR,
                 BT_BD_ADDR_HEX(paired_server_mac));
      }
    }
    nvs_close(nvs_handle);
  }
}

static esp_err_t ble_paired_save_mac(esp_bd_addr_t mac) {
  nvs_handle_t nvs_handle;
  esp_err_t err =
      nvs_open(BLE_PAIRED_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(GATTC_TAG, "Failed to open NVS for paired MAC: %s",
             esp_err_to_name(err));
    return err;
  }

  err = nvs_set_blob(nvs_handle, BLE_PAIRED_NVS_KEY_MAC, mac,
                     sizeof(esp_bd_addr_t));
  if (err == ESP_OK) {
    uint8_t valid = 1;
    err = nvs_set_u8(nvs_handle, BLE_PAIRED_NVS_KEY_VALID, valid);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }

  if (err == ESP_OK) {
    memcpy(paired_server_mac, mac, sizeof(esp_bd_addr_t));
    has_paired_server = true;
    ESP_LOGI(GATTC_TAG, "Saved paired server MAC: " BT_BD_ADDR_STR,
             BT_BD_ADDR_HEX(mac));
  } else {
    ESP_LOGE(GATTC_TAG, "Failed to save paired MAC: %s", esp_err_to_name(err));
  }

  nvs_close(nvs_handle);
  return err;
}

esp_err_t ble_clear_paired_device(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err =
      nvs_open(BLE_PAIRED_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  uint8_t valid = 0;
  err = nvs_set_u8(nvs_handle, BLE_PAIRED_NVS_KEY_VALID, valid);
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }

  if (err == ESP_OK) {
    memset(paired_server_mac, 0, sizeof(esp_bd_addr_t));
    has_paired_server = false;
    ESP_LOGI(GATTC_TAG, "Cleared paired server MAC");
  }

  nvs_close(nvs_handle);
  return err;
}

bool ble_has_paired_device(void) { return has_paired_server; }

static void reconnect_timeout_callback(TimerHandle_t xTimer) {
  // Don't call BLE APIs directly from timer callback (limited stack)
  // Instead, notify the reconnect handler task
  if (reconnect_task_handle != NULL) {
    xTaskNotifyGive(reconnect_task_handle);
  }
}

static void reconnect_handler_task(void *pvParameters) {
  while (1) {
    // Wait for notification from timer callback
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (searching_for_paired && !ble_is_connected()) {
      ESP_LOGW(GATTC_TAG,
               "Paired server not found after %d seconds, scanning for any %s "
               "device...",
               RECONNECT_TIMEOUT_MS / 1000, device_name);
      searching_for_paired = false;
      // Stop current scan - will restart in SCAN_STOP_COMPLETE_EVT handler
      pending_scan_restart = true;
      esp_ble_gap_stop_scanning();
    }
  }
}

static void start_reconnect_timer(void) {
  if (reconnect_timer == NULL) {
    reconnect_timer =
        xTimerCreate("reconnect_timer", pdMS_TO_TICKS(RECONNECT_TIMEOUT_MS),
                     pdFALSE, // One-shot timer
                     NULL, reconnect_timeout_callback);
  }
  if (reconnect_timer != NULL) {
    xTimerStart(reconnect_timer, 0);
    reconnect_start_time = esp_timer_get_time() / 1000; // Convert to ms
    ESP_LOGI(GATTC_TAG, "Started reconnect timer (%d seconds)",
             RECONNECT_TIMEOUT_MS / 1000);
  }
}

static void stop_reconnect_timer(void) {
  if (reconnect_timer != NULL) {
    xTimerStop(reconnect_timer, 0);
  }
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
  if (db == NULL) {
    ESP_LOGE(GATTC_TAG, " %s db is NULL", __func__);
    return;
  }

  if (handle == db[SPP_IDX_SPP_DATA_NTY_VAL].attribute_handle) {
    if (p_data->notify.value_len ==
        60) { // Combined VESC + BMS + motor config data
      // First process VESC data (first 14 bytes)
      // All values are little-endian (LSB first, MSB second)
      // temp_mos (bytes 0-1)
      int16_t temp_mos =
          p_data->notify.value[0] | ((int16_t)p_data->notify.value[1] << 8);
      latest_temp_mos = temp_mos / 100.0f;

      // temp_motor (bytes 2-3)
      int16_t temp_motor =
          p_data->notify.value[2] | ((int16_t)p_data->notify.value[3] << 8);
      latest_temp_motor = temp_motor / 100.0f;

      // current_motor (bytes 4-5)
      int16_t current_motor =
          p_data->notify.value[4] | ((int16_t)p_data->notify.value[5] << 8);
      latest_current_motor = current_motor / 100.0f;

      // current_in (bytes 6-7)
      int16_t current_in =
          p_data->notify.value[6] | ((int16_t)p_data->notify.value[7] << 8);
      latest_current_in = current_in / 100.0f;

      // rpm (bytes 8-11) - little-endian
      int32_t rpm_raw = ((int32_t)p_data->notify.value[8]) |
                        ((int32_t)p_data->notify.value[9] << 8) |
                        ((int32_t)p_data->notify.value[10] << 16) |
                        ((int32_t)p_data->notify.value[11] << 24);

      latest_erpm = rpm_raw;

      // voltage (bytes 12-13)
      int16_t voltage =
          p_data->notify.value[12] | ((int16_t)p_data->notify.value[13] << 8);
      latest_voltage = voltage / 100.0f;

      // total_voltage (bytes 14-15)
      int16_t total_voltage =
          p_data->notify.value[14] | ((int16_t)p_data->notify.value[15] << 8);
      bms_total_voltage = total_voltage / 100.0f;

      // current (bytes 16-17)
      int16_t bms_current_raw =
          p_data->notify.value[16] | ((int16_t)p_data->notify.value[17] << 8);
      bms_current = bms_current_raw / 100.0f;

      // remaining_capacity (bytes 18-19)
      int16_t remaining_cap =
          p_data->notify.value[18] | ((int16_t)p_data->notify.value[19] << 8);
      bms_remaining_capacity = remaining_cap / 100.0f;

      // nominal_capacity (bytes 20-21)
      int16_t nominal_cap =
          p_data->notify.value[20] | ((int16_t)p_data->notify.value[21] << 8);
      bms_nominal_capacity = nominal_cap / 100.0f;

      // num_cells (byte 22)
      bms_num_cells = p_data->notify.value[22];

      // cell_voltages (bytes 23-54, 16 cells * 2 bytes each) - little-endian
      for (int i = 0; i < bms_num_cells && i < 16; i++) {
        int16_t cell_voltage =
            p_data->notify.value[23 + i * 2] |
            ((int16_t)p_data->notify.value[23 + i * 2 + 1] << 8);
        bms_cell_voltages[i] = cell_voltage / 1000.0f; // Convert to volts
      }

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

      ESP_LOGI(GATTC_TAG, "Combined Data Received:");
      ESP_LOGI(GATTC_TAG,
               "VESC: V=%.2fV, RPM=%ld, Motor=%.2fA, In=%.2fA, TempMos=%.2f°C, "
               "TempMotor=%.2f°C",
               latest_voltage, latest_erpm, latest_current_motor,
               latest_current_in, latest_temp_mos, latest_temp_motor);
      ESP_LOGI(GATTC_TAG,
               "BMS: Total V=%.2fV, Current=%.2fA, Remaining=%.2fAh, Cells=%d",
               bms_total_voltage, bms_current, bms_remaining_capacity,
               bms_num_cells);
    } else {
      ESP_LOGW(GATTC_TAG, "Unexpected data length: %d (expected 60)",
               p_data->notify.value_len);
    }
  }
}

static void free_gattc_srv_db(void) {
  if (is_connect_mutex != NULL &&
      xSemaphoreTake(is_connect_mutex, portMAX_DELAY) == pdTRUE) {
    is_connect = false;
    xSemaphoreGive(is_connect_mutex);
  } else {
    is_connect = false;
  }
  spp_gattc_if = 0xff;
  spp_conn_id = 0;
  spp_mtu_size = 23;
  cmd = 0;
  spp_srv_start_handle = 0;
  spp_srv_end_handle = 0;
  notify_value_p = NULL;
  notify_value_offset = 0;
  notify_value_count = 0;
  if (db) {
    free(db);
    db = NULL;
  }
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
    // the unit of the duration is second
    uint32_t duration = 0xFFFF;
    ESP_LOGI(GATTC_TAG, "Enable Ble Scan:during time %04" PRIx32 " minutes.",
             duration);
    esp_ble_gap_start_scanning(duration);
    break;
  }
  case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
    // scan start complete event to indicate scan start successfully or failed
    if ((err = param->scan_start_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(GATTC_TAG, "Scan start failed: %s", esp_err_to_name(err));
      break;
    }
    ESP_LOGI(GATTC_TAG, "Scan start successfully");
    break;
  case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
    if ((err = param->scan_stop_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(GATTC_TAG, "Scan stop failed: %s", esp_err_to_name(err));
      pending_scan_restart = false;
      break;
    }
    ESP_LOGI(GATTC_TAG, "Scan stop successfully");
    if (pending_scan_restart) {
      // Restart scan after reconnect timeout (switching from paired-only to any
      // device)
      pending_scan_restart = false;
      ESP_LOGI(GATTC_TAG, "Restarting scan for any device...");
      esp_ble_gap_start_scanning(SCAN_ALL_THE_TIME);
    } else if (!ble_is_connected()) {
      ESP_LOGI(GATTC_TAG, "Connect to the remote device.");
      esp_ble_gattc_open(gl_profile_tab[PROFILE_APP_ID].gattc_if,
                         scan_rst.scan_rst.bda, scan_rst.scan_rst.ble_addr_type,
                         true);
    }
    break;
  case ESP_GAP_BLE_SCAN_RESULT_EVT: {
    esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
    switch (scan_result->scan_rst.search_evt) {
    case ESP_GAP_SEARCH_INQ_RES_EVT:
      adv_name =
          esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
                                   ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);

      // Check if device name matches
      if (adv_name != NULL &&
          strncmp((char *)adv_name, device_name, adv_name_len) == 0) {

        if (searching_for_paired && has_paired_server) {
          // Looking for specific paired server - check MAC address
          if (memcmp(scan_result->scan_rst.bda, paired_server_mac,
                     sizeof(esp_bd_addr_t)) == 0) {
            ESP_LOGI(GATTC_TAG,
                     "Found paired server " BT_BD_ADDR_STR ", RSSI: %d",
                     BT_BD_ADDR_HEX(scan_result->scan_rst.bda),
                     scan_result->scan_rst.rssi);
            stop_reconnect_timer();
            memcpy(&scan_rst, scan_result, sizeof(esp_ble_gap_cb_param_t));
            esp_ble_gap_stop_scanning();
          } else {
            // Found a different GS-THUMB device, ignore during paired search
            ESP_LOGD(GATTC_TAG,
                     "Found non-paired %s device " BT_BD_ADDR_STR
                     ", ignoring...",
                     device_name, BT_BD_ADDR_HEX(scan_result->scan_rst.bda));
          }
        } else {
          // Open search - connect to any matching device
          ESP_LOGI(GATTC_TAG, "Found device %s (" BT_BD_ADDR_STR "), RSSI: %d",
                   device_name, BT_BD_ADDR_HEX(scan_result->scan_rst.bda),
                   scan_result->scan_rst.rssi);
          memcpy(&scan_rst, scan_result, sizeof(esp_ble_gap_cb_param_t));
          esp_ble_gap_stop_scanning();
        }
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
    break;
  case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
    if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
      int rssi = param->read_rssi_cmpl.rssi;
      ui_update_connection_quality(rssi);
      // printf("Connection Quality: %d%% (RSSI: %d dBm)\n", connection_quality,
      // rssi);
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

  default:
    break;
  }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                         esp_ble_gattc_cb_param_t *param) {
  ESP_LOGI(GATTC_TAG, "EVT %d, gattc if %d", event, gattc_if);

  /* If event is register event, store the gattc_if for each profile */
  if (event == ESP_GATTC_REG_EVT) {
    if (param->reg.status == ESP_GATT_OK) {
      gl_profile_tab[param->reg.app_id].gattc_if = gattc_if;
    } else {
      ESP_LOGI(GATTC_TAG, "Reg app failed, app_id %04x, status %d",
               param->reg.app_id, param->reg.status);
      return;
    }
  }
  /* If the gattc_if equal to profile A, call profile A cb handler,
   * so here call each profile's callback */
  do {
    int idx;
    for (idx = 0; idx < PROFILE_NUM; idx++) {
      if (gattc_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a
                                             certain gatt_if, need to call every
                                             profile cb function */
          gattc_if == gl_profile_tab[idx].gattc_if) {
        if (gl_profile_tab[idx].gattc_cb) {
          gl_profile_tab[idx].gattc_cb(event, gattc_if, param);
        }
      }
    }
  } while (0);
}

static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
  esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

  switch (event) {
  case ESP_GATTC_REG_EVT:
    ESP_LOGI(GATTC_TAG, "REG EVT, set scan params");

    // Check if we have a paired server to reconnect to
    if (has_paired_server) {
      ESP_LOGI(GATTC_TAG,
               "Starting search for paired server " BT_BD_ADDR_STR "...",
               BT_BD_ADDR_HEX(paired_server_mac));
      searching_for_paired = true;
      start_reconnect_timer();
    } else {
      ESP_LOGI(GATTC_TAG, "No paired server, scanning for any %s device...",
               device_name);
      searching_for_paired = false;
    }

    esp_ble_gap_set_scan_params(&ble_scan_params);
    break;
  case ESP_GATTC_CONNECT_EVT:
    ESP_LOGI(GATTC_TAG, "ESP_GATTC_CONNECT_EVT: conn_id=%d, gatt_if = %d",
             spp_conn_id, gattc_if);
    ESP_LOGI(GATTC_TAG, "REMOTE BDA: " BT_BD_ADDR_STR,
             BT_BD_ADDR_HEX(p_data->connect.remote_bda));
    spp_gattc_if = gattc_if;
    if (is_connect_mutex != NULL &&
        xSemaphoreTake(is_connect_mutex, portMAX_DELAY) == pdTRUE) {
      is_connect = true;
      xSemaphoreGive(is_connect_mutex);
    } else {
      is_connect = true;
    }
    spp_conn_id = p_data->connect.conn_id;
    memcpy(gl_profile_tab[PROFILE_APP_ID].remote_bda,
           p_data->connect.remote_bda, sizeof(esp_bd_addr_t));

    // Stop reconnect timer if running
    stop_reconnect_timer();
    searching_for_paired = false;

    // Save server MAC if this is a new device or different from stored
    if (!has_paired_server ||
        memcmp(paired_server_mac, p_data->connect.remote_bda,
               sizeof(esp_bd_addr_t)) != 0) {
      ble_paired_save_mac(p_data->connect.remote_bda);
    }

    // Initiate encryption/pairing
    ESP_LOGI(GATTC_TAG, "Initiating BLE encryption...");
    esp_ble_set_encryption(p_data->connect.remote_bda,
                           ESP_BLE_SEC_ENCRYPT_MITM);

    esp_ble_gattc_search_service(spp_gattc_if, spp_conn_id, &spp_service_uuid);

    // Send serial notification for config tool
    printf("#>DATA ble_status=connected\n");
    break;
  case ESP_GATTC_DISCONNECT_EVT:
    ESP_LOGI(GATTC_TAG, "disconnect");

    // Send serial notification for config tool
    printf("#>DATA ble_status=disconnected\n");

    // Reset speed and battery values to 0 when disconnected
    latest_erpm = 0;
    latest_voltage = 0.0f;
    latest_current_motor = 0.0f;
    latest_current_in = 0.0f;
    latest_temp_mos = 0.0f;
    latest_temp_motor = 0.0f;

    // Reset BMS battery values to 0
    bms_total_voltage = 0.0f;
    bms_current = 0.0f;
    bms_remaining_capacity = 0.0f;
    bms_nominal_capacity = 0.0f;
    bms_num_cells = 0;

    // Clear cell voltages array
    memset(bms_cell_voltages, 0, sizeof(bms_cell_voltages));

    ESP_LOGI(GATTC_TAG,
             "Speed and battery values reset to 0 due to disconnection");

    if (db != NULL &&
        ((db + SPP_IDX_SPP_DATA_RECV_VAL)->properties &
         (ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE))) {
      uint8_t neutral_buffer[3] = {VESC_NEUTRAL_VALUE & 0xFF,
                                   (VESC_NEUTRAL_VALUE >> 8) & 0xFF, 0};
      esp_err_t ret = esp_ble_gattc_write_char(
          spp_gattc_if, spp_conn_id,
          (db + SPP_IDX_SPP_DATA_RECV_VAL)->attribute_handle,
          sizeof(neutral_buffer), neutral_buffer, ESP_GATT_WRITE_TYPE_NO_RSP,
          ESP_GATT_AUTH_REQ_NONE);
      if (ret != ESP_OK) {
        ESP_LOGW(GATTC_TAG, "Failed to send neutral value on disconnect: %s",
                 esp_err_to_name(ret));
      }
    }
    ui_update_speed(0);
    ui_update_skate_battery_percentage(0);

    free_gattc_srv_db();

    // Restart scanning - try paired server first if available
    if (has_paired_server) {
      ESP_LOGI(GATTC_TAG, "Reconnecting to paired server " BT_BD_ADDR_STR "...",
               BT_BD_ADDR_HEX(paired_server_mac));
      searching_for_paired = true;
      start_reconnect_timer();
    } else {
      searching_for_paired = false;
    }
    esp_ble_gap_start_scanning(SCAN_ALL_THE_TIME);
    break;
  case ESP_GATTC_SEARCH_RES_EVT:
    ESP_LOGI(GATTC_TAG,
             "ESP_GATTC_SEARCH_RES_EVT: start_handle = %d, end_handle = %d, "
             "UUID:0x%04x",
             p_data->search_res.start_handle, p_data->search_res.end_handle,
             p_data->search_res.srvc_id.uuid.uuid.uuid16);
    spp_srv_start_handle = p_data->search_res.start_handle;
    spp_srv_end_handle = p_data->search_res.end_handle;
    break;
  case ESP_GATTC_SEARCH_CMPL_EVT:
    ESP_LOGI(GATTC_TAG, "SEARCH_CMPL: conn_id = %x, status %d", spp_conn_id,
             p_data->search_cmpl.status);
    esp_ble_gattc_send_mtu_req(gattc_if, spp_conn_id);
    break;
  case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
    ESP_LOGI(GATTC_TAG, "Index = %d,status = %d,handle = %d", cmd,
             p_data->reg_for_notify.status, p_data->reg_for_notify.handle);
    if (p_data->reg_for_notify.status != ESP_GATT_OK) {
      ESP_LOGE(GATTC_TAG, "ESP_GATTC_REG_FOR_NOTIFY_EVT, status = %d",
               p_data->reg_for_notify.status);
      break;
    }
    uint16_t notify_en = 1;
    esp_ble_gattc_write_char_descr(
        spp_gattc_if, spp_conn_id, (db + cmd + 1)->attribute_handle,
        sizeof(notify_en), (uint8_t *)&notify_en, ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE);

    break;
  }
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
      break;
    }
    switch (cmd) {
    case SPP_IDX_SPP_DATA_NTY_VAL:
      cmd = SPP_IDX_SPP_STATUS_VAL;
      xQueueSend(cmd_reg_queue, &cmd, 10 / portTICK_PERIOD_MS);
      break;
    case SPP_IDX_SPP_STATUS_VAL:
#ifdef SUPPORT_HEARTBEAT
      cmd = SPP_IDX_SPP_HEARTBEAT_VAL;
      xQueueSend(cmd_reg_queue, &cmd, 10 / portTICK_PERIOD_MS);
#endif
      break;
#ifdef SUPPORT_HEARTBEAT
    case SPP_IDX_SPP_HEARTBEAT_VAL:
      xQueueSend(cmd_heartbeat_queue, &cmd, 10 / portTICK_PERIOD_MS);
      break;
#endif
    default:
      break;
    };
    break;
  case ESP_GATTC_CFG_MTU_EVT:
    if (p_data->cfg_mtu.status != ESP_OK) {
      break;
    }
    ESP_LOGI(GATTC_TAG, "+MTU:%d", p_data->cfg_mtu.mtu);
    spp_mtu_size = p_data->cfg_mtu.mtu;

    // Free existing db if any (prevent memory leak on MTU reconfig)
    if (db != NULL) {
      free(db);
      db = NULL;
    }

    db = (esp_gattc_db_elem_t *)malloc(count * sizeof(esp_gattc_db_elem_t));
    if (db == NULL) {
      ESP_LOGE(GATTC_TAG, "%s:malloc db failed", __func__);
      break;
    }
    if (esp_ble_gattc_get_db(spp_gattc_if, spp_conn_id, spp_srv_start_handle,
                             spp_srv_end_handle, db, &count) != ESP_GATT_OK) {
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
      switch ((db + i)->type) {
      case ESP_GATT_DB_PRIMARY_SERVICE:
        ESP_LOGI(GATTC_TAG,
                 "attr_type = "
                 "PRIMARY_SERVICE,attribute_handle=%d,start_handle=%d,end_"
                 "handle=%d,properties=0x%x,uuid=0x%04x",
                 (db + i)->attribute_handle, (db + i)->start_handle,
                 (db + i)->end_handle, (db + i)->properties,
                 (db + i)->uuid.uuid.uuid16);
        break;
      case ESP_GATT_DB_SECONDARY_SERVICE:
        ESP_LOGI(GATTC_TAG,
                 "attr_type = "
                 "SECONDARY_SERVICE,attribute_handle=%d,start_handle=%d,end_"
                 "handle=%d,properties=0x%x,uuid=0x%04x",
                 (db + i)->attribute_handle, (db + i)->start_handle,
                 (db + i)->end_handle, (db + i)->properties,
                 (db + i)->uuid.uuid.uuid16);
        break;
      case ESP_GATT_DB_CHARACTERISTIC:
        ESP_LOGI(GATTC_TAG,
                 "attr_type = "
                 "CHARACTERISTIC,attribute_handle=%d,start_handle=%d,end_"
                 "handle=%d,properties=0x%x,uuid=0x%04x",
                 (db + i)->attribute_handle, (db + i)->start_handle,
                 (db + i)->end_handle, (db + i)->properties,
                 (db + i)->uuid.uuid.uuid16);
        break;
      case ESP_GATT_DB_DESCRIPTOR:
        ESP_LOGI(GATTC_TAG,
                 "attr_type = "
                 "DESCRIPTOR,attribute_handle=%d,start_handle=%d,end_handle=%d,"
                 "properties=0x%x,uuid=0x%04x",
                 (db + i)->attribute_handle, (db + i)->start_handle,
                 (db + i)->end_handle, (db + i)->properties,
                 (db + i)->uuid.uuid.uuid16);
        break;
      case ESP_GATT_DB_INCLUDED_SERVICE:
        ESP_LOGI(GATTC_TAG,
                 "attr_type = "
                 "INCLUDED_SERVICE,attribute_handle=%d,start_handle=%d,end_"
                 "handle=%d,properties=0x%x,uuid=0x%04x",
                 (db + i)->attribute_handle, (db + i)->start_handle,
                 (db + i)->end_handle, (db + i)->properties,
                 (db + i)->uuid.uuid.uuid16);
        break;
      case ESP_GATT_DB_ALL:
        ESP_LOGI(GATTC_TAG,
                 "attr_type = "
                 "ESP_GATT_DB_ALL,attribute_handle=%d,start_handle=%d,end_"
                 "handle=%d,properties=0x%x,uuid=0x%04x",
                 (db + i)->attribute_handle, (db + i)->start_handle,
                 (db + i)->end_handle, (db + i)->properties,
                 (db + i)->uuid.uuid.uuid16);
        break;
      default:
        break;
      }
    }
    cmd = SPP_IDX_SPP_DATA_NTY_VAL;
    xQueueSend(cmd_reg_queue, &cmd, 10 / portTICK_PERIOD_MS);
    break;
  case ESP_GATTC_SRVC_CHG_EVT:
    break;
  default:
    break;
  }
}

void spp_client_reg_task(void *arg) {
  uint16_t cmd_id;
  for (;;) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if (xQueueReceive(cmd_reg_queue, &cmd_id, portMAX_DELAY)) {
      if (db != NULL) {
        if (cmd_id == SPP_IDX_SPP_DATA_NTY_VAL) {
          ESP_LOGI(GATTC_TAG, "Index = %d,UUID = 0x%04x, handle = %d", cmd_id,
                   (db + SPP_IDX_SPP_DATA_NTY_VAL)->uuid.uuid.uuid16,
                   (db + SPP_IDX_SPP_DATA_NTY_VAL)->attribute_handle);
          esp_ble_gattc_register_for_notify(
              spp_gattc_if, gl_profile_tab[PROFILE_APP_ID].remote_bda,
              (db + SPP_IDX_SPP_DATA_NTY_VAL)->attribute_handle);
        } else if (cmd_id == SPP_IDX_SPP_STATUS_VAL) {
          ESP_LOGI(GATTC_TAG, "Index = %d,UUID = 0x%04x, handle = %d", cmd_id,
                   (db + SPP_IDX_SPP_STATUS_VAL)->uuid.uuid.uuid16,
                   (db + SPP_IDX_SPP_STATUS_VAL)->attribute_handle);
          esp_ble_gattc_register_for_notify(
              spp_gattc_if, gl_profile_tab[PROFILE_APP_ID].remote_bda,
              (db + SPP_IDX_SPP_STATUS_VAL)->attribute_handle);
        }
#ifdef SUPPORT_HEARTBEAT
        else if (cmd_id == SPP_IDX_SPP_HEARTBEAT_VAL) {
          ESP_LOGI(GATTC_TAG, "Index = %d,UUID = 0x%04x, handle = %d", cmd_id,
                   (db + SPP_IDX_SPP_HEARTBEAT_VAL)->uuid.uuid.uuid16,
                   (db + SPP_IDX_SPP_HEARTBEAT_VAL)->attribute_handle);
          esp_ble_gattc_register_for_notify(
              spp_gattc_if, gl_profile_tab[PROFILE_APP_ID].remote_bda,
              (db + SPP_IDX_SPP_HEARTBEAT_VAL)->attribute_handle);
        }
#endif
      }
    }
  }
}

#ifdef SUPPORT_HEARTBEAT
void spp_heart_beat_task(void *arg) {
  // Register with task watchdog
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  uint16_t cmd_id;

  for (;;) {
    // Reset watchdog before delay
    esp_task_wdt_reset();
    vTaskDelay(50 / portTICK_PERIOD_MS);
    if (xQueueReceive(cmd_heartbeat_queue, &cmd_id, portMAX_DELAY)) {
      // Reset watchdog after receiving command
      esp_task_wdt_reset();
      while (1) {
        if (ble_is_connected() && (db != NULL) &&
            ((db + SPP_IDX_SPP_HEARTBEAT_VAL)->properties &
             (ESP_GATT_CHAR_PROP_BIT_WRITE_NR |
              ESP_GATT_CHAR_PROP_BIT_WRITE))) {
          esp_ble_gattc_write_char(
              spp_gattc_if, spp_conn_id,
              (db + SPP_IDX_SPP_HEARTBEAT_VAL)->attribute_handle,
              sizeof(heartbeat_s), (uint8_t *)heartbeat_s,
              ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
          // Reset watchdog before long delay
          esp_task_wdt_reset();
          vTaskDelay(5000 / portTICK_PERIOD_MS);
        } else {
          ESP_LOGI(GATTC_TAG, "disconnect");
          break;
        }
      }
    }
  }
}
#endif

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

  // register the scan callback function to the gap module
  if ((status = esp_ble_gap_register_callback(esp_gap_cb)) != ESP_OK) {
    ESP_LOGE(GATTC_TAG, "gap register error: %s",
             esp_err_to_name_r(status, err_msg, sizeof(err_msg)));
    return;
  }
  // register the callback function to the gattc module
  if ((status = esp_ble_gattc_register_callback(esp_gattc_cb)) != ESP_OK) {
    ESP_LOGE(GATTC_TAG, "gattc register error: %s",
             esp_err_to_name_r(status, err_msg, sizeof(err_msg)));
    return;
  }
  esp_ble_gattc_app_register(PROFILE_APP_ID);

  esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(200);
  if (local_mtu_ret) {
    ESP_LOGE(GATTC_TAG, "set local  MTU failed: %s",
             esp_err_to_name_r(local_mtu_ret, err_msg, sizeof(err_msg)));
  }

  // Configure BLE Security
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

  cmd_reg_queue = xQueueCreate(10, sizeof(uint32_t));
  xTaskCreate(spp_client_reg_task, "spp_client_reg_task", 2048, NULL, 10, NULL);

  // Create reconnect handler task (handles BLE operations that would overflow
  // timer task stack)
  xTaskCreate(reconnect_handler_task, "reconnect_handler", 3072, NULL, 5,
              &reconnect_task_handle);

#ifdef SUPPORT_HEARTBEAT
  cmd_heartbeat_queue = xQueueCreate(10, sizeof(uint32_t));
  xTaskCreate(spp_heart_beat_task, "spp_heart_beat_task", 2048, NULL, 10, NULL);
#endif
}

void uart_task(void *pvParameters) {
  uart_event_t event;
  for (;;) {
    // Waiting for UART event.
    if (xQueueReceive(spp_uart_queue, (void *)&event,
                      (TickType_t)portMAX_DELAY)) {
      switch (event.type) {
      // Event of UART receiving data
      case UART_DATA:
        if (event.size && ble_is_connected() && (db != NULL) &&
            ((db + SPP_IDX_SPP_DATA_RECV_VAL)->properties &
             (ESP_GATT_CHAR_PROP_BIT_WRITE_NR |
              ESP_GATT_CHAR_PROP_BIT_WRITE))) {
          uint8_t *temp = NULL;
          temp = (uint8_t *)malloc(sizeof(uint8_t) * event.size);
          if (temp == NULL) {
            ESP_LOGE(GATTC_TAG, "malloc failed,%s L#%d", __func__, __LINE__);
            break;
          }
          memset(temp, 0x0, event.size);
          uart_read_bytes(UART_NUM_0, temp, event.size, portMAX_DELAY);
          esp_ble_gattc_write_char(
              spp_gattc_if, spp_conn_id,
              (db + SPP_IDX_SPP_DATA_RECV_VAL)->attribute_handle, event.size,
              temp, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
          free(temp);
        }
        break;
      default:
        break;
      }
    }
  }
}

static void spp_uart_init(void) {
  uart_config_t uart_config = {
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_RTS,
      .rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_DEFAULT,
  };

  // Install UART driver, and get the queue.
  esp_err_t ret =
      uart_driver_install(UART_NUM_0, 4096, 8192, 10, &spp_uart_queue, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(GATTC_TAG, "Failed to install UART driver: %s",
             esp_err_to_name(ret));
    return;
  }

  // Set UART parameters
  ret = uart_param_config(UART_NUM_0, &uart_config);
  if (ret != ESP_OK) {
    ESP_LOGE(GATTC_TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
    return;
  }

  // Set UART pins
  ret = uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (ret != ESP_OK) {
    ESP_LOGE(GATTC_TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
    return;
  }

  ESP_LOGI(GATTC_TAG,
           "UART initialized successfully for BLE data transmission");
}

void spp_client_demo_init(void) {
  esp_err_t ret;

  esp_log_level_set(GATTC_TAG, ESP_LOG_WARN);

  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

  nvs_flash_init();

  // Load aux output state from NVS
  aux_output_load_state();

  // Load BLE trim offset from NVS
  ble_trim_load_offset();

  // Load paired server MAC from NVS
  ble_paired_load_mac();

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
  spp_uart_init();
  xTaskCreate(adc_send_task, "adc_send_task", 4096, NULL, 8, NULL);
  xTaskCreate(log_rssi_task, "log_rssi_task", 2048, NULL, 4, NULL);
}

static void adc_send_task(void *pvParameters) {
  // Register with task watchdog
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  uint8_t data_buffer[3]; // 2 bytes for ADC value + 1 byte for aux output

  while (1) {
    if (ble_is_connected() && db != NULL &&
        ((db + SPP_IDX_SPP_DATA_RECV_VAL)->properties &
         (ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE))) {

      // Capture handle once; db can be set to NULL by disconnect callback
      // before we use it
      uint16_t data_handle = (db + SPP_IDX_SPP_DATA_RECV_VAL)->attribute_handle;

      uint32_t adc_value;
      bool throttle_inverted = false;

      // SAFETY: Block throttle on low battery - force neutral
      if (battery_is_low_voltage()) {
        adc_value = VESC_NEUTRAL_VALUE;
        ESP_LOGW(GATTC_TAG, "Low battery - throttle blocked, sending neutral");
      } else {
#ifdef CONFIG_TARGET_DUAL_THROTTLE
        adc_value = get_throttle_brake_ble_value();
#elif defined(CONFIG_TARGET_LITE)
        if (throttle_should_use_neutral()) {
          adc_value = VESC_NEUTRAL_VALUE;
        } else {
          adc_value = adc_get_latest_value();
        }

        vesc_config_t config;
        esp_err_t err = vesc_config_load(&config);

        if (err == ESP_OK) {
          if (config.invert_throttle) {
            adc_value = 255 - adc_value;
            throttle_inverted = true;
          }
        }
#endif
      }

      uint8_t final_ble_value;
      int8_t effective_trim =
          throttle_inverted ? -ble_trim_offset : ble_trim_offset;
      int32_t new_center = VESC_NEUTRAL_VALUE + effective_trim;

      if (new_center < 0)
        new_center = 0;
      if (new_center > 255)
        new_center = 255;

      if (adc_value <= VESC_NEUTRAL_VALUE) {
        if (VESC_NEUTRAL_VALUE > 0) {
          int32_t scaled = (int32_t)((float)adc_value * (float)new_center /
                                         (float)VESC_NEUTRAL_VALUE +
                                     0.5f);
          final_ble_value =
              (uint8_t)(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
        } else {
          final_ble_value = 0;
        }
      } else {
        int32_t upper_output_range = 255 - new_center;
        int32_t upper_input_range = 255 - VESC_NEUTRAL_VALUE;
        if (upper_input_range > 0) {
          int32_t scaled =
              new_center + (int32_t)((float)(adc_value - VESC_NEUTRAL_VALUE) *
                                         (float)upper_output_range /
                                         (float)upper_input_range +
                                     0.5f);
          final_ble_value =
              (uint8_t)(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
        } else {
          final_ble_value = (uint8_t)new_center;
        }
      }

      data_buffer[0] = (uint8_t)(final_ble_value & 0xFF);
      data_buffer[1] = (uint8_t)((final_ble_value >> 8) & 0xFF);
      data_buffer[2] = aux_output_state ? 1 : 0;

      if (db != NULL) {
        esp_err_t ret = esp_ble_gattc_write_char(
            spp_gattc_if, spp_conn_id, data_handle,
            sizeof(data_buffer), // 3 bytes
            data_buffer, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        if (ret != ESP_OK) {
          ESP_LOGW(GATTC_TAG, "Failed to send throttle value: %s",
                   esp_err_to_name(ret));
        }
      }

      // Reset watchdog before delay
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(ADC_SEND_INTERVAL_MS));
    }
  }

  float get_latest_voltage(void) { return latest_voltage; }

  int32_t get_latest_erpm(void) { return latest_erpm; }

  float get_latest_current_motor(void) { return latest_current_motor; }

  float get_latest_current_in(void) { return latest_current_in; }

  float get_bms_total_voltage(void) { return bms_total_voltage; }

  float get_bms_current(void) { return bms_current; }

  float get_bms_remaining_capacity(void) { return bms_remaining_capacity; }

  float get_bms_nominal_capacity(void) { return bms_nominal_capacity; }

  uint8_t get_bms_num_cells(void) { return bms_num_cells; }

  float get_bms_cell_voltage(uint8_t cell_index) {
    if (cell_index < bms_num_cells && cell_index < 16) {
      return bms_cell_voltages[cell_index];
    }
    return 0.0f;
  }

  static void log_rssi_task(void *pvParameters) {
    while (1) {
      if (ble_is_connected() && spp_gattc_if != 0xff) {
        esp_err_t ret = esp_ble_gap_read_rssi(scan_rst.scan_rst.bda);
        if (ret != ESP_OK) {
          ESP_LOGE(GATTC_TAG, "Read RSSI failed: %s", esp_err_to_name(ret));
        }
      }
      vTaskDelay(pdMS_TO_TICKS(RSSI_READ_INTERVAL_MS));
    }
  }

  int get_bms_battery_percentage(void) {
    if (bms_nominal_capacity <= 0.0f)
      return -1;

    float percentage = (bms_remaining_capacity / bms_nominal_capacity) * 100.0f;

    if (percentage > 100.0f)
      percentage = 100.0f;
    if (percentage < 0.0f)
      percentage = 0.0f;

    return (int)percentage;
  }

  bool ble_is_connected(void) {
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
