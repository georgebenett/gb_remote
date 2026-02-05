#include "ui_updater.h"
#include "esp_log.h"
#include "throttle.h"
#include "battery.h"
#include "ble.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_err.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "vesc_config.h"
#include "hw_config.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>
#include "esp_task_wdt.h"
#include "viber.h"

#define TAG "UI_UPDATER"
#define TRIP_NVS_NAMESPACE "trip_data"
#define NVS_KEY_TRIP_KM "trip_km"

// UI Command Queue for thread-safe UI updates
#define UI_CMD_QUEUE_SIZE 32

typedef enum {
    UI_CMD_UPDATE_SPEED,
    UI_CMD_UPDATE_SPEED_UNIT,
    UI_CMD_UPDATE_BATTERY_PERCENTAGE,
    UI_CMD_UPDATE_BATTERY_VOLTAGE,
    UI_CMD_UPDATE_SKATE_BATTERY_PERCENTAGE,
    UI_CMD_UPDATE_SKATE_BATTERY_VOLTAGE,
    UI_CMD_UPDATE_CONNECTION_ICON,
    UI_CMD_UPDATE_TRIP_DISTANCE,
    UI_CMD_RESET_TRIP_DISTANCE,
    UI_CMD_UPDATE_AUX_INDICATOR,
} ui_cmd_type_t;

typedef struct {
    ui_cmd_type_t type;
    union {
        int32_t speed;
        bool speed_unit_mph;
        struct {
            int percentage;
            bool is_charging;
        } battery;
        float voltage;
        int skate_percentage;
        float skate_voltage;
        uint8_t connection_quality;
        float trip_km;
        bool aux_state;
    } data;
} ui_cmd_t;

static QueueHandle_t ui_cmd_queue = NULL;
static SemaphoreHandle_t lvgl_mutex = NULL;
static const TickType_t LVGL_MUTEX_TIMEOUT = pdMS_TO_TICKS(10);

static volatile bool force_config_reload = false;

static uint8_t connection_quality = 0;
static float total_trip_km = 0.0f;
static uint32_t last_update_time = 0;

extern volatile bool entering_power_off_mode;

#define SPEED_UPDATE_MS       50
#define TRIP_UPDATE_MS       1000    // 1Hz for distance
#define BATTERY_UPDATE_MS    1000    // 1Hz for battery
#define CONNECTION_UPDATE_MS 5000    // 0.2Hz for connection

static lv_obj_t* get_current_screen(void) {
    return lv_scr_act();
}

void ui_updater_init(void) {
    lvgl_mutex = xSemaphoreCreateMutex();
    if (lvgl_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
    } else {
        ESP_LOGI(TAG, "LVGL mutex created with priority inheritance");
    }

    // Create UI command queue
    ui_cmd_queue = xQueueCreate(UI_CMD_QUEUE_SIZE, sizeof(ui_cmd_t));
    if (ui_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UI command queue");
    } else {
        ESP_LOGI(TAG, "UI command queue created (size: %d)", UI_CMD_QUEUE_SIZE);
    }

    // Initialize NVS for trip data
    esp_err_t err = ui_init_trip_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize trip NVS, trip data may not be saved");
    }

    // Initialize with current time
    last_update_time = esp_timer_get_time() / 1000;

    // Load trip distance from NVS
    ui_load_trip_distance();
}

bool take_lvgl_mutex(void) {
    if (lvgl_mutex == NULL) return false;
    return xSemaphoreTake(lvgl_mutex, LVGL_MUTEX_TIMEOUT) == pdTRUE;
}

bool take_lvgl_mutex_for_handler(void) {
    if (lvgl_mutex == NULL) return false;
    return xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

SemaphoreHandle_t get_lvgl_mutex_handle(void) {
    return lvgl_mutex;
}

void give_lvgl_mutex(void) {
    if (lvgl_mutex != NULL) {
        xSemaphoreGive(lvgl_mutex);
    }
}

// Helper to send UI command to queue
static bool ui_queue_send(ui_cmd_t *cmd) {
    if (ui_cmd_queue == NULL) {
        return false;
    }

    if (xQueueSend(ui_cmd_queue, cmd, 0) != pdTRUE) {
        // Queue full - remove oldest and try again
        ui_cmd_t dummy;
        xQueueReceive(ui_cmd_queue, &dummy, 0);
        return xQueueSend(ui_cmd_queue, cmd, 0) == pdTRUE;
    }
    return true;
}

void ui_update_speed(int32_t value) {
    if (entering_power_off_mode || !objects.speedlabel) return;

    static int32_t last_value = -1;

    // Only update if value has changed
    if (value != last_value) {
        ui_cmd_t cmd = {
            .type = UI_CMD_UPDATE_SPEED,
            .data.speed = value
        };
        if (ui_queue_send(&cmd)) {
            last_value = value;
        }
    }
}

void ui_update_battery_percentage(int percentage) {
    if (entering_power_off_mode) return;
    if (objects.controller_battery_text == NULL || objects.controller_battery == NULL) return;

    int gpio_level = gpio_get_level(BATTERY_IS_CHARGING_GPIO);
    bool is_charging = (gpio_level == 0);  // Inverted: LOW means charging

    ui_cmd_t cmd = {
        .type = UI_CMD_UPDATE_BATTERY_PERCENTAGE,
        .data.battery = {
            .percentage = percentage,
            .is_charging = is_charging
        }
    };
    ui_queue_send(&cmd);
}

void ui_update_battery_voltage_display(float voltage) {
    if (entering_power_off_mode) return;
    if (objects.display_voltage == NULL) return;

    float battery_voltage = battery_get_voltage();

    ui_cmd_t cmd = {
        .type = UI_CMD_UPDATE_BATTERY_VOLTAGE,
        .data.voltage = battery_voltage
    };
    ui_queue_send(&cmd);
}

void ui_update_skate_battery_percentage(int percentage) {
    if (entering_power_off_mode) return;
    if (objects.skate_battery_text == NULL) return;

    ui_cmd_t cmd = {
        .type = UI_CMD_UPDATE_SKATE_BATTERY_PERCENTAGE,
        .data.skate_percentage = percentage
    };
    ui_queue_send(&cmd);
}

void ui_update_skate_battery_voltage_display(float voltage) {
    if (entering_power_off_mode) return;
    if (objects.skate_battery_text == NULL) return;

    ui_cmd_t cmd = {
        .type = UI_CMD_UPDATE_SKATE_BATTERY_VOLTAGE,
        .data.skate_voltage = voltage
    };
    ui_queue_send(&cmd);
}

int get_connection_quality(void) {
    return connection_quality;
}

void ui_update_connection_quality(int rssi) {
    if (rssi >= 0) {
        connection_quality = 0;
    } else {
        connection_quality = ((rssi + 100) * 100) / 70;

        if (connection_quality > 100) connection_quality = 100;

    }
    ui_update_connection_icon();
}

void ui_update_connection_icon(void) {
    if (entering_power_off_mode) return;
    if (objects.connection_icon == NULL) return;

    ui_cmd_t cmd = {
        .type = UI_CMD_UPDATE_CONNECTION_ICON,
        .data.connection_quality = connection_quality
    };
    ui_queue_send(&cmd);
}

void ui_update_trip_distance(int32_t speed_kmh) {
    if (entering_power_off_mode) return;
    if (objects.odometer == NULL) return;

    uint32_t current_time = esp_timer_get_time() / 1000;

    if (last_update_time > 0) {
        float elapsed_hours = (current_time - last_update_time) / 3600000.0f;
        float distance = (speed_kmh * elapsed_hours);

        total_trip_km += distance;
        if (total_trip_km > 999.0f) {
            ESP_LOGI(TAG, "Trip distance exceeded 999 units, resetting to 0");
            total_trip_km = 0.0f;
        }
    }

    last_update_time = current_time;

    ui_cmd_t cmd = {
        .type = UI_CMD_UPDATE_TRIP_DISTANCE,
        .data.trip_km = total_trip_km
    };
    ui_queue_send(&cmd);
}

void ui_reset_trip_distance(void) {
    total_trip_km = 0.0f;

    // Save reset value to NVS
    esp_err_t err = ui_save_trip_distance();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save trip distance reset to NVS: %s", esp_err_to_name(err));
    }

    ui_cmd_t cmd = {
        .type = UI_CMD_RESET_TRIP_DISTANCE
    };
    ui_queue_send(&cmd);
}

esp_err_t ui_save_trip_distance(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(TRIP_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS for trip data: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, NVS_KEY_TRIP_KM, &total_trip_km, sizeof(float));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving trip distance: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Trip distance saved: %.2f km", total_trip_km);
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t ui_load_trip_distance(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(TRIP_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No trip data found, starting from 0");
            total_trip_km = 0.0f;
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Error opening NVS for trip data: %s", esp_err_to_name(err));
        return err;
    }

    size_t required_size = sizeof(float);
    err = nvs_get_blob(nvs_handle, NVS_KEY_TRIP_KM, &total_trip_km, &required_size);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No trip data found, starting from 0");
            total_trip_km = 0.0f;
            err = ESP_OK;
        } else {
            ESP_LOGE(TAG, "Error loading trip distance: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "Trip distance loaded: %.2f km", total_trip_km);
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t ui_init_trip_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(TRIP_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

void ui_check_mutex_health(void) {
    static uint32_t last_check_time = 0;
    uint32_t current_time = esp_timer_get_time() / 1000000;

    if (current_time - last_check_time >= 30) {
        if (lvgl_mutex != NULL && xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(1)) != pdTRUE) {
            ESP_LOGW(TAG, "LVGL mutex appears to be stuck, recreating");

            SemaphoreHandle_t new_mutex = xSemaphoreCreateMutex();
            if (new_mutex != NULL) {
                lvgl_mutex = new_mutex;

                ESP_LOGW(TAG, "LVGL mutex replaced");
            } else {
                ESP_LOGE(TAG, "Failed to create new LVGL mutex");
            }
        } else if (lvgl_mutex != NULL) {
            xSemaphoreGive(lvgl_mutex);
        }

        last_check_time = current_time;
    }
}

void ui_update_speed_unit(bool is_mph) {
    if (entering_power_off_mode || !objects.static_speed) return;

    ui_cmd_t cmd = {
        .type = UI_CMD_UPDATE_SPEED_UNIT,
        .data.speed_unit_mph = is_mph
    };
    ui_queue_send(&cmd);
}

static void speed_update_task(void *pvParameters) {
    // Register with task watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    vesc_config_t config;
    ESP_ERROR_CHECK(vesc_config_load(&config));

    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t frequency = pdMS_TO_TICKS(SPEED_UPDATE_MS);
    uint32_t config_reload_counter = 0;
    const uint32_t CONFIG_RELOAD_INTERVAL = 50;

    while (1) {
        vTaskDelayUntil(&last_wake_time, frequency);

        config_reload_counter++;
        if (config_reload_counter >= CONFIG_RELOAD_INTERVAL || force_config_reload) {
            esp_err_t err = vesc_config_load(&config);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to reload configuration: %s", esp_err_to_name(err));
            }
            config_reload_counter = 0;
            force_config_reload = false;
        }

        if (ble_is_connected()) {
            int32_t speed = vesc_config_get_speed(&config);
            if (speed >= 0) {
                ui_update_speed(speed);
                ui_update_speed_unit(config.speed_unit_mph);
            }
        }
        esp_task_wdt_reset();
    }
}

static void trip_distance_update_task(void *pvParameters) {
    // Register with task watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    vesc_config_t config;
    ESP_ERROR_CHECK(vesc_config_load(&config));

    uint32_t config_reload_counter = 0;
    const uint32_t CONFIG_RELOAD_INTERVAL = 10;

    while (1) {
        config_reload_counter++;
        if (config_reload_counter >= CONFIG_RELOAD_INTERVAL || force_config_reload) {
            esp_err_t err = vesc_config_load(&config);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to reload configuration: %s", esp_err_to_name(err));
            }
            config_reload_counter = 0;
            force_config_reload = false;
        }

        int32_t speed = vesc_config_get_speed(&config);
        ui_update_trip_distance(speed);
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(TRIP_UPDATE_MS));
    }
}

static void battery_update_task(void *pvParameters) {
    // Register with task watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    static int displayed_percentage = -1;
    static uint32_t last_change_time = 0;
    const uint32_t RATE_LIMIT_MS = 5000;

    while (1) {

        int battery_percentage = battery_get_percentage();
        float battery_voltage = battery_get_voltage();

        if (battery_percentage >= 0) {
            int display_percentage = battery_percentage;

            if (displayed_percentage >= 0) {
                uint32_t current_time = esp_timer_get_time() / 1000;

                if (current_time - last_change_time >= RATE_LIMIT_MS) {
                    if (battery_percentage > displayed_percentage) {
                        display_percentage = displayed_percentage + 1;
                        last_change_time = current_time;
                    } else if (battery_percentage < displayed_percentage) {
                        display_percentage = displayed_percentage - 1;
                        last_change_time = current_time;
                    } else {
                        display_percentage = displayed_percentage;
                    }
                } else {
                    display_percentage = displayed_percentage;
                }
            }

            displayed_percentage = display_percentage;
            ui_update_battery_percentage(display_percentage);
            ui_update_battery_voltage_display(battery_voltage);
        }

        if (ble_is_connected()) {
            float bms_voltage = get_bms_total_voltage();
            bool bms_connected = (bms_voltage > 0.1f);

            if (!bms_connected) {
                float vesc_voltage = get_latest_voltage();

                if (vesc_voltage > 0.1f) {
                    ui_update_skate_battery_voltage_display(vesc_voltage);
                } else {
                    ui_update_skate_battery_percentage(0);
                }
            } else {
                int skate_battery_percentage = get_bms_battery_percentage();
                if (skate_battery_percentage >= 0) {
                    ui_update_skate_battery_percentage(skate_battery_percentage);
                }
            }
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(BATTERY_UPDATE_MS));
    }
}

static void connection_update_task(void *pvParameters) {
    // Register with task watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (1) {
        ui_update_connection_icon();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(CONNECTION_UPDATE_MS));
    }
}

// UI Command Processor Task - handles queued UI updates
static void ui_cmd_processor_task(void *pvParameters) {
    // Register with task watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    ui_cmd_t cmd;
    char str_buf[16];

    while (1) {
        esp_task_wdt_reset();

        // Block waiting for commands with timeout to allow watchdog reset
        if (xQueueReceive(ui_cmd_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Only update if on home screen (for most commands)
                bool on_home = (get_current_screen() == objects.home_screen);

                switch (cmd.type) {
                    case UI_CMD_UPDATE_SPEED:
                        if (on_home && objects.speedlabel != NULL) {
                            lv_label_set_text_fmt(objects.speedlabel, "%ld", cmd.data.speed);
                        }
                        break;

                    case UI_CMD_UPDATE_SPEED_UNIT:
                        if (on_home && objects.static_speed != NULL) {
                            lv_label_set_text(objects.static_speed, cmd.data.speed_unit_mph ? "mi/h" : "km/h");
                        }
                        break;

                    case UI_CMD_UPDATE_BATTERY_PERCENTAGE:
                        if (on_home && objects.controller_battery != NULL && objects.controller_battery_text != NULL) {
                            if (cmd.data.battery.is_charging) {
                                lv_img_set_src(objects.controller_battery, &img_battery_charging);
                                lv_label_set_text_fmt(objects.controller_battery_text, "%d", cmd.data.battery.percentage);
                                lv_obj_set_style_text_color(objects.controller_battery_text, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
                            } else {
                                lv_img_set_src(objects.controller_battery, &img_battery);
                                lv_label_set_text_fmt(objects.controller_battery_text, "%d", cmd.data.battery.percentage);
                                lv_obj_set_style_text_color(objects.controller_battery_text, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
                            }
                        }
                        break;

                    case UI_CMD_UPDATE_BATTERY_VOLTAGE:
                        if (on_home && objects.display_voltage != NULL) {
                            snprintf(str_buf, sizeof(str_buf), "%dmV", (int)(cmd.data.voltage * 1000 + 0.5f));
                            lv_label_set_text(objects.display_voltage, str_buf);
                        }
                        break;

                    case UI_CMD_UPDATE_SKATE_BATTERY_PERCENTAGE:
                        if (on_home && objects.skate_battery_text != NULL) {
                            lv_label_set_text_fmt(objects.skate_battery_text, "%d", cmd.data.skate_percentage);
                        }
                        break;

                    case UI_CMD_UPDATE_SKATE_BATTERY_VOLTAGE:
                        if (on_home && objects.skate_battery_text != NULL) {
                            int volts = (int)cmd.data.skate_voltage;
                            int tenths = (int)((cmd.data.skate_voltage - volts) * 10 + 0.5f);
                            if (tenths >= 10) {
                                tenths = 0;
                                volts++;
                            }
                            snprintf(str_buf, sizeof(str_buf), "%d.%d", volts, tenths);
                            lv_label_set_text(objects.skate_battery_text, str_buf);
                        }
                        break;

                    case UI_CMD_UPDATE_CONNECTION_ICON:
                        if (on_home && objects.connection_icon != NULL) {
                            const void* icon_src = NULL;
                            if (!ble_is_connected()) {
                                icon_src = &img_connection_0;
                            } else if (cmd.data.connection_quality >= 30) {
                                icon_src = &img_100_connection;
                            } else if (cmd.data.connection_quality >= 15) {
                                icon_src = &img_66_connection;
                            } else if (cmd.data.connection_quality >= 5) {
                                icon_src = &img_33_connection;
                            } else {
                                icon_src = &img_connection_0;
                            }
                            lv_img_set_src(objects.connection_icon, icon_src);
                        }
                        break;

                    case UI_CMD_UPDATE_TRIP_DISTANCE:
                        if (on_home && objects.odometer != NULL) {
                            snprintf(str_buf, sizeof(str_buf), "%.1f", cmd.data.trip_km);
                            lv_label_set_text(objects.odometer, str_buf);
                            lv_obj_invalidate(objects.odometer);
                        }
                        break;

                    case UI_CMD_RESET_TRIP_DISTANCE:
                        if (on_home && objects.odometer != NULL) {
                            lv_label_set_text(objects.odometer, "0.0");
                            lv_obj_invalidate(objects.odometer);
                        }
                        break;

                    case UI_CMD_UPDATE_AUX_INDICATOR:
                        if (objects.aux_output != NULL) {
                            if (cmd.data.aux_state) {
                                lv_obj_set_style_opa(objects.aux_output, LV_OPA_COVER, 0);
                            } else {
                                lv_obj_set_style_opa(objects.aux_output, LV_OPA_TRANSP, 0);
                            }
                        }
                        break;
                }
                xSemaphoreGive(lvgl_mutex);
            } else {
                // Re-queue the command if we couldn't get mutex (rare case)
                if (xQueueSendToFront(ui_cmd_queue, &cmd, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Failed to re-queue UI command, update dropped");
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
    }
}

void ui_start_update_tasks(void) {
    vTaskDelay(pdMS_TO_TICKS(100));

    // Start the UI command processor task first (highest priority for UI responsiveness)
    xTaskCreate(ui_cmd_processor_task, "ui_cmd_proc", 3072, NULL, 5, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));

    xTaskCreate(speed_update_task, "speed_update", 4096, NULL, 4, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    xTaskCreate(trip_distance_update_task, "trip_update", 4096, NULL, 3, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    xTaskCreate(battery_update_task, "battery_update", 4096, NULL, 2, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    xTaskCreate(connection_update_task, "conn_update", 4096, NULL, 2, NULL);
}

void ui_force_config_reload(void) {
    force_config_reload = true;
}

void ui_create_aux_output_indicator(void) {
    if (objects.aux_output == NULL) {
        ESP_LOGW(TAG, "objects.aux_output is NULL");
        return;
    }

    // Set initial visibility based on saved state
    ui_update_aux_output_indicator();
}

void ui_update_aux_output_indicator(void) {
    if (objects.aux_output == NULL) return;

    bool aux_state = ble_get_aux_output_state();

    ui_cmd_t cmd = {
        .type = UI_CMD_UPDATE_AUX_INDICATOR,
        .data.aux_state = aux_state
    };
    ui_queue_send(&cmd);
}

static void splash_timer_cb(lv_timer_t *timer) {
    lv_disp_load_scr(objects.home_screen);
}

void ui_show_splash_screen(void) {
    viber_play_pattern(VIBER_PATTERN_SINGLE_SHORT);
    lv_disp_load_scr(objects.splash_screen);

    lv_timer_t *splash_timer = lv_timer_create(splash_timer_cb, 4000, NULL);
    lv_timer_set_repeat_count(splash_timer, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
}
