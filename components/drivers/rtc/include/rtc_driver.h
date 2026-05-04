/**
 * ================================================================
 * rtc_driver.h
 * Real-Time Clock driver — Internal ESP32-C3 RTC
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 * Prototype: Internal 32 kHz RC oscillator via POSIX time API
 *
 * WHAT THIS DRIVER DOES
 * ---------------------
 * Wraps the ESP-IDF POSIX gettimeofday()/settimeofday() interface
 * so that the rest of the firmware never calls POSIX time functions
 * directly. This makes the time source completely swappable.
 *
 * WHAT THIS DRIVER DOES NOT DO
 * ----------------------------
 * - Does not know about WiFi, NTP, or BLE sync
 * - Does not do timezone conversion (time_service handles that)
 * - Does not own any tasks or timers
 * - Does not know what the time means to the watch application
 *
 * TIME SOURCE DECISIONS
 * ---------------------
 * Prototype v1 : Internal RTC, set by wifi_sync.c at boot (NTP)
 *                or manually via Settings UI
 * Production v2: DS3231 external RTC (I2C 0x68). Replace ONLY
 *                rtc_driver.c — all callers are unaffected because
 *                the API surface is identical.
 *
 * DRIFT WARNING
 * -------------
 * The ESP32-C3 internal RC oscillator drifts ~250-500 ppm
 * (~1-2 minutes per day). This is acceptable for a prototype.
 * Time resets to 1 Jan 1970 00:00:00 after every power cycle.
 * Re-sync with wifi_sync.c on each boot during testing.
 *
 * ================================================================
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/* ================================================================
 * watch_time_t — the time struct used everywhere in firmware
 * ================================================================ */
typedef struct {
    uint8_t  hour;    /* 0-23 */
    uint8_t  minute;  /* 0-59 */
    uint8_t  second;  /* 0-59 */
    uint8_t  day;     /* 1-31 */
    uint8_t  month;   /* 1-12 */
    uint16_t year;    /* e.g. 2026 */
    uint8_t  weekday; /* 0=Sunday ... 6=Saturday */
} watch_time_t;

/* ================================================================
 * SECTION 1 — LIFECYCLE
 * ================================================================ */

/**
 * @brief  Validate that the internal RTC is running and readable.
 *
 * On the ESP32-C3, the RTC is always running — this function
 * confirms the POSIX time API is functional and logs the current
 * time (which will be epoch 0 unless already set).
 *
 * @return ESP_OK on success.
 */
esp_err_t rtc_driver_init(void);

/* ================================================================
 * SECTION 2 — GET / SET TIME
 * ================================================================ */

/**
 * @brief  Get the current date and time.
 *
 * Reads the ESP32-C3 internal RTC via gettimeofday().
 * Thread-safe — no shared mutable state.
 *
 * @param  out  Pointer to a watch_time_t struct to fill.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out is NULL.
 */
esp_err_t rtc_get_time(watch_time_t *out);

/**
 * @brief  Set the full date and time.
 *
 * Calls settimeofday() to update the internal RTC.
 * Called by wifi_sync.c after NTP, or by Settings UI after
 * manual time entry. Thread-safe.
 *
 * @param  hour   0-23
 * @param  min    0-59
 * @param  sec    0-59
 * @param  day    1-31
 * @param  month  1-12
 * @param  year   e.g. 2026
 * @return ESP_OK on success.
 */
esp_err_t rtc_set_time(uint8_t hour, uint8_t min, uint8_t sec,
                        uint8_t day,  uint8_t month, uint16_t year);

/**
 * @brief  Set only hours, minutes, seconds (keep current date).
 *
 * Convenience wrapper around rtc_set_time() for use by the
 * Settings → Set Time screen where only H:M:S is shown.
 *
 * @param  hour  0-23
 * @param  min   0-59
 * @param  sec   0-59
 * @return ESP_OK on success.
 */
esp_err_t rtc_set_hms(uint8_t hour, uint8_t min, uint8_t sec);
