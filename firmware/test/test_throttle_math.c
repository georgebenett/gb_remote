/*
 * Host tests for the throttle math. These two functions decide what the motor
 * does, so the properties below matter more than the point checks: output is
 * always in range, never jumps to full throttle from a fault, and never
 * decreases when you push harder.
 *
 *   cc -O1 -Wall -Wextra -o /tmp/t main/throttle_math.c
 * test/test_throttle_math.c -lm && /tmp/t
 */
#include "../main/throttle_math.h"
#include <assert.h>
#include <stdio.h>

/* A representative calibration: a pot that never reaches the rails. */
#define T_MIN 300u
#define T_MAX 3700u
#define B_MIN 250u
#define B_MAX 3800u

static uint8_t map(int32_t t, int32_t b) {
  return throttle_map_ble_value(t, b, T_MIN, T_MAX, B_MIN, B_MAX);
}

static void test_map_endpoints(void) {
  assert(map(T_MIN, B_MIN) == VESC_NEUTRAL_VALUE); /* resting = neutral */
  assert(map(T_MAX, B_MIN) == 255);                /* full throttle */
  assert(map(T_MIN, B_MAX) == 0);                  /* full brake */
  assert(map(T_MAX, B_MAX) == 0); /* brake wins over throttle */

  /* Half throttle sits halfway between neutral and full. */
  uint8_t half = map((T_MIN + T_MAX) / 2, B_MIN);
  assert(half >= 190 && half <= 192);
}

static void test_map_clamps_out_of_range_readings(void) {
  /* A reading below the calibrated floor must not wrap or read as throttle. */
  assert(map(0, B_MIN) == VESC_NEUTRAL_VALUE);
  assert(map(-1, B_MIN) == VESC_NEUTRAL_VALUE);
  assert(map(4095, B_MIN) == 255);
  /* Out-of-range brake clamps the same way. */
  assert(map(T_MAX, 0) == 255);
  assert(map(T_MAX, 4095) == 0);
}

static void test_map_rejects_bad_calibration(void) {
  /* Empty or inverted ranges must fail to neutral, never to a live throttle. */
  assert(throttle_map_ble_value(3000, 250, 500, 500, B_MIN, B_MAX) ==
         VESC_NEUTRAL_VALUE);
  assert(throttle_map_ble_value(3000, 250, T_MIN, T_MAX, 900, 900) ==
         VESC_NEUTRAL_VALUE);
  assert(throttle_map_ble_value(3000, 250, 3700, 300, B_MIN, B_MAX) ==
         VESC_NEUTRAL_VALUE);
}

static void test_map_is_monotonic_and_bounded(void) {
  /* More throttle never produces less output; more brake never produces more.
   */
  int last = -1;
  for (int32_t t = T_MIN; t <= (int32_t)T_MAX; t++) {
    uint8_t v = map(t, B_MIN);
    assert(v >= VESC_NEUTRAL_VALUE); /* brake released: never below neutral */
    assert(v >= last);
    last = v;
  }
  last = 256;
  for (int32_t b = B_MIN; b <= (int32_t)B_MAX; b++) {
    uint8_t v = map(T_MAX, b);
    assert(v <= last);
    last = v;
  }
}

static void test_trim_deadband(void) {
  /* Everything within the deadband collapses onto neutral at zero trim. */
  for (int v = VESC_NEUTRAL_VALUE - THROTTLE_NEUTRAL_DEADBAND;
       v <= VESC_NEUTRAL_VALUE + THROTTLE_NEUTRAL_DEADBAND; v++) {
    assert(throttle_apply_trim((uint8_t)v, 0) == VESC_NEUTRAL_VALUE);
  }
  /* Just outside it, output steps one count off neutral - no jump. */
  assert(throttle_apply_trim(VESC_NEUTRAL_VALUE + THROTTLE_NEUTRAL_DEADBAND + 1,
                             0) == VESC_NEUTRAL_VALUE + 1);
  assert(throttle_apply_trim(VESC_NEUTRAL_VALUE - THROTTLE_NEUTRAL_DEADBAND - 1,
                             0) == VESC_NEUTRAL_VALUE - 1);
}

static void test_trim_preserves_span_and_shifts_centre(void) {
  for (int trim = -127; trim <= 127; trim++) {
    int8_t t = (int8_t)trim;
    /* The rails stay the rails whatever the trim. */
    assert(throttle_apply_trim(0, t) == 0);
    assert(throttle_apply_trim(255, t) == 255);
    /* Neutral in lands exactly on the trimmed centre. int8_t trim keeps that
     * centre inside 1..255, which is why the implementation needs no clamp. */
    int center = VESC_NEUTRAL_VALUE + trim;
    assert(center >= 1 && center <= 255);
    assert(throttle_apply_trim(VESC_NEUTRAL_VALUE, t) == center);
  }
}

static void test_trim_is_monotonic_over_every_offset(void) {
  /* Exhaustive: 256 inputs x 255 trims. No trim may invert the stick or push
   * the output outside 0-255. */
  for (int trim = -127; trim <= 127; trim++) {
    int last = -1;
    for (int v = 0; v <= 255; v++) {
      int out = throttle_apply_trim((uint8_t)v, (int8_t)trim);
      assert(out >= 0 && out <= 255);
      assert(out >= last);
      last = out;
    }
  }
}

/* Exact values on both sides of neutral. These pin the half-LSB rounding: each
 * lower-half case lands one count higher than truncation would give. */
static void test_trim_exact_values(void) {
  struct {
    uint8_t in;
    int8_t trim;
    uint8_t want;
  } cases[] = {
      {1, -30, 1},    {1, -10, 1},     {7, 10, 9},
      {4, 20, 5},     {2, 40, 3},      {200, 0, 193},
      {200, 20, 202}, {255, -30, 255}, {180, -10, 163},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    assert(throttle_apply_trim(cases[i].in, cases[i].trim) == cases[i].want);
  }
}

static void test_trim_zero_has_no_step_at_deadband(void) {
  /* With no trim, consecutive inputs never move the output by more than
   * the stretch factor rounds to (2 counts), so there is no jump off neutral.
   */
  for (int v = 1; v <= 255; v++) {
    int d = throttle_apply_trim((uint8_t)v, 0) -
            throttle_apply_trim((uint8_t)(v - 1), 0);
    assert(d >= 0 && d <= 2);
  }
}

/* The curve reshapes the middle of the travel only: neutral and both rails are
 * fixed points in every mode at every strength, so no curve can shrink the
 * range or move the deadband. */
static void test_curve_fixed_points(void) {
  for (uint8_t mode = 0; mode < THROTTLE_CURVE_MODE_COUNT; mode++) {
    for (int c = THROTTLE_CURVE_MIN; c <= THROTTLE_CURVE_MAX; c++) {
      int8_t curve = (int8_t)c;
      assert(throttle_apply_curve(VESC_NEUTRAL_VALUE, curve, curve, mode) ==
             VESC_NEUTRAL_VALUE);
      assert(throttle_apply_curve(255, curve, curve, mode) == 255);
      assert(throttle_apply_curve(0, curve, curve, mode) == 0);
    }
  }
}

static void test_curve_zero_is_linear(void) {
  /* Zero strength must be a no-op in every mode, so "off" is exactly off. */
  for (uint8_t mode = 0; mode < THROTTLE_CURVE_MODE_COUNT; mode++) {
    for (int v = 0; v <= 255; v++) {
      assert(throttle_apply_curve((uint8_t)v, 0, 0, mode) == v);
    }
  }
  /* An unknown mode falls back to linear rather than to some other shape. */
  for (int v = 0; v <= 255; v++) {
    assert(throttle_apply_curve((uint8_t)v, 50, -50, 99) == v);
  }
}

static void test_curve_is_monotonic_and_bounded(void) {
  /* Exhaustive over mode x strength x input: pushing harder never gives less,
   * and nothing ever leaves 0-255. */
  for (uint8_t mode = 0; mode < THROTTLE_CURVE_MODE_COUNT; mode++) {
    for (int c = THROTTLE_CURVE_MIN; c <= THROTTLE_CURVE_MAX; c++) {
      int last = -1;
      for (int v = 0; v <= 255; v++) {
        int out = throttle_apply_curve((uint8_t)v, (int8_t)c, (int8_t)c, mode);
        assert(out >= 0 && out <= 255);
        assert(out >= last);
        last = out;
      }
    }
  }
}

static void test_curve_direction(void) {
  /* Negative strength is softer than linear, positive is more aggressive -
   * same sign convention as VESC's throttle exponent. Checked at half travel
   * on both sides of neutral, in every mode. */
  const uint8_t half_thr = VESC_NEUTRAL_VALUE + 64;
  const uint8_t half_brk = VESC_NEUTRAL_VALUE - 64;
  for (uint8_t mode = 0; mode < THROTTLE_CURVE_MODE_COUNT; mode++) {
    assert(throttle_apply_curve(half_thr, -30, 0, mode) < half_thr);
    assert(throttle_apply_curve(half_thr, 30, 0, mode) > half_thr);
    assert(throttle_apply_curve(half_brk, 0, -30, mode) > half_brk);
    assert(throttle_apply_curve(half_brk, 0, 30, mode) < half_brk);
  }
}

static void test_curve_sides_are_independent(void) {
  /* A brake curve must not touch the throttle half, or vice versa. */
  for (uint8_t mode = 0; mode < THROTTLE_CURVE_MODE_COUNT; mode++) {
    for (int v = VESC_NEUTRAL_VALUE; v <= 255; v++) {
      assert(throttle_apply_curve((uint8_t)v, 0, 50, mode) == v);
    }
    for (int v = 0; v <= VESC_NEUTRAL_VALUE; v++) {
      assert(throttle_apply_curve((uint8_t)v, 50, 0, mode) == v);
    }
  }
}

/* The curve feeds the trim, so the composition is what actually reaches the
 * receiver: it must still be in range, monotonic and neutral-preserving. */
static void test_curve_then_trim_composes(void) {
  for (uint8_t mode = 0; mode < THROTTLE_CURVE_MODE_COUNT; mode++) {
    for (int trim = -40; trim <= 40; trim += 10) {
      int last = -1;
      for (int v = 0; v <= 255; v++) {
        int out = throttle_apply_trim(
            throttle_apply_curve((uint8_t)v, -35, 35, mode), (int8_t)trim);
        assert(out >= 0 && out <= 255);
        assert(out >= last);
        last = out;
      }
      assert(throttle_apply_trim(
                 throttle_apply_curve(VESC_NEUTRAL_VALUE, -35, 35, mode),
                 (int8_t)trim) == VESC_NEUTRAL_VALUE + trim);
    }
  }
}

int main(void) {
  test_map_endpoints();
  test_map_clamps_out_of_range_readings();
  test_map_rejects_bad_calibration();
  test_map_is_monotonic_and_bounded();
  test_trim_deadband();
  test_trim_preserves_span_and_shifts_centre();
  test_trim_is_monotonic_over_every_offset();
  test_trim_exact_values();
  test_trim_zero_has_no_step_at_deadband();
  test_curve_fixed_points();
  test_curve_zero_is_linear();
  test_curve_is_monotonic_and_bounded();
  test_curve_direction();
  test_curve_sides_are_independent();
  test_curve_then_trim_composes();
  printf("throttle_math: all checks passed\n");
  return 0;
}
