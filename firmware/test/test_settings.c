// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/*
 * Host tests for the settings rules: whatever NVS hands back, the remote runs
 * on in-range values, and the speed readout matches the motor config.
 *
 *   cc -Wall -Wextra -Itest/stubs -o /tmp/s main/settings_rules.c
 * test/test_settings.c && /tmp/s
 */
#include "../main/settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_defaults_are_sane(void) {
  settings_t s = settings_defaults;
  settings_sanitize(&s);
  assert(memcmp(&s, &settings_defaults, sizeof(s)) == 0);
  assert(s.motor_poles == 10 && s.gear_ratio_x1000 == 1000 &&
         s.wheel_diameter_mm == 100);
  assert(s.ride_profile == RIDE_PROFILE_ROCKET && s.speed_limit_kmh == 0);
}

static void test_garbage_falls_back(void) {
  settings_t s;
  memset(&s, 0xA5, sizeof(s));
  s.speed_unit_mph = s.dual_connection = s.smart_reverse = s.no_reverse =
      s.assist_push = s.invert_throttle = s.aux_on = false;
  settings_sanitize(&s);
  assert(s.throttle_curve_acc == 0 && s.throttle_curve_brake == 0);
  assert(s.throttle_curve_mode == THROTTLE_CURVE_EXPO);
  assert(s.assist_strength == ASSIST_STRENGTH_DEFAULT);
  assert(s.assist_decay == ASSIST_DECAY_DEFAULT);
  assert(s.speed_limit_kmh == 0);
  assert(s.ride_profile == RIDE_PROFILE_ROCKET);
  assert(s.battery_cells == 0);
  assert(s.backlight_pct == SETTINGS_BACKLIGHT_DEFAULT);
  assert(s.haptic_pct == SETTINGS_HAPTIC_DEFAULT);
  assert(s.ble_trim >= -127);
}

static void test_speed_limit_legacy_and_range(void) {
  const uint8_t legacy[] = {0, 20, 25, 32, 40};
  for (uint8_t i = 0; i < 5; i++) {
    settings_t s = settings_defaults;
    s.speed_limit_kmh = i;
    settings_sanitize(&s);
    assert(s.speed_limit_kmh == legacy[i]);
  }
  for (int kmh = 5; kmh < 256; kmh++) {
    settings_t s = settings_defaults;
    s.speed_limit_kmh = (uint8_t)kmh;
    settings_sanitize(&s);
    assert(s.speed_limit_kmh ==
           (speed_limit_kmh_valid((uint8_t)kmh) ? kmh : 0));
  }
}

static void test_speed(void) {
  settings_t s = settings_defaults;
  s.motor_poles = 14, s.gear_ratio_x1000 = 2200, s.wheel_diameter_mm = 115;
  /* 7 pole pairs, 2.2:1, 115 mm: 30000 erpm is ~42 km/h. */
  assert(settings_speed(&s, 30000) == 42);
  assert(settings_speed(&s, -30000) == 42);
  s.speed_unit_mph = true;
  assert(settings_speed(&s, 30000) == 26);
  s.motor_poles = 0;
  assert(settings_speed(&s, 30000) == 0);
}

int main(void) {
  test_defaults_are_sane();
  test_garbage_falls_back();
  test_speed_limit_legacy_and_range();
  test_speed();
  printf("settings: all checks passed\n");
  return 0;
}
