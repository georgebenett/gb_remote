#include "viber.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_config.h"
#include "nvs.h"

#define TAG "VIBER"

#define NVS_NAMESPACE_HAPTIC "haptic_cfg"
#define NVS_KEY_INTENSITY "intensity"
#define HAPTIC_INTENSITY_DEFAULT 100

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
static uint8_t haptic_intensity = HAPTIC_INTENSITY_DEFAULT; // 0-100%

typedef struct {
  const uint32_t *durations;
  uint8_t count;
  volatile bool is_running;
} viber_task_params_t;

static viber_task_params_t task_params = {0};

static void viber_task(void *pvParameters);
static void buzzer_off(void);

static void buzzer_on(uint32_t freq_hz) {
  ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
  ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY);
  ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

esp_err_t viber_play_early_boot_ack(void) {
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
  err = ledc_channel_config(&channel_conf);
  if (err != ESP_OK) {
    return err;
  }

  ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY_FULL);
  ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
  vTaskDelay(pdMS_TO_TICKS(SHORT_DURATION));
  buzzer_off();

  return ESP_OK;
}

static void haptic_on(void) {
  uint32_t duty = ((uint32_t)haptic_intensity * BUZZER_DUTY_FULL) / 100;
  ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty);
  ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void buzzer_off(void) {
  ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
  ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

esp_err_t viber_init(void) {
  if (viber_initialized) {
    return ESP_OK;
  }

  ledc_timer_config_t timer_conf = {
      .speed_mode = BUZZER_LEDC_MODE,
      .timer_num = BUZZER_LEDC_TIMER,
      .duty_resolution = BUZZER_DUTY_RES,
      .freq_hz = BUZZER_HAPTIC_FREQ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

  ledc_channel_config_t channel_conf = {
      .speed_mode = BUZZER_LEDC_MODE,
      .channel = BUZZER_LEDC_CHANNEL,
      .timer_sel = BUZZER_LEDC_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = VIBER_PIN,
      .duty = 0,
      .hpoint = 0,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&channel_conf));

  nvs_handle_t nvs_handle;
  if (nvs_open(NVS_NAMESPACE_HAPTIC, NVS_READONLY, &nvs_handle) == ESP_OK) {
    nvs_get_u8(nvs_handle, NVS_KEY_INTENSITY, &haptic_intensity);
    nvs_close(nvs_handle);
  }

  task_params.is_running = false;
  task_params.durations = NULL;
  task_params.count = 0;

  xTaskCreate(viber_task, "viber_task", 2048, NULL, 2, NULL);

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

esp_err_t viber_play_pattern(viber_pattern_t pattern) {
  if ((unsigned)pattern >= sizeof(pattern_table) / sizeof(pattern_table[0])) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!viber_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  task_params.is_running = false;
  buzzer_off();
  vTaskDelay(pdMS_TO_TICKS(10));

  task_params.durations = pattern_table[pattern].durations;
  task_params.count = pattern_table[pattern].count;
  task_params.is_running = true;

  return ESP_OK;
}

static void play_note(uint32_t freq_hz, uint32_t duration_ms) {
  buzzer_on(freq_hz);
  vTaskDelay(pdMS_TO_TICKS(duration_ms));
  buzzer_off();
  vTaskDelay(pdMS_TO_TICKS(SONG_NOTE_GAP_MS));
}

static void play_song(const song_note_t *notes, uint8_t count) {
  task_params.is_running = false;
  buzzer_off();
  for (uint8_t i = 0; i < count; i++) {
    play_note(notes[i].freq_hz, notes[i].duration_ms);
  }
}

esp_err_t viber_set_intensity(uint8_t percent) {
  if (percent > 100) {
    return ESP_ERR_INVALID_ARG;
  }
  haptic_intensity = percent;

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE_HAPTIC, NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    err = nvs_set_u8(nvs_handle, NVS_KEY_INTENSITY, percent);
    if (err == ESP_OK) {
      err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
  }
  return err;
}

uint8_t viber_get_intensity(void) { return haptic_intensity; }

esp_err_t viber_play_startup_song(void) {
  if (!viber_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  play_song(startup_song, sizeof(startup_song) / sizeof(startup_song[0]));
  return ESP_OK;
}

static void viber_task(void *pvParameters) {
  while (1) {
    if (task_params.is_running && task_params.durations &&
        task_params.count > 0) {
      for (uint8_t i = 0; i < task_params.count; i++) {
        if (!task_params.is_running) {
          break;
        }

        if (i % 2 == 0) {
          // Even indices: full-power spin for physical vibration
          haptic_on();
          vTaskDelay(pdMS_TO_TICKS(task_params.durations[i]));
          buzzer_off();
        } else {
          // Odd indices: pause
          vTaskDelay(pdMS_TO_TICKS(task_params.durations[i]));
        }
      }
      task_params.is_running = false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
