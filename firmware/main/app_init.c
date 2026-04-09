/**
 * @file app_init.c
 * @brief Application initialization implementation
 */

#include "app_init.h"
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

#define TAG "APP_INIT"

/** Max time to wait for throttle calibration before proceeding (ms). */
#define CALIBRATION_WAIT_TIMEOUT_MS (10 * 60 * 1000)

static void init_nvs(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
}

static void init_system(void) {
  init_nvs();
  if (vesc_config_init() != ESP_OK) {
    ESP_LOGW(TAG, "VESC config init failed, using defaults");
  }
  if (viber_init() != ESP_OK) {
    ESP_LOGW(TAG, "Viber init failed, haptic feedback disabled");
  }
}

static void init_ui(void) {
  ui_init();
  ui_create_aux_output_indicator();
  vesc_config_t config;
  if (vesc_config_load(&config) == ESP_OK) {
    ui_update_speed_unit(config.speed_unit_mph);
    ESP_LOGI(TAG, "Speed unit: %s", config.speed_unit_mph ? "mph" : "km/h");
  } else {
    ui_update_speed_unit(false);
  }
}

void app_init_log_startup(void) {
  ESP_LOGI(TAG, "Starting Application");
  ESP_LOGI(TAG, "Firmware version: %s", FW_VERSION);
  ESP_LOGI(TAG, "Build date: %s %s", BUILD_DATE, BUILD_TIME);
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
  uint32_t wait_start = xTaskGetTickCount();
  while (!throttle_is_calibrated()) {
    if ((xTaskGetTickCount() - wait_start) >=
        pdMS_TO_TICKS(CALIBRATION_WAIT_TIMEOUT_MS)) {
      ESP_LOGW(TAG, "Calibration wait timeout, proceeding (throttle will stay "
                    "neutral until calibrated)");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  usb_serial_init();
  usb_serial_start_task();
  spp_client_demo_init();
  ESP_LOGI(TAG, "BLE initialization complete");
  button_start_monitoring();
  ui_show_splash_screen();
  viber_play_startup_song();
  lcd_fade_to_saved_brightness();
}
