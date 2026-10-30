// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "settings.h"

const settings_t settings_defaults = {
    .motor_poles = 10, // until the VESC reports its own
    .gear_ratio_x1000 = 1000,
    .wheel_diameter_mm = 100,
    .battery_cell_type = 0, // high-drain li-ion (P42A/30Q/40T class)
    .throttle_curve_mode = THROTTLE_CURVE_EXPO,
    .assist_strength = ASSIST_STRENGTH_DEFAULT,
    .assist_decay = ASSIST_DECAY_DEFAULT,
    .ride_profile = RIDE_PROFILE_ROCKET,
    .backlight_pct = SETTINGS_BACKLIGHT_DEFAULT,
    .haptic_pct = SETTINGS_HAPTIC_DEFAULT,
};

/* Out-of-range values are dropped, not clamped: a bad read gives the rider the
 * default, not a shape they never asked for. */
void settings_sanitize(settings_t *s) {
  const settings_t *d = &settings_defaults;
  if (s->throttle_curve_acc < THROTTLE_CURVE_MIN ||
      s->throttle_curve_acc > THROTTLE_CURVE_MAX)
    s->throttle_curve_acc = d->throttle_curve_acc;
  if (s->throttle_curve_brake < THROTTLE_CURVE_MIN ||
      s->throttle_curve_brake > THROTTLE_CURVE_MAX)
    s->throttle_curve_brake = d->throttle_curve_brake;
  if (s->throttle_curve_mode >= THROTTLE_CURVE_MODE_COUNT)
    s->throttle_curve_mode = d->throttle_curve_mode;
  if (s->assist_strength < ASSIST_STRENGTH_MIN ||
      s->assist_strength > ASSIST_STRENGTH_MAX)
    s->assist_strength = d->assist_strength;
  if (s->assist_decay < ASSIST_DECAY_MIN || s->assist_decay > ASSIST_DECAY_MAX)
    s->assist_decay = d->assist_decay;
  // Older firmware saved a mode: Off, 20/25 km/h, 20/25 mph.
  static const uint8_t legacy_kmh[] = {0, 20, 25, 32, 40};
  if (s->speed_limit_kmh < sizeof(legacy_kmh))
    s->speed_limit_kmh = legacy_kmh[s->speed_limit_kmh];
  if (!speed_limit_kmh_valid(s->speed_limit_kmh))
    s->speed_limit_kmh = d->speed_limit_kmh;
  if (s->ride_profile >= RIDE_PROFILE_COUNT)
    s->ride_profile = d->ride_profile;
  if (s->battery_cells > 20)
    s->battery_cells = d->battery_cells;
  if (s->backlight_pct > 100)
    s->backlight_pct = d->backlight_pct;
  if (s->haptic_pct > 100)
    s->haptic_pct = d->haptic_pct;
  if (s->ble_trim < -127)
    s->ble_trim = -127;
}

int32_t settings_speed(const settings_t *s, int32_t erpm) {
  if (s->motor_poles == 0 || s->gear_ratio_x1000 == 0)
    return 0;
  float rpm = (float)erpm / (s->motor_poles / 2.0f);
  float wheel_rpm = rpm / (s->gear_ratio_x1000 / 1000.0f);
  float circumference_m = s->wheel_diameter_mm / 1000.0f * 3.14159265f;
  float kmh = wheel_rpm * circumference_m * 60.0f / 1000.0f;
  if (kmh < 0.0f)
    kmh = -kmh;
  if (s->speed_unit_mph)
    kmh *= 0.621371f;
  return (int32_t)kmh;
}

// Tuned defaults modeled on Boosted-style Beginner/Eco/Expert/Pro tiers, not a
// certified standard. The ramp is applied as an ease-in in throttle.c.
uint16_t settings_ride_profile_ramp_ms(uint8_t ride_profile) {
  static const uint16_t ramp_ms[RIDE_PROFILE_COUNT] = {
      5000, // Beginner
      2500, // Eco
      1000, // Medium
      0,    // Rocket: instant
  };
  return ride_profile < RIDE_PROFILE_COUNT ? ramp_ms[ride_profile] : 0;
}

// Share of the receiver's max motor current each profile may drive.
uint8_t settings_ride_profile_current_pct(uint8_t ride_profile) {
  static const uint8_t pct[RIDE_PROFILE_COUNT] = {40, 60, 80, 100};
  return ride_profile < RIDE_PROFILE_COUNT ? pct[ride_profile] : 100;
}
