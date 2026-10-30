#include "settings.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#define TAG "SETTINGS"

typedef struct {
  settings_t s;
  settings_cal_t cal;
} store_t;

typedef enum { F_BOOL, F_U8, F_I8, F_U16, F_U32 } kind_t;

typedef struct {
  const char *ns;
  const char *key;
  kind_t kind;
  uint16_t off;
} field_t;

#define S(f) offsetof(store_t, s.f)
#define C(f) offsetof(store_t, cal.f)

// The namespaces and keys earlier firmware wrote, so nothing is migrated.
// Grouped by namespace: each run is one nvs_open.
static const field_t pref_fields[] = {
    {"vesc_cfg", "speed_unit", F_BOOL, S(speed_unit_mph)},
    {"vesc_cfg", "dual_conn", F_BOOL, S(dual_connection)},
    {"vesc_cfg", "batt_cells", F_U8, S(battery_cells)},
    {"vesc_cfg", "batt_celltyp", F_U8, S(battery_cell_type)},
    {"vesc_cfg", "smart_rev", F_BOOL, S(smart_reverse)},
    {"vesc_cfg", "no_reverse", F_BOOL, S(no_reverse)},
    {"vesc_cfg", "curve_acc", F_I8, S(throttle_curve_acc)},
    {"vesc_cfg", "curve_brk", F_I8, S(throttle_curve_brake)},
    {"vesc_cfg", "curve_mode", F_U8, S(throttle_curve_mode)},
    {"vesc_cfg", "assist_push", F_BOOL, S(assist_push)},
    {"vesc_cfg", "assist_str", F_U8, S(assist_strength)},
    {"vesc_cfg", "assist_dec", F_U8, S(assist_decay)},
    {"vesc_cfg", "speed_limit", F_U8, S(speed_limit_kmh)},
    {"vesc_cfg", "ride_profile", F_U8, S(ride_profile)},
    {"vesc_cfg", "inv_throttle", F_BOOL, S(invert_throttle)},
    {"lcd_cfg", "backlight", F_U8, S(backlight_pct)},
    {"haptic_cfg", "intensity", F_U8, S(haptic_pct)},
    {"aux_cfg", "aux_state", F_BOOL, S(aux_on)},
    {"ble_cfg", "trim_offset", F_I8, S(ble_trim)},
    {"snake", "high_score", F_U16, S(snake_high_score)},
};

static const field_t cal_fields[] = {
    {"adc_cal", "cal_done", F_BOOL, C(done)},
    {"adc_cal", "min_val", F_U32, C(throttle_min)},
    {"adc_cal", "max_val", F_U32, C(throttle_max)},
    {"adc_cal", "brake_min_val", F_U32, C(brake_min)},
    {"adc_cal", "brake_max_val", F_U32, C(brake_max)},
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const size_t kind_size[] = {1, 1, 1, 2, 4};

static store_t store;
static portMUX_TYPE store_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t save_mutex_buf;
static SemaphoreHandle_t save_mutex;
static atomic_uint epoch;

static esp_err_t read_field(nvs_handle_t h, const field_t *f, void *dst) {
  switch (f->kind) {
  case F_BOOL: {
    uint8_t v;
    esp_err_t err = nvs_get_u8(h, f->key, &v);
    if (err == ESP_OK)
      *(bool *)dst = v != 0;
    return err;
  }
  case F_U8:
    return nvs_get_u8(h, f->key, dst);
  case F_I8:
    return nvs_get_i8(h, f->key, dst);
  case F_U16:
    return nvs_get_u16(h, f->key, dst);
  case F_U32:
    return nvs_get_u32(h, f->key, dst);
  }
  return ESP_ERR_INVALID_ARG;
}

static esp_err_t write_field(nvs_handle_t h, const field_t *f,
                             const void *src) {
  switch (f->kind) {
  case F_BOOL:
    return nvs_set_u8(h, f->key, *(const bool *)src ? 1 : 0);
  case F_U8:
    return nvs_set_u8(h, f->key, *(const uint8_t *)src);
  case F_I8:
    return nvs_set_i8(h, f->key, *(const int8_t *)src);
  case F_U16:
    return nvs_set_u16(h, f->key, *(const uint16_t *)src);
  case F_U32:
    return nvs_set_u32(h, f->key, *(const uint32_t *)src);
  }
  return ESP_ERR_INVALID_ARG;
}

/* Writes the fields of `next` that differ from the stored copy. */
static esp_err_t save_fields(const store_t *next, const field_t *fields,
                             size_t n) {
  esp_err_t first_err = ESP_OK;
  nvs_handle_t h = 0;
  const char *open_ns = NULL;
  for (size_t i = 0; i <= n; i++) {
    const field_t *f = i < n ? &fields[i] : NULL;
    if (open_ns != NULL && (f == NULL || strcmp(f->ns, open_ns) != 0)) {
      esp_err_t err = nvs_commit(h);
      nvs_close(h);
      open_ns = NULL;
      if (first_err == ESP_OK)
        first_err = err;
    }
    if (f == NULL)
      break;
    const uint8_t *nv = (const uint8_t *)next + f->off;
    if (memcmp(nv, (const uint8_t *)&store + f->off, kind_size[f->kind]) == 0)
      continue;
    if (open_ns == NULL) {
      esp_err_t err = nvs_open(f->ns, NVS_READWRITE, &h);
      if (err != ESP_OK) {
        if (first_err == ESP_OK)
          first_err = err;
        continue;
      }
      open_ns = f->ns;
    }
    esp_err_t err = write_field(h, f, nv);
    if (err != ESP_OK && first_err == ESP_OK)
      first_err = err;
  }
  return first_err;
}

static void load_fields(store_t *s, const field_t *fields, size_t n) {
  nvs_handle_t h = 0;
  const char *open_ns = NULL;
  bool ns_missing = false;
  for (size_t i = 0; i < n; i++) {
    const field_t *f = &fields[i];
    if (open_ns == NULL || strcmp(f->ns, open_ns) != 0) {
      if (open_ns != NULL && !ns_missing)
        nvs_close(h);
      open_ns = f->ns;
      esp_err_t err = nvs_open(f->ns, NVS_READONLY, &h);
      ns_missing = err != ESP_OK;
      if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(TAG, "%s: %s, using defaults", f->ns, esp_err_to_name(err));
    }
    if (!ns_missing) {
      esp_err_t err = read_field(h, f, (uint8_t *)s + f->off);
      if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(TAG, "%s/%s: %s", f->ns, f->key, esp_err_to_name(err));
    }
  }
  if (open_ns != NULL && !ns_missing)
    nvs_close(h);
}

esp_err_t settings_init(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  // A broken NVS still boots, on defaults: a remote that won't start is worse.
  if (err != ESP_OK)
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));

  store_t s = {.s = settings_defaults};
  load_fields(&s, pref_fields, COUNT(pref_fields));
  load_fields(&s, cal_fields, COUNT(cal_fields));
  settings_sanitize(&s.s);

  save_mutex = xSemaphoreCreateMutexStatic(&save_mutex_buf);
  taskENTER_CRITICAL(&store_lock);
  store = s;
  taskEXIT_CRITICAL(&store_lock);
  atomic_fetch_add(&epoch, 1);
  ESP_LOGI(TAG, "Loaded");
  return err;
}

void settings_get(settings_t *out) {
  taskENTER_CRITICAL(&store_lock);
  *out = store.s;
  taskEXIT_CRITICAL(&store_lock);
}

void settings_get_cal(settings_cal_t *out) {
  taskENTER_CRITICAL(&store_lock);
  *out = store.cal;
  taskEXIT_CRITICAL(&store_lock);
}

uint32_t settings_epoch(void) { return atomic_load(&epoch); }

static void log_save(esp_err_t err) {
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
}

esp_err_t settings_set(const settings_t *in) {
  xSemaphoreTake(save_mutex, portMAX_DELAY);
  store_t next;
  taskENTER_CRITICAL(&store_lock);
  next = store;
  taskEXIT_CRITICAL(&store_lock);
  next.s = *in;
  settings_sanitize(&next.s);
  esp_err_t err = save_fields(&next, pref_fields, COUNT(pref_fields));
  taskENTER_CRITICAL(&store_lock);
  // Motor config is the VESC's, never the caller's (possibly stale) copy.
  next.s.motor_poles = store.s.motor_poles;
  next.s.gear_ratio_x1000 = store.s.gear_ratio_x1000;
  next.s.wheel_diameter_mm = store.s.wheel_diameter_mm;
  store.s = next.s;
  taskEXIT_CRITICAL(&store_lock);
  atomic_fetch_add(&epoch, 1);
  xSemaphoreGive(save_mutex);
  log_save(err);
  return err;
}

esp_err_t settings_set_cal(const settings_cal_t *in) {
  xSemaphoreTake(save_mutex, portMAX_DELAY);
  store_t next;
  taskENTER_CRITICAL(&store_lock);
  next = store;
  taskEXIT_CRITICAL(&store_lock);
  next.cal = *in;
  esp_err_t err = save_fields(&next, cal_fields, COUNT(cal_fields));
  taskENTER_CRITICAL(&store_lock);
  store.cal = next.cal;
  taskEXIT_CRITICAL(&store_lock);
  atomic_fetch_add(&epoch, 1);
  xSemaphoreGive(save_mutex);
  log_save(err);
  return err;
}

void settings_update_motor(uint8_t motor_poles, uint16_t gear_ratio_x1000,
                           uint16_t wheel_diameter_mm) {
  bool changed;
  taskENTER_CRITICAL(&store_lock);
  changed = store.s.motor_poles != motor_poles ||
            store.s.gear_ratio_x1000 != gear_ratio_x1000 ||
            store.s.wheel_diameter_mm != wheel_diameter_mm;
  store.s.motor_poles = motor_poles;
  store.s.gear_ratio_x1000 = gear_ratio_x1000;
  store.s.wheel_diameter_mm = wheel_diameter_mm;
  taskEXIT_CRITICAL(&store_lock);
  if (changed) {
    atomic_fetch_add(&epoch, 1);
    ESP_LOGI(TAG, "Motor config from VESC: %d poles, gear %.3f, wheel %d mm",
             motor_poles, gear_ratio_x1000 / 1000.0f, wheel_diameter_mm);
  }
}
