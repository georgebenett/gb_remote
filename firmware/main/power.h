#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include "driver/gpio.h"

#define RESET_DEBOUNCE_TIME_MS 2000
//#define INACTIVITY_TIMEOUT_MS (5 * 60 * 1000)  // 5 minutes
#define INACTIVITY_TIMEOUT_MS INT32_MAX

extern volatile bool entering_power_off_mode;

void power_init(void);
void power_reset_inactivity_timer(void);
void power_check_inactivity(bool is_ble_connected);
void power_shutdown(void);

#endif // POWER_H

