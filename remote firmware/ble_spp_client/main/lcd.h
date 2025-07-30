#pragma once

#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"

// Display configuration
#define TFT_MOSI_PIN GPIO_NUM_10
#define TFT_SCLK_PIN GPIO_NUM_8
#define TFT_CS_PIN   GPIO_NUM_6
#define TFT_DC_PIN   GPIO_NUM_7
#define TFT_RST_PIN  GPIO_NUM_21

#define LV_HOR_RES_MAX 240
#define LV_VER_RES_MAX 320

// Function declarations
void lcd_init(void);
lv_obj_t* lcd_create_label(const char* initial_text);
void lcd_start_tasks(void);
void lcd_enable_update(void);
void lcd_disable_update(void);


