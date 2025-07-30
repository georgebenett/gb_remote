#ifndef SLEEP_H
#define SLEEP_H

#include <stdbool.h>
#include "esp_sleep.h"
#include "button.h"

#define INACTIVITY_TIMEOUT_MS 240000  //4 minutes

// Add this global flag declaration
extern volatile bool entering_sleep_mode;

void sleep_init(void);
void sleep_start_monitoring(void);
void sleep_reset_inactivity_timer(void);
void sleep_check_inactivity(bool is_ble_connected);
void sleep_enter_deep_sleep(void);

#endif // SLEEP_H