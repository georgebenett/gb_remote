/**
 * @file main.c
 * @brief Main entry point for the GB Remote firmware
 */

#include "battery.h"
#include "ble.h"
#include "button.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "nvs_flash.h"
#include "power.h"
#include "throttle.h"
#include "ui.h"
#include "ui_updater.h"
#include "usb_serial.h"
#include "version.h"
#include "vesc_config.h"
#include "viber.h"

#define TAG "MAIN"

static void initialize_nvs(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
}

static void initialize_system(void) {
  initialize_nvs();

  if (vesc_config_init() != ESP_OK) {
    ESP_LOGW(TAG, "VESC config init failed, using defaults");
  }

  if (viber_init() != ESP_OK) {
    ESP_LOGW(TAG, "Viber init failed, haptic feedback disabled");
  }
}

static void initialize_ui(void) {
  ui_init();
  ui_create_aux_output_indicator();

  vesc_config_t config;
  esp_err_t err = vesc_config_load(&config);
  if (err == ESP_OK) {
    ui_update_speed_unit(config.speed_unit_mph);
    ESP_LOGI(TAG, "Speed unit set to: %s",
             config.speed_unit_mph ? "mph" : "km/h");
  } else {
    ESP_LOGW(TAG, "Failed to load speed unit config, using default km/h");
    ui_update_speed_unit(false);
  }
}

static void initialize_pre_boot_charging(void) {
  // Initialize the bare minimum needed to show the charging screen
  // before committing to a full boot (BLE, throttle, etc.)
  ESP_ERROR_CHECK(battery_init());
  battery_start_monitoring();
  lcd_init();
  initialize_ui();
  lcd_fade_to_saved_brightness();

  // Block here until the user holds the power button
  power_wait_for_power_button();
}

static void initialize_communication(void) {
  usb_serial_init();
  usb_serial_start_task();
  spp_client_demo_init();
  ESP_LOGI(TAG, "BLE initialization complete");
}

static void initialize_monitoring(void) {
  // battery_init and battery_start_monitoring are already called in
  // initialize_pre_boot_charging; just start the button monitoring task here
  button_start_monitoring();
}

static void log_startup_info(void) {
  ESP_LOGI(TAG, "Starting Application");
  ESP_LOGI(TAG, "Firmware version: %s", FW_VERSION);
  ESP_LOGI(TAG, "Build date: %s %s", BUILD_DATE, BUILD_TIME);
  ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);
  ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());
}

void app_main(void) {
  log_startup_info();

  // Power-on sequence (sleeps if button not held long enough)
  ESP_ERROR_CHECK(button_init_main());
  power_init();

  // Initialize all subsystems
  initialize_system();

  // Initialize ADC and throttle calibration first (needed early)
  ESP_ERROR_CHECK(adc_init());
  adc_start_task();

  // Early init for charging mode: battery + LCD + UI, then block until
  // the power button is held if the device wasn't booted by a button press
  initialize_pre_boot_charging();

  // Wait for throttle calibration to complete (adc_start_task is already running)
  while (!throttle_is_calibrated()) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  initialize_communication();
  initialize_monitoring();

  // Show splash and fade in
  ui_show_splash_screen();
  lcd_fade_to_saved_brightness();

  // Main loop: monitor for inactivity
  while (1) {
    power_check_inactivity(ble_is_connected());
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
