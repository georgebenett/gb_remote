#ifndef UI_UPDATER_H
#define UI_UPDATER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "screens.h"
#include "ui.h"
#include <stdint.h>

// Task update intervals
#define SPEED_UPDATE_MS 50        // 20Hz for smooth speed display
#define TRIP_UPDATE_MS 1000       // 1Hz for distance
#define BATTERY_UPDATE_MS 1000    // 1Hz for battery
#define CONNECTION_UPDATE_MS 3000 // 0.33Hz for connection icon

// Timing constants
#define TASK_STARTUP_DELAY_MS 100   // Staggered task startup delay
#define MUTEX_RETRY_DELAY_MS 5      // Delay when mutex unavailable
#define SPLASH_SCREEN_DELAY_MS 1000 // Post-splash delay

// UI Command Queue for thread-safe UI updates
#define UI_CMD_QUEUE_SIZE 32

extern const lv_img_dsc_t img_battery_charging;
extern const lv_img_dsc_t img_battery;
extern const lv_img_dsc_t img_connection_0;
extern const lv_img_dsc_t img_33_connection;
extern const lv_img_dsc_t img_66_connection;
extern const lv_img_dsc_t img_100_connection;

void ui_updater_init(void);
void ui_update_speed(int32_t value);
void ui_update_battery_percentage(int percentage);
void ui_update_motor_current(float current);
void ui_update_battery_current(float current);
void ui_update_consumption(float consumption);
void ui_update_connection_quality(int rssi);
void ui_update_connection_icon(void);
void ui_update_trip_distance(int32_t speed_kmh);
void ui_reset_trip_distance(void);
void ui_update_skate_battery_percentage(int percentage);
void ui_update_skate_battery_voltage_display(float voltage);

esp_err_t ui_save_trip_distance(void);
esp_err_t ui_load_trip_distance(void);

bool take_lvgl_mutex(void);
bool take_lvgl_mutex_for_handler(void);
void give_lvgl_mutex(void);
SemaphoreHandle_t get_lvgl_mutex_handle(void);
void ui_check_mutex_health(void);
esp_err_t ui_init_trip_nvs(void);
void ui_start_update_tasks(void);
void ui_force_config_reload(void);
void ui_update_speed_unit(bool is_mph);

// Aux output indicator
void ui_create_aux_output_indicator(void);
void ui_update_aux_output_indicator(void);

// Splash screen
void ui_show_splash_screen(void);
/** Show splash then auto-switch to home after 4s (for mode 1→2 transition).
 * Call with LVGL mutex held. */
void ui_show_splash_then_home(void);

#endif // UI_UPDATER_H