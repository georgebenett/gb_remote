#pragma once

// Target configuration header
// This file provides hardware target-specific configuration based on Kconfig settings

#include "sdkconfig.h"

// LCD Display Configuration
#define LCD_HOR_RES_MAX CONFIG_LCD_HOR_RES
#define LCD_VER_RES_MAX CONFIG_LCD_VER_RES
#define LCD_OFFSET_X CONFIG_LCD_OFFSET_X
#define LCD_OFFSET_Y CONFIG_LCD_OFFSET_Y

// Target identification
#ifdef CONFIG_TARGET_DUAL_THROTTLE
#define TARGET_NAME "dual_throttle"
#elif defined(CONFIG_TARGET_LITE)
#define TARGET_NAME "lite"
#else
#define TARGET_NAME "unknown"
#endif

// Throttle logic configuration can be added here if needed
// For example:
// #ifdef CONFIG_TARGET_DUAL_THROTTLE
//   // dual_throttle throttle settings
// #elif defined(CONFIG_TARGET_LITE)
//   // lite throttle settings
// #endif

