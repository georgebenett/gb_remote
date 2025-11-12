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
#include "vesc_config.h"
#include "hw_config.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>

#define TAG "UI_UPDATER"
#define TRIP_NVS_NAMESPACE "trip_data"
#define NVS_KEY_TRIP_KM "trip_km"

static SemaphoreHandle_t lvgl_mutex = NULL;
static const TickType_t LVGL_MUTEX_TIMEOUT = pdMS_TO_TICKS(10);

static volatile bool force_config_reload = false;

static uint8_t connection_quality = 0;
static float total_trip_km = 0.0f;
static uint32_t last_update_time = 0;

extern volatile bool entering_power_off_mode;

#define SPEED_UPDATE_MS       20    // 50Hz for speed updates
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

void ui_update_speed(int32_t value) {
    if (entering_power_off_mode || !objects.speedlabel) return;

    static int32_t last_value = -1;

    // Only update if value has changed
    if (value != last_value) {
        if (take_lvgl_mutex()) {
            if (get_current_screen() == objects.home_screen) {
                lv_label_set_text_fmt(objects.speedlabel, "%ld", value);
            }
            give_lvgl_mutex();
            last_value = value;
        }
    }
}

void ui_update_battery_percentage(int percentage) {
    if (entering_power_off_mode) return;

    if (objects.controller_battery_text == NULL || objects.controller_battery == NULL) return;

    int gpio_level = gpio_get_level(BATTERY_IS_CHARGING_GPIO);
    bool is_charging = (gpio_level == 0);  // Inverted: LOW means charging

    if (!take_lvgl_mutex()) {
        return;
    }

    if (get_current_screen() == objects.home_screen) {
        if (is_charging) {
            lv_img_set_src(objects.controller_battery, &img_battery_charging);
            lv_label_set_text_fmt(objects.controller_battery_text, "%d", percentage);
            lv_obj_set_style_text_color(objects.controller_battery_text, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            lv_img_set_src(objects.controller_battery, &img_battery);
            lv_label_set_text_fmt(objects.controller_battery_text, "%d", percentage);
            lv_obj_set_style_text_color(objects.controller_battery_text, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }

    give_lvgl_mutex();
}

void ui_update_battery_voltage_display(float voltage) {
    if (entering_power_off_mode) return;

    if (objects.controller_battery_text == NULL) return;

    float battery_voltage = battery_get_voltage();
    char voltage_str[16];
    snprintf(voltage_str, sizeof(voltage_str), "%dmV", (int)(battery_voltage * 1000 + 0.5f)); // Round to nearest millivolt

    if (take_lvgl_mutex()) {
        if (get_current_screen() == objects.home_screen) {
            lv_label_set_text(objects.display_voltage, voltage_str);
        }
        give_lvgl_mutex();
    }
}

void ui_update_skate_battery_percentage(int percentage) {
    if (entering_power_off_mode) return;

    if (objects.skate_battery_text == NULL) return;

    if (take_lvgl_mutex()) {
        if (get_current_screen() == objects.home_screen) {
            lv_label_set_text_fmt(objects.skate_battery_text, "%d", percentage);
        }
        give_lvgl_mutex();
    }
}

void ui_update_skate_battery_voltage_display(float voltage) {
    if (entering_power_off_mode) return;

    if (objects.skate_battery_text == NULL) return;
    char voltage_str[16];
    int volts = (int)voltage;
    int tenths = (int)((voltage - volts) * 10 + 0.5f);
    if (tenths >= 10) {
        tenths = 0;
        volts++;
    }
    snprintf(voltage_str, sizeof(voltage_str), "%d.%d", volts, tenths);

    if (take_lvgl_mutex()) {
        if (get_current_screen() == objects.home_screen) {
            lv_label_set_text(objects.skate_battery_text, voltage_str);
        }
        give_lvgl_mutex();
    }
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

    if (take_lvgl_mutex()) {
        if (get_current_screen() == objects.home_screen) {
            const void* icon_src = NULL;
            if (!is_connect) {
                icon_src = &img_connection_0;
            } else if (connection_quality >= 30) {
                icon_src = &img_100_connection;
            } else if (connection_quality >= 15) {
                icon_src = &img_66_connection;
            } else if (connection_quality >= 5) {
                icon_src = &img_33_connection;
            } else {
                icon_src = &img_connection_0;
            }

            lv_img_set_src(objects.connection_icon, icon_src);
        }
        give_lvgl_mutex();
    }
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

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", total_trip_km);

    if (take_lvgl_mutex()) {
        if (get_current_screen() == objects.home_screen) {
            lv_label_set_text(objects.odometer, buf);
            lv_obj_invalidate(objects.odometer);
        }
        give_lvgl_mutex();
    }
}

void ui_reset_trip_distance(void) {
    total_trip_km = 0.0f;

    if (!take_lvgl_mutex()) {
        ESP_LOGW(TAG, "Failed to take LVGL mutex for trip reset");
        return;
    }

    if (objects.odometer != NULL && get_current_screen() == objects.home_screen) {
        lv_label_set_text(objects.odometer, "0.0");
        lv_obj_invalidate(objects.odometer); // Force redraw
    }

    give_lvgl_mutex();
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

    if (take_lvgl_mutex()) {
        if (get_current_screen() == objects.home_screen) {
            lv_label_set_text(objects.static_speed, is_mph ? "mi/h" : "km/h");
        }
        give_lvgl_mutex();
    }
}

static void speed_update_task(void *pvParameters) {
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

        if (is_connect) {
            int32_t speed = vesc_config_get_speed(&config);
            if (speed >= 0 && speed <= 100) {
                ui_update_speed(speed);
                ui_update_speed_unit(config.speed_unit_mph);
            }
        }
    }
}

static void trip_distance_update_task(void *pvParameters) {
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
        vTaskDelay(pdMS_TO_TICKS(TRIP_UPDATE_MS));
    }
}

static void battery_update_task(void *pvParameters) {
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

        if (is_connect) {
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

        vTaskDelay(pdMS_TO_TICKS(BATTERY_UPDATE_MS));
    }
}

static void connection_update_task(void *pvParameters) {
    while (1) {
        ui_update_connection_icon();
        vTaskDelay(pdMS_TO_TICKS(CONNECTION_UPDATE_MS));
    }
}

void ui_start_update_tasks(void) {
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


