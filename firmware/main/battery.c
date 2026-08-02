#include "battery.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hw_config.h"
#include "lcd.h"
#include "power.h"
#include "throttle.h"
#include "ui.h"
#include "ui_updater.h"
#include "viber.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG "BATTERY"
static bool battery_initialized = false;
static float latest_battery_voltage = 0.0f;
static bool low_voltage_alerted = false;
static bool low_voltage_shutdown_triggered = false;

// Battery state of charge lookup tables: resting per-cell voltage -> SoC.
// Cells inside one chemistry family track each other within ~1-2% SoC, which is
// well under the sag a throttle pull causes, so the families below are what
// actually differ - not individual part numbers.
typedef struct {
  float voltage;
  float soc; // state of charge in %
} soc_point_t;

// Molicel P42A/P45B, Samsung 30Q/40T/30T, Sony-Murata VTC6. Also the curve for
// the remote's own internal cell.
static const soc_point_t soc_li_ion_high_drain[] = {
    {4.15, 100}, {4.10, 90}, {3.98, 80}, {3.85, 70}, {3.80, 60}, {3.75, 50},
    {3.70, 40},  {3.65, 30}, {3.55, 20}, {3.45, 10}, {3.30, 5},  {2.80, 0}};

// Molicel P50B, Samsung 50S, LG M50LT. Higher capacity, sits a little lower
// through the middle of the pack and has a longer tail under 3.5V.
static const soc_point_t soc_li_ion_high_capacity[] = {
    {4.15, 100}, {4.08, 90}, {3.95, 80}, {3.85, 70}, {3.76, 60}, {3.70, 50},
    {3.64, 40},  {3.58, 30}, {3.50, 20}, {3.38, 10}, {3.20, 5},  {2.80, 0}};

// LiFePO4: 3.65V full charge, ~3.4V rested, an extremely flat 20-80% plateau,
// empty at 2.5V. Percentage between 20% and 80% is a coarse guess by nature.
static const soc_point_t soc_lifepo4[] = {
    {3.50, 100}, {3.40, 95}, {3.34, 90}, {3.32, 80}, {3.31, 70},
    {3.30, 60},  {3.29, 50}, {3.28, 40}, {3.26, 30}, {3.23, 20},
    {3.13, 10},  {3.00, 5},  {2.50, 0}};

// LiPo pouch packs: holds voltage higher through the middle than 18650/21700
// cells and is charged to a full 4.2V.
static const soc_point_t soc_lipo[] = {
    {4.20, 100}, {4.10, 90}, {4.00, 80}, {3.93, 70}, {3.87, 60}, {3.82, 50},
    {3.79, 40},  {3.75, 30}, {3.71, 20}, {3.66, 10}, {3.50, 5},  {3.20, 0}};

typedef struct {
  const soc_point_t *points;
  uint8_t count;
} soc_curve_t;

#define SOC_CURVE(table) {table, sizeof(table) / sizeof((table)[0])}

static const soc_curve_t soc_curves[BATTERY_CELL_TYPE_COUNT] = {
    [BATTERY_CELL_LI_ION_HIGH_DRAIN] = SOC_CURVE(soc_li_ion_high_drain),
    [BATTERY_CELL_LI_ION_HIGH_CAPACITY] = SOC_CURVE(soc_li_ion_high_capacity),
    [BATTERY_CELL_LIFEPO4] = SOC_CURVE(soc_lifepo4),
    [BATTERY_CELL_LIPO] = SOC_CURVE(soc_lipo),
};

// Convert voltage to state of charge using lookup table with interpolation
static float curve_voltage_to_soc(const soc_curve_t *curve, float v) {
  const soc_point_t *t = curve->points;

  if (v >= t[0].voltage)
    return 100.0f;
  if (v <= t[curve->count - 1].voltage)
    return 0.0f;

  for (int i = 0; i < curve->count - 1; i++) {
    if (v <= t[i].voltage && v >= t[i + 1].voltage) {

      float dv = t[i].voltage - t[i + 1].voltage;
      float dsoc = t[i].soc - t[i + 1].soc;

      float ratio = (v - t[i + 1].voltage) / dv;

      return t[i + 1].soc + ratio * dsoc;
    }
  }
  return 0.0f; // fallback
}

static float voltage_to_soc(float v) {
  return curve_voltage_to_soc(&soc_curves[BATTERY_CELL_LI_ION_HIGH_DRAIN], v);
}

static float battery_voltage_samples[BATTERY_VOLTAGE_SAMPLES] = {0};
static int battery_sample_index = 0;
static bool battery_samples_filled = false;

static void battery_monitoring_task(void *pvParameters);
static void battery_low_voltage_shutdown(void);

float battery_read_voltage(void);

esp_err_t adc_battery_init(void) {
  if (!adc_is_initialized() || !adc_get_handle()) {
    ESP_LOGE(TAG, "ADC not properly initialized");
    return ESP_FAIL;
  }

  adc_oneshot_unit_handle_t adc1_handle = adc_get_handle();

  adc_oneshot_chan_cfg_t battery_config = {.atten = ADC_ATTEN_DB_12,
                                           .bitwidth = ADC_BITWIDTH_12};

  esp_err_t ret = adc_oneshot_config_channel(adc1_handle, BATTERY_VOLTAGE_PIN,
                                             &battery_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Battery ADC channel configuration failed");
    return ret;
  }

  ESP_LOGI(TAG, "Battery ADC initialized successfully on ADC1_CH%d",
           BATTERY_VOLTAGE_PIN);
  return ESP_OK;
}

// Simple comparison function for qsort
static int compare_int32(const void *a, const void *b) {
  int32_t va = *(const int32_t *)a;
  int32_t vb = *(const int32_t *)b;
  return (va > vb) - (va < vb);
}

int32_t adc_read_battery_voltage(uint8_t channel) {
  if (!adc_is_initialized() || !adc_get_handle()) {
    ESP_LOGE(TAG, "ADC not properly initialized");
    return -1;
  }

  adc_oneshot_unit_handle_t adc1_handle = adc_get_handle();
  SemaphoreHandle_t adc_mutex = adc_get_mutex();

  // Take multiple readings with outlier rejection
  const int NUM_SAMPLES = 10;
  const int MIN_VALID_SAMPLES = 6;
  const int TRIM_COUNT = 2; // Remove 2 highest and 2 lowest values
  int32_t samples[NUM_SAMPLES];
  int valid_samples = 0;

  for (int i = 0; i < NUM_SAMPLES; i++) {
    int adc_raw = 0;
    if (adc_mutex && xSemaphoreTake(adc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      esp_err_t ret = adc_oneshot_read(adc1_handle, channel, &adc_raw);
      xSemaphoreGive(adc_mutex);
      if (ret == ESP_OK && adc_raw >= 0 && adc_raw <= 4095) {
        samples[valid_samples] = adc_raw;
        valid_samples++;
      }
    }

    // Small delay between samples for ADC settling
    vTaskDelay(pdMS_TO_TICKS(ADC_SAMPLE_MS));
  }

  if (valid_samples < MIN_VALID_SAMPLES) {
    ESP_LOGW(TAG, "Insufficient valid battery ADC samples: %d/%d",
             valid_samples, NUM_SAMPLES);
    return -1;
  }

  // Sort samples for outlier rejection (trimmed mean)
  qsort(samples, valid_samples, sizeof(int32_t), compare_int32);

  // Calculate trimmed mean (exclude TRIM_COUNT from each end if we have enough
  // samples)
  int start_idx = (valid_samples > 2 * TRIM_COUNT) ? TRIM_COUNT : 0;
  int end_idx = (valid_samples > 2 * TRIM_COUNT) ? valid_samples - TRIM_COUNT
                                                 : valid_samples;
  int32_t sum = 0;
  int count = 0;

  for (int i = start_idx; i < end_idx; i++) {
    sum += samples[i];
    count++;
  }

  return count > 0 ? (sum / count) : -1;
}

esp_err_t battery_init(void) {
  if (battery_initialized) {
    ESP_LOGI(TAG, "Battery monitoring already initialized");
    return ESP_OK;
  }

  esp_err_t ret = adc_battery_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize battery ADC: %s", esp_err_to_name(ret));
    return ret;
  }

  gpio_config_t probe_conf = {.pin_bit_mask = (1ULL << BATTERY_PROBE_PIN),
                              .mode = GPIO_MODE_OUTPUT,
                              .pull_up_en = GPIO_PULLUP_DISABLE,
                              .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE};
  ret = gpio_config(&probe_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure battery probe pin: %s",
             esp_err_to_name(ret));
    return ret;
  }
  // Start with probe pin LOW (disabled)
  gpio_set_level(BATTERY_PROBE_PIN, 0);
  ESP_LOGI(TAG, "Battery probe pin GPIO %d initialized", BATTERY_PROBE_PIN);

  gpio_config_t charging_conf = {.pin_bit_mask =
                                     (1ULL << BATTERY_IS_CHARGING_GPIO),
                                 .mode = GPIO_MODE_INPUT,
                                 .pull_up_en = GPIO_PULLUP_ENABLE,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .intr_type = GPIO_INTR_DISABLE};
  ret = gpio_config(&charging_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure battery charging status GPIO: %s",
             esp_err_to_name(ret));
    return ret;
  }
  ESP_LOGI(TAG, "Battery charging status GPIO %d initialized",
           BATTERY_IS_CHARGING_GPIO);

  esp_log_level_set("gpio", ESP_LOG_WARN);

  battery_initialized = true;
  ESP_LOGI(TAG, "Battery monitoring initialized successfully for ADC1_CH%d",
           BATTERY_VOLTAGE_PIN);
  return ESP_OK;
}

void battery_start_monitoring(void) {
  xTaskCreate(battery_monitoring_task, "battery_monitor", 4096, NULL, 5, NULL);
}

float battery_read_voltage(void) {
  gpio_set_level(BATTERY_PROBE_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(BATTERY_TASK_STARTUP_DELAY_MS));

  int32_t adc_value = adc_read_battery_voltage(BATTERY_VOLTAGE_PIN);

  gpio_set_level(BATTERY_PROBE_PIN, 0);

  if (adc_value < 0) {
    ESP_LOGW(TAG, "No valid ADC samples obtained");
    return -1.0f;
  }

  float adc_voltage =
      ((float)adc_value / ADC_RESOLUTION) * ADC_REFERENCE_VOLTAGE;

  float divided_voltage = adc_voltage * VOLTAGE_DIVIDER_RATIO;

  float calibrated_voltage =
      divided_voltage * BATTERY_VOLTAGE_SCALE + BATTERY_VOLTAGE_OFFSET;

  return calibrated_voltage;
}

float battery_get_voltage(void) {
  if (!battery_samples_filled && battery_sample_index == 0) {
    return latest_battery_voltage;
  }

  float sum = 0.0f;
  int count =
      battery_samples_filled ? BATTERY_VOLTAGE_SAMPLES : battery_sample_index;

  for (int i = 0; i < count; i++) {
    sum += battery_voltage_samples[i];
  }

  return sum / count;
}

static void battery_monitoring_task(void *pvParameters) {
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  TickType_t last_wake_time = xTaskGetTickCount();
  int low_voltage_count = 0;
  const int LOW_VOLTAGE_CONFIRM_COUNT =
      3; // Require 3 consecutive low readings (1.5 seconds)

  while (1) {
    float voltage = battery_read_voltage();

    if (voltage > 0.0f) {
      latest_battery_voltage = voltage;

      battery_voltage_samples[battery_sample_index] = voltage;
      battery_sample_index =
          (battery_sample_index + 1) % BATTERY_VOLTAGE_SAMPLES;

      if (battery_sample_index == 0) {
        battery_samples_filled = true;
      }

      if (voltage < BATTERY_LOW_VOLTAGE_THRESHOLD) {
        low_voltage_count++;

        if (!low_voltage_alerted) {
          ESP_LOGW(TAG, "Battery voltage low: %.2fV (threshold: %.2fV)",
                   voltage, BATTERY_LOW_VOLTAGE_THRESHOLD);
          viber_play_pattern(VIBER_PATTERN_ALERT);
          low_voltage_alerted = true;
        }

        // Shutdown after confirming low voltage for multiple readings
        if (low_voltage_count >= LOW_VOLTAGE_CONFIRM_COUNT) {
          battery_low_voltage_shutdown();
          // Should not reach here, but break just in case
          break;
        }
      } else {
        // Voltage recovered, reset counters
        if (low_voltage_count > 0) {
          ESP_LOGI(TAG, "Battery voltage recovered: %.2fV", voltage);
          low_voltage_count = 0;
          low_voltage_alerted = false;
        }
      }
    } else {
      ESP_LOGW(TAG, "Invalid battery reading");
    }

    esp_task_wdt_reset();
    vTaskDelayUntil(&last_wake_time,
                    pdMS_TO_TICKS(BATTERY_MONITOR_INTERVAL_MS));
  }

  vTaskDelete(NULL);
}

int battery_get_percentage(void) {
  float voltage = latest_battery_voltage;

  if (voltage <= 0.0f) {
    return -1; // Invalid reading
  }

  float soc = voltage_to_soc(voltage);
  return (int)(soc + 0.5f); // Round to nearest integer
}

int battery_pack_percentage(float pack_voltage, uint8_t cells,
                            uint8_t cell_type) {
  if (cells == 0 || pack_voltage <= 0.0f) {
    return -1;
  }
  if (cell_type >= BATTERY_CELL_TYPE_COUNT) {
    cell_type = BATTERY_CELL_LI_ION_HIGH_DRAIN;
  }
  // ponytail: no sag compensation - reads low under throttle. Add a current-
  // based IR correction if the dip bothers users.
  float soc =
      curve_voltage_to_soc(&soc_curves[cell_type], pack_voltage / (float)cells);
  return (int)(soc + 0.5f);
}

bool battery_is_low_voltage(void) {
  float voltage = battery_get_voltage();
  return (voltage > 0.0f && voltage < BATTERY_LOW_VOLTAGE_THRESHOLD);
}

static void battery_low_voltage_shutdown(void) {
  if (low_voltage_shutdown_triggered) {
    return; // Already triggered shutdown
  }

  low_voltage_shutdown_triggered = true;

  viber_play_pattern(VIBER_PATTERN_ALERT);
  vTaskDelay(pdMS_TO_TICKS(LOW_BATTERY_ALERT_DELAY_MS));

  if (take_lvgl_mutex()) {
    if (objects.low_battery_screen != NULL) {
      lv_disp_load_scr(objects.low_battery_screen);
      lv_obj_invalidate(objects.low_battery_screen);
    }
    give_lvgl_mutex();
  }

  // Give user a moment to see the warning
  vTaskDelay(pdMS_TO_TICKS(LOW_BATTERY_WARNING_MS));

  lcd_fade_backlight(lcd_get_backlight(), 0, LCD_BACKLIGHT_FADE_DURATION_MS);

  vTaskDelay(pdMS_TO_TICKS(NVS_FLUSH_DELAY_MS));

  // Turn off power hold pin to prevent over-discharge
  ESP_LOGI(TAG, "Turning off power hold pin to prevent battery over-discharge");
  gpio_set_level(POWER_HOLD_GPIO, 0);

  // Small delay to ensure power is cut
  vTaskDelay(pdMS_TO_TICKS(POWER_OFF_SETTLE_MS));

  // Force deep sleep as final safety measure
  esp_deep_sleep_start();
}
