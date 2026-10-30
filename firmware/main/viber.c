#include "viber.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hw_config.h"
#include "settings.h"

#define TAG "VIBER"

// LEDC configuration — use Timer 1 / Channel 1 (Timer 0 / Channel 0 are LCD)
#define BUZZER_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_1
#define BUZZER_DUTY_RES LEDC_TIMER_10_BIT
#define BUZZER_DUTY 123 // 12% of 1023 — max before motor starts vibrating
#define BUZZER_HAPTIC_FREQ 1300 // Hz — motor resonance frequency
#define BUZZER_DUTY_FULL 1023   // 100% duty — full spin for haptic vibration

// Musical note frequencies (Hz)
#define NOTE_C5 523
#define NOTE_E5 659
#define NOTE_G5 784
#define NOTE_C6 1047

#define SONG_NOTE_GAP_MS 40 // brief silence between notes

typedef struct {
  uint32_t freq_hz;
  uint32_t duration_ms;
} song_note_t;

// Startup: C major arpeggio ascending (cheerful power-on)
static const song_note_t startup_song[] = {
    {NOTE_C5, 120},
    {NOTE_E5, 120},
    {NOTE_G5, 120},
    {NOTE_C6, 250},
};

static bool viber_initialized = false;
static uint8_t haptic_intensity = SETTINGS_HAPTIC_DEFAULT; // 0-100%

/* One request slot, overwritten: the newest request replaces a queued one
 * and pre-empts the one playing. Callers never block. */
#define REQ_STARTUP_SONG 0xFF
static QueueHandle_t req_queue;

static esp_err_t ledc_setup(void) {
  ledc_timer_config_t timer_conf = {
      .speed_mode = BUZZER_LEDC_MODE,
      .timer_num = BUZZER_LEDC_TIMER,
      .duty_resolution = BUZZER_DUTY_RES,
      .freq_hz = BUZZER_HAPTIC_FREQ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  esp_err_t err = ledc_timer_config(&timer_conf);
  if (err != ESP_OK) {
    return err;
  }
  ledc_channel_config_t channel_conf = {
      .speed_mode = BUZZER_LEDC_MODE,
      .channel = BUZZER_LEDC_CHANNEL,
      .timer_sel = BUZZER_LEDC_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = VIBER_PIN,
      .duty = 0,
      .hpoint = 0,
  };
  return ledc_channel_config(&channel_conf);
}

static void output(uint32_t freq_hz, uint32_t duty) {
  ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
  ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty);
  ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void output_off(void) {
  ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
  ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

esp_err_t viber_play_early_boot_ack(void) {
  esp_err_t err = ledc_setup();
  if (err != ESP_OK) {
    return err;
  }
  output(BUZZER_HAPTIC_FREQ, BUZZER_DUTY_FULL);
  vTaskDelay(pdMS_TO_TICKS(SHORT_DURATION));
  output_off();
  return ESP_OK;
}

static void viber_task(void *pvParameters);

esp_err_t viber_init(void) {
  if (viber_initialized) {
    return ESP_OK;
  }
  esp_err_t err = ledc_setup();
  if (err != ESP_OK) {
    return err;
  }
  settings_t cfg;
  settings_get(&cfg);
  haptic_intensity = cfg.haptic_pct;

  req_queue = xQueueCreate(1, sizeof(uint8_t));
  if (req_queue == NULL ||
      xTaskCreate(viber_task, "viber_task", 2048, NULL, 2, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  viber_initialized = true;
  ESP_LOGI(TAG, "Viber/buzzer initialized on GPIO %d, intensity=%d%%",
           VIBER_PIN, haptic_intensity);
  return ESP_OK;
}

// Even indices vibrate, odd indices pause (see viber_task).
#define PATTERN(name, ...) static const uint32_t name[] = {__VA_ARGS__}
PATTERN(pat_very_short, VERY_SHORT_DURATION);
PATTERN(pat_single_short, SHORT_DURATION);
PATTERN(pat_single_long, LONG_DURATION);
PATTERN(pat_double_short, SHORT_DURATION, PAUSE_DURATION, SHORT_DURATION);
PATTERN(pat_success, SHORT_DURATION, PAUSE_DURATION, LONG_DURATION);
PATTERN(pat_error, SHORT_DURATION, PAUSE_DURATION, SHORT_DURATION,
        PAUSE_DURATION, SHORT_DURATION);
PATTERN(pat_alert, LONG_DURATION, PAUSE_DURATION, SHORT_DURATION,
        PAUSE_DURATION, LONG_DURATION);
#undef PATTERN

#define PATTERN_ENTRY(p) {p, sizeof(p) / sizeof(p[0])}
static const struct {
  const uint32_t *durations;
  uint8_t count;
} pattern_table[] = {
    [VIBER_PATTERN_VERY_SHORT] = PATTERN_ENTRY(pat_very_short),
    [VIBER_PATTERN_SINGLE_SHORT] = PATTERN_ENTRY(pat_single_short),
    [VIBER_PATTERN_SINGLE_LONG] = PATTERN_ENTRY(pat_single_long),
    [VIBER_PATTERN_DOUBLE_SHORT] = PATTERN_ENTRY(pat_double_short),
    [VIBER_PATTERN_SUCCESS] = PATTERN_ENTRY(pat_success),
    [VIBER_PATTERN_ERROR] = PATTERN_ENTRY(pat_error),
    [VIBER_PATTERN_ALERT] = PATTERN_ENTRY(pat_alert),
};
#undef PATTERN_ENTRY

static esp_err_t request(uint8_t req) {
  if (!viber_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  xQueueOverwrite(req_queue, &req);
  return ESP_OK;
}

esp_err_t viber_play_pattern(viber_pattern_t pattern) {
  if ((unsigned)pattern >= sizeof(pattern_table) / sizeof(pattern_table[0])) {
    return ESP_ERR_INVALID_ARG;
  }
  return request((uint8_t)pattern);
}

esp_err_t viber_play_startup_song(void) { return request(REQ_STARTUP_SONG); }

esp_err_t viber_set_intensity(uint8_t percent) {
  if (percent > 100) {
    return ESP_ERR_INVALID_ARG;
  }
  haptic_intensity = percent;
  settings_t cfg;
  settings_get(&cfg);
  cfg.haptic_pct = percent;
  return settings_set(&cfg);
}

uint8_t viber_get_intensity(void) { return haptic_intensity; }

/* Holds the current output for `ms`; true (with the request in *next) if a
 * new request cut it short. */
static bool hold(uint32_t ms, uint8_t *next) {
  return xQueueReceive(req_queue, next, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void viber_task(void *pvParameters) {
  uint8_t req;
  bool pending = false;
  for (;;) {
    if (!pending) {
      xQueueReceive(req_queue, &req, portMAX_DELAY);
    }
    pending = false;
    if (req == REQ_STARTUP_SONG) {
      size_t n = sizeof(startup_song) / sizeof(startup_song[0]);
      for (size_t i = 0; i < n && !pending; i++) {
        output(startup_song[i].freq_hz, BUZZER_DUTY);
        pending = hold(startup_song[i].duration_ms, &req);
        output_off();
        if (!pending) {
          pending = hold(SONG_NOTE_GAP_MS, &req);
        }
      }
    } else {
      const uint32_t *d = pattern_table[req].durations;
      for (uint8_t i = 0; i < pattern_table[req].count && !pending; i++) {
        if (i % 2 == 0) {
          output(BUZZER_HAPTIC_FREQ,
                 (uint32_t)haptic_intensity * BUZZER_DUTY_FULL / 100);
        }
        pending = hold(d[i], &req);
        output_off();
      }
    }
  }
}
