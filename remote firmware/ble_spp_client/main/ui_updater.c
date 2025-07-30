#include "ui_updater.h"
#include "esp_log.h"
#include "adc.h"
#include "battery.h"
#include "ble_spp_client.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_err.h"
#include "freertos/semphr.h"
#include "vesc_config.h"

#define TAG "UI_UPDATER"
#define TRIP_NVS_NAMESPACE "trip_data"
#define NVS_KEY_TRIP_KM "trip_km"

// Add mutex for LVGL operations
static SemaphoreHandle_t lvgl_mutex = NULL;
static const TickType_t LVGL_MUTEX_TIMEOUT = pdMS_TO_TICKS(100); // 100ms timeout

static uint8_t connection_quality = 0;
static float total_trip_km = 0.0f;
static uint32_t last_update_time = 0;

// Add this static variable to track previous battery percentage
static int previous_battery_percentage = -1;

extern volatile bool entering_sleep_mode;

// Add these at the top with other defines
#define SPEED_UPDATE_MS       10    // 100Hz for responsive speed
#define TRIP_UPDATE_MS       100    // 10Hz for distance
#define BATTERY_UPDATE_MS    500    // 2Hz for battery
#define CONNECTION_UPDATE_MS 5000    // 0.2Hz for connection

static lv_obj_t* get_current_screen(void) {
    return lv_scr_act();
}

void ui_updater_init(void) {
    // Create mutex for LVGL operations
    lvgl_mutex = xSemaphoreCreateMutex();
    if (lvgl_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
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

// Helper function to take the LVGL mutex
bool take_lvgl_mutex(void) {
    if (lvgl_mutex == NULL) return false;
    return xSemaphoreTake(lvgl_mutex, LVGL_MUTEX_TIMEOUT) == pdTRUE;
}

// Helper function to release the LVGL mutex
void give_lvgl_mutex(void) {
    if (lvgl_mutex != NULL) {
        xSemaphoreGive(lvgl_mutex);
    }
}

void ui_update_speed(int32_t value) {
    if (entering_sleep_mode || !ui_Label1) return;

    static int32_t last_value = -1;

    // Only update if value has changed
    if (value != last_value) {
        if (take_lvgl_mutex()) {
            if (get_current_screen() == ui_home_screen) {
                lv_label_set_text_fmt(ui_Label1, "%ld", value);
            }
            give_lvgl_mutex();
            last_value = value;
        }
    }
}

void ui_update_battery_percentage(int percentage) {
    // Skip updates if we're entering sleep mode
    if (entering_sleep_mode) return;

    if (ui_controller_battery_text == NULL || ui_controller_battery_icon == NULL) return;

    if (!take_lvgl_mutex()) {
        ESP_LOGW(TAG, "Failed to take LVGL mutex for battery update");
        return;
    }

    // Only update if home screen is active
    if (get_current_screen() == ui_home_screen) {
        // Check if battery percentage has changed
        if (previous_battery_percentage != -1) {
            // Change icon based on whether percentage is increasing or decreasing
            if (percentage > previous_battery_percentage + 2) {
                // Battery percentage increased - show charging icon
                lv_img_set_src(ui_controller_battery_icon, &ui_img_battery_charging_icon_png);
                lv_obj_set_style_text_color(ui_controller_battery_text, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

            } else if (percentage < previous_battery_percentage) {
                // Battery percentage decreased - show normal icon
                lv_img_set_src(ui_controller_battery_icon, &ui_img_battery_icon_png);
                lv_obj_set_style_text_color(ui_controller_battery_text, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }

        // Update the previous percentage
        previous_battery_percentage = percentage;

        // Format with percentage symbol
        lv_label_set_text_fmt(ui_controller_battery_text, "%d", percentage);
    }

    give_lvgl_mutex();
}

void ui_update_skate_battery_percentage(int percentage) {
    // Skip updates if we're entering sleep mode
    if (entering_sleep_mode) return;

    if (ui_skate_battery_text == NULL) return;

    if (!take_lvgl_mutex()) {
        ESP_LOGW(TAG, "Failed to take LVGL mutex for skate battery update");
        return;
    }

    // Only update if home screen is active
    if (get_current_screen() == ui_home_screen) {
        // Format with percentage symbol
        lv_label_set_text_fmt(ui_skate_battery_text, "%d", percentage);
    }

    give_lvgl_mutex();
}

int get_connection_quality(void) {
    return connection_quality;
}

void ui_update_connection_quality(int rssi) {
    // If RSSI is 0 or positive, consider it as disconnected
    if (rssi >= 0) {
        connection_quality = 0;
    } else {
        // Normalize RSSI to percentage
        connection_quality = ((rssi + 100) * 100) / 70;

        // Clamp percentage between 0 and 100
        if (connection_quality > 100) connection_quality = 100;

    }
    // Update the UI
    ui_update_connection_icon();
}

void ui_update_connection_icon(void) {
    // Skip updates if we're entering sleep mode
    if (entering_sleep_mode) return;

    if (ui_no_connection_icon == NULL) return;

    if (!take_lvgl_mutex()) {
        ESP_LOGW(TAG, "Failed to take LVGL mutex for connection icon update");
        return;
    }

    // Only update if home screen is active
    if (get_current_screen() == ui_home_screen) {
        if (!is_connect) {
            lv_img_set_src(ui_no_connection_icon, &ui_img_no_connection_png);
        } else if (connection_quality < 15) {
            lv_img_set_src(ui_no_connection_icon, &ui_img_33_connection_png);
        } else if (connection_quality < 25) {
            lv_img_set_src(ui_no_connection_icon, &ui_img_66_connection_png);
        } else {
            lv_img_set_src(ui_no_connection_icon, &ui_img_full_connection_png);
        }
    }

    give_lvgl_mutex();
}

void ui_update_trip_distance(int32_t speed_kmh) {
    // Skip updates if we're entering sleep mode
    if (entering_sleep_mode) return;

    if (ui_tripkm == NULL) return;

    uint32_t current_time = esp_timer_get_time() / 1000; // Current time in milliseconds

    if (last_update_time > 0) {
        // Calculate elapsed time in hours
        float elapsed_hours = (current_time - last_update_time) / 3600000.0f;

        // Calculate distance traveled during this period (speed * time)
        float distance_km = (speed_kmh * elapsed_hours);

        // Add to total trip distance
        total_trip_km += distance_km;

        // Reset trip distance if it exceeds 999km
        if (total_trip_km > 999.0f) {
            ESP_LOGI(TAG, "Trip distance exceeded 999km, resetting to 0");
            total_trip_km = 0.0f;
        }
    }

    // Update last time
    last_update_time = current_time;

    if (!take_lvgl_mutex()) {
        ESP_LOGW(TAG, "Failed to take LVGL mutex for trip distance update");
        return;
    }

    // Only update if home screen is active
    if (get_current_screen() == ui_home_screen) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", total_trip_km);
        lv_label_set_text(ui_tripkm, buf);
        lv_obj_invalidate(ui_tripkm); // Force redraw
    }

    give_lvgl_mutex();
}

// Add a function to reset the trip distance
void ui_reset_trip_distance(void) {
    total_trip_km = 0.0f;

    if (!take_lvgl_mutex()) {
        ESP_LOGW(TAG, "Failed to take LVGL mutex for trip reset");
        return;
    }

    if (ui_tripkm != NULL && get_current_screen() == ui_home_screen) {
        lv_label_set_text(ui_tripkm, "0.0");
        lv_obj_invalidate(ui_tripkm); // Force redraw
    }

    give_lvgl_mutex();
}

// Add this function to save trip data to NVS
esp_err_t ui_save_trip_distance(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS
    err = nvs_open(TRIP_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS for trip data: %s", esp_err_to_name(err));
        return err;
    }

    // Save trip distance as a float (stored as a blob)
    err = nvs_set_blob(nvs_handle, NVS_KEY_TRIP_KM, &total_trip_km, sizeof(float));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving trip distance: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Trip distance saved: %.2f km", total_trip_km);
    }

    nvs_close(nvs_handle);
    return err;
}

// Add this function to load trip data from NVS
esp_err_t ui_load_trip_distance(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS
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

    // Get the size of the stored blob
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

// Add this function to initialize NVS for trip data
esp_err_t ui_init_trip_nvs(void) {
    // Check if namespace exists, if not create it
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(TRIP_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

// Add a function to check and reset mutex if needed
void ui_check_mutex_health(void) {
    static uint32_t last_check_time = 0;
    uint32_t current_time = esp_timer_get_time() / 1000000; // Current time in seconds

    // Check every 30 seconds
    if (current_time - last_check_time >= 30) {
        // Try to take the mutex with a very short timeout
        if (lvgl_mutex != NULL && xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(1)) != pdTRUE) {
            // Mutex might be stuck, recreate it
            ESP_LOGW(TAG, "LVGL mutex appears to be stuck, recreating");

            // Create a new mutex
            SemaphoreHandle_t new_mutex = xSemaphoreCreateMutex();
            if (new_mutex != NULL) {
                lvgl_mutex = new_mutex;

                // We can't delete the old mutex since it's stuck, but we can at least
                // replace it with a working one
                ESP_LOGW(TAG, "LVGL mutex replaced");
            } else {
                ESP_LOGE(TAG, "Failed to create new LVGL mutex");
            }
        } else if (lvgl_mutex != NULL) {
            // If we could take the mutex, give it back
            xSemaphoreGive(lvgl_mutex);
        }

        last_check_time = current_time;
    }
}

static void speed_update_task(void *pvParameters) {
    static vesc_config_t config;
    ESP_ERROR_CHECK(vesc_config_load(&config));

    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t frequency = pdMS_TO_TICKS(SPEED_UPDATE_MS);

    while (1) {
        vTaskDelayUntil(&last_wake_time, frequency);

        if (is_connect) {
            int32_t speed = vesc_config_get_speed(&config);
            if (speed >= 0 && speed <= 100) {  // Adjust max speed as needed
                ui_update_speed(speed);
            } else {
                ESP_LOGW(TAG, "Invalid speed value received: %ld", speed);
            }
        }
    }
}

static void trip_distance_update_task(void *pvParameters) {
    vesc_config_t config;
    ESP_ERROR_CHECK(vesc_config_load(&config));

    while (1) {
        int32_t speed = vesc_config_get_speed(&config);
        ui_update_trip_distance(speed);
        vTaskDelay(pdMS_TO_TICKS(TRIP_UPDATE_MS));
    }
}

static void battery_update_task(void *pvParameters) {
    while (1) {
        int battery_percentage = battery_get_percentage();
        if (battery_percentage >= 0) {
            ui_update_battery_percentage(battery_percentage);
        }

        if (is_connect) {
            int skate_battery_percentage = get_bms_battery_percentage();
            if (skate_battery_percentage >= 0) {
                ui_update_skate_battery_percentage(skate_battery_percentage);
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
    // Speed updates - High priority (4)
    xTaskCreate(speed_update_task, "speed_update",
                2048, NULL, 4, NULL);

    // Other tasks with lower priorities
    xTaskCreate(trip_distance_update_task, "trip_update", 2048, NULL, 3, NULL);
    xTaskCreate(battery_update_task, "battery_update", 2048, NULL, 2, NULL);
    xTaskCreate(connection_update_task, "conn_update", 2048, NULL, 2, NULL);
}


