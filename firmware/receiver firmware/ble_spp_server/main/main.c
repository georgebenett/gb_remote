#include "ble_spp_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "adc.h"
#include "bldc_interface.h"
#include "bldc_interface_uart.h"
#include "driver/uart.h"
#include "soc/gpio_num.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "bms.h"
#include "cJSON.h"
#include "esp_timer.h"

static const char *TAG = "MAIN";



static mc_values stored_values;

// Add this function before app_main
static void send_packet(unsigned char *data, unsigned int len) {
    // Configure UART for VESC communication
    const uart_port_t uart_num = UART_NUM_1;  // Using UART1
    uart_write_bytes(uart_num, (const char*)data, len);
}

// Add this after the send_packet function and before app_main
static void bldc_values_received(mc_values *values) {
    // Store the values without logging
    stored_values = *values;
}

// Add this function before app_main
static void vesc_task(void *pvParameters) {
    while (1) {
        bldc_interface_get_values();
        vTaskDelay(pdMS_TO_TICKS(50));

    }
}


static void uart_rx_task(void *pvParameters) {
    uint8_t data[128];
    while (1) {
        int len = uart_read_bytes(UART_NUM_1, data, sizeof(data), pdMS_TO_TICKS(10));
        for (int i = 0; i < len; i++) {
            bldc_interface_uart_process_byte(data[i]);
        }
        bldc_interface_uart_run_timer();
    }
}

void print_stored_values(void) {
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "VESC Data:");
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "Input voltage: %.2f V", stored_values.v_in);
    ESP_LOGI(TAG, "Temperature MOS: %.2f °C", stored_values.temp_mos);
    ESP_LOGI(TAG, "Temperature Motor: %.2f °C", stored_values.temp_motor);
    ESP_LOGI(TAG, "Current Motor: %.2f A", stored_values.current_motor);
    ESP_LOGI(TAG, "Current Input: %.2f A", stored_values.current_in);
    ESP_LOGI(TAG, "ID: %.2f A", stored_values.id);
    ESP_LOGI(TAG, "IQ: %.2f A", stored_values.iq);
    ESP_LOGI(TAG, "RPM: %.1f RPM", stored_values.rpm);
    ESP_LOGI(TAG, "Duty cycle: %.1f %%", stored_values.duty_now * 100.0);
    ESP_LOGI(TAG, "Amp Hours Drawn: %.4f Ah", stored_values.amp_hours);
    ESP_LOGI(TAG, "Amp Hours Regen: %.4f Ah", stored_values.amp_hours_charged);
    ESP_LOGI(TAG, "Watt Hours Drawn: %.4f Wh", stored_values.watt_hours);
    ESP_LOGI(TAG, "Watt Hours Regen: %.4f Wh", stored_values.watt_hours_charged);
    ESP_LOGI(TAG, "Tachometer: %d counts", stored_values.tachometer);
    ESP_LOGI(TAG, "Tachometer Abs: %d counts", stored_values.tachometer_abs);
    ESP_LOGI(TAG, "PID Position: %.2f", stored_values.pid_pos);
    ESP_LOGI(TAG, "VESC ID: %d", stored_values.vesc_id);

    // Print fault code if any
    const char* fault_str = bldc_interface_fault_to_string(stored_values.fault_code);
    ESP_LOGI(TAG, "Fault Code: %s", fault_str);
    ESP_LOGI(TAG, "----------------------------------------");
}

// Add before app_main
mc_values* get_stored_vesc_values(void) {
    return &stored_values;
}

void send_telemetry_data(const mc_values* vesc_data, const bms_values_t* bms_data) {
    cJSON *root = cJSON_CreateObject();
    char number_str[16];

    // Add timestamp
    cJSON_AddNumberToObject(root, "timestamp", esp_timer_get_time() / 1000); // Convert to milliseconds

    // Add VESC data
    cJSON *vesc = cJSON_CreateObject();
    snprintf(number_str, sizeof(number_str), "%.3f", vesc_data->v_in);
    cJSON_AddNumberToObject(vesc, "voltage", atof(number_str));

    snprintf(number_str, sizeof(number_str), "%.3f", vesc_data->current_motor);
    cJSON_AddNumberToObject(vesc, "current_motor", atof(number_str));

    snprintf(number_str, sizeof(number_str), "%.3f", vesc_data->current_in);
    cJSON_AddNumberToObject(vesc, "current_input", atof(number_str));

    snprintf(number_str, sizeof(number_str), "%.3f", vesc_data->duty_now);
    cJSON_AddNumberToObject(vesc, "duty", atof(number_str));

    snprintf(number_str, sizeof(number_str), "%.3f", vesc_data->rpm);
    cJSON_AddNumberToObject(vesc, "rpm", atof(number_str));

    snprintf(number_str, sizeof(number_str), "%.3f", vesc_data->temp_mos);
    cJSON_AddNumberToObject(vesc, "temp_mos", atof(number_str));

    snprintf(number_str, sizeof(number_str), "%.3f", vesc_data->temp_motor);
    cJSON_AddNumberToObject(vesc, "temp_motor", atof(number_str));

    cJSON_AddItemToObject(root, "vesc", vesc);

    // Add BMS data
    cJSON *bms = cJSON_CreateObject();
    snprintf(number_str, sizeof(number_str), "%.3f", bms_data->total_voltage);
    cJSON_AddNumberToObject(bms, "total_voltage", atof(number_str));

    snprintf(number_str, sizeof(number_str), "%.3f", bms_data->current);
    cJSON_AddNumberToObject(bms, "current", atof(number_str));

    snprintf(number_str, sizeof(number_str), "%.3f", bms_data->remaining_capacity);
    cJSON_AddNumberToObject(bms, "remaining_capacity", atof(number_str));

    snprintf(number_str, sizeof(number_str), "%.3f", bms_data->nominal_capacity);
    cJSON_AddNumberToObject(bms, "nominal_capacity", atof(number_str));

    // Add cell voltages array
    cJSON *cells = cJSON_CreateArray();
    for (int i = 0; i < bms_data->num_cells; i++) {
        snprintf(number_str, sizeof(number_str), "%.3f", bms_data->cell_voltages[i]);
        cJSON_AddItemToArray(cells, cJSON_CreateNumber(atof(number_str)));
    }
    cJSON_AddItemToObject(bms, "cell_voltages", cells);
    cJSON_AddItemToObject(root, "bms", bms);

    // Convert to string and print with newline for read_data.py
    char *json_string = cJSON_PrintUnformatted(root);
    printf("%s\n", json_string);

    // Cleanup
    cJSON_free(json_string);
    cJSON_Delete(root);
}

void app_main(void)
{
    esp_err_t ret;

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize LED
    ESP_ERROR_CHECK(led_init());

    // Initialize BMS
    ESP_ERROR_CHECK(bms_uart_init());

    // Initialize Bluetooth
    ret = ble_spp_server_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BLE SPP server: %s", esp_err_to_name(ret));
        return;
    }

    // Set BLE TX power to maximum
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);

    // Start the server
    ret = ble_spp_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start BLE SPP server: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "BLE SPP server started successfully");

    adc_init();
    bldc_interface_uart_init(send_packet);
    bldc_interface_set_rx_value_func(bldc_values_received);

    // Create UART receive task
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 5, NULL);

    // Create task to periodically request VESC values
    xTaskCreate(vesc_task, "vesc_task", 2048, NULL, 5, NULL);

}
