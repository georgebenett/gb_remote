#ifndef POWER_H
#define POWER_H

#include "driver/gpio.h"
#include <stdbool.h>

/** Power mode: charging-only (minimal init) or full (BLE, throttle, etc.). */
typedef enum {
  POWER_MODE_CHARGING, /**< Charging screen only; minimal subsystems. */
  POWER_MODE_FULL      /**< Full application; all subsystems running. */
} power_mode_t;

#define RESET_DEBOUNCE_TIME_MS 2000
#define INACTIVITY_TIMEOUT_MS (5 * 60 * 1000) /* 5 minutes */

#define SHUTDOWN_ANIMATION_TIME 650
#define SHUTDOWN_FEEDBACK_DELAY_MS 100
#define BUTTON_DEBOUNCE_DELAY_MS 50
#define BUTTON_POLL_INTERVAL_MS 10
#define LOW_BATTERY_ALERT_DELAY_MS 500 // Haptic feedback delay
#define LOW_BATTERY_WARNING_MS 2000    // Show warning before shutdown
#define POWER_OFF_SETTLE_MS 100        // Delay after power pin toggle

bool power_is_entering_off_mode(void);
power_mode_t power_get_mode(void);

/** Must be called first. Handles wake-from-sleep; holds power on. */
void power_init(void);

/**
 * Run charging mode: show charging screen and block until long-press (proceed
 * to full) or USB disconnect (sleep). Returns immediately if USB not connected
 * or button already held (caller should then do full init).
 * Requires: button task running, minimal init (battery, lcd, ui) done.
 */
void power_run_charging_mode(void);

void power_reset_inactivity_timer(void);
void power_check_inactivity(bool is_ble_connected);

/** Battery critical → low battery screen and power off, without the BLE
 *  shutdown command, so the receiver's link-loss failsafe brakes. */
void power_check_battery(void);

/** When in full mode with charging screen visible, USB disconnect → sleep. */
void power_check_charging_screen_usb(void);

void power_shutdown(void);

/** Signal charging mode loop to exit and proceed to full boot (e.g. from USB
 * command). */
void power_request_full_boot(void);

#endif /* POWER_H */
