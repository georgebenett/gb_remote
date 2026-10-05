#include "lcd.h"
#include "battery.h"
#include "ble.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "throttle.h"
#include "ui.h"
#include "ui_updater.h"
#include "vesc_config.h"

#define TAG "LCD"
static uint8_t current_backlight_pwm =
    0; // Track current backlight PWM duty (0-255)
static esp_lcd_panel_handle_t panel_handle = NULL;
static lv_display_t *disp;

// LVGL draw buffers: 64 rows of RGB565, two so drawing overlaps sending.
// Fewer, bigger chunks per frame; one SPI DMA transfer caps out at 32 KB,
// which 64 rows of the 240 px wide panel stay under.
#define LVGL_BUFFER_BYTES (LV_HOR_RES_MAX * 64 * 2)

/* The LVGL task sleeps until LVGL's next timer is due, at most this long, so
 * CONFIG_LV_DEF_REFR_PERIOD is the real frame cap rather than rounding up to
 * a fixed wake-up beat. */
#define LVGL_MAX_SLEEP_MS 10

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map);
static uint32_t lv_tick_ms(void);
static void lvgl_handler_task(void *pvParameters);

// The SPI transfer finished: LVGL may reuse the buffer it just sent.
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx) {
  lv_display_flush_ready(disp);
  return false;
}

void lcd_init(void) {

  spi_bus_config_t buscfg = {.mosi_io_num = TFT_MOSI_PIN,
                             .sclk_io_num = TFT_SCLK_PIN,
                             .miso_io_num = -1,
                             .quadwp_io_num = -1,
                             .quadhd_io_num = -1,
                             .max_transfer_sz = LVGL_BUFFER_BYTES};
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io_config = {
      .dc_gpio_num = TFT_DC_PIN,
      .cs_gpio_num = TFT_CS_PIN,
      .pclk_hz = 80 * 1000 * 1000,
      .spi_mode = 0,
      .trans_queue_depth = 10,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .on_color_trans_done = notify_lvgl_flush_ready,
  };

  esp_lcd_panel_io_handle_t io_handle;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle));

  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = TFT_RST_PIN,
      .rgb_endian = ESP_LCD_COLOR_SPACE_RGB,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

  ledc_timer_config_t ledc_timer = {.speed_mode = LEDC_MODE,
                                    .timer_num = LEDC_TIMER,
                                    .duty_resolution = LEDC_DUTY_RES,
                                    .freq_hz = LEDC_FREQUENCY,
                                    .clk_cfg = LEDC_AUTO_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

  ledc_channel_config_t ledc_channel = {.speed_mode = LEDC_MODE,
                                        .channel = LEDC_CHANNEL,
                                        .timer_sel = LEDC_TIMER,
                                        .intr_type = LEDC_INTR_DISABLE,
                                        .gpio_num = TFT_BL_PIN,
                                        .duty = 0,
                                        .hpoint = 0};
  ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
  current_backlight_pwm = 0;
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));

  vTaskDelay(pdMS_TO_TICKS(50));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
  ESP_ERROR_CHECK(
      esp_lcd_panel_mirror(panel_handle, true, true)); // 180 degree rotation
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));

  lv_init();

  void *buf1 = heap_caps_malloc(LVGL_BUFFER_BYTES, MALLOC_CAP_DMA);
  void *buf2 = heap_caps_malloc(LVGL_BUFFER_BYTES, MALLOC_CAP_DMA);
  if (buf1 == NULL || buf2 == NULL) {
    ESP_LOGE(TAG, "Failed to allocate display buffers - system cannot start");
    esp_restart();
  }

  disp = lv_display_create(LV_HOR_RES_MAX, LV_VER_RES_MAX);
  lv_display_set_offset(disp, LCD_OFFSET_X, LCD_OFFSET_Y);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_buffers(disp, buf1, buf2, LVGL_BUFFER_BYTES,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_tick_set_cb(lv_tick_ms); // 1 ms resolution, no periodic timer

  ui_updater_init();
  lcd_start_tasks();
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
  (void)d;
  /* The panel takes RGB565 high byte first; LVGL 9 dropped LV_COLOR_16_SWAP. */
  lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
  esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1,
                            area->y2 + 1, px_map);
  // lv_display_flush_ready() comes from notify_lvgl_flush_ready()
}

static uint32_t lv_tick_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_handler_task(void *pvParameters) {
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
  ESP_ERROR_CHECK(esp_task_wdt_reset());

  while (1) {
    uint32_t sleep_ms = LVGL_MAX_SLEEP_MS;
    esp_task_wdt_reset();

    SemaphoreHandle_t mutex = get_lvgl_mutex_handle();
    if (mutex != NULL && xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      sleep_ms = lv_timer_handler(); // until the next LVGL timer is due
      give_lvgl_mutex();
    } else {
      static uint32_t mutex_fail_count = 0;
      if (++mutex_fail_count % 100 == 0) {
        ESP_LOGW(TAG, "Failed to get LVGL mutex for handler (count: %lu)",
                 (unsigned long)mutex_fail_count);
      }
    }
    /* At least a tick, so lower-priority tasks on this core still run. */
    TickType_t ticks = pdMS_TO_TICKS(LV_MIN(sleep_ms, LVGL_MAX_SLEEP_MS));
    vTaskDelay(ticks > 0 ? ticks : 1);
  }
}

void lcd_start_tasks(void) {
  TaskHandle_t lvgl_handler_handle = NULL;
  BaseType_t result = xTaskCreatePinnedToCore(
      lvgl_handler_task, "lvgl_handler", 8192, NULL, 8, &lvgl_handler_handle,
      1 /* Bluetooth owns core 0; drawing gets core 1 */);
  if (result != pdPASS) {
    ESP_LOGE("LCD", "Failed to create lvgl_handler task");
  } else {
    ESP_LOGI("LCD", "lvgl_handler task created with priority 8 on CPU 1");
  }
  ui_start_update_tasks();
}

void lcd_set_backlight(uint8_t brightness) {
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, brightness));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
  current_backlight_pwm = brightness;
}

uint8_t lcd_get_backlight(void) { return current_backlight_pwm; }

void lcd_fade_backlight(uint8_t start, uint8_t end, uint16_t duration_ms) {
  if (start == end) {
    lcd_set_backlight(end);
    return;
  }

  const uint16_t num_steps = 100;
  const uint16_t step_delay_ms = duration_ms / num_steps;

  int16_t start_val = (int16_t)start;
  int16_t end_val = (int16_t)end;
  int16_t delta = end_val - start_val;

  for (uint16_t i = 0; i <= num_steps; i++) {
    int16_t current = start_val + (delta * i) / num_steps;

    if (current < 0)
      current = 0;
    if (current > 255)
      current = 255;

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, (uint8_t)current));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));

    if (i < num_steps) {
      vTaskDelay(pdMS_TO_TICKS(step_delay_ms));
    }
  }

  current_backlight_pwm = end;
}

uint8_t lcd_load_saved_brightness(void) {
  uint8_t brightness = LCD_BACKLIGHT_DEFAULT;
  nvs_handle_t nvs_handle;

  if (nvs_open("lcd_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
    uint8_t saved_brightness;
    if (nvs_get_u8(nvs_handle, "backlight", &saved_brightness) == ESP_OK) {
      brightness = saved_brightness;
      ESP_LOGI(TAG, "Loaded saved backlight brightness: %d%%",
               saved_brightness);
    }
    nvs_close(nvs_handle);
  }

  return brightness;
}

void lcd_fade_to_saved_brightness(void) {
  uint8_t target_brightness = lcd_load_saved_brightness();

  // Map percentage (1-100) to PWM duty (0-255)
  uint8_t target_pwm = (target_brightness * 255) / 100;
  uint8_t current_pwm = lcd_get_backlight();

  // Already at or very close to target – skip fade to avoid visible dip when
  // coming from charging screen (already at saved brightness).
  if (current_pwm == target_pwm ||
      (current_pwm > target_pwm && current_pwm - target_pwm <= 2) ||
      (target_pwm > current_pwm && target_pwm - current_pwm <= 2)) {
    lcd_set_backlight(target_pwm);
    return;
  }

  lcd_fade_backlight(current_pwm, target_pwm, LCD_BACKLIGHT_FADE_DURATION_MS);
}
