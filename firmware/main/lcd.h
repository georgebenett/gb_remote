#pragma once

#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "hw_config.h"
#include "target_config.h"

// Display configuration - now from target_config.h
#define LV_HOR_RES_MAX                  LCD_HOR_RES_MAX
#define LV_VER_RES_MAX                  LCD_VER_RES_MAX

//Display Backlight values
#define LCD_BACKLIGHT_MIN               0
#define LCD_BACKLIGHT_DEFAULT           50
#define LCD_BACKLIGHT_MAX               100
#define LCD_BACKLIGHT_FADE_DURATION_MS  1000  // Default fade duration in milliseconds

// Backlight LEDC configuration
#define LEDC_TIMER                      LEDC_TIMER_0
#define LEDC_MODE                       LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL                    LEDC_CHANNEL_0
#define LEDC_DUTY_RES                   LEDC_TIMER_8_BIT  // 8-bit resolution (0-255)
#define LEDC_FREQUENCY                  5000  // 5kHz frequency

// Function declarations
void lcd_init(void);
lv_obj_t* lcd_create_label(const char* initial_text);
void lcd_start_tasks(void);
void lcd_enable_update(void);
void lcd_disable_update(void);
void lcd_set_backlight(uint8_t brightness);  // Set backlight brightness (0-255, where 255 is full brightness)
uint8_t lcd_get_backlight(void);             // Get current backlight PWM duty (0-255)
void lcd_fade_backlight(uint8_t start, uint8_t end, uint16_t duration_ms);
uint8_t lcd_load_saved_brightness(void);     // Load backlight brightness from NVS (returns default if not found)
void lcd_fade_to_saved_brightness(void);     // Fade backlight to saved/default brightness

