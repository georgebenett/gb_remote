#ifndef POWER_H
#define POWER_H

#include "driver/gpio.h"
#include <stdbool.h>

#define RESET_DEBOUNCE_TIME_MS 2000
#define INACTIVITY_TIMEOUT_MS (5 * 60 * 1000) // 5 minutes

#define SHUTDOWN_FEEDBACK_DELAY_MS 100
#define BUTTON_DEBOUNCE_DELAY_MS 50
#define BUTTON_POLL_INTERVAL_MS 10
#define NVS_RETRY_DELAY_MS 50
#define NVS_FLUSH_DELAY_MS 200

bool power_is_entering_off_mode(void);

void power_init(void);
void power_wait_for_power_button(void);
void power_reset_inactivity_timer(void);
void power_check_inactivity(bool is_ble_connected);
/** If on charging screen and USB disconnected, fades backlight and enters
 * sleep. Call from main loop. */
void power_check_charging_screen_usb(void);
void power_shutdown(void);

#endif // POWER_H
