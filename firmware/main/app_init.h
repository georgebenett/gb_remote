/**
 * @file app_init.h
 * @brief Application initialization stages (used by main.c)
 *
 * Required inits use ESP_ERROR_CHECK (abort on failure). Optional inits
 * (VESC config, viber) log and continue on failure.
 */

#pragma once

/** Log firmware version, target, IDF version. */
void app_init_log_startup(void);

/** Button, power, NVS, VESC config, viber, ADC, button monitoring. */
void app_init_early(void);

/** Charging mode: battery, LCD, UI, then block until full boot or sleep. */
void app_run_charging_mode(void);

/**
 * After charging: wait for throttle calibration (with timeout), then BLE/USB,
 * UI splash, brightness. If calibration never completes, proceeds after timeout
 * so the device remains usable (throttle stays neutral until calibrated).
 */
void app_init_after_charging(void);
