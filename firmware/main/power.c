#include "power.h"
#include "ble.h"
#include "button.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_config.h"
#include "lcd.h"
#include "lvgl.h"
#include "nvs.h"
#include "ui.h"
#include "ui_updater.h"
#include "viber.h"

#define TAG "POWER"

static TickType_t last_activity_time;
static TickType_t last_reset_time = 0;

static lv_anim_t arc_anim;
static bool arc_animation_active = false;

// Shared flag for power-off state (volatile ensures visibility across tasks)
static volatile bool entering_power_off_mode = false;

bool power_is_entering_off_mode(void) { return entering_power_off_mode; }

static bool button_released_since_boot = false;

// Forward declarations
static bool power_check_wake_from_sleep(void);
static void power_sleep_immediate(void);

static void set_bar_value(void *obj, int32_t v) {
  if (entering_power_off_mode) {
    return;
  }

  lv_bar_set_value(obj, v, LV_ANIM_OFF);

  if (v >= 100) {
    ESP_LOGI(TAG, "Bar filled - Shutting down");
    viber_play_pattern(VIBER_PATTERN_DOUBLE_SHORT);
    entering_power_off_mode = true;
    vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_FEEDBACK_DELAY_MS));
    power_shutdown();
  }
}

static void power_button_callback(button_event_t event, void *user_data) {
  static bool long_press_triggered = false;

  switch (event) {
  case BUTTON_EVENT_PRESSED:
    long_press_triggered = false;
    break;

  case BUTTON_EVENT_RELEASED:
    button_released_since_boot = true;

    if (arc_animation_active) {
      if (take_lvgl_mutex()) {
        lv_anim_del(objects.shutting_down_bar, set_bar_value);
        lv_bar_set_value(objects.shutting_down_bar, 0, LV_ANIM_OFF);
        arc_animation_active = false;
        lv_disp_load_scr(objects.home_screen);
        lv_obj_invalidate(objects.home_screen);
        give_lvgl_mutex();
      } else {
        arc_animation_active = false;
      }
    }
    long_press_triggered = false;
    break;

  case BUTTON_EVENT_LONG_PRESS:
    if (!button_released_since_boot) {
      ESP_LOGI(TAG,
               "Long press ignored - button must be released first after boot");
      break;
    }

    if (!long_press_triggered) {
      long_press_triggered = true;
      if (take_lvgl_mutex()) {
        lv_disp_load_scr(objects.shutdown_screen);
        lv_obj_invalidate(objects.shutdown_screen);
        lv_anim_init(&arc_anim);
        lv_anim_set_var(&arc_anim, objects.shutting_down_bar);
        lv_anim_set_exec_cb(&arc_anim, set_bar_value);
        lv_anim_set_time(&arc_anim, 2000);
        lv_anim_set_values(&arc_anim, 0, 100);
        lv_anim_start(&arc_anim);
        arc_animation_active = true;
        give_lvgl_mutex();
      }
    }
    break;

  case BUTTON_EVENT_DOUBLE_PRESS:
    // Toggle auxiliary output on double press (only if BLE connected)
    if (ble_is_connected()) {
      ble_toggle_aux_output();
      ui_update_aux_output_indicator();
      viber_play_pattern(VIBER_PATTERN_SINGLE_SHORT);
    }
    break;
  }
}

static bool power_check_wake_from_sleep(void) {
  esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();

  if (wakeup_cause == ESP_SLEEP_WAKEUP_GPIO) {
    ESP_LOGI(TAG, "Woke from deep sleep via GPIO");

    gpio_config_t button_conf = {.pin_bit_mask = (1ULL << MAIN_BUTTON_GPIO),
                                 .mode = GPIO_MODE_INPUT,
                                 .pull_up_en = GPIO_PULLUP_ENABLE,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&button_conf);

    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_DELAY_MS));

    bool button_pressed = (gpio_get_level(MAIN_BUTTON_GPIO) == 0);

    if (button_pressed) {
      ESP_LOGI(TAG, "Button is pressed after wake, waiting for long press");

      TickType_t start_time = xTaskGetTickCount();
      const TickType_t long_press_ticks =
          pdMS_TO_TICKS(BUTTON_LONG_PRESS_TIME_MS);

      while ((xTaskGetTickCount() - start_time) < long_press_ticks) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
        button_pressed = (gpio_get_level(MAIN_BUTTON_GPIO) == 0);

        if (!button_pressed) {
          ESP_LOGI(TAG,
                   "Button released before long press - going back to sleep");
          return false; // Button released too early, go back to sleep
        }
      }

      // Button was held for long press duration
      ESP_LOGI(TAG, "Long press detected - turning device on");
      return true; // Device should turn on
    } else {
      ESP_LOGI(TAG, "Button not pressed after wake - going back to sleep");
      return false; // Button not pressed, go back to sleep
    }
  }

  return true; // Not waking from sleep, normal boot
}

void power_init(void) {
  // Check wake reason first - go back to sleep if button wasn't held long
  // enough
  if (!power_check_wake_from_sleep()) {
    power_sleep_immediate();
    // Never returns
  }

  // Button held long enough, proceed with power-on
  gpio_config_t POWER_HOLD_GPIO_conf = {.pin_bit_mask =
                                            (1ULL << POWER_HOLD_GPIO),
                                        .mode = GPIO_MODE_OUTPUT,
                                        .pull_up_en = GPIO_PULLUP_DISABLE,
                                        .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                        .intr_type = GPIO_INTR_DISABLE};
  ESP_ERROR_CHECK(gpio_config(&POWER_HOLD_GPIO_conf));
  ESP_ERROR_CHECK(gpio_set_level(POWER_HOLD_GPIO, 1));

  button_register_callback(power_button_callback, NULL);

  last_activity_time = xTaskGetTickCount();
}

void power_reset_inactivity_timer(void) {
  TickType_t current_time = xTaskGetTickCount();

  if ((current_time - last_reset_time) * portTICK_PERIOD_MS >=
      RESET_DEBOUNCE_TIME_MS) {
    last_activity_time = current_time;
    last_reset_time = current_time;
  }
}

void power_check_inactivity(bool is_ble_connected) {
  TickType_t current_time = xTaskGetTickCount();
  TickType_t elapsed_time =
      (current_time - last_activity_time) * portTICK_PERIOD_MS;

  if (!is_ble_connected && button_released_since_boot &&
      elapsed_time >= INACTIVITY_TIMEOUT_MS) {
    ESP_LOGI(TAG, "Inactivity timeout reached (%u ms) - shutting down",
             (unsigned int)elapsed_time);
    power_shutdown();
  }
}

static void power_enter_sleep(void) {
  ESP_LOGI(TAG, "Configuring button GPIO %d as wake-up source",
           MAIN_BUTTON_GPIO);

  gpio_config_t button_conf = {.pin_bit_mask = (1ULL << MAIN_BUTTON_GPIO),
                               .mode = GPIO_MODE_INPUT,
                               .pull_up_en = GPIO_PULLUP_ENABLE,
                               .pull_down_en = GPIO_PULLDOWN_DISABLE,
                               .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&button_conf);

  esp_sleep_enable_gpio_wakeup();
  gpio_wakeup_enable(MAIN_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
  gpio_set_level(POWER_HOLD_GPIO, 0);
  esp_deep_sleep_start();
}

void power_shutdown(void) {
  ESP_LOGI(TAG, "Preparing for shutdown");

  uint8_t current_brightness = LCD_BACKLIGHT_DEFAULT;
  nvs_handle_t nvs_handle;
  if (nvs_open("lcd_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
    uint8_t saved_brightness;
    if (nvs_get_u8(nvs_handle, "backlight", &saved_brightness) == ESP_OK) {
      current_brightness = saved_brightness;
    }
    nvs_close(nvs_handle);
  }

  uint8_t current_pwm = (current_brightness * 255) / 100;
  uint8_t min_pwm = (LCD_BACKLIGHT_MIN * 255) / 100;
  lcd_fade_backlight(current_pwm, min_pwm, LCD_BACKLIGHT_FADE_DURATION_MS);

  // Save trip distance with retry on failure
  esp_err_t err = ui_save_trip_distance();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save trip distance: %s, retrying...",
             esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(NVS_RETRY_DELAY_MS));
    err = ui_save_trip_distance();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Retry failed: %s", esp_err_to_name(err));
    }
  }

  // Allow sufficient time for NVS flash operations to complete
  vTaskDelay(pdMS_TO_TICKS(NVS_FLUSH_DELAY_MS));

  power_enter_sleep();
}

static void power_sleep_immediate(void) {
  ESP_LOGI(TAG, "Going to sleep immediately (wake-up check failed)");
  power_enter_sleep();
}
