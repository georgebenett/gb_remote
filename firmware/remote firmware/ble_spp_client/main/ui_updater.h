#ifndef UI_UPDATER_H
#define UI_UPDATER_H

#include <stdint.h>
#include "ui/ui.h"
#include "esp_err.h"
// Function to initialize the UI updater
void ui_updater_init(void);

extern const lv_img_dsc_t ui_img_battery_charging_icon_png;
extern const lv_img_dsc_t ui_img_battery_icon_png;
extern const lv_img_dsc_t ui_img_no_connection_png;
extern const lv_img_dsc_t ui_img_33_connection_png;
extern const lv_img_dsc_t ui_img_66_connection_png;
extern const lv_img_dsc_t ui_img_full_connection_png;

// Add after the other extern declarations
extern lv_obj_t* ui_skate_battery_text;

// Functions to update specific UI elements
void ui_update_speed(int32_t value);
void ui_update_battery_percentage(int percentage);
void ui_update_battery_voltage(float voltage);
void ui_update_motor_current(float current);
void ui_update_battery_current(float current);
void ui_update_consumption(float consumption);
void ui_update_connection_quality(int rssi);
void ui_update_connection_icon(void);
void ui_update_trip_distance(int32_t speed_kmh);
void ui_reset_trip_distance(void);
void ui_update_skate_battery_percentage(int percentage);

esp_err_t ui_save_trip_distance(void);
esp_err_t ui_load_trip_distance(void);

// Add these function declarations
bool take_lvgl_mutex(void);
void give_lvgl_mutex(void);
void ui_check_mutex_health(void);
esp_err_t ui_init_trip_nvs(void);

// Add after the other function declarations
void ui_start_update_tasks(void);

// Add function to force configuration reload
void ui_force_config_reload(void);

// Add this function declaration
void ui_update_speed_unit(bool is_mph);

#endif // UI_UPDATER_H