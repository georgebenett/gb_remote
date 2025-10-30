#pragma once

#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"

// Display configuration

#define TFT_MOSI_PIN GPIO_NUM_17
#define TFT_SCLK_PIN GPIO_NUM_16
#define TFT_CS_PIN   GPIO_NUM_15
#define TFT_DC_PIN   GPIO_NUM_14
#define TFT_RST_PIN  GPIO_NUM_18
#define TFT_BL_PIN   GPIO_NUM_13

#define LV_HOR_RES_MAX 172   
#define LV_VER_RES_MAX 320

// Function declarations
void lcd_init(void);
lv_obj_t* lcd_create_label(const char* initial_text);
void lcd_start_tasks(void);
void lcd_enable_update(void);
void lcd_disable_update(void);
void lcd_set_backlight(uint8_t brightness);  // Set backlight brightness (0-255, where 255 is full brightness)


