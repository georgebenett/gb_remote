// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "rx_ota.h"
#include "ble.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gb_protocol.h"
#include "power.h"
#include "ui_updater.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TAG "RX_OTA"

#define RX_FW_PARTITION_SUBTYPE 0x40 // partitions.csv: rx_fw
#define RX_FW_MAGIC 0x315A4247       // "GBZ1"

// rx_fw as pack_rx_fw.py writes it: this header, then the receiver's app image
// as one zlib stream, which is what goes over the air.
typedef struct {
  uint32_t magic;
  uint32_t raw_len; // app image bytes
  uint32_t z_len;   // zlib bytes after the header
  char version[32]; // the image's esp_app_desc_t version
} rx_fw_hdr_t;

#define CHUNK_HEADER 5 // cmd + offset
#define ATT_HEADER 3
#define CHUNK_MAX 490 // + 12 header bytes = two full 251-byte radio packets
#define REPLY_TIMEOUT_MS 2000
#define BEGIN_TIMEOUT_MS 30000 // ERASING -> OK: erases what isn't blank yet
#define END_TIMEOUT_MS 15000 // receiver hashes the whole image before replying
#define MAX_RETRIES 5
#define WINDOW 24    // chunks in flight; well past the receiver's 4 KB ack step
#define DRAIN_MS 300 // quiet time that means no more replies are in flight

typedef struct {
  uint8_t cmd;
  uint8_t status;
  uint32_t next;
} reply_t;

static QueueHandle_t replies = NULL;
static atomic_bool active;
static bool attempted = false; // one try per boot: a bad image must not loop
static const esp_partition_t *part = NULL;
static uint32_t image_len = 0; // zlib stream bytes
static uint32_t raw_len = 0;
static uint8_t bundled[3];
static int target_link = 0;
static char from_str[12];
static char to_str[12];

bool rx_ota_active(void) { return active; }

static bool probe_image(void) {
  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  RX_FW_PARTITION_SUBTYPE, NULL);
  rx_fw_hdr_t hdr;
  if (part == NULL ||
      esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
      hdr.magic != RX_FW_MAGIC || hdr.z_len == 0 ||
      hdr.z_len > part->size - sizeof(hdr) ||
      sscanf(hdr.version, "%hhu.%hhu.%hhu", &bundled[0], &bundled[1],
             &bundled[2]) != 3) {
    return false;
  }
  image_len = hdr.z_len;
  raw_len = hdr.raw_len;
  ESP_LOGI(TAG, "rx_fw: gb_receiver v%.32s (%lu bytes, %lu packed)",
           hdr.version, (unsigned long)raw_len, (unsigned long)image_len);
  return true;
}

void rx_ota_init(void) {
  if (!probe_image()) {
    ESP_LOGW(TAG, "rx_fw: no receiver image, receivers will not be updated");
    part = NULL;
  }
}

/** Wait for the reply to `cmd`; stale replies to other commands are dropped. */
static bool wait_for(uint8_t cmd, uint32_t timeout_ms, reply_t *r) {
  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  for (;;) {
    TickType_t left = deadline - xTaskGetTickCount();
    if ((int32_t)left <= 0 || xQueueReceive(replies, r, left) != pdTRUE) {
      return false;
    }
    if (r->cmd == cmd) {
      return true;
    }
  }
}

static bool send_and_wait(const uint8_t *buf, uint16_t len, uint32_t timeout_ms,
                          reply_t *r) {
  return ble_receiver_write_cmd(target_link, buf, len) == ESP_OK &&
         wait_for(buf[0], timeout_ms, r);
}

/** Make the receiver drop a half-done update and take the throttle again. */
static void abort_update(void) {
  uint8_t end = GB_CMD_OTA_END;
  ble_receiver_write_cmd(target_link, &end, 1);
}

/** After a lost chunk or reply: let the in-flight replies land and return
 *  where the receiver last said to resume, or `resume` if it said nothing. */
static uint32_t drain(uint32_t resume) {
  reply_t r;
  while (xQueueReceive(replies, &r, pdMS_TO_TICKS(DRAIN_MS)) == pdTRUE) {
    if (r.cmd == GB_CMD_OTA_DATA) {
      resume = r.next;
    }
  }
  return resume;
}

// Keeps WINDOW chunks in flight; every reply carries the receiver's written
// offset, so a lost ack is covered by the next one.
static void rx_ota_task(void *arg) {
  static uint8_t buf[CHUNK_HEADER + CHUNK_MAX];
  reply_t r;

  xQueueReset(replies);
  active = true; // throttle neutral from here: the receiver erases first
  buf[0] = GB_CMD_OTA_BEGIN;
  gb_wr_u32(&buf[1], image_len);
  gb_wr_u32(&buf[5], raw_len);
  if (!send_and_wait(buf, 9, REPLY_TIMEOUT_MS, &r) ||
      r.status != GB_OTA_ERASING) {
    ESP_LOGW(TAG, "Receiver declined the update"); // e.g. it is moving
    abort_update();
    active = false;
    vTaskDelete(NULL);
  }
  ble_receiver_ota_link(target_link, true);
  ui_fw_update_show(from_str, to_str);
  TickType_t t0 = xTaskGetTickCount();
  bool ok =
      wait_for(GB_CMD_OTA_BEGIN, BEGIN_TIMEOUT_MS, &r) && r.status == GB_OTA_OK;
  TickType_t t_erased = xTaskGetTickCount();

  uint32_t chunk = ble_receiver_mtu(target_link) - ATT_HEADER - CHUNK_HEADER;
  if (chunk > CHUNK_MAX) {
    chunk = CHUNK_MAX;
  }
  uint32_t sent = 0;
  uint32_t acked = 0;
  int retries = 0;
  int resyncs = 0;
  int shown = 0;
  while (ok && acked < image_len) {
    power_reset_inactivity_timer();
    bool congested = false;
    while (sent < image_len && sent - acked < WINDOW * chunk) {
      if (!ble_receiver_can_send(target_link)) {
        congested = true; // Bluedroid queue full: retry in a tick
        break;
      }
      uint32_t n = image_len - sent < chunk ? image_len - sent : chunk;
      buf[0] = GB_CMD_OTA_DATA;
      gb_wr_u32(&buf[1], sent);
      if (esp_partition_read(part, sizeof(rx_fw_hdr_t) + sent,
                             &buf[CHUNK_HEADER], n) != ESP_OK) {
        ok = false;
        break;
      }
      if (ble_receiver_write_cmd(target_link, buf, CHUNK_HEADER + n) !=
          ESP_OK) {
        congested = true;
        break;
      }
      sent += n;
    }
    if (!ok) {
      break;
    }
    // Acks come every 4 KB, so only a full window or a fully sent image may
    // wait long for one.
    bool got = xQueueReceive(replies, &r,
                             congested ? 1 : pdMS_TO_TICKS(REPLY_TIMEOUT_MS)) ==
               pdTRUE;
    if (!got && congested) {
      ok = ble_receiver_is_connected(target_link);
      continue;
    }
    if (got && r.cmd != GB_CMD_OTA_DATA) {
      continue;
    }
    if (got && (r.next > image_len ||
                (r.status != GB_OTA_OK && r.status != GB_OTA_BAD_OFFSET))) {
      ok = false;
    } else if (got && r.status == GB_OTA_OK) {
      acked = r.next;
      if (sent < acked) {
        sent = acked;
      }
      retries = 0;
      int pct = (int)((uint64_t)acked * 100 / image_len);
      if (pct != shown) {
        shown = pct;
        ui_fw_update_progress(pct);
      }
    } else {
      // A chunk or its reply went missing: resume where the receiver is.
      ok = ++retries <= MAX_RETRIES && ble_receiver_is_connected(target_link);
      resyncs++;
      acked = sent = drain(got ? r.next : acked);
      ok = ok && acked <= image_len;
    }
  }
  TickType_t t_sent = xTaskGetTickCount();
  if (ok) {
    buf[0] = GB_CMD_OTA_END;
    ok = send_and_wait(buf, 1, END_TIMEOUT_MS, &r) && r.status == GB_OTA_OK;
  }
  if (!ok) {
    abort_update();
  }
  uint32_t send_ms = pdTICKS_TO_MS(t_sent - t_erased);
  ESP_LOGW(TAG,
           "Receiver update %s at %lu/%lu: erase %lu ms, send %lu ms "
           "(%lu KB/s, %d resyncs), verify %lu ms",
           ok ? "done" : "failed", (unsigned long)acked,
           (unsigned long)image_len,
           (unsigned long)pdTICKS_TO_MS(t_erased - t0), (unsigned long)send_ms,
           (unsigned long)(send_ms ? acked / send_ms : 0), resyncs,
           (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - t_sent));
  ui_fw_update_done(ok);
  active = false;
  ble_receiver_ota_link(target_link, false);
  vTaskDelete(NULL);
}

void rx_ota_on_version(int link_idx, uint8_t major, uint8_t minor,
                       uint8_t patch) {
  if (attempted) {
    return;
  }
  uint8_t rx[3] = {major, minor, patch};
  if (part == NULL || memcmp(bundled, rx, sizeof(rx)) <= 0) {
    return;
  }
  if (replies == NULL) {
    replies = xQueueCreate(2 * WINDOW, sizeof(reply_t));
  }
  attempted = true;
  target_link = link_idx;
  snprintf(from_str, sizeof(from_str), "%u.%u.%u", major, minor, patch);
  snprintf(to_str, sizeof(to_str), "%u.%u.%u", bundled[0], bundled[1],
           bundled[2]);
  ESP_LOGW(TAG, "Receiver %d on %s, updating to %s", link_idx, from_str,
           to_str);
  ESP_ERROR_CHECK(xTaskCreate(rx_ota_task, "rx_ota", 4096, NULL, 5, NULL) ==
                          pdPASS
                      ? ESP_OK
                      : ESP_ERR_NO_MEM);
}

void rx_ota_on_reply(const uint8_t *v, uint16_t len) {
  if (replies == NULL || len < 6) {
    return;
  }
  reply_t r = {.cmd = v[0], .status = v[1], .next = gb_rd_u32(&v[2])};
  xQueueSend(replies, &r, 0);
}
