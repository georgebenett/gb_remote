/**
 * @file app_init.c
 * @brief Application initialization implementation
 */

#include "app_init.h"
#include "battery.h"
#include "ble.h"
#include "button.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "power.h"
#include "rx_ota.h"
#include "settings.h"
#include "throttle.h"
#include "ui.h"
#include "ui_updater.h"
#include "usb_serial.h"
#include "version.h"
#include "viber.h"

#define TAG "APP_INIT"

static void init_system(void) {
  esp_log_level_set("gpio", ESP_LOG_WARN);
  esp_pm_config_t pm = {.max_freq_mhz = 240, .min_freq_mhz = 80};
  if (esp_pm_configure(&pm) != ESP_OK) {
    ESP_LOGW(TAG, "Power management not configured, staying at 240 MHz");
  }
  if (settings_init() != ESP_OK) {
    ESP_LOGW(TAG, "Settings init failed, using defaults");
  }
  if (viber_init() != ESP_OK) {
    ESP_LOGW(TAG, "Viber init failed, haptic feedback disabled");
  }
}

static void init_ui(void) {
  /* lcd_init() already started the LVGL task on the other core. LVGL 9
   * allocates on every draw and its allocator isn't thread-safe, so build the
   * screens under the same mutex or the two corrupt its heap. */
  xSemaphoreTake(get_lvgl_mutex_handle(), portMAX_DELAY);
  ui_init();
  give_lvgl_mutex();
  settings_t config;
  settings_get(&config);
  // Bind the home-screen widgets before anything tries to update them.
  ui_set_dual_home_screen(config.dual_connection);
  ui_update_aux_output_indicator();
  ui_update_speed_unit(config.speed_unit_mph);
}

void app_init_log_startup(void) {
  ESP_LOGI(TAG, "Starting Application");
  ESP_LOGI(TAG, "Firmware version: %s", FW_VERSION);
  ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);
  ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());
}

void app_init_early(void) {
  ESP_ERROR_CHECK(button_init_main());
  power_init();
  init_system();
  ESP_ERROR_CHECK(adc_init());
  adc_start_task();
  button_start_monitoring();
}

void app_run_charging_mode(void) {
  ESP_ERROR_CHECK(battery_init());
  battery_start_monitoring();
  lcd_init();
  init_ui();
  usb_serial_init();
  usb_serial_start_task();
  power_run_charging_mode(); /* Blocks until long-press, USB boot cmd, or USB
                                disconnect */
}

void app_init_after_charging(void) {
  if (!throttle_is_calibrated()) {
    ESP_LOGW(TAG, "No throttle calibration, proceeding uncalibrated");
  }
  rx_ota_init();
  ble_init();
  ESP_LOGI(TAG, "BLE initialization complete");
  xSemaphoreTake(get_lvgl_mutex_handle(), portMAX_DELAY);
  ui_show_splash_screen();
  give_lvgl_mutex();
  viber_play_startup_song();
}
