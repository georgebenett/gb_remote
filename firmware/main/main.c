/**
 * @file main.c
 * @brief Main entry point for the GB Remote firmware
 *
 * Two power modes:
 * - Charging mode: minimal init (battery, LCD, UI). Show charging screen;
 *   block until long-press (→ full boot) or USB disconnect (→ sleep).
 * - Full mode: all subsystems (BLE, throttle, comms). Home/splash, inactivity
 *   timeout, charging screen USB monitoring.
 */

#include "app_init.h"
#include "ble.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power.h"

void app_main(void) {
  app_init_log_startup();
  app_init_early();
  app_run_charging_mode(); /* Blocks until full boot or sleep */

  app_init_after_charging();

  for (;;) {
    power_check_charging_screen_usb();
    power_check_inactivity(ble_is_connected());
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
