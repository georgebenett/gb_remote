// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef SETTINGS_H
#define SETTINGS_H

#include "esp_err.h"
#include "throttle_math.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Everything the remote persists. Loaded from NVS once at boot into RAM;
 * getters copy from RAM, setters validate, write through and bump
 * settings_epoch(). No other module touches NVS.
 */

// Speed limit cap range, whole km/h; 0 means off.
#define SPEED_LIMIT_MIN_KMH 20
#define SPEED_LIMIT_MAX_KMH 50

static inline bool speed_limit_kmh_valid(uint8_t kmh) {
  return kmh == 0 || (kmh >= SPEED_LIMIT_MIN_KMH && kmh <= SPEED_LIMIT_MAX_KMH);
}

// Ride profiles: Beginner, Eco, Medium, Rocket.
#define RIDE_PROFILE_COUNT 4
#define RIDE_PROFILE_ROCKET (RIDE_PROFILE_COUNT - 1) // instant, no cap

// Assistive push tuning, clamped again by the receiver that runs it.
#define ASSIST_STRENGTH_DEFAULT 15 // percent of the VESC current limit
#define ASSIST_STRENGTH_MIN 5
#define ASSIST_STRENGTH_MAX 80
#define ASSIST_DECAY_DEFAULT 30 // motor rpm/s bled off the held speed
#define ASSIST_DECAY_MIN 10
#define ASSIST_DECAY_MAX 120

#define SETTINGS_BACKLIGHT_DEFAULT 50 // percent
#define SETTINGS_HAPTIC_DEFAULT 100   // percent

typedef struct {
  // Motor config from the VESC over BLE; RAM only, never saved.
  uint8_t motor_poles;
  uint16_t gear_ratio_x1000;
  uint16_t wheel_diameter_mm;
  // User preferences
  bool speed_unit_mph;       // false = km/h
  bool dual_connection;      // Allow connecting to two receivers simultaneously
  uint8_t battery_cells;     // Skate pack series count: 0 = unset (show pack
                             // voltage), else 5..20 (estimate percentage)
  uint8_t battery_cell_type; // Cell chemistry (battery_cell_type_t) picking
                             // which SoC curve the estimate uses
  bool smart_reverse;        // VESC-style smart reverse, run by the receiver
  bool no_reverse;           // Below neutral only brakes, run by the receiver
  int8_t throttle_curve_acc; // Throttle curve, tenths (see throttle_math.h)
  int8_t throttle_curve_brake; // Brake curve, tenths
  uint8_t throttle_curve_mode; // throttle_curve_mode_t
  bool assist_push;        // Assistive push (endless mode), run by the receiver
  uint8_t assist_strength; // Assistive push strength, percent of current limit
  uint8_t assist_decay;    // Assistive push decay, motor rpm/s
  uint8_t speed_limit_kmh; // Speed cap in whole km/h, 0 off, else
                           // SPEED_LIMIT_MIN..MAX_KMH; enforced by the receiver
  uint8_t ride_profile;    // 0 Beginner .. 3 Rocket: eased throttle ramp here,
                           // drive current cap on the receiver. Rocket = off.
  bool invert_throttle;    // LITE: lever direction
  uint8_t backlight_pct;
  uint8_t haptic_pct;
  bool aux_on;     // Aux output requested by the remote
  int8_t ble_trim; // Output trim, -127..127
  uint16_t snake_high_score;
} settings_t;

// Throttle (and on DUAL, brake) ADC range from calibration.
typedef struct {
  bool done;
  uint32_t throttle_min, throttle_max;
  uint32_t brake_min, brake_max;
} settings_cal_t;

/** Load everything from NVS; missing or invalid keys keep their defaults. */
esp_err_t settings_init(void);
void settings_get(settings_t *out);
/** Validate (out-of-range fields fall back to defaults) and save. RAM is
 *  updated even if the flash write fails; the error is returned. */
esp_err_t settings_set(const settings_t *in);
/** Bumped by every settings_set(), so pollers re-read only on change. */
uint32_t settings_epoch(void);

void settings_get_cal(settings_cal_t *out);
esp_err_t settings_set_cal(const settings_cal_t *in);

// Pure, in settings_rules.c (host-tested by test/test_settings.c):
extern const settings_t settings_defaults;
/** Replace out-of-range fields of `s` with their defaults. */
void settings_sanitize(settings_t *s);
/** Board speed in the configured unit, from the motor config in `s`. */
int32_t settings_speed(const settings_t *s, int32_t erpm);

// Time (ms) a profile takes to ramp neutral -> full throttle; 0 = instant.
// Braking is never ramped.
uint16_t settings_ride_profile_ramp_ms(uint8_t ride_profile);

// Percent of max motor current the profile may drive; enforced by the receiver.
uint8_t settings_ride_profile_current_pct(uint8_t ride_profile);

// Motor config from the VESC (called by BLE when telemetry arrives).
void settings_update_motor(uint8_t motor_poles, uint16_t gear_ratio_x1000,
                           uint16_t wheel_diameter_mm);

#endif // SETTINGS_H
