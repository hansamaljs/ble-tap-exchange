/**
 * ================================================================
 * buzzer_driver.h
 * Passive piezo buzzer driver — LEDC PWM tone generation
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 * Hardware: Passive piezo on GPIO20 via 100Ω series resistor
 *
 * WHAT THIS DRIVER DOES
 * ---------------------
 * - Drives the passive piezo using LEDC (LED PWM Controller)
 * - LEDC generates square waves in hardware — CPU is free
 * - Provides low-level buzzer_play_tone() for raw frequency control
 * - Provides named high-level sound functions for every UX event
 *
 * WHAT THIS DRIVER DOES NOT DO
 * ----------------------------
 * - Does not know about UI, BLE state, or alarm schedules
 * - Does not schedule repeating sounds (caller loops if needed)
 * - Does not own any FreeRTOS timers (prototype: blocking delays)
 *
 * SOUND MAP OVERVIEW
 * ------------------
 *   Button feedback : click, long_press, nav_up, nav_down,
 *                     confirm, cancel
 *   TapShare BLE    : ble_scanning, ble_connected, tap_detected,
 *                     data_sent, data_received, share_saved,
 *                     share_discarded, ble_timeout
 *   Alarm           : alarm_ring (loop this), alarm_snooze,
 *                     alarm_dismiss
 *
 * IMPORTANT: buzzer_alarm_ring() must be called in a loop by
 * alarm_task — it plays ONE ring cycle then returns. The caller
 * decides when to stop.
 * ================================================================
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/* ================================================================
 * SECTION 1 — LIFECYCLE
 * ================================================================ */

/**
 * @brief  Initialise LEDC timer and channel for buzzer output.
 *         Call once at startup before any sound functions.
 * @return ESP_OK on success.
 */
esp_err_t buzzer_driver_init(void);

/**
 * @brief  Reset LEDC channel and GPIO. Call on shutdown.
 */
void buzzer_driver_deinit(void);

/* ================================================================
 * SECTION 2 — LOW-LEVEL (use for testing)
 * ================================================================ */

/**
 * @brief  Play a square wave tone at freq_hz for duration_ms,
 *         then silence.
 *
 * Blocking: returns after duration_ms has elapsed.
 * Safe to call from any task context.
 *
 * @param  freq_hz     Frequency in Hz (200–4000 Hz recommended)
 * @param  duration_ms Duration in milliseconds
 * @return ESP_OK on success.
 */
esp_err_t buzzer_play_tone(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief  Stop any playing tone immediately (set duty to 0).
 */
void buzzer_silence(void);

/* ================================================================
 * SECTION 3 — BUTTON FEEDBACK SOUNDS
 * ================================================================ */

/** Short 25 ms click — every button press */
void buzzer_click(void);

/** Rising two-tone — long press detected */
void buzzer_long_press(void);

/** High brief tick — UP navigation */
void buzzer_nav_up(void);

/** Low brief tick — DOWN navigation */
void buzzer_nav_down(void);

/** Rising double-beep — action confirmed (save, set alarm, etc.) */
void buzzer_confirm(void);

/** Single low tone — action cancelled / BACK pressed */
void buzzer_cancel(void);

/* ================================================================
 * SECTION 4 — TAPSHARE BLE SOUNDS
 * ================================================================ */

/** Slow pulse — BLE scanning is active (call once per scan cycle) */
void buzzer_ble_scanning(void);

/** Rising three-tone chord — GATT connection established */
void buzzer_ble_connected(void);

/** Double rising chirp — IMU tap collision detected */
void buzzer_tap_detected(void);

/** Fast ascending sweep — profile data transmitted */
void buzzer_data_sent(void);

/** Fast descending sweep — profile data received */
void buzzer_data_received(void);

/** Four-note triumphant rise — contact saved (auto or manual) */
void buzzer_share_saved(void);

/** Single low thud — contact discarded by user */
void buzzer_share_discarded(void);

/** Two-tone descending drop — BLE timeout / connection lost */
void buzzer_ble_timeout(void);

/** Two quiet ticks — auto-save countdown warning (fires at T=5s) */
void buzzer_autosave_warning(void);

/* ================================================================
 * SECTION 5 — ALARM SOUNDS
 * ================================================================ */

/**
 * @brief  Play ONE alarm ring cycle (3 beeps + gap).
 *         Call this in a loop from alarm_task until dismissed.
 *         Each call takes approximately 950 ms.
 */
void buzzer_alarm_ring(void);

/** Descending double-tone — alarm snoozed */
void buzzer_alarm_snooze(void);

/** Single quiet low beep — alarm dismissed */
void buzzer_alarm_dismiss(void);
