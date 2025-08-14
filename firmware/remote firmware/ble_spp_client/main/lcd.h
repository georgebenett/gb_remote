#pragma once

#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"

// Display configuration
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define TFT_MOSI_PIN GPIO_NUM_10
#define TFT_SCLK_PIN GPIO_NUM_8
#define TFT_CS_PIN   GPIO_NUM_6
#define TFT_DC_PIN   GPIO_NUM_7
#define TFT_RST_PIN  GPIO_NUM_21
#define TFT_GND_PIN GPIO_NUM_20
#define TFT_VCC_PIN GPIO_NUM_9

#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define TFT_MOSI_PIN GPIO_NUM_40
#define TFT_SCLK_PIN GPIO_NUM_41
#define TFT_CS_PIN   GPIO_NUM_6
#define TFT_DC_PIN   GPIO_NUM_7
#define TFT_RST_PIN  GPIO_NUM_39
#define TFT_GND_PIN GPIO_NUM_45
#define TFT_VCC_PIN GPIO_NUM_47
#endif


#define LV_HOR_RES_MAX 240
#define LV_VER_RES_MAX 320

// Function declarations
void lcd_init(void);
lv_obj_t* lcd_create_label(const char* initial_text);
void lcd_start_tasks(void);
void lcd_enable_update(void);
void lcd_disable_update(void);


