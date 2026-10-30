#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Receiver firmware update over BLE, from the gb_receiver.bin flashed into the
 * rx_fw partition. Runs when a receiver reports a version older than it. */

/** Read and log the bundled image's version. Call once, before BLE starts. */
void rx_ota_init(void);

/** A receiver answered BLE_CMD_GET_FW_VERSION. Starts an update (once per
 *  remote boot) when the bundled image is newer. Called on the BTC task. */
void rx_ota_on_version(int link_idx, uint8_t major, uint8_t minor,
                       uint8_t patch);

/** A status notification whose first byte is an OTA command (0x0A-0x0C). */
void rx_ota_on_reply(const uint8_t *v, uint16_t len);

/** True while the receiver is taking an update: send throttle as neutral. */
bool rx_ota_active(void);
