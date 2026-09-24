#include "throttle_math.h"
#include <math.h>

uint8_t throttle_map_ble_value(int32_t throttle_raw, int32_t brake_raw,
                               uint32_t throttle_min, uint32_t throttle_max,
                               uint32_t brake_min, uint32_t brake_max) {
  int32_t t_min = (int32_t)throttle_min, t_max = (int32_t)throttle_max;
  int32_t b_min = (int32_t)brake_min, b_max = (int32_t)brake_max;

  /* An inverted or empty calibration would divide by zero or run the scale
   * backwards; neutral is the safe answer. */
  if (t_max <= t_min || b_max <= b_min) {
    return VESC_NEUTRAL_VALUE;
  }

  if (throttle_raw < t_min)
    throttle_raw = t_min;
  if (throttle_raw > t_max)
    throttle_raw = t_max;
  if (brake_raw < b_min)
    brake_raw = b_min;
  if (brake_raw > b_max)
    brake_raw = b_max;

  /* Brake factor is a continuous multiplier that attenuates throttle output
   * (0.0 = no attenuation, 1.0 = full attenuation), rather than a hard
   * override. */
  float brake_factor = (float)(brake_raw - b_min) / (float)(b_max - b_min);
  float throttle_factor =
      (float)(throttle_raw - t_min) / (float)(t_max - t_min);

  uint8_t throttle_ble_value =
      VESC_NEUTRAL_VALUE +
      (uint8_t)(throttle_factor * (255 - VESC_NEUTRAL_VALUE));

  return (uint8_t)(throttle_ble_value * (1.0f - brake_factor));
}

uint8_t throttle_apply_trim(uint8_t value, int8_t trim_offset) {
  /* trim_offset is int8_t, so centre spans 1..255 and cannot leave the
   * output range - no clamp needed here. Asserted in the host tests. */
  int32_t center = VESC_NEUTRAL_VALUE + trim_offset;
  int32_t lo_edge = VESC_NEUTRAL_VALUE - THROTTLE_NEUTRAL_DEADBAND;
  int32_t hi_edge = VESC_NEUTRAL_VALUE + THROTTLE_NEUTRAL_DEADBAND;

  /* Inside the deadband snaps to centre. Outside it, each half is rescaled
   * from the deadband edge (not neutral) to the rail, so output ramps up
   * from centre instead of jumping by the deadband width, and the full 0-255
   * span is preserved. */
  int32_t scaled;
  if (value >= lo_edge && value <= hi_edge) {
    scaled = center;
  } else if (value < lo_edge) {
    scaled = (int32_t)((float)value * (float)center / (float)lo_edge + 0.5f);
  } else {
    scaled =
        center + (int32_t)((float)(value - hi_edge) * (float)(255 - center) /
                               (float)(255 - hi_edge) +
                           0.5f);
  }

  if (scaled < 0)
    scaled = 0;
  if (scaled > 255)
    scaled = 255;
  return (uint8_t)scaled;
}

/* One side of the curve on a 0..1 magnitude, from VESC's
 * utils_throttle_curve(). Every branch maps 0 to 0 and 1 to 1. */
static float curve_shape(float a, float curve, uint8_t mode) {
  float ret;

  if (mode == THROTTLE_CURVE_EXPO) {
    ret = (curve >= 0.0f) ? 1.0f - powf(1.0f - a, 1.0f + curve)
                          : powf(a, 1.0f - curve);
  } else if (mode == THROTTLE_CURVE_NATURAL) {
    if (fabsf(curve) < 1e-10f) {
      ret = a;
    } else if (curve >= 0.0f) {
      ret = 1.0f - ((expf(curve * (1.0f - a)) - 1.0f) / (expf(curve) - 1.0f));
    } else {
      ret = (expf(-curve * a) - 1.0f) / (expf(-curve) - 1.0f);
    }
  } else if (mode == THROTTLE_CURVE_POLY) {
    ret = (curve >= 0.0f) ? 1.0f - ((1.0f - a) / (1.0f + curve * a))
                          : a / (1.0f - curve * (1.0f - a));
  } else {
    ret = a; /* unknown mode is linear, never a surprise shape */
  }

  /* Rounding above can land a hair outside. */
  if (ret < 0.0f)
    ret = 0.0f;
  if (ret > 1.0f)
    ret = 1.0f;
  return ret;
}

uint8_t throttle_apply_curve(uint8_t value, int8_t curve_acc,
                             int8_t curve_brake, uint8_t mode) {
  if (value == VESC_NEUTRAL_VALUE) {
    return value;
  }

  /* Spans differ: 127 above neutral, 128 below. */
  if (value > VESC_NEUTRAL_VALUE) {
    const float span = 255.0f - VESC_NEUTRAL_VALUE;
    float a = (float)(value - VESC_NEUTRAL_VALUE) / span;
    return VESC_NEUTRAL_VALUE +
           (uint8_t)(curve_shape(a, curve_acc / 10.0f, mode) * span + 0.5f);
  }

  const float span = (float)VESC_NEUTRAL_VALUE;
  float a = (float)(VESC_NEUTRAL_VALUE - value) / span;
  return VESC_NEUTRAL_VALUE -
         (uint8_t)(curve_shape(a, curve_brake / 10.0f, mode) * span + 0.5f);
}
