/**
 * ================================================================
 * rtc_driver.c
 * Real-Time Clock driver — Implementation (Internal ESP32-C3 RTC)
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 *
 * HOW THIS WORKS
 * --------------
 * The ESP32-C3 uses the POSIX time standard internally.
 * - settimeofday(struct timeval *tv, ...) sets the clock
 * - gettimeofday(struct timeval *tv, ...) reads it
 * - localtime_r(time_t *t, struct tm *result) converts to
 *   hours/minutes/seconds/date fields
 *
 * We wrap all of this behind rtc_get_time() / rtc_set_time()
 * so that the DS3231 upgrade (v2) only requires replacing this
 * one file. The API surface stays exactly the same.
 *
 * TIMEZONE NOTE
 * -------------
 * This driver deals in LOCAL time — what the user sees on screen.
 * wifi_sync.c calls setenv("TZ", ...) after NTP sync to configure
 * the local timezone. localtime_r() then converts UTC automatically.
 * This driver does not call setenv() — that is wifi_sync's job.
 * ================================================================
 */

#include "rtc_driver.h"
#include "esp_log.h"
#include <sys/time.h>
#include <time.h>
#include <string.h>

static const char *TAG = "RTC";

/* ================================================================
 * SECTION 1 — LIFECYCLE
 * ================================================================ */

esp_err_t rtc_driver_init(void)
{
    /* The internal RTC is always running on the ESP32-C3.
     * We just do a sanity read and log the result. */
    watch_time_t t;
    esp_err_t ret = rtc_get_time(&t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rtc_get_time failed during init");
        return ret;
    }

    ESP_LOGI(TAG, "RTC driver init OK");
    ESP_LOGI(TAG, "Current time: %04d-%02d-%02d %02d:%02d:%02d",
             t.year, t.month, t.day, t.hour, t.minute, t.second);

    if (t.year < 2020) {
        ESP_LOGW(TAG, "Time not set (epoch 0 or pre-2020). "
                       "Call rtc_set_time() or use wifi_sync.c to set via NTP.");
    }

    return ESP_OK;
}

/* ================================================================
 * SECTION 2 — GET / SET TIME
 * ================================================================ */

esp_err_t rtc_get_time(watch_time_t *out)
{
    if (!out) {
        ESP_LOGE(TAG, "rtc_get_time: out pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);

    /* Convert POSIX time_t to broken-down local time */
    struct tm local;
    localtime_r(&tv.tv_sec, &local);

    out->second  = (uint8_t)local.tm_sec;
    out->minute  = (uint8_t)local.tm_min;
    out->hour    = (uint8_t)local.tm_hour;
    out->day     = (uint8_t)local.tm_mday;
    out->month   = (uint8_t)(local.tm_mon + 1);   /* tm_mon is 0-based */
    out->year    = (uint16_t)(local.tm_year + 1900); /* tm_year = years since 1900 */
    out->weekday = (uint8_t)local.tm_wday;         /* 0=Sunday */

    return ESP_OK;
}

esp_err_t rtc_set_time(uint8_t hour, uint8_t min, uint8_t sec,
                        uint8_t day, uint8_t month, uint16_t year)
{
    /* Validate inputs */
    if (hour > 23 || min > 59 || sec > 59) {
        ESP_LOGE(TAG, "Invalid time: %02d:%02d:%02d", hour, min, sec);
        return ESP_ERR_INVALID_ARG;
    }
    if (day < 1 || day > 31 || month < 1 || month > 12) {
        ESP_LOGE(TAG, "Invalid date: %04d-%02d-%02d", year, month, day);
        return ESP_ERR_INVALID_ARG;
    }

    /* Build a struct tm for mktime() */
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_hour  = hour;
    t.tm_min   = min;
    t.tm_sec   = sec;
    t.tm_mday  = day;
    t.tm_mon   = month - 1;        /* tm_mon is 0-based */
    t.tm_year  = year  - 1900;     /* tm_year = years since 1900 */
    t.tm_isdst = -1;               /* Let mktime() determine DST */

    /* mktime() converts local struct tm → UTC time_t */
    time_t epoch = mktime(&t);
    if (epoch == (time_t)(-1)) {
        ESP_LOGE(TAG, "mktime() failed for %04d-%02d-%02d %02d:%02d:%02d",
                 year, month, day, hour, min, sec);
        return ESP_FAIL;
    }

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "settimeofday() failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Time set: %04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, min, sec);
    return ESP_OK;
}

esp_err_t rtc_set_hms(uint8_t hour, uint8_t min, uint8_t sec)
{
    /* Read current date, then set time with new H:M:S */
    watch_time_t current;
    esp_err_t ret = rtc_get_time(&current);
    if (ret != ESP_OK) return ret;

    return rtc_set_time(hour, min, sec,
                         current.day, current.month, current.year);
}
