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
#include "ui.h"
#include "ui_updater.h"
#include "viber.h"

#define TAG "POWER"

/* --------------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */

static TickType_t last_activity_time;
static TickType_t last_reset_time = 0;
static power_mode_t current_mode = POWER_MODE_FULL;

static lv_anim_t arc_anim;
static bool arc_animation_active = false;
static volatile bool entering_power_off_mode = false;
static bool button_released_since_boot = false;
static bool shutdown_armed = false;

/** Set by button callback when long-press in charging mode; breaks charging
 * loop. */
static volatile bool charging_mode_long_press_received = false;

/** Set by USB command handler to request full boot from charging mode. */
static volatile bool charging_mode_usb_boot_requested = false;

bool power_is_entering_off_mode(void) { return entering_power_off_mode; }
power_mode_t power_get_mode(void) { return current_mode; }

/* --------------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */
static bool power_check_wake_from_sleep(void);
static void power_sleep_immediate(void);
static void power_enter_sleep(void);

/* One-shot timer callback: fade backlight up after charging screen is drawn */
static void charging_screen_fade_up_timer_cb(lv_timer_t *timer) {
  (void)timer;
  lcd_fade_to_saved_brightness();
}

/* --------------------------------------------------------------------------
 * Shutdown bar animation (full mode): USB connected → charging screen
 * ----------------------------------------------------------------------- */
static void set_bar_value(void *obj, int32_t v) {
  if (entering_power_off_mode) {
    return;
  }

  lv_bar_set_value(obj, v, LV_ANIM_OFF);

  if (v >= 100) {

    viber_play_pattern(VIBER_PATTERN_DOUBLE_SHORT);
    bool usb_connected = (gpio_get_level(USB_DETECT_GPIO) == 1);
    if (usb_connected) {

      arc_animation_active = false;
      current_mode = POWER_MODE_CHARGING;
      ble_enter_charging_mode();
      vTaskDelay(pdMS_TO_TICKS(1000));
      lv_bar_set_value(objects.shutting_down_bar, 0, LV_ANIM_OFF);
      lcd_fade_backlight(lcd_get_backlight(), 0,
                         LCD_BACKLIGHT_FADE_DURATION_MS);
      lv_disp_load_scr(objects.charging_screen);
      lv_obj_invalidate(objects.charging_screen);
      /* One-shot timer: let LVGL draw the charging screen, then fade up */
      lv_timer_t *t =
          lv_timer_create(charging_screen_fade_up_timer_cb, 300, NULL);
      lv_timer_set_repeat_count(t, 1);
      return;
    }
    ESP_LOGI(TAG, "Bar filled - USB not connected - Shutting down");
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
        shutdown_armed = false;
        lv_obj_add_flag(objects.power_lock, LV_OBJ_FLAG_HIDDEN);
        lv_disp_load_scr(objects.home_screen);
        lv_obj_invalidate(objects.home_screen);
        give_lvgl_mutex();
      } else {
        arc_animation_active = false;
        shutdown_armed = false;
      }
    }
    long_press_triggered = false;
    break;

  case BUTTON_EVENT_POWER_OFF_ARMED:
    // Step 1: single tap confirmed — arm and show power lock icon
    if (current_mode == POWER_MODE_CHARGING)
      break;
    if (!button_released_since_boot)
      break;
    shutdown_armed = true;
    if (take_lvgl_mutex()) {
      lv_obj_clear_flag(objects.power_lock, LV_OBJ_FLAG_HIDDEN);
      give_lvgl_mutex();
    }
    break;

  case BUTTON_EVENT_POWER_OFF_CANCELLED:
    // Arm window expired without hold — disarm and hide power lock icon
    shutdown_armed = false;
    if (take_lvgl_mutex()) {
      lv_obj_add_flag(objects.power_lock, LV_OBJ_FLAG_HIDDEN);
      give_lvgl_mutex();
    }
    break;

  case BUTTON_EVENT_LONG_PRESS:
    /* Charging mode: long-press = switch on (full boot) */
    if (current_mode == POWER_MODE_CHARGING) {
      charging_mode_long_press_received = true;
      current_mode = POWER_MODE_FULL;
      power_reset_inactivity_timer();
      ble_leave_charging_mode();
      if (take_lvgl_mutex()) {
        ui_show_splash_then_home();
        give_lvgl_mutex();
      }
      break;
    }
    if (!button_released_since_boot) {
      ESP_LOGI(TAG,
               "Long press ignored - button must be released first after boot");
      break;
    }

    /* Step 2: long press while armed → show shutdown screen + start bar */
    if (!long_press_triggered && shutdown_armed) {
      long_press_triggered = true;
      shutdown_armed = false;
      if (take_lvgl_mutex()) {
        lv_disp_load_scr(objects.shutdown_screen);
        lv_obj_invalidate(objects.shutdown_screen);
        lv_anim_init(&arc_anim);
        lv_anim_set_var(&arc_anim, objects.shutting_down_bar);
        lv_anim_set_exec_cb(&arc_anim, set_bar_value);
        lv_anim_set_time(&arc_anim, SHUTDOWN_ANIMATION_TIME);
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

/* --------------------------------------------------------------------------
 * Wake-from-sleep: require long-press to stay on
 * ----------------------------------------------------------------------- */
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
      // Restore button to interrupt mode for when button task starts later
      gpio_config_t button_restore = {.pin_bit_mask =
                                          (1ULL << MAIN_BUTTON_GPIO),
                                      .mode = GPIO_MODE_INPUT,
                                      .pull_up_en = GPIO_PULLUP_ENABLE,
                                      .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                      .intr_type = GPIO_INTR_ANYEDGE};
      gpio_config(&button_restore);
      return true; // Device should turn on
    } else {
      ESP_LOGI(TAG, "Button not pressed after wake - going back to sleep");
      return false; // Button not pressed, go back to sleep
    }
  }

  return true; /* Not waking from sleep, normal boot */
}

/* --------------------------------------------------------------------------
 * Charging mode: block until long-press (full boot) or USB disconnect (sleep)
 * ----------------------------------------------------------------------- */

void power_run_charging_mode(void) {
  gpio_config_t usb_conf = {
      .pin_bit_mask = (1ULL << USB_DETECT_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&usb_conf);
  vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_DELAY_MS));

  bool usb_connected = (gpio_get_level(USB_DETECT_GPIO) == 1);
  bool button_held = (gpio_get_level(MAIN_BUTTON_GPIO) == 0);

  if (!usb_connected || button_held) {
    ESP_LOGI(TAG, "Full boot (USB=%d, BTN=%d)", usb_connected, button_held);
    return;
  }

  current_mode = POWER_MODE_CHARGING;
  charging_mode_long_press_received = false;
  charging_mode_usb_boot_requested = false;
  ESP_LOGI(TAG, "Charging mode: USB connected, button not held");

  if (take_lvgl_mutex()) {
    lv_disp_load_scr(objects.charging_screen);
    lv_obj_invalidate(objects.charging_screen);
    give_lvgl_mutex();
  }
  vTaskDelay(pdMS_TO_TICKS(800));
  lcd_fade_to_saved_brightness();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));

    if (gpio_get_level(USB_DETECT_GPIO) == 0) {
      current_mode = POWER_MODE_FULL;
      ESP_LOGI(TAG, "Charging mode: USB disconnected - powering off");
      lcd_fade_backlight(lcd_get_backlight(), 0,
                         LCD_BACKLIGHT_FADE_DURATION_MS);
      power_enter_sleep();
      /* never returns */
    }

    if (charging_mode_long_press_received) {
      current_mode = POWER_MODE_FULL;
      ESP_LOGI(TAG, "Charging mode: long press - proceeding to full boot");
      return;
    }

    if (charging_mode_usb_boot_requested) {
      current_mode = POWER_MODE_FULL;
      ESP_LOGI(TAG,
               "Charging mode: USB boot requested - proceeding to full boot");
      return;
    }
  }
}

void power_request_full_boot(void) { charging_mode_usb_boot_requested = true; }

/* --------------------------------------------------------------------------
 * Power hold and button callback
 * ----------------------------------------------------------------------- */

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
  /* No timeout while charging screen is visible */
  if (lv_scr_act() == objects.charging_screen) {
    return;
  }

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

/* Full mode: when charging screen is visible (e.g. after bar filled + USB),
 * USB disconnect → sleep. */
void power_check_charging_screen_usb(void) {
  if (lv_scr_act() != objects.charging_screen) {
    return;
  }
  if (gpio_get_level(USB_DETECT_GPIO) != 0) {
    return;
  }
  ESP_LOGI(TAG, "USB disconnected on charging screen - powering off");

  lcd_fade_backlight(lcd_get_backlight(), 0, LCD_BACKLIGHT_FADE_DURATION_MS);
  power_enter_sleep();
  /* never returns */
}

/* --------------------------------------------------------------------------
 * Sleep / shutdown
 * ----------------------------------------------------------------------- */
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

  viber_play_shutdown_song();

  uint8_t current_pwm = lcd_get_backlight();
  uint8_t min_pwm = (LCD_BACKLIGHT_MIN * 255) / 100;
  lcd_fade_backlight(current_pwm, min_pwm, LCD_BACKLIGHT_FADE_DURATION_MS);

  // Allow sufficient time for any pending NVS operations to complete
  vTaskDelay(pdMS_TO_TICKS(NVS_FLUSH_DELAY_MS));

  power_enter_sleep();
}

static void power_sleep_immediate(void) {
  ESP_LOGI(TAG, "Going to sleep immediately (wake-up check failed)");
  power_enter_sleep();
}
