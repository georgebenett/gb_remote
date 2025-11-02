#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include "driver/gpio.h"

#define POWER_OFF_GPIO GPIO_NUM_4

// Add this global flag declaration
extern volatile bool entering_power_off_mode;

void power_init(void);
void power_start_monitoring(void);
void power_reset_inactivity_timer(void);
void power_check_inactivity(bool is_ble_connected);
void power_shutdown(void);

#endif // POWER_H

