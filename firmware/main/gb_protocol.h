/*
 * Remote <-> receiver BLE protocol. gb_receiver owns this file;
 * bundle_to_remotes.sh copies it into gb_remote and gb_remote_dual_pro, so
 * edit it here only. Host test: test/test_gb_protocol.c.
 *
 * All multi-byte fields are little-endian. New fields go at the end of a
 * frame and receivers accept longer frames, so old peers keep working.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GB_PROTOCOL_VERSION 1

#define GB_RECEIVER_ADV_NAME "GS-THUMB" // receiver advert, remote connects
#define GB_REMOTE_ADV_NAME "GS-REM"     // remote pairing advert, receiver scans

#define GB_UUID_SERVICE 0xABF0
#define GB_UUID_THROTTLE 0xABF1  // remote -> receiver, gb_throttle_frame_t
#define GB_UUID_TELEMETRY 0xABF2 // receiver -> remote, gb_telemetry_t
#define GB_UUID_COMMAND 0xABF3   // remote -> receiver, [gb_cmd_t, args...]
#define GB_UUID_STATUS 0xABF4    // receiver -> remote, [gb_cmd_t, ...]

typedef enum {
  GB_CMD_RESET_ODOMETER = 0x01,    // [] -> status [0x01, 0 = ok]
  GB_CMD_SHUTDOWN = 0x02,          // [] remote powers off on purpose
  GB_CMD_SET_SMART_REVERSE = 0x03, // [enabled]
  GB_CMD_SET_ASSIST_PUSH = 0x04,   // [enabled, strength %, decay rpm/s]
  GB_CMD_SET_NO_REVERSE = 0x05,    // [enabled]
  GB_CMD_SET_SPEED_LIMIT = 0x06,   // [cap km/h], 0 = off
  GB_CMD_GET_FW_VERSION =
      0x07,               // [] -> status [0x07, major, minor, patch, mac x6]
  GB_STATUS_FRAME = 0x08, // status only, once a second: gb_status_frame_t
  GB_CMD_SET_CURRENT_SCALE = 0x09, // [pct] drive current share
  GB_CMD_OTA_BEGIN = 0x0A, // [zlib_len u32, image_len u32] -> ERASING, then OK
  GB_CMD_OTA_DATA = 0x0B,  // [offset u32, zlib bytes...]
  GB_CMD_OTA_END = 0x0C,   // [] verify, switch slot, reboot; early, it aborts
} gb_cmd_t;

// Every OTA reply is [cmd, gb_ota_status_t, next_offset u32] on status.
typedef enum {
  GB_OTA_OK = 0,
  GB_OTA_REFUSED = 1, // wheels turning, or no update in progress
  GB_OTA_FLASH_ERR = 2,
  GB_OTA_BAD_OFFSET = 3, // next_offset says where to resume
  GB_OTA_ERASING = 4,    // BEGIN accepted; OK follows once erased
} gb_ota_status_t;

// 0 = full brake, 128 = neutral, 255 = full throttle.
#define GB_THROTTLE_NEUTRAL 128

typedef struct __attribute__((packed)) {
  uint16_t throttle; // 0..255, receiver clamps
  uint8_t aux_on;
} gb_throttle_frame_t;

// Scaled fields are value * 100 unless noted.
typedef struct __attribute__((packed)) {
  int16_t temp_mos;
  int16_t temp_motor;
  int16_t current_motor;
  int16_t current_in;
  int32_t erpm; // not scaled
  int16_t v_in;
  int16_t bms_total_voltage;
  int16_t bms_current;
  int16_t bms_remaining_ah;
  int16_t bms_nominal_ah;
  uint8_t bms_cells;
  int16_t cell_mv[16]; // not scaled, zero past bms_cells
  uint8_t motor_poles;
  uint16_t gear_ratio_x1000;
  uint16_t wheel_diameter_mm;
  uint8_t aux_on;
  int32_t trip_km_x100;
} gb_telemetry_t;

_Static_assert(sizeof(gb_throttle_frame_t) == 3, "throttle frame is 3 bytes");
_Static_assert(sizeof(gb_telemetry_t) == 65, "telemetry frame is 65 bytes");
_Static_assert(__builtin_offsetof(gb_telemetry_t, motor_poles) == 55,
               "telemetry layout");
_Static_assert(__builtin_offsetof(gb_telemetry_t, trip_km_x100) == 61,
               "telemetry layout");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "frames are memcpy'd as little-endian");

#define GB_STATUS_FLAG_SMART_REVERSE (1u << 0)
#define GB_STATUS_FLAG_NO_REVERSE (1u << 1)
#define GB_STATUS_FLAG_ASSIST_PUSH (1u << 2)
#define GB_STATUS_FLAG_FAILSAFE (1u << 3)
#define GB_STATUS_FLAG_BMS (1u << 4)

typedef struct __attribute__((packed)) {
  uint8_t id; // GB_STATUS_FRAME
  uint8_t flags;
  uint8_t vesc_fault;
  uint8_t failsafe_reason;
  uint8_t speed_limit_kmh;
  uint8_t assist_strength_pct;
  uint8_t assist_decay;
  uint32_t uptime_s;
} gb_status_frame_t;

_Static_assert(sizeof(gb_status_frame_t) == 11, "status frame is 11 bytes");

static inline uint16_t gb_rd_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static inline int16_t gb_rd_i16(const uint8_t *p) {
  return (int16_t)gb_rd_u16(p);
}
static inline uint32_t gb_rd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static inline int32_t gb_rd_i32(const uint8_t *p) {
  return (int32_t)gb_rd_u32(p);
}
static inline void gb_wr_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* Telemetry frames shorter than gb_telemetry_t are rejected; longer ones are
 * from a newer receiver and decode their known prefix. */
static inline bool gb_telemetry_decode(const uint8_t *buf, uint16_t len,
                                       gb_telemetry_t *out) {
  if (len < sizeof(*out))
    return false;
  memcpy(out, buf, sizeof(*out));
  return true;
}

/* Exact advertised-name match: a prefix or an empty name is not a match. */
static inline bool gb_adv_name_matches(const uint8_t *name, uint8_t len,
                                       const char *want) {
  return name != NULL && len == strlen(want) && memcmp(name, want, len) == 0;
}
