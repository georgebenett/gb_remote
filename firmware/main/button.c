#include "button.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_config.h"
#include <string.h>

static const char *TAG = "BUTTON";

typedef struct {
  button_callback_t callback;
  void *user_data;
  bool in_use;
} button_callback_entry_t;

static button_config_t button_cfg;
static TickType_t press_start_time = 0;
static TickType_t last_release_time = 0;
static bool first_press_registered = false;
static TaskHandle_t button_task_handle = NULL;
static button_callback_entry_t callbacks[MAX_CALLBACKS] = {0};
static volatile bool isr_triggered = false;

// ISR handler - runs in IRAM, minimal work
static void IRAM_ATTR button_isr_handler(void *arg) {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  isr_triggered = true;
  if (button_task_handle != NULL) {
    vTaskNotifyGiveFromISR(button_task_handle, &xHigherPriorityTaskWoken);
  }
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void notify_callbacks(button_event_t event) {
  for (int i = 0; i < MAX_CALLBACKS; i++) {
    if (callbacks[i].in_use && callbacks[i].callback) {
      callbacks[i].callback(event, callbacks[i].user_data);
    }
  }
}

void button_register_callback(button_callback_t callback, void *user_data) {
  for (int i = 0; i < MAX_CALLBACKS; i++) {
    if (!callbacks[i].in_use) {
      callbacks[i].callback = callback;
      callbacks[i].user_data = user_data;
      callbacks[i].in_use = true;
      return;
    }
  }
  ESP_LOGW(TAG, "No free callback slots available");
}

static bool read_button_state(void) {
  bool state = gpio_get_level(button_cfg.gpio_num);
  return button_cfg.active_low ? !state : state;
}

static void button_monitor_task(void *pvParameters) {
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  bool button_pressed = false;
  bool long_press_sent = false;
  bool power_off_armed = false;
  bool second_press_candidate = false;
  TickType_t arm_start_time = 0;

  // On startup, if button is already pressed, wait for it to be released first
  if (read_button_state()) {
    while (read_button_state()) {
      vTaskDelay(pdMS_TO_TICKS(50));
      esp_task_wdt_reset(); // Keep watchdog happy while waiting
    }
    notify_callbacks(BUTTON_EVENT_RELEASED);
    vTaskDelay(pdMS_TO_TICKS(100));
  } else {
    notify_callbacks(BUTTON_EVENT_RELEASED);
  }

  gpio_install_isr_service(0);
  gpio_isr_handler_add(button_cfg.gpio_num, button_isr_handler, NULL);

  ESP_LOGI(TAG, "Button interrupt handler installed on GPIO %d",
           button_cfg.gpio_num);

  while (1) {
    // Wait for interrupt or timeout (for long press detection while held).
    // Also wake up to service the double-press expiry and arm-window timeout.
    TickType_t wait_time;
    if (button_pressed) {
      wait_time = pdMS_TO_TICKS(LONG_PRESS_CHECK_MS);
    } else if (first_press_registered) {
      uint32_t elapsed_ms =
          (xTaskGetTickCount() - last_release_time) * portTICK_PERIOD_MS;
      uint32_t remaining_ms = elapsed_ms < button_cfg.double_press_time_ms
                                  ? button_cfg.double_press_time_ms - elapsed_ms
                                  : 1;
      wait_time = pdMS_TO_TICKS(remaining_ms);
    } else if (power_off_armed) {
      wait_time = pdMS_TO_TICKS(LONG_PRESS_CHECK_MS);
    } else {
      wait_time = pdMS_TO_TICKS(WDT_RESET_INTERVAL_MS);
    }
    uint32_t notification = ulTaskNotifyTake(pdTRUE, wait_time);

    esp_task_wdt_reset();

    if (notification > 0 || isr_triggered) {
      isr_triggered = false;
      vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME_MS));

      // Clear any notifications that came during debounce
      ulTaskNotifyTake(pdTRUE, 0);
      isr_triggered = false;
    }

    bool current_state_pressed = read_button_state();

    // Double-press window expired: stop treating the next press as a double
    // tap. Power-off was already armed immediately on first release.
    if (first_press_registered && !current_state_pressed) {
      uint32_t elapsed_ms =
          (xTaskGetTickCount() - last_release_time) * portTICK_PERIOD_MS;
      if (elapsed_ms >= button_cfg.double_press_time_ms) {
        first_press_registered = false;
      }
    }

    // Check arm window timeout → cancel power-off
    if (power_off_armed && !current_state_pressed) {
      uint32_t elapsed_ms =
          (xTaskGetTickCount() - arm_start_time) * portTICK_PERIOD_MS;
      if (elapsed_ms >= POWER_OFF_ARM_WINDOW_MS) {
        power_off_armed = false;
        notify_callbacks(BUTTON_EVENT_POWER_OFF_CANCELLED);
      }
    }

    if (current_state_pressed && !button_pressed) {
      TickType_t current_time = xTaskGetTickCount();
      bool within_double_press_window =
          first_press_registered &&
          ((current_time - last_release_time) * portTICK_PERIOD_MS <
           button_cfg.double_press_time_ms);

      // A press inside the double-press window may become either:
      // 1) a quick second tap => double press on release
      // 2) a hold => shutdown long-press
      // So do not cancel the already-sent shutdown arm yet. Only stop the
      // local arm timeout while this press is active.
      second_press_candidate = within_double_press_window;
      power_off_armed = false;

      press_start_time = current_time;
      button_pressed = true;
      long_press_sent = false;
      notify_callbacks(BUTTON_EVENT_PRESSED);

    } else if (current_state_pressed && button_pressed) {
      uint32_t press_duration =
          (xTaskGetTickCount() - press_start_time) * portTICK_PERIOD_MS;
      if (!long_press_sent && press_duration >= button_cfg.long_press_time_ms) {
        long_press_sent = true;

        // This press is no longer a double-press candidate; treat it as a hold.
        second_press_candidate = false;
        first_press_registered = false;

        notify_callbacks(BUTTON_EVENT_LONG_PRESS);
      }

    } else if (!current_state_pressed && button_pressed) {
      button_pressed = false;

      if (!long_press_sent) {
        TickType_t current_time = xTaskGetTickCount();
        if (second_press_candidate) {
          first_press_registered = false;
          second_press_candidate = false;
          notify_callbacks(BUTTON_EVENT_POWER_OFF_CANCELLED);
          notify_callbacks(BUTTON_EVENT_DOUBLE_PRESS);
        } else {
          first_press_registered = true;
          last_release_time = current_time;
          second_press_candidate = false;

          // Arm power-off immediately on first short press release. If a second
          // tap arrives inside the double-press window, it will cancel this arm
          // and become a double press instead.
          if (!power_off_armed) {
            power_off_armed = true;
            arm_start_time = current_time;
            notify_callbacks(BUTTON_EVENT_POWER_OFF_ARMED);
          }
        }
      } else {
        second_press_candidate = false;
      }

      notify_callbacks(BUTTON_EVENT_RELEASED);
    }
  }
}

esp_err_t button_init(const button_config_t *config) {
  if (config == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  memcpy(&button_cfg, config, sizeof(button_config_t));

  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << config->gpio_num),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE // Interrupt on both press and release
  };

  esp_err_t ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure button GPIO %d: %s", config->gpio_num,
             esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "Button initialized on GPIO %d (interrupt-based)",
           config->gpio_num);

  return ESP_OK;
}

esp_err_t button_init_main(void) {
  button_config_t config = {.gpio_num = MAIN_BUTTON_GPIO,
                            .long_press_time_ms = BUTTON_LONG_PRESS_TIME_MS,
                            .double_press_time_ms = BUTTON_DOUBLE_PRESS_TIME_MS,
                            .active_low = true};

  return button_init(&config);
}

void button_start_monitoring(void) {
  if (button_task_handle != NULL) {
    return; // Already started (e.g. before charging screen)
  }
  xTaskCreate(button_monitor_task, "button_monitor", TASK_STACK_SIZE, NULL,
              TASK_PRIORITY, &button_task_handle);
}