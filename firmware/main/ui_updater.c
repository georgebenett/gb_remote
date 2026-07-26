#include "ui_updater.h"
#include "battery.h"
#include "ble.h"
#include "button.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hw_config.h"
#include "lcd.h"
#include "power.h"
#include "snake.h"
#include "target_config.h"
#include "throttle.h"
#include "version.h"
#include "vesc_config.h"
#include "viber.h"
#include <stdio.h>
#include <string.h>

#define TAG "UI_UPDATER"

#define KM_TO_MI 0.621371f

typedef enum {
  UI_CMD_UPDATE_SPEED,
  UI_CMD_UPDATE_SPEED_UNIT,
  UI_CMD_UPDATE_BATTERY_PERCENTAGE,
  UI_CMD_UPDATE_SKATE_BATTERY_PERCENTAGE,
  UI_CMD_UPDATE_SKATE_BATTERY_VOLTAGE,
  UI_CMD_UPDATE_CONNECTION_ICON,
  UI_CMD_UPDATE_TRIP_DISTANCE,
  UI_CMD_RESET_TRIP_DISTANCE,
  UI_CMD_UPDATE_AUX_INDICATOR,
  UI_CMD_RESET_SKATE_DISPLAY,
} ui_cmd_type_t;

typedef struct {
  ui_cmd_type_t type;
  uint8_t receiver; // Receiver slot the payload belongs to (0 unless dual)
  union {
    int32_t speed;
    bool speed_unit_mph;
    struct {
      int percentage;
      bool is_charging;
    } battery;
    int skate_percentage;
    float skate_voltage;
    struct {
      uint8_t quality;
      bool connected;
    } connection;
    float trip_km;
    bool aux_state;
  } data;
} ui_cmd_t;

/* The home screen exists in two layouts: the default one with a single
 * receiver column, and home_screen_dual, which repeats the link icon, board
 * battery arc and odometer once per receiver. Which one is live follows the
 * dual_connection preference. Everything downstream talks to these bindings
 * instead of `objects` directly so the update paths are identical in both. */
#define UI_MAX_RECEIVERS BLE_MAX_RECEIVERS

typedef struct {
  lv_obj_t *screen;
  lv_obj_t *speedlabel;
  lv_obj_t *static_speed;
  lv_obj_t *throttle_warning;
  lv_obj_t *remote_arc;
  lv_obj_t *remote_battery_text;
  lv_obj_t *aux_output;
  uint8_t receiver_count;
  lv_obj_t *skate_arc[UI_MAX_RECEIVERS];
  lv_obj_t *skate_battery_text[UI_MAX_RECEIVERS];
  lv_obj_t *skateboard_icon[UI_MAX_RECEIVERS];
  lv_obj_t *connection_icon[UI_MAX_RECEIVERS];
  lv_obj_t *odometer[UI_MAX_RECEIVERS];
} home_widgets_t;

static home_widgets_t home_ui;

static QueueHandle_t ui_cmd_queue = NULL;
static SemaphoreHandle_t lvgl_mutex = NULL;
static const TickType_t LVGL_MUTEX_TIMEOUT = pdMS_TO_TICKS(50);
volatile lv_timer_t *splash_transition_timer = NULL;
volatile bool splash_transition_active = false;

static volatile bool force_config_reload = false;

// Shared UI state (volatile ensures visibility across tasks)
static volatile uint8_t connection_quality = 0;
static volatile bool speed_unit_mph = false;
static volatile float total_trip_km = 0.0f;

static lv_obj_t *get_current_screen(void) { return lv_scr_act(); }

static void bind_single_home_widgets(void) {
  memset(&home_ui, 0, sizeof(home_ui));
  home_ui.screen = objects.home_screen;
  home_ui.speedlabel = objects.speedlabel;
  home_ui.static_speed = objects.static_speed;
  home_ui.throttle_warning = objects.throttle_not_calibrated_text;
  home_ui.remote_arc = objects.remote_arc;
  home_ui.remote_battery_text = objects.controller_battery_text;
  home_ui.aux_output = objects.aux_output;
  home_ui.receiver_count = 1;
  home_ui.skate_arc[0] = objects.skateboard_arc;
  home_ui.skate_battery_text[0] = objects.skate_battery_text;
  home_ui.skateboard_icon[0] = objects.skateboard_icon;
  home_ui.connection_icon[0] = objects.connection_icon;
  home_ui.odometer[0] = objects.odometer;
}

/* home_screen_dual repeats the receiver widgets, `_1` for the first receiver
 * and `_2` for the second. The board icon is the exception: on dual_throttle
 * EEZ nested skateboard_icon_2 inside skateboard_arc_1 (and vice versa), so
 * each icon is bound to the arc it actually lives in. */
static void bind_dual_home_widgets(void) {
  memset(&home_ui, 0, sizeof(home_ui));
  home_ui.screen = objects.home_screen_dual;
  home_ui.speedlabel = objects.speedlabel_1;
  home_ui.static_speed = objects.static_speed_1;
  home_ui.throttle_warning = objects.throttle_not_calibrated_text_1;
  home_ui.remote_arc = objects.remote_arc_1;
  home_ui.remote_battery_text = objects.controller_battery_text_1;
  home_ui.aux_output = objects.aux_output_1;
  home_ui.receiver_count = 2;

  home_ui.skate_arc[0] = objects.skateboard_arc_1;
  home_ui.skate_battery_text[0] = objects.skate_battery_text_1;
  home_ui.connection_icon[0] = objects.connection_icon_1;
  home_ui.odometer[0] = objects.odometer_1;

  home_ui.skate_arc[1] = objects.skateboard_arc_2;
  home_ui.skate_battery_text[1] = objects.skate_battery_text_2;
  home_ui.connection_icon[1] = objects.connection_icon_2;
  home_ui.odometer[1] = objects.odometer_2;

#ifdef CONFIG_TARGET_DUAL_THROTTLE
  home_ui.skateboard_icon[0] = objects.skateboard_icon_2;
  home_ui.skateboard_icon[1] = objects.skateboard_icon_1;
#else
  home_ui.skateboard_icon[0] = objects.skateboard_icon_1;
  home_ui.skateboard_icon[1] = objects.skateboard_icon_2;
#endif
}

lv_obj_t *ui_get_home_screen(void) {
  return home_ui.screen != NULL ? home_ui.screen : objects.home_screen;
}

void ui_set_dual_home_screen(bool enabled) {
  /* Rebinding and the screen swap belong in the same critical section: the
   * command processor reads home_ui under this mutex, and leaving the bindings
   * pointing at a screen other than the one on display would stall every
   * update. Toggling is a rare, user-initiated action, so retrying is cheap. */
  bool locked = false;
  for (int attempt = 0; attempt < 5 && !locked; attempt++) {
    locked = take_lvgl_mutex();
    if (!locked)
      vTaskDelay(pdMS_TO_TICKS(MUTEX_RETRY_DELAY_MS));
  }

  lv_obj_t *previous = home_ui.screen;

  if (enabled) {
    bind_dual_home_widgets();
  } else {
    bind_single_home_widgets();
  }

  // Swap live if a home screen is what the user is currently looking at.
  if (previous != NULL && previous != home_ui.screen &&
      home_ui.screen != NULL && get_current_screen() == previous) {
    lv_disp_load_scr(home_ui.screen);
    lv_obj_invalidate(home_ui.screen);
  }

  if (locked)
    give_lvgl_mutex();

  ESP_LOGI(TAG, "Home screen layout: %s receiver",
           home_ui.receiver_count > 1 ? "dual" : "single");
}

/* On the single-receiver screen every slot shares one set of widgets, so
 * report the aggregate ("whichever receiver is up") state for it. The dual
 * screen has real per-slot widgets and reads per-slot telemetry. */
static bool receiver_is_connected(int idx) {
  return home_ui.receiver_count <= 1 ? ble_is_connected()
                                     : ble_receiver_is_connected(idx);
}

static bool receiver_get_rssi(int idx, int8_t *out_rssi) {
  if (home_ui.receiver_count <= 1) {
    for (int i = 0; i < UI_MAX_RECEIVERS; i++) {
      if (ble_receiver_get_rssi(i, out_rssi))
        return true;
    }
    return false;
  }
  return ble_receiver_get_rssi(idx, out_rssi);
}

static float receiver_bms_voltage(int idx) {
  return home_ui.receiver_count <= 1 ? get_bms_total_voltage()
                                     : ble_receiver_get_bms_total_voltage(idx);
}

static int receiver_bms_percentage(int idx) {
  return home_ui.receiver_count <= 1
             ? get_bms_battery_percentage()
             : ble_receiver_get_bms_battery_percentage(idx);
}

static float receiver_vesc_voltage(int idx) {
  return home_ui.receiver_count <= 1 ? get_latest_voltage()
                                     : ble_receiver_get_vesc_voltage(idx);
}

/** Map a receiver slot onto a widget column, or -1 when it has none. */
static int widget_slot_for_receiver(int receiver) {
  if (receiver < 0)
    return -1;
  if (home_ui.receiver_count <= 1)
    return 0; // Single column shows whichever receiver reported last
  return receiver < home_ui.receiver_count ? receiver : -1;
}

static uint8_t rssi_to_quality(int rssi) {
  if (rssi >= 0)
    return 0;
  int quality = ((rssi + 100) * 100) / 70;
  if (quality > 100)
    quality = 100;
  if (quality < 0)
    quality = 0;
  return (uint8_t)quality;
}

static void set_arc_indicator_color_for_pct(lv_obj_t *arc, int pct) {
  lv_color_t color;
  if (pct <= 15) {
    color = lv_color_make(255, 59, 48);
  } else if (pct <= 35) {
    color = lv_color_make(255, 149, 0);
  } else {
    color = lv_color_hex(0x04de71);
  }
  lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
}

static void splash_fade_up_timer_cb(lv_timer_t *timer) {
  (void)timer;
  lcd_fade_to_saved_brightness();
}

void ui_updater_init(void) {
  lvgl_mutex = xSemaphoreCreateMutex();
  if (lvgl_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create LVGL mutex");
  } else {
    ESP_LOGI(TAG, "LVGL mutex created with priority inheritance");
  }

  // Create UI command queue
  ui_cmd_queue = xQueueCreate(UI_CMD_QUEUE_SIZE, sizeof(ui_cmd_t));
  if (ui_cmd_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create UI command queue");
  } else {
    ESP_LOGI(TAG, "UI command queue created (size: %d)", UI_CMD_QUEUE_SIZE);
  }
}

bool take_lvgl_mutex(void) {
  if (lvgl_mutex == NULL)
    return false;
  return xSemaphoreTake(lvgl_mutex, LVGL_MUTEX_TIMEOUT) == pdTRUE;
}

bool take_lvgl_mutex_for_handler(void) {
  if (lvgl_mutex == NULL)
    return false;
  return xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

SemaphoreHandle_t get_lvgl_mutex_handle(void) { return lvgl_mutex; }

void give_lvgl_mutex(void) {
  if (lvgl_mutex != NULL) {
    xSemaphoreGive(lvgl_mutex);
  }
}

// Helper to send UI command to queue
static bool ui_queue_send(ui_cmd_t *cmd) {
  if (ui_cmd_queue == NULL) {
    return false;
  }

  if (xQueueSend(ui_cmd_queue, cmd, 0) != pdTRUE) {
    // Queue full - remove oldest and try again
    ui_cmd_t dummy;
    xQueueReceive(ui_cmd_queue, &dummy, 0);
    return xQueueSend(ui_cmd_queue, cmd, 0) == pdTRUE;
  }
  return true;
}

void ui_update_speed(int32_t value) {
  if (power_is_entering_off_mode() || !home_ui.speedlabel)
    return;

  static int32_t last_value = -1;

  // Only update if value has changed
  if (value != last_value) {
    ui_cmd_t cmd = {.type = UI_CMD_UPDATE_SPEED, .data.speed = value};
    if (ui_queue_send(&cmd)) {
      last_value = value;
    }
  }
}

void ui_update_battery_percentage(int percentage) {
  if (power_is_entering_off_mode())
    return;
  if (home_ui.remote_battery_text == NULL)
    return;

  int gpio_level = gpio_get_level(BATTERY_IS_CHARGING_GPIO);
  bool is_charging = (gpio_level == 0); // Inverted: LOW means charging

  ui_cmd_t cmd = {
      .type = UI_CMD_UPDATE_BATTERY_PERCENTAGE,
      .data.battery = {.percentage = percentage, .is_charging = is_charging}};
  ui_queue_send(&cmd);
}

void ui_update_skate_battery_percentage(int receiver, int percentage) {
  if (power_is_entering_off_mode())
    return;
  int slot = widget_slot_for_receiver(receiver);
  if (slot < 0 || home_ui.skate_battery_text[slot] == NULL)
    return;

  ui_cmd_t cmd = {.type = UI_CMD_UPDATE_SKATE_BATTERY_PERCENTAGE,
                  .receiver = (uint8_t)slot,
                  .data.skate_percentage = percentage};
  ui_queue_send(&cmd);
}

void ui_update_skate_battery_voltage_display(int receiver, float voltage) {
  if (power_is_entering_off_mode())
    return;
  int slot = widget_slot_for_receiver(receiver);
  if (slot < 0 || home_ui.skate_battery_text[slot] == NULL)
    return;

  ui_cmd_t cmd = {.type = UI_CMD_UPDATE_SKATE_BATTERY_VOLTAGE,
                  .receiver = (uint8_t)slot,
                  .data.skate_voltage = voltage};
  ui_queue_send(&cmd);
}

void ui_reset_skate_display(int receiver) {
  int slot = widget_slot_for_receiver(receiver);
  if (slot < 0)
    return;

  ui_cmd_t cmd = {.type = UI_CMD_RESET_SKATE_DISPLAY,
                  .receiver = (uint8_t)slot};
  ui_queue_send(&cmd);
}

int get_connection_quality(void) { return connection_quality; }

void ui_update_connection_quality(int rssi) {
  connection_quality = rssi_to_quality(rssi);
  ui_update_connection_icon();
}

void ui_update_connection_icon(void) {
  if (power_is_entering_off_mode())
    return;

  for (int i = 0; i < home_ui.receiver_count; i++) {
    if (home_ui.connection_icon[i] == NULL &&
        home_ui.skateboard_icon[i] == NULL)
      continue;

    bool connected = receiver_is_connected(i);
    int8_t rssi = 0;
    uint8_t quality = 0;
    if (connected && receiver_get_rssi(i, &rssi)) {
      quality = rssi_to_quality(rssi);
    }

    ui_cmd_t cmd = {
        .type = UI_CMD_UPDATE_CONNECTION_ICON,
        .receiver = (uint8_t)i,
        .data.connection = {.quality = quality, .connected = connected}};
    ui_queue_send(&cmd);
  }
}

void ui_update_trip_distance(int receiver, float trip_km_val) {
  if (power_is_entering_off_mode())
    return;
  int slot = widget_slot_for_receiver(receiver);
  if (slot < 0 || home_ui.odometer[slot] == NULL)
    return;

  total_trip_km = trip_km_val;

  ui_cmd_t cmd = {.type = UI_CMD_UPDATE_TRIP_DISTANCE,
                  .receiver = (uint8_t)slot,
                  .data.trip_km = trip_km_val};
  ui_queue_send(&cmd);
}

void ui_reset_trip_distance(void) {
  total_trip_km = 0.0f;

  for (int i = 0; i < home_ui.receiver_count; i++) {
    ui_cmd_t cmd = {.type = UI_CMD_RESET_TRIP_DISTANCE, .receiver = (uint8_t)i};
    ui_queue_send(&cmd);
  }
}

void ui_check_mutex_health(void) {
  static uint32_t last_check_time = 0;
  uint32_t current_time = esp_timer_get_time() / 1000000;

  if (current_time - last_check_time >= 30) {
    if (lvgl_mutex != NULL &&
        xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(1)) != pdTRUE) {
      ESP_LOGW(TAG, "LVGL mutex appears to be stuck, recreating");

      SemaphoreHandle_t new_mutex = xSemaphoreCreateMutex();
      if (new_mutex != NULL) {
        lvgl_mutex = new_mutex;

        ESP_LOGW(TAG, "LVGL mutex replaced");
      } else {
        ESP_LOGE(TAG, "Failed to create new LVGL mutex");
      }
    } else if (lvgl_mutex != NULL) {
      xSemaphoreGive(lvgl_mutex);
    }

    last_check_time = current_time;
  }
}

void ui_update_speed_unit(bool is_mph) {
  if (power_is_entering_off_mode() || !home_ui.static_speed)
    return;

  ui_cmd_t cmd = {.type = UI_CMD_UPDATE_SPEED_UNIT,
                  .data.speed_unit_mph = is_mph};
  ui_queue_send(&cmd);
}

static void speed_update_task(void *pvParameters) {
  // Register with task watchdog
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  vesc_config_t config;
  ESP_ERROR_CHECK(vesc_config_load(&config));

  TickType_t last_wake_time = xTaskGetTickCount();
  const TickType_t frequency = pdMS_TO_TICKS(SPEED_UPDATE_MS);
  uint32_t config_reload_counter = 0;
  const uint32_t CONFIG_RELOAD_INTERVAL = 50;

  while (1) {
    vTaskDelayUntil(&last_wake_time, frequency);

    config_reload_counter++;
    if (config_reload_counter >= CONFIG_RELOAD_INTERVAL ||
        force_config_reload) {
      esp_err_t err = vesc_config_load(&config);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reload configuration: %s",
                 esp_err_to_name(err));
      }
      config_reload_counter = 0;
      force_config_reload = false;
    }

    if (ble_is_connected()) {
      int32_t speed = vesc_config_get_speed(&config);
      if (speed >= 0) {
        ui_update_speed(speed);
        ui_update_speed_unit(config.speed_unit_mph);
      }
    }
    esp_task_wdt_reset();
  }
}

static void battery_update_task(void *pvParameters) {
  // Register with task watchdog
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  static int displayed_percentage = -1;
  static uint32_t last_change_time = 0;
  const uint32_t RATE_LIMIT_MS = 5000;
  bool was_connected[UI_MAX_RECEIVERS] = {false};

  while (1) {
    int battery_percentage = battery_get_percentage();

    if (battery_percentage >= 0) {
      int display_percentage = battery_percentage;

      if (displayed_percentage >= 0) {
        uint32_t current_time = esp_timer_get_time() / 1000;

        if (current_time - last_change_time >= RATE_LIMIT_MS) {
          if (battery_percentage > displayed_percentage) {
            display_percentage = displayed_percentage + 1;
            last_change_time = current_time;
          } else if (battery_percentage < displayed_percentage) {
            display_percentage = displayed_percentage - 1;
            last_change_time = current_time;
          } else {
            display_percentage = displayed_percentage;
          }
        } else {
          display_percentage = displayed_percentage;
        }
      }

      displayed_percentage = display_percentage;
      ui_update_battery_percentage(display_percentage);
    }

    for (int i = 0; i < home_ui.receiver_count; i++) {
      if (receiver_is_connected(i)) {
        was_connected[i] = true;
        float bms_voltage = receiver_bms_voltage(i);
        bool bms_connected = (bms_voltage > 0.1f);

        if (!bms_connected) {
          float vesc_voltage = receiver_vesc_voltage(i);

          ESP_LOGI(TAG, "battery_update: receiver %d BMS off, vesc=%.2fV", i,
                   vesc_voltage);

          if (vesc_voltage > 0.1f) {
            ui_update_skate_battery_voltage_display(i, vesc_voltage);
          }
        } else {
          int skate_battery_percentage = receiver_bms_percentage(i);
          if (skate_battery_percentage >= 0) {
            ui_update_skate_battery_percentage(i, skate_battery_percentage);
          }
        }
      } else if (was_connected[i]) {
        was_connected[i] = false;
        ui_reset_skate_display(i);
      }
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(BATTERY_UPDATE_MS));
  }
}

static void connection_update_task(void *pvParameters) {
  // Not registered with watchdog - this is a non-critical UI task
  // and its 5s update interval would conflict with watchdog timeout

  while (1) {
    ui_update_connection_icon();
    vTaskDelay(pdMS_TO_TICKS(CONNECTION_UPDATE_MS));
  }
}

// UI Command Processor Task - handles queued UI updates
static void ui_cmd_processor_task(void *pvParameters) {
  // Register with task watchdog
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  ui_cmd_t cmd;
  char str_buf[16];

  while (1) {
    esp_task_wdt_reset();

    // Block waiting for commands with timeout to allow watchdog reset
    if (xQueueReceive(ui_cmd_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Only update if on home screen (for most commands)
        bool on_home = (get_current_screen() == home_ui.screen);
        // Widget column this command targets; commands without a receiver
        // leave `receiver` at 0, which is always a valid column.
        int slot = cmd.receiver < home_ui.receiver_count ? cmd.receiver : -1;

        switch (cmd.type) {
        case UI_CMD_UPDATE_SPEED:
          if (on_home && home_ui.speedlabel != NULL) {
            lv_label_set_text_fmt(home_ui.speedlabel, "%ld", cmd.data.speed);
          }
          break;

        case UI_CMD_UPDATE_SPEED_UNIT:
          speed_unit_mph = cmd.data.speed_unit_mph;
          if (on_home && home_ui.static_speed != NULL) {
            lv_label_set_text(home_ui.static_speed,
                              cmd.data.speed_unit_mph ? "mph" : "km/h");
          }
          break;

        case UI_CMD_UPDATE_BATTERY_PERCENTAGE:
          if (on_home && home_ui.remote_battery_text != NULL) {
            lv_label_set_text_fmt(
                home_ui.remote_battery_text, "%s%d%%",
                cmd.data.battery.is_charging ? LV_SYMBOL_CHARGE " " : "",
                cmd.data.battery.percentage);
          }
          if (on_home && home_ui.remote_arc != NULL) {
            int pct = cmd.data.battery.percentage;
            if (pct < 0)
              pct = 0;
            if (pct > 100)
              pct = 100;
            lv_arc_set_value(home_ui.remote_arc, (int16_t)pct);
            set_arc_indicator_color_for_pct(home_ui.remote_arc, pct);
          }
          if (get_current_screen() == objects.charging_screen &&
              objects.charging_screen_percentage != NULL) {
            lv_label_set_text_fmt(objects.charging_screen_percentage,
                                  "%d%% charged", cmd.data.battery.percentage);
            if (objects.charging_arc != NULL) {
              int pct = cmd.data.battery.percentage;
              if (pct < 0)
                pct = 0;
              if (pct > 100)
                pct = 100;
              lv_arc_set_value(objects.charging_arc, (int16_t)pct);
            }
          }
          break;

        case UI_CMD_UPDATE_SKATE_BATTERY_PERCENTAGE:
          if (on_home && slot >= 0 &&
              home_ui.skate_battery_text[slot] != NULL) {
            lv_label_set_text_fmt(home_ui.skate_battery_text[slot], "%d%%",
                                  cmd.data.skate_percentage);
            lv_obj_set_style_text_color(home_ui.skate_battery_text[slot],
                                        receiver_is_connected(slot)
                                            ? lv_color_white()
                                            : lv_color_make(48, 48, 48),
                                        LV_PART_MAIN);
          }
          if (on_home && slot >= 0 && home_ui.skate_arc[slot] != NULL) {
            int pct = cmd.data.skate_percentage;
            if (pct < 0)
              pct = 0;
            if (pct > 100)
              pct = 100;
            lv_arc_set_value(home_ui.skate_arc[slot], (int16_t)pct);
            set_arc_indicator_color_for_pct(home_ui.skate_arc[slot], pct);
          }
          break;

        case UI_CMD_UPDATE_SKATE_BATTERY_VOLTAGE:
          if (on_home && slot >= 0 &&
              home_ui.skate_battery_text[slot] != NULL) {
            int volts = (int)cmd.data.skate_voltage;
            int tenths = (int)((cmd.data.skate_voltage - volts) * 10 + 0.5f);
            if (tenths >= 10) {
              tenths = 0;
              volts++;
            }
            snprintf(str_buf, sizeof(str_buf), "%d.%dV", volts, tenths);
            lv_label_set_text(home_ui.skate_battery_text[slot], str_buf);
            lv_obj_set_style_text_color(home_ui.skate_battery_text[slot],
                                        receiver_is_connected(slot)
                                            ? lv_color_white()
                                            : lv_color_make(48, 48, 48),
                                        LV_PART_MAIN);
          }
          break;

        case UI_CMD_UPDATE_CONNECTION_ICON:
          if (on_home && slot >= 0 && home_ui.connection_icon[slot] != NULL) {
            const void *icon_src = NULL;
            if (!cmd.data.connection.connected) {
              icon_src = &img_no_connection;
            } else if (cmd.data.connection.quality >= 30) {
              icon_src = &img_100_connection;
            } else if (cmd.data.connection.quality >= 15) {
              icon_src = &img_66_connection;
            } else if (cmd.data.connection.quality >= 5) {
              icon_src = &img_33_connection;
            } else {
              icon_src = &img_connection_0;
            }
            lv_img_set_src(home_ui.connection_icon[slot], icon_src);
          }
          if (on_home && slot >= 0 && home_ui.skateboard_icon[slot] != NULL) {
            lv_img_set_src(home_ui.skateboard_icon[slot],
                           cmd.data.connection.connected
                               ? &img_skateboard_icon_connected
                               : &img_skateboard_no_connection);
          }
          break;

        case UI_CMD_UPDATE_TRIP_DISTANCE:
          if (on_home && slot >= 0 && home_ui.odometer[slot] != NULL) {
            if (speed_unit_mph) {
              snprintf(str_buf, sizeof(str_buf), "%.1f mi",
                       cmd.data.trip_km * KM_TO_MI);
            } else {
              snprintf(str_buf, sizeof(str_buf), "%.1f km", cmd.data.trip_km);
            }
            lv_label_set_text(home_ui.odometer[slot], str_buf);
            lv_obj_invalidate(home_ui.odometer[slot]);
          }
          break;

        case UI_CMD_RESET_TRIP_DISTANCE:
          if (on_home && slot >= 0 && home_ui.odometer[slot] != NULL) {
            lv_label_set_text(home_ui.odometer[slot],
                              speed_unit_mph ? "0.0 mi" : "0.0 km");
            lv_obj_invalidate(home_ui.odometer[slot]);
          }
          break;

        case UI_CMD_UPDATE_AUX_INDICATOR:
          if (home_ui.aux_output != NULL) {
            if (cmd.data.aux_state) {
              lv_obj_set_style_opa(home_ui.aux_output, LV_OPA_COVER, 0);
            } else {
              lv_obj_set_style_opa(home_ui.aux_output, LV_OPA_TRANSP, 0);
            }
          }
          break;

        case UI_CMD_RESET_SKATE_DISPLAY:
          if (on_home && slot >= 0 &&
              home_ui.skate_battery_text[slot] != NULL) {
            lv_label_set_text(home_ui.skate_battery_text[slot], "--");
            lv_obj_set_style_text_color(home_ui.skate_battery_text[slot],
                                        lv_color_make(48, 48, 48),
                                        LV_PART_MAIN);
          }
          if (on_home && slot >= 0 && home_ui.skate_arc[slot] != NULL) {
            lv_arc_set_value(home_ui.skate_arc[slot], 0);
          }
          // Aux is a remote-wide indicator; clear it once no receiver is left.
          if (home_ui.aux_output != NULL && !ble_is_connected()) {
            lv_obj_set_style_opa(home_ui.aux_output, LV_OPA_TRANSP, 0);
          }
          break;
        }
        xSemaphoreGive(lvgl_mutex);
      } else {
        // Re-queue the command if we couldn't get mutex (rare case)
        if (xQueueSendToFront(ui_cmd_queue, &cmd, 0) != pdTRUE) {
          ESP_LOGW(TAG, "Failed to re-queue UI command, update dropped");
        }
        vTaskDelay(pdMS_TO_TICKS(MUTEX_RETRY_DELAY_MS));
      }
    }
  }
}

void ui_start_update_tasks(void) {
  vTaskDelay(pdMS_TO_TICKS(UI_TASK_STARTUP_DELAY_MS));

  // Start the UI command processor task first (highest priority for UI
  // responsiveness)
  xTaskCreate(ui_cmd_processor_task, "ui_cmd_proc", 3072, NULL, 5, NULL);
  vTaskDelay(pdMS_TO_TICKS(UI_TASK_STARTUP_DELAY_MS));

  xTaskCreate(speed_update_task, "speed_update", 4096, NULL, 4, NULL);
  vTaskDelay(pdMS_TO_TICKS(UI_TASK_STARTUP_DELAY_MS));
  xTaskCreate(battery_update_task, "battery_update", 4096, NULL, 2, NULL);
  vTaskDelay(pdMS_TO_TICKS(UI_TASK_STARTUP_DELAY_MS));
  xTaskCreate(connection_update_task, "conn_update", 4096, NULL, 2, NULL);
}

void ui_force_config_reload(void) { force_config_reload = true; }

void ui_create_aux_output_indicator(void) {
  if (home_ui.aux_output == NULL) {
    ESP_LOGW(TAG, "home screen aux_output is NULL");
    return;
  }

  // Set initial visibility based on saved state
  ui_update_aux_output_indicator();
}

void ui_hide_throttle_not_calibrated_text(void) {
  if (home_ui.throttle_warning == NULL)
    return;
  if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    lv_obj_add_flag(home_ui.throttle_warning, LV_OBJ_FLAG_HIDDEN);
    xSemaphoreGive(lvgl_mutex);
  }
}

void ui_show_throttle_not_calibrated_text(void) {
  if (home_ui.throttle_warning == NULL)
    return;
  lv_obj_clear_flag(home_ui.throttle_warning, LV_OBJ_FLAG_HIDDEN);
}

void ui_update_aux_output_indicator(void) {
  if (home_ui.aux_output == NULL)
    return;

  bool aux_state = ble_get_receiver_aux_output_state();

  ui_cmd_t cmd = {.type = UI_CMD_UPDATE_AUX_INDICATOR,
                  .data.aux_state = aux_state};
  ui_queue_send(&cmd);
}

static void splash_timer_cb(lv_timer_t *timer) {
  if (timer == splash_transition_timer) {
    splash_transition_timer = NULL;
  }
  splash_transition_active = false;

  if (snake_is_running()) {
    return;
  }

  lv_obj_t *home = ui_get_home_screen();
  lv_disp_load_scr(home);

  // Home screen reached — activate BLE scanning and connection
  ble_resume();
  if (!throttle_is_calibrated()) {
    ui_show_throttle_not_calibrated_text();
  }
  lv_obj_invalidate(home);
}

/** Cancel any pending splash transition. Caller must hold LVGL mutex. */
static void ui_cancel_splash_transition_locked(void) {
  if (splash_transition_timer != NULL) {
    lv_timer_del((lv_timer_t *)splash_transition_timer);
    splash_transition_timer = NULL;
  }
  splash_transition_active = false;
}

/** Show splash and schedule transition to home after 4s. Caller must hold LVGL
 * mutex. */
void ui_show_splash_then_home(void) {
  if (objects.firmware_text != NULL) {
    char version_str[64];
    snprintf(version_str, sizeof(version_str), "%s (%s)", FW_VERSION,
             TARGET_NAME);
    lv_label_set_text(objects.firmware_text, version_str);
  }
  if (objects.mac_addr_text != NULL) {
    uint8_t mac[6] = {0};
    char mac_str[18];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
    lv_label_set_text(objects.mac_addr_text, mac_str);
  }
  ui_cancel_splash_transition_locked();
  splash_transition_active = true;
  lv_disp_load_scr(objects.splash_screen);
  lv_obj_invalidate(objects.splash_screen);
  splash_transition_timer = lv_timer_create(splash_timer_cb, 1200, NULL);
  lv_timer_set_repeat_count((lv_timer_t *)splash_transition_timer, 1);
}

void ui_show_splash_screen(void) {
  // Set firmware version + MAC labels now, just before showing the splash
  // screen
  if (objects.firmware_text != NULL) {
    char version_str[64];
    snprintf(version_str, sizeof(version_str), "%s (%s)", FW_VERSION,
             TARGET_NAME);
    lv_label_set_text(objects.firmware_text, version_str);
  }
  if (objects.mac_addr_text != NULL) {
    uint8_t mac[6] = {0};
    char mac_str[18];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
    lv_label_set_text(objects.mac_addr_text, mac_str);
  }

  lcd_set_backlight(0);
  ui_cancel_splash_transition_locked();
  splash_transition_active = true;
  lv_disp_load_scr(objects.splash_screen);
  lv_obj_invalidate(objects.splash_screen);

  lv_timer_t *fade_timer =
      lv_timer_create(splash_fade_up_timer_cb, SPLASH_FADE_UP_DELAY_MS, NULL);
  lv_timer_set_repeat_count(fade_timer, 1);

  splash_transition_timer = lv_timer_create(splash_timer_cb, 1200, NULL);
  lv_timer_set_repeat_count((lv_timer_t *)splash_transition_timer, 1);
  vTaskDelay(pdMS_TO_TICKS(SPLASH_SCREEN_DELAY_MS));
}
