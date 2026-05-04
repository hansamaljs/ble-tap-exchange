/**
 * ================================================================
 * wifi_sync.h
 * One-shot WiFi NTP time synchronisation utility
 * ================================================================
 * This is NOT a driver. It is a prototype testing utility.
 *
 * PURPOSE
 * -------
 * Sets the internal RTC via WiFi NTP at boot. Used during
 * prototype development and testing where a companion BLE app
 * is not yet available.
 *
 * USAGE
 * -----
 *   // In app_main(), before tasks_start_all():
 *   #include "test_config.h"   // defines TEST_WIFI_SSID / PASS
 *   wifi_sync(TEST_WIFI_SSID, TEST_WIFI_PASS);
 *
 * After wifi_sync() returns, gettimeofday() returns accurate UTC
 * and the RTC driver reads correct local time (after TZ is set).
 *
 * PRODUCTION PATH
 * ---------------
 * In production, ble_service.c will call rtc_set_time() when
 * the companion phone app sends the current time via a GATT write.
 * wifi_sync.c will not be compiled into the production build.
 * ================================================================
 */

#pragma once

#include "esp_err.h"

/**
 * @brief  Connect to WiFi, sync time via NTP, disconnect.
 *
 * Blocking — returns when sync is complete or on timeout.
 * Sets the TZ environment variable for Colombo/IST+5:30 by default.
 * Change the TZ string inside wifi_sync.c for other timezones.
 *
 * Call this ONCE from app_main() before creating any FreeRTOS tasks.
 *
 * @param  ssid      WiFi network name (null-terminated)
 * @param  password  WiFi password (null-terminated, or "" for open)
 * @return ESP_OK on success, ESP_FAIL on connection/sync timeout
 */
esp_err_t wifi_sync(const char *ssid, const char *password);
