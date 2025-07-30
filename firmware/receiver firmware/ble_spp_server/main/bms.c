// jiabaida_driver.c

#include "bms.h"
#include "string.h"
#include "esp_log.h"
#include "hw_config.h"

static const char *TAG = BMS_TAG;
static bms_values_t stored_bms_values = {0};

#define START_BYTE 0xDD
#define STOP_BYTE  0x77

static void calculate_checksum(const uint8_t *data, size_t len, uint8_t *chk_high, uint8_t *chk_low) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    uint16_t checksum = ~sum + 1;
    *chk_high = (checksum >> 8) & 0xFF;
    *chk_low = checksum & 0xFF;
}

// Forward declarations of static functions
static void bms_read_task(void *pvParameters);
static void print_bms_values(uint8_t *data, size_t len);

esp_err_t bms_uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = BMS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(BMS_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(BMS_UART_PORT, BMS_UART_TX_PIN, BMS_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(BMS_UART_PORT, BMS_BUF_SIZE * 2, 0, 0, NULL, 0));

    // Create task to read and print BMS values
    xTaskCreate(bms_read_task, "bms_read_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "BMS UART initialized");
    return ESP_OK;
}

static esp_err_t send_command(uint8_t status, uint8_t cmd, const uint8_t *data, size_t data_len, uint8_t *response, size_t *response_len) {
    uint8_t frame[256];
    size_t idx = 0;

    frame[idx++] = START_BYTE;
    frame[idx++] = status;
    frame[idx++] = cmd;
    frame[idx++] = (uint8_t)data_len;

    if (data && data_len > 0) {
        memcpy(&frame[idx], data, data_len);
        idx += data_len;
    }

    uint8_t chk_high, chk_low;
    calculate_checksum(&frame[2], data_len + 2, &chk_high, &chk_low);
    frame[idx++] = chk_high;
    frame[idx++] = chk_low;
    frame[idx++] = STOP_BYTE;

    uart_flush(BMS_UART_PORT);
    uart_write_bytes(BMS_UART_PORT, (const char *)frame, idx);

    // Add a small delay after sending
    vTaskDelay(pdMS_TO_TICKS(20));  // 20ms delay

    // Response timeout is ~100ms
    int len = uart_read_bytes(BMS_UART_PORT, response, BMS_BUF_SIZE, 100 / portTICK_PERIOD_MS);

    if (len > 0) {
        //ESP_LOGI(TAG, "Sent command 0x%02X, received %d bytes", cmd, len);
        *response_len = len;
        return ESP_OK;
    } else {
        //ESP_LOGW(TAG, "No response from BMS for command 0x%02X", cmd);
        return ESP_FAIL;
    }
}

esp_err_t bms_read_basic_info(uint8_t *response, size_t *response_len) {
    return send_command(0xA5, 0x03, NULL, 0, response, response_len);
}

esp_err_t bms_read_cell_voltages(uint8_t *response, size_t *response_len) {
    return send_command(0xA5, 0x04, NULL, 0, response, response_len);
}

esp_err_t bms_read_bms_version(uint8_t *response, size_t *response_len) {
    return send_command(0xA5, 0x05, NULL, 0, response, response_len);
}

esp_err_t bms_control_mos(mos_control_t mode) {
    uint8_t data[2] = {0x00, mode};
    uint8_t response[64];
    size_t response_len = 0;
    return send_command(0x5A, 0xE1, data, sizeof(data), response, &response_len);
}

static void print_bms_values(uint8_t *data, size_t len) {
    if (len < 4) {
        ESP_LOGE(TAG, "Invalid packet length");
        return;
    }

    if (data[0] != START_BYTE) {
        ESP_LOGE(TAG, "Invalid start byte");
        return;
    }

    uint8_t status = data[1];
    uint8_t data_len = data[3];

    switch (status) {
        case 0x04:  // Cell Voltages
            if (len >= 6) {
                stored_bms_values.num_cells = data_len / 2;
                for (int i = 0; i < stored_bms_values.num_cells && (i * 2 + 4) < len - 3; i++) {
                    uint16_t cell_mv = (data[4 + i * 2] << 8) | data[5 + i * 2];
                    stored_bms_values.cell_voltages[i] = cell_mv / 1000.0f;
                }
            }
            break;

        case 0x03:  // Basic Info
            if (len >= 34) {
                stored_bms_values.total_voltage = (data[4] << 8 | data[5]) / 100.0f;
                stored_bms_values.current = ((int16_t)(data[6] << 8 | data[7])) / 100.0f;
                stored_bms_values.remaining_capacity = (data[8] << 8 | data[9]) / 100.0f;
                stored_bms_values.nominal_capacity = (data[10] << 8 | data[11]) / 100.0f;

                mc_values* vesc_data = get_stored_vesc_values();
                if (vesc_data != NULL) {
                    send_telemetry_data(vesc_data, &stored_bms_values);
                }
            }
            break;

        case 0x05:  // Version Response
            if (len >= 8) {
                // Version info handling (if needed)
            }
            break;

        default:
            ESP_LOGW(TAG, "Unknown status: 0x%02X", status);
            break;
    }
}

static void bms_read_task(void *pvParameters) {
    uint8_t response[256];
    size_t response_len;
    uint8_t consecutive_failures = 0;
    const uint8_t MAX_FAILURES = 3;
    bool is_connected = true;

    while (1) {
        // Read basic info
        if (bms_read_basic_info(response, &response_len) == ESP_OK) {
            print_bms_values(response, response_len);
            consecutive_failures = 0;
            is_connected = true;
        } else {
            consecutive_failures++;
            if (consecutive_failures >= MAX_FAILURES) {
                if (is_connected) {
                    ESP_LOGW(TAG, "BMS disconnected");
                    is_connected = false;
                    // Clear BMS values when disconnected
                    memset(&stored_bms_values, 0, sizeof(bms_values_t));
                    // Send one telemetry update with zeros
                    mc_values* vesc_data = get_stored_vesc_values();
                    if (vesc_data != NULL) {
                        send_telemetry_data(vesc_data, &stored_bms_values);
                    }
                }
                // When disconnected, slow down the retry rate significantly
                vTaskDelay(pdMS_TO_TICKS(1000)); // 1 second between retries
                continue; // Skip other commands when disconnected
            }
        }

        if (is_connected) {
            vTaskDelay(pdMS_TO_TICKS(50));

            // Only try other commands if connected
            if (bms_read_cell_voltages(response, &response_len) == ESP_OK) {
                print_bms_values(response, response_len);
            }

            vTaskDelay(pdMS_TO_TICKS(50));

            static uint8_t version_counter = 0;
            if (++version_counter >= 10) {
                bms_read_bms_version(response, &response_len);
                version_counter = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bms_values_t* get_stored_bms_values(void) {
    return &stored_bms_values;
}
