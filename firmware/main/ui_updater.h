#ifndef UI_UPDATER_H
#define UI_UPDATER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "screens.h"
#include "snake.h"
#include "ui.h"
#include <stdint.h>

// Task update intervals
#define SPEED_UPDATE_MS 50        // 20Hz for smooth speed display
#define BATTERY_UPDATE_MS 1000    // 1Hz for battery
#define CONNECTION_UPDATE_MS 3000 // 0.33Hz for connection icon

// Timing constants
#define UI_TASK_STARTUP_DELAY_MS 20 // Staggered task startup delay
#define MUTEX_RETRY_DELAY_MS 5      // Delay when mutex unavailable
#define SPLASH_SCREEN_DELAY_MS 50   // Post-splash delay
#define SPLASH_FADE_UP_DELAY_MS 120 // Allow first splash frame to render

// UI Command Queue for thread-safe UI updates
#define UI_CMD_QUEUE_SIZE 32

extern const lv_img_dsc_t img_battery_charging;
extern const lv_img_dsc_t img_battery;
extern const lv_img_dsc_t img_connection_0;
extern const lv_img_dsc_t img_33_connection;
extern const lv_img_dsc_t img_66_connection;
extern const lv_img_dsc_t img_100_connection;
extern const lv_img_dsc_t img_skateboard_icon_connected;
extern const lv_img_dsc_t img_skateboard_no_connection;
extern const lv_img_dsc_t img_no_connection;

void ui_updater_init(void);
void ui_update_speed(int32_t value);
void ui_update_battery_percentage(int percentage);
void ui_update_connection_icon(void);
void ui_reset_trip_distance(void);

/* Per-receiver updates. `receiver` is a slot in [0, BLE_MAX_RECEIVERS); on the
 * single-receiver home screen every slot maps onto the one set of widgets. */
void ui_update_trip_distance(int receiver, float trip_km_val);
void ui_update_skate_battery_percentage(int receiver, int percentage);
void ui_update_skate_battery_voltage_display(int receiver, float voltage);
/** Blank a receiver's battery readout (shown as "--" while disconnected). */
void ui_reset_skate_display(int receiver);

/** Point the home-screen bindings at the single- or dual-receiver layout and,
 *  when a home screen is already showing, swap to the other one. */
void ui_set_dual_home_screen(bool enabled);
/** The home screen for the active layout — use instead of objects.home_screen
 *  when loading or invalidating it. */
lv_obj_t *ui_get_home_screen(void);
/** Reveal the "throttle not calibrated" warning on the active home screen.
 *  Caller must hold the LVGL mutex. */
void ui_show_throttle_not_calibrated_text(void);

bool take_lvgl_mutex(void);
void give_lvgl_mutex(void);
SemaphoreHandle_t get_lvgl_mutex_handle(void);
void ui_start_update_tasks(void);
void ui_force_config_reload(void);
void ui_update_speed_unit(bool is_mph);

// Throttle calibration status
void ui_hide_throttle_not_calibrated_text(void);

// Aux output indicator
void ui_update_aux_output_indicator(void);

// Splash screen
void ui_show_splash_screen(void);
/** Show splash then auto-switch to home after 4s (for mode 1→2 transition).
 * Call with LVGL mutex held. */
void ui_show_splash_then_home(void);

// Easter egg: consume power-button events for splash combo and snake controls.
bool ui_handle_easter_egg_button_event(int event);

#endif // UI_UPDATER_H