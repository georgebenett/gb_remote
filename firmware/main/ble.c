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

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "driver/uart.h"

#include "esp_bt.h"
#include "nvs_flash.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_system.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "target_config.h"
#include "throttle.h"
#include "level_assistant.h"
#include "ui_updater.h"
#include "vesc_config.h"
#include "ble.h"
#define DEVICE_NAME                 "GS-THUMB"
#define GATTC_TAG                   "GATTC_SPP_DEMO"

#define PROFILE_NUM                 1
#define PROFILE_APP_ID              0
#define BT_BD_ADDR_STR              "%02x:%02x:%02x:%02x:%02x:%02x"
#define BT_BD_ADDR_HEX(addr)        addr[0],addr[1],addr[2],addr[3],addr[4],addr[5]
#define ESP_GATT_SPP_SERVICE_UUID   0xABF0
#define SCAN_ALL_THE_TIME           0

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

///Declare static functions
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void adc_send_task(void *pvParameters);
static void log_rssi_task(void *pvParameters);

/* One gatt-based profile one app_id and one gattc_if, this array will store the gattc_if returned by ESP_GATTS_REG_EVT */
static struct gattc_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_ID] = {
        .gattc_cb = gattc_profile_event_handler,
        .gattc_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
};

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

bool is_connect = false;
static const char device_name[] = DEVICE_NAME;
static uint16_t spp_conn_id = 0;
static uint16_t spp_mtu_size = 23;
static uint16_t cmd = 0;
static uint16_t spp_srv_start_handle = 0;
static uint16_t spp_srv_end_handle = 0;
static uint16_t spp_gattc_if = 0xff;
static char * notify_value_p = NULL;
static int notify_value_offset = 0;
static int notify_value_count = 0;
static uint16_t count = SPP_IDX_NB;
static esp_gattc_db_elem_t *db = NULL;
static esp_ble_gap_cb_param_t scan_rst;
static QueueHandle_t cmd_reg_queue = NULL;
QueueHandle_t spp_uart_queue = NULL;

#ifdef SUPPORT_HEARTBEAT
static uint8_t  heartbeat_s[9] = {'E','s','p','r','e','s','s','i','f'};
static QueueHandle_t cmd_heartbeat_queue = NULL;
#endif

static esp_bt_uuid_t spp_service_uuid = {
    .len  = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = ESP_GATT_SPP_SERVICE_UUID,},
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

float get_latest_temp_mos(void)
{
    return latest_temp_mos;
}

float get_latest_temp_motor(void)
{
    return latest_temp_motor;
}

static void notify_event_handler(esp_ble_gattc_cb_param_t * p_data)
{
    uint8_t handle = 0;

    if(p_data->notify.is_notify == true){
        ESP_LOGI(GATTC_TAG,"+NOTIFY:handle = %d,length = %d ", p_data->notify.handle, p_data->notify.value_len);
    }else{
        ESP_LOGI(GATTC_TAG,"+INDICATE:handle = %d,length = %d ", p_data->notify.handle, p_data->notify.value_len);
    }

    handle = p_data->notify.handle;
    if(db == NULL) {
        ESP_LOGE(GATTC_TAG, " %s db is NULL", __func__);
        return;
    }

    if(handle == db[SPP_IDX_SPP_DATA_NTY_VAL].attribute_handle){
        if(p_data->notify.value_len == 55) {  // Combined VESC (14) + BMS (41) data
            // First process VESC data (first 14 bytes)
            // temp_mos (bytes 0-1)
            int16_t temp_mos = (p_data->notify.value[0] << 8) | p_data->notify.value[1];
            latest_temp_mos = temp_mos / 100.0f;

            // temp_motor (bytes 2-3)
            int16_t temp_motor = (p_data->notify.value[2] << 8) | p_data->notify.value[3];
            latest_temp_motor = temp_motor / 100.0f;

            // current_motor (bytes 4-5)
            int16_t current_motor = (p_data->notify.value[4] << 8) | p_data->notify.value[5];
            latest_current_motor = current_motor / 100.0f;

            // current_in (bytes 6-7)
            int16_t current_in = (p_data->notify.value[6] << 8) | p_data->notify.value[7];
            latest_current_in = current_in / 100.0f;

            // rpm (bytes 8-11)
            int32_t rpm_raw = ((int32_t)p_data->notify.value[8] << 24) |
                             ((int32_t)p_data->notify.value[9] << 16) |
                             ((int32_t)p_data->notify.value[10] << 8) |
                             (int32_t)p_data->notify.value[11];


            latest_erpm = rpm_raw;


            // voltage (bytes 12-13)
            int16_t voltage = (p_data->notify.value[12] << 8) | p_data->notify.value[13];
            latest_voltage = voltage / 100.0f;

            // total_voltage (bytes 14-15)
            int16_t total_voltage = (p_data->notify.value[14] << 8) | p_data->notify.value[15];
            bms_total_voltage = total_voltage / 100.0f;

            // current (bytes 16-17)
            int16_t bms_current_raw = (p_data->notify.value[16] << 8) | p_data->notify.value[17];
            bms_current = bms_current_raw / 100.0f;

            // remaining_capacity (bytes 18-19)
            int16_t remaining_cap = (p_data->notify.value[18] << 8) | p_data->notify.value[19];
            bms_remaining_capacity = remaining_cap / 100.0f;

            // nominal_capacity (bytes 20-21)
            int16_t nominal_cap = (p_data->notify.value[20] << 8) | p_data->notify.value[21];
            bms_nominal_capacity = nominal_cap / 100.0f;

            // num_cells (byte 22)
            bms_num_cells = p_data->notify.value[22];

            // cell_voltages (bytes 23-54, 16 cells * 2 bytes each)
            for(int i = 0; i < bms_num_cells && i < 16; i++) {
                int16_t cell_voltage = (p_data->notify.value[23 + i*2] << 8) | p_data->notify.value[24 + i*2];
                bms_cell_voltages[i] = cell_voltage / 1000.0f;  // Convert to volts
            }

            ESP_LOGI(GATTC_TAG, "Combined Data Received:");
            ESP_LOGI(GATTC_TAG, "VESC: V=%.2fV, RPM=%ld, Motor=%.2fA, In=%.2fA, TempMos=%.2f°C, TempMotor=%.2f°C",
                    latest_voltage, latest_erpm, latest_current_motor, latest_current_in,
                    latest_temp_mos, latest_temp_motor);
            ESP_LOGI(GATTC_TAG, "BMS: Total V=%.2fV, Current=%.2fA, Remaining=%.2fAh, Cells=%d",
                    bms_total_voltage, bms_current, bms_remaining_capacity, bms_num_cells);
        } else {
            ESP_LOGW(GATTC_TAG, "Unexpected data length: %d (expected 55)", p_data->notify.value_len);
        }
    }
}

static void free_gattc_srv_db(void)
{
    is_connect = false;
    spp_gattc_if = 0xff;
    spp_conn_id = 0;
    spp_mtu_size = 23;
    cmd = 0;
    spp_srv_start_handle = 0;
    spp_srv_end_handle = 0;
    notify_value_p = NULL;
    notify_value_offset = 0;
    notify_value_count = 0;
    if(db){
        free(db);
        db = NULL;
    }
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    uint8_t *adv_name = NULL;
    uint8_t adv_name_len = 0;
    esp_err_t err;

    switch(event){
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        if((err = param->scan_param_cmpl.status) != ESP_BT_STATUS_SUCCESS){
            ESP_LOGE(GATTC_TAG, "Scan param set failed: %s", esp_err_to_name(err));
            break;
        }
        //the unit of the duration is second
        uint32_t duration = 0xFFFF;
        ESP_LOGI(GATTC_TAG, "Enable Ble Scan:during time %04" PRIx32 " minutes.",duration);
        esp_ble_gap_start_scanning(duration);
        break;
    }
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        //scan start complete event to indicate scan start successfully or failed
        if ((err = param->scan_start_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTC_TAG, "Scan start failed: %s", esp_err_to_name(err));
            break;
        }
        ESP_LOGI(GATTC_TAG, "Scan start successfully");
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if ((err = param->scan_stop_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTC_TAG, "Scan stop failed: %s", esp_err_to_name(err));
            break;
        }
        ESP_LOGI(GATTC_TAG, "Scan stop successfully");
        if (is_connect == false) {
            ESP_LOGI(GATTC_TAG, "Connect to the remote device.");
            esp_ble_gattc_open(gl_profile_tab[PROFILE_APP_ID].gattc_if, scan_rst.scan_rst.bda, scan_rst.scan_rst.ble_addr_type, true);
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
        switch (scan_result->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT:
            adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);

            // Only print logs if the device name matches
            if (adv_name != NULL && strncmp((char *)adv_name, device_name, adv_name_len) == 0) {
                ESP_LOGI(GATTC_TAG, "Found device %s, RSSI: %d",
                        device_name, scan_result->scan_rst.rssi);
                memcpy(&scan_rst, scan_result, sizeof(esp_ble_gap_cb_param_t));
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
        if ((err = param->adv_stop_cmpl.status) != ESP_BT_STATUS_SUCCESS){
            ESP_LOGE(GATTC_TAG, "Adv stop failed: %s", esp_err_to_name(err));
        }else {
            ESP_LOGI(GATTC_TAG, "Stop adv successfully");
        }
        break;
    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
        if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            int rssi = param->read_rssi_cmpl.rssi;
            ui_update_connection_quality(rssi);
            //printf("Connection Quality: %d%% (RSSI: %d dBm)\n", connection_quality, rssi);
        } else {
            ESP_LOGE(GATTC_TAG, "RSSI read failed: %d", param->read_rssi_cmpl.status);
        }
        break;
    default:
        break;
    }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    ESP_LOGI(GATTC_TAG, "EVT %d, gattc if %d", event, gattc_if);

    /* If event is register event, store the gattc_if for each profile */
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gattc_if = gattc_if;
        } else {
            ESP_LOGI(GATTC_TAG, "Reg app failed, app_id %04x, status %d", param->reg.app_id, param->reg.status);
            return;
        }
    }
    /* If the gattc_if equal to profile A, call profile A cb handler,
     * so here call each profile's callback */
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            if (gattc_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
                    gattc_if == gl_profile_tab[idx].gattc_if) {
                if (gl_profile_tab[idx].gattc_cb) {
                    gl_profile_tab[idx].gattc_cb(event, gattc_if, param);
                }
            }
        }
    } while (0);
}

static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(GATTC_TAG, "REG EVT, set scan params");
        esp_ble_gap_set_scan_params(&ble_scan_params);
        break;
    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_CONNECT_EVT: conn_id=%d, gatt_if = %d", spp_conn_id, gattc_if);
        ESP_LOGI(GATTC_TAG, "REMOTE BDA:");
        esp_log_buffer_hex(GATTC_TAG, gl_profile_tab[PROFILE_APP_ID].remote_bda, sizeof(esp_bd_addr_t));
        spp_gattc_if = gattc_if;
        is_connect = true;
        spp_conn_id = p_data->connect.conn_id;
        memcpy(gl_profile_tab[PROFILE_APP_ID].remote_bda, p_data->connect.remote_bda, sizeof(esp_bd_addr_t));
        esp_ble_gattc_search_service(spp_gattc_if, spp_conn_id, &spp_service_uuid);
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(GATTC_TAG, "disconnect");

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

        ESP_LOGI(GATTC_TAG, "Speed and battery values reset to 0 due to disconnection");

        // Trigger UI updates to show 0 values
        ui_update_speed(0);
        ui_update_skate_battery_percentage(0);

        free_gattc_srv_db();
        esp_ble_gap_start_scanning(SCAN_ALL_THE_TIME);
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_SEARCH_RES_EVT: start_handle = %d, end_handle = %d, UUID:0x%04x",p_data->search_res.start_handle,p_data->search_res.end_handle,p_data->search_res.srvc_id.uuid.uuid.uuid16);
        spp_srv_start_handle = p_data->search_res.start_handle;
        spp_srv_end_handle = p_data->search_res.end_handle;
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGI(GATTC_TAG, "SEARCH_CMPL: conn_id = %x, status %d", spp_conn_id, p_data->search_cmpl.status);
        esp_ble_gattc_send_mtu_req(gattc_if, spp_conn_id);
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        ESP_LOGI(GATTC_TAG,"Index = %d,status = %d,handle = %d",cmd, p_data->reg_for_notify.status, p_data->reg_for_notify.handle);
        if(p_data->reg_for_notify.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG, "ESP_GATTC_REG_FOR_NOTIFY_EVT, status = %d", p_data->reg_for_notify.status);
            break;
        }
        uint16_t notify_en = 1;
        esp_ble_gattc_write_char_descr(
                spp_gattc_if,
                spp_conn_id,
                (db+cmd+1)->attribute_handle,
                sizeof(notify_en),
                (uint8_t *)&notify_en,
                ESP_GATT_WRITE_TYPE_NO_RSP,
                ESP_GATT_AUTH_REQ_NONE);

        break;
    }
    case ESP_GATTC_NOTIFY_EVT:
        ESP_LOGI(GATTC_TAG,"ESP_GATTC_NOTIFY_EVT");
        notify_event_handler(p_data);
        break;
    case ESP_GATTC_READ_CHAR_EVT:
        ESP_LOGI(GATTC_TAG,"ESP_GATTC_READ_CHAR_EVT");
        break;
    case ESP_GATTC_WRITE_CHAR_EVT:
        ESP_LOGI(GATTC_TAG,"ESP_GATTC_WRITE_CHAR_EVT:status = %d,handle = %d", param->write.status, param->write.handle);
        if(param->write.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG, "ESP_GATTC_WRITE_CHAR_EVT, error status = %d", p_data->write.status);
            break;
        }
        break;
    case ESP_GATTC_PREP_WRITE_EVT:
        break;
    case ESP_GATTC_EXEC_EVT:
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        ESP_LOGI(GATTC_TAG,"ESP_GATTC_WRITE_DESCR_EVT: status =%d,handle = %d", p_data->write.status, p_data->write.handle);
        if(p_data->write.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG, "ESP_GATTC_WRITE_DESCR_EVT, error status = %d", p_data->write.status);
            break;
        }
        switch(cmd){
        case SPP_IDX_SPP_DATA_NTY_VAL:
            cmd = SPP_IDX_SPP_STATUS_VAL;
            xQueueSend(cmd_reg_queue, &cmd,10/portTICK_PERIOD_MS);
            break;
        case SPP_IDX_SPP_STATUS_VAL:
#ifdef SUPPORT_HEARTBEAT
            cmd = SPP_IDX_SPP_HEARTBEAT_VAL;
            xQueueSend(cmd_reg_queue, &cmd, 10/portTICK_PERIOD_MS);
#endif
            break;
#ifdef SUPPORT_HEARTBEAT
        case SPP_IDX_SPP_HEARTBEAT_VAL:
            xQueueSend(cmd_heartbeat_queue, &cmd, 10/portTICK_PERIOD_MS);
            break;
#endif
        default:
            break;
        };
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        if(p_data->cfg_mtu.status != ESP_OK){
            break;
        }
        ESP_LOGI(GATTC_TAG,"+MTU:%d", p_data->cfg_mtu.mtu);
        spp_mtu_size = p_data->cfg_mtu.mtu;

        db = (esp_gattc_db_elem_t *)malloc(count*sizeof(esp_gattc_db_elem_t));
        if(db == NULL){
            ESP_LOGE(GATTC_TAG,"%s:malloc db failed",__func__);
            break;
        }
        if(esp_ble_gattc_get_db(spp_gattc_if, spp_conn_id, spp_srv_start_handle, spp_srv_end_handle, db, &count) != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG,"%s:get db failed",__func__);
            break;
        }
        if(count != SPP_IDX_NB){
            ESP_LOGE(GATTC_TAG,"%s:get db count != SPP_IDX_NB, count = %d, SPP_IDX_NB = %d",__func__,count,SPP_IDX_NB);
            break;
        }
        for(int i = 0;i < SPP_IDX_NB;i++){
            switch((db+i)->type){
            case ESP_GATT_DB_PRIMARY_SERVICE:
                ESP_LOGI(GATTC_TAG,"attr_type = PRIMARY_SERVICE,attribute_handle=%d,start_handle=%d,end_handle=%d,properties=0x%x,uuid=0x%04x",\
                        (db+i)->attribute_handle, (db+i)->start_handle, (db+i)->end_handle, (db+i)->properties, (db+i)->uuid.uuid.uuid16);
                break;
            case ESP_GATT_DB_SECONDARY_SERVICE:
                ESP_LOGI(GATTC_TAG,"attr_type = SECONDARY_SERVICE,attribute_handle=%d,start_handle=%d,end_handle=%d,properties=0x%x,uuid=0x%04x",\
                        (db+i)->attribute_handle, (db+i)->start_handle, (db+i)->end_handle, (db+i)->properties, (db+i)->uuid.uuid.uuid16);
                break;
            case ESP_GATT_DB_CHARACTERISTIC:
                ESP_LOGI(GATTC_TAG,"attr_type = CHARACTERISTIC,attribute_handle=%d,start_handle=%d,end_handle=%d,properties=0x%x,uuid=0x%04x",\
                        (db+i)->attribute_handle, (db+i)->start_handle, (db+i)->end_handle, (db+i)->properties, (db+i)->uuid.uuid.uuid16);
                break;
            case ESP_GATT_DB_DESCRIPTOR:
                ESP_LOGI(GATTC_TAG,"attr_type = DESCRIPTOR,attribute_handle=%d,start_handle=%d,end_handle=%d,properties=0x%x,uuid=0x%04x",\
                        (db+i)->attribute_handle, (db+i)->start_handle, (db+i)->end_handle, (db+i)->properties, (db+i)->uuid.uuid.uuid16);
                break;
            case ESP_GATT_DB_INCLUDED_SERVICE:
                ESP_LOGI(GATTC_TAG,"attr_type = INCLUDED_SERVICE,attribute_handle=%d,start_handle=%d,end_handle=%d,properties=0x%x,uuid=0x%04x",\
                        (db+i)->attribute_handle, (db+i)->start_handle, (db+i)->end_handle, (db+i)->properties, (db+i)->uuid.uuid.uuid16);
                break;
            case ESP_GATT_DB_ALL:
                ESP_LOGI(GATTC_TAG,"attr_type = ESP_GATT_DB_ALL,attribute_handle=%d,start_handle=%d,end_handle=%d,properties=0x%x,uuid=0x%04x",\
                        (db+i)->attribute_handle, (db+i)->start_handle, (db+i)->end_handle, (db+i)->properties, (db+i)->uuid.uuid.uuid16);
                break;
            default:
                break;
            }
        }
        cmd = SPP_IDX_SPP_DATA_NTY_VAL;
        xQueueSend(cmd_reg_queue, &cmd, 10/portTICK_PERIOD_MS);
        break;
    case ESP_GATTC_SRVC_CHG_EVT:
        break;
    default:
        break;
    }
}

void spp_client_reg_task(void* arg)
{
    uint16_t cmd_id;
    for(;;) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        if(xQueueReceive(cmd_reg_queue, &cmd_id, portMAX_DELAY)) {
            if(db != NULL) {
                if(cmd_id == SPP_IDX_SPP_DATA_NTY_VAL){
                    ESP_LOGI(GATTC_TAG,"Index = %d,UUID = 0x%04x, handle = %d", cmd_id, (db+SPP_IDX_SPP_DATA_NTY_VAL)->uuid.uuid.uuid16, (db+SPP_IDX_SPP_DATA_NTY_VAL)->attribute_handle);
                    esp_ble_gattc_register_for_notify(spp_gattc_if, gl_profile_tab[PROFILE_APP_ID].remote_bda, (db+SPP_IDX_SPP_DATA_NTY_VAL)->attribute_handle);
                }else if(cmd_id == SPP_IDX_SPP_STATUS_VAL){
                    ESP_LOGI(GATTC_TAG,"Index = %d,UUID = 0x%04x, handle = %d", cmd_id, (db+SPP_IDX_SPP_STATUS_VAL)->uuid.uuid.uuid16, (db+SPP_IDX_SPP_STATUS_VAL)->attribute_handle);
                    esp_ble_gattc_register_for_notify(spp_gattc_if, gl_profile_tab[PROFILE_APP_ID].remote_bda, (db+SPP_IDX_SPP_STATUS_VAL)->attribute_handle);
                }
#ifdef SUPPORT_HEARTBEAT
                else if(cmd_id == SPP_IDX_SPP_HEARTBEAT_VAL){
                    ESP_LOGI(GATTC_TAG,"Index = %d,UUID = 0x%04x, handle = %d", cmd_id, (db+SPP_IDX_SPP_HEARTBEAT_VAL)->uuid.uuid.uuid16, (db+SPP_IDX_SPP_HEARTBEAT_VAL)->attribute_handle);
                    esp_ble_gattc_register_for_notify(spp_gattc_if, gl_profile_tab[PROFILE_APP_ID].remote_bda, (db+SPP_IDX_SPP_HEARTBEAT_VAL)->attribute_handle);
                }
#endif
            }
        }
    }
}

#ifdef SUPPORT_HEARTBEAT
void spp_heart_beat_task(void * arg)
{
    uint16_t cmd_id;

    for(;;) {
        vTaskDelay(50 / portTICK_PERIOD_MS);
        if(xQueueReceive(cmd_heartbeat_queue, &cmd_id, portMAX_DELAY)) {
            while(1){
                if((is_connect == true) && (db != NULL) && ((db+SPP_IDX_SPP_HEARTBEAT_VAL)->properties & (ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE))){
                    esp_ble_gattc_write_char( spp_gattc_if,
                                              spp_conn_id,
                                              (db+SPP_IDX_SPP_HEARTBEAT_VAL)->attribute_handle,
                                              sizeof(heartbeat_s),
                                              (uint8_t *)heartbeat_s,
                                              ESP_GATT_WRITE_TYPE_NO_RSP,
                                              ESP_GATT_AUTH_REQ_NONE);
                    vTaskDelay(5000 / portTICK_PERIOD_MS);
                }else{
                    ESP_LOGI(GATTC_TAG,"disconnect");
                    break;
                }
            }
        }
    }
}
#endif

void ble_client_appRegister(void)
{
    esp_err_t status;
    char err_msg[20];

    ESP_LOGI(GATTC_TAG, "register callback");

    //register the scan callback function to the gap module
    if ((status = esp_ble_gap_register_callback(esp_gap_cb)) != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "gap register error: %s", esp_err_to_name_r(status, err_msg, sizeof(err_msg)));
        return;
    }
    //register the callback function to the gattc module
    if ((status = esp_ble_gattc_register_callback(esp_gattc_cb)) != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "gattc register error: %s", esp_err_to_name_r(status, err_msg, sizeof(err_msg)));
        return;
    }
    esp_ble_gattc_app_register(PROFILE_APP_ID);

    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(200);
    if (local_mtu_ret){
        ESP_LOGE(GATTC_TAG, "set local  MTU failed: %s", esp_err_to_name_r(local_mtu_ret, err_msg, sizeof(err_msg)));
    }

    cmd_reg_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(spp_client_reg_task, "spp_client_reg_task", 2048, NULL, 10, NULL);

#ifdef SUPPORT_HEARTBEAT
    cmd_heartbeat_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(spp_heart_beat_task, "spp_heart_beat_task", 2048, NULL, 10, NULL);
#endif
}

void uart_task(void *pvParameters)
{
    uart_event_t event;
    for (;;) {
        //Waiting for UART event.
        if (xQueueReceive(spp_uart_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
            switch (event.type) {
            //Event of UART receiving data
            case UART_DATA:
                if (event.size && (is_connect == true) && (db != NULL) && ((db+SPP_IDX_SPP_DATA_RECV_VAL)->properties & (ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE))) {
                    uint8_t * temp = NULL;
                    temp = (uint8_t *)malloc(sizeof(uint8_t)*event.size);
                    if(temp == NULL){
                        ESP_LOGE(GATTC_TAG, "malloc failed,%s L#%d", __func__, __LINE__);
                        break;
                    }
                    memset(temp, 0x0, event.size);
                    uart_read_bytes(UART_NUM_0,temp,event.size,portMAX_DELAY);
                    esp_ble_gattc_write_char( spp_gattc_if,
                                              spp_conn_id,
                                              (db+SPP_IDX_SPP_DATA_RECV_VAL)->attribute_handle,
                                              event.size,
                                              temp,
                                              ESP_GATT_WRITE_TYPE_NO_RSP,
                                              ESP_GATT_AUTH_REQ_NONE);
                    free(temp);
                }
                break;
            default:
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

static void spp_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_RTS,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    //Install UART driver, and get the queue.
    esp_err_t ret = uart_driver_install(UART_NUM_0, 4096, 8192, 10, &spp_uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return;
    }

    //Set UART parameters
    ret = uart_param_config(UART_NUM_0, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
        return;
    }

    //Set UART pins
    ret = uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(GATTC_TAG, "UART initialized successfully for BLE data transmission");

}

void spp_client_demo_init(void)
{
    esp_err_t ret;

    esp_log_level_set(GATTC_TAG, ESP_LOG_WARN);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    nvs_flash_init();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(GATTC_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(GATTC_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(GATTC_TAG, "%s init bluetooth", __func__);

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(GATTC_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(GATTC_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ble_client_appRegister();
    spp_uart_init();
    xTaskCreate(adc_send_task, "adc_send_task", 4096, NULL, 8, NULL);
    xTaskCreate(log_rssi_task, "log_rssi_task", 2048, NULL, 4, NULL);
}

static void adc_send_task(void *pvParameters) {
    uint8_t data_buffer[2];  // Just 2 bytes for a 12-bit ADC value

    while (1) {
        if (is_connect && db != NULL &&
            ((db+SPP_IDX_SPP_DATA_RECV_VAL)->properties &
             (ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE))){

            uint32_t adc_value;

#ifdef CONFIG_TARGET_DUAL_THROTTLE
            adc_value = get_throttle_brake_ble_value();

            vesc_config_t config;
            esp_err_t err = vesc_config_load(&config);

            if (err == ESP_OK) {
                int32_t current_erpm = get_latest_erpm();
                adc_value = level_assistant_process(adc_value, current_erpm, config.level_assistant);
            }
#elif defined(CONFIG_TARGET_LITE)
            if (throttle_should_use_neutral()) {
                adc_value = 127;
            } else {
                adc_value = adc_get_latest_value();
            }

            vesc_config_t config;
            esp_err_t err = vesc_config_load(&config);

            if (err == ESP_OK) {
                int32_t current_erpm = get_latest_erpm();
                adc_value = level_assistant_process(adc_value, current_erpm, config.level_assistant);

                // Apply throttle inversion after level assistant processing (lite mode only)
                if (config.invert_throttle) {
                    // Apply throttle inversion by inverting the ADC value
                    // Since ADC is 12-bit (0-4095), we invert by subtracting from max value
                    adc_value = 4095 - adc_value;
                }
            }
#endif

            // Pack the ADC value into 2 bytes (little-endian)
            data_buffer[0] = (uint8_t)(adc_value & 0xFF);         // Low byte
            data_buffer[1] = (uint8_t)((adc_value >> 8) & 0xFF);  // High byte

            esp_ble_gattc_write_char(
                spp_gattc_if,
                spp_conn_id,
                (db+SPP_IDX_SPP_DATA_RECV_VAL)->attribute_handle,
                sizeof(data_buffer),  // 2 bytes
                data_buffer,
                ESP_GATT_WRITE_TYPE_NO_RSP,
                ESP_GATT_AUTH_REQ_NONE
            );
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

float get_latest_voltage(void)
{
    return latest_voltage;
}

int32_t get_latest_erpm(void)
{
    return latest_erpm;
}

float get_latest_current_motor(void)
{
    return latest_current_motor;
}

float get_latest_current_in(void)
{
    return latest_current_in;
}

float get_bms_total_voltage(void)
{
    return bms_total_voltage;
}

float get_bms_current(void)
{
    return bms_current;
}

float get_bms_remaining_capacity(void)
{
    return bms_remaining_capacity;
}

float get_bms_nominal_capacity(void)
{
    return bms_nominal_capacity;
}

uint8_t get_bms_num_cells(void)
{
    return bms_num_cells;
}

float get_bms_cell_voltage(uint8_t cell_index)
{
    if(cell_index < bms_num_cells && cell_index < 16) {
        return bms_cell_voltages[cell_index];
    }
    return 0.0f;
}

static void log_rssi_task(void *pvParameters) {
    while (1) {
        if (is_connect && spp_gattc_if != 0xff) {
            esp_err_t ret = esp_ble_gap_read_rssi(scan_rst.scan_rst.bda);
            if (ret != ESP_OK) {
                ESP_LOGE(GATTC_TAG, "Read RSSI failed: %s", esp_err_to_name(ret));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Check RSSI every second
    }
}

int get_bms_battery_percentage(void) {
    if (bms_nominal_capacity <= 0.0f) return -1;

    float percentage = (bms_remaining_capacity / bms_nominal_capacity) * 100.0f;

    if (percentage > 100.0f) percentage = 100.0f;
    if (percentage < 0.0f) percentage = 0.0f;

    return (int)percentage;
}


