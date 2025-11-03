#pragma once

#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "hw_config.h"
#include "target_config.h"

// Display configuration - now from target_config.h
#define LV_HOR_RES_MAX LCD_HOR_RES_MAX
#define LV_VER_RES_MAX LCD_VER_RES_MAX

//Display Backlight values
#define LCD_BACKLIGHT_MIN 0
#define LCD_BACKLIGHT_DIM 1
#define LCD_BACKLIGHT_DEFAULT 50
#define LCD_BACKLIGHT_MAX 100
#define LCD_BACKLIGHT_FADE_DURATION_MS 1000  // Default fade duration in milliseconds



// Function declarations
void lcd_init(void);
lv_obj_t* lcd_create_label(const char* initial_text);
void lcd_start_tasks(void);
void lcd_enable_update(void);
void lcd_disable_update(void);
void lcd_set_backlight(uint8_t brightness);  // Set backlight brightness (0-255, where 255 is full brightness)
void lcd_fade_backlight(uint8_t start, uint8_t end, uint16_t duration_ms);

