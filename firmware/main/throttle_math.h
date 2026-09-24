#ifndef THROTTLE_MATH_H
#define THROTTLE_MATH_H

#include <stdint.h>

/*
 * Pure throttle math: this decides what the motor does, so it is kept separate
 * from the ADC driver and covered by host tests.
 *
 *   cc -O1 -Wall -Wextra -o /tmp/t main/throttle_math.c
 * test/test_throttle_math.c && /tmp/t
 */

/* Output scale to the receiver: 0 = full brake, 128 = neutral, 255 = throttle.
 */
#define VESC_NEUTRAL_VALUE 128

/* Counts either side of neutral that snap to it, so a resting thumb cannot
 * creep the motor. */
#define THROTTLE_NEUTRAL_DEADBAND 15

/* Throttle spans neutral..255 across its calibrated range; brake attenuates
 * that continuously, so full brake reaches 0 whatever the throttle is doing.
 * Out-of-range readings clamp. A bad calibration returns neutral, never
 * throttle. */
uint8_t throttle_map_ble_value(int32_t throttle_raw, int32_t brake_raw,
                               uint32_t throttle_min, uint32_t throttle_max,
                               uint32_t brake_min, uint32_t brake_max);

/* Snap to neutral inside the deadband; outside it, ramp from the trimmed centre
 * at the deadband edge to the rail, so the output keeps the full 0-255 span
 * with no step at the deadband boundary. */
uint8_t throttle_apply_trim(uint8_t value, int8_t trim_offset);

/* Modes from utils_throttle_curve() in VESC's util/utils_math.c. */
typedef enum {
  THROTTLE_CURVE_EXPO = 0,
  THROTTLE_CURVE_NATURAL,
  THROTTLE_CURVE_POLY,
  THROTTLE_CURVE_MODE_COUNT,
} throttle_curve_mode_t;

/* Strength in tenths so it fits a byte: -50..50 is -5.0..5.0. Negative soft,
 * positive sharp, 0 linear in every mode. */
#define THROTTLE_CURVE_MIN (-50)
#define THROTTLE_CURVE_MAX 50

/* Reshape a mapped 0-255 value around neutral; neutral and both rails stay
 * fixed, so the range and deadband are untouched. Applied before the trim. */
uint8_t throttle_apply_curve(uint8_t value, int8_t curve_acc,
                             int8_t curve_brake, uint8_t mode);

#endif // THROTTLE_MATH_H
