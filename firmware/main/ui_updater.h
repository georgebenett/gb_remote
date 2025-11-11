#ifndef UI_UPDATER_H
#define UI_UPDATER_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "ui.h"
#include "screens.h"
void ui_updater_init(void);

extern const lv_img_dsc_t img_battery_charging;
extern const lv_img_dsc_t img_battery;
extern const lv_img_dsc_t img_connection_0;
extern const lv_img_dsc_t img_33_connection;
extern const lv_img_dsc_t img_66_connection;
extern const lv_img_dsc_t img_100_connection;

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

#endif // UI_UPDATER_H