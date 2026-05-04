/**
 * ================================================================
 * tests/04_rtc/main/main.c
 * RTC Driver + WiFi NTP Sync Test
 * ================================================================
 * Tests:
 *   1. wifi_sync() — connect to WiFi, get NTP time, disconnect
 *   2. rtc_get_time() — read back the synced time
 *   3. rtc_set_time() — manually set a specific date/time
 *   4. rtc_set_hms()  — update only H:M:S, keep current date
 *   5. Continuous read — print time every second for 30 s
 *
 * SETUP REQUIRED
 * --------------
 * 1. Copy tests/test_config.h.template → tests/test_config.h
 * 2. Fill in your WiFi credentials in test_config.h
 * 3. test_config.h is gitignored — never committed
 *
 * EXPECTED SERIAL OUTPUT
 * ----------------------
 *   WIFI_SYNC: Got IP: 192.168.x.x
 *   WIFI_SYNC: NTP sync OK. Local time: 2026-04-14 15:30:22 (IST-5:30)
 *   RTC_TEST:  PASS: rtc_get_time returns valid struct
 *   RTC_TEST:  Current: 2026-04-14 15:30:22 (Mon)
 *   RTC_TEST:  PASS: rtc_set_time changed the time
 *   RTC_TEST:  After set_time: 2025-12-25 08:00:00
 *   RTC_TEST:  PASS: rtc_set_hms updated only H:M:S
 *   RTC_TEST:  After set_hms: 2025-12-25 12:34:56
 *   (then 30 seconds of: RTC_TEST: Tick: 12:34:57)
 * ================================================================
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "rtc_driver.h"
#include "wifi_sync.h"

/* WiFi credentials — create test_config.h from the template */
#include "../../../test_config.h"

static const char *TAG = "RTC_TEST";

/* Simple test assertion macro */
#define TEST_ASSERT(label, condition) do {                    \
    if (condition) {                                           \
        ESP_LOGI(TAG, "PASS: %s", label);                    \
    } else {                                                   \
        ESP_LOGE(TAG, "FAIL: %s", label);                    \
    }                                                          \
} while(0)

static const char *weekday_name(uint8_t wd) {
    static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    return (wd < 7) ? days[wd] : "???";
}

void app_main(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " RTC + WiFi NTP Sync Test — ESP-IDF 5.4.3");
    ESP_LOGI(TAG, "================================================");

    /* ── NVS (required by WiFi) ──────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ── Init RTC driver ─────────────────────────────────────── */
    ESP_LOGI(TAG, "--- RTC init (time will be epoch 0 before NTP) ---");
    ret = rtc_driver_init();
    TEST_ASSERT("rtc_driver_init returns ESP_OK", ret == ESP_OK);

    /* ── WiFi NTP sync ───────────────────────────────────────── */
    ESP_LOGI(TAG, "--- WiFi NTP sync (SSID: %s) ---", TEST_WIFI_SSID);
    ret = wifi_sync(TEST_WIFI_SSID, TEST_WIFI_PASS);
    TEST_ASSERT("wifi_sync returns ESP_OK", ret == ESP_OK);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NTP sync failed. Continuing with manual time set test.");
        ESP_LOGW(TAG, "Check WiFi credentials in test_config.h");
    }

    /* ── Test 1: rtc_get_time() after NTP ────────────────────── */
    ESP_LOGI(TAG, "--- Test 1: rtc_get_time() after NTP ---");
    watch_time_t t;
    ret = rtc_get_time(&t);
    TEST_ASSERT("rtc_get_time returns ESP_OK", ret == ESP_OK);
    TEST_ASSERT("year is plausible (>= 2024)", t.year >= 2024);
    TEST_ASSERT("hour is valid (< 24)", t.hour < 24);
    TEST_ASSERT("minute is valid (< 60)", t.minute < 60);
    TEST_ASSERT("second is valid (< 60)", t.second < 60);
    TEST_ASSERT("month is valid (1-12)", t.month >= 1 && t.month <= 12);
    TEST_ASSERT("day is valid (1-31)", t.day >= 1 && t.day <= 31);

    ESP_LOGI(TAG, "Current: %04d-%02d-%02d %02d:%02d:%02d (%s)",
             t.year, t.month, t.day,
             t.hour, t.minute, t.second,
             weekday_name(t.weekday));

    /* ── Test 2: rtc_set_time() ──────────────────────────────── */
    ESP_LOGI(TAG, "--- Test 2: rtc_set_time() ---");
    ret = rtc_set_time(8, 0, 0, 25, 12, 2025);  /* 25 Dec 2025 08:00:00 */
    TEST_ASSERT("rtc_set_time returns ESP_OK", ret == ESP_OK);

    watch_time_t after_set;
    rtc_get_time(&after_set);
    TEST_ASSERT("hour updated to 8", after_set.hour == 8);
    TEST_ASSERT("minute updated to 0", after_set.minute == 0);
    TEST_ASSERT("year updated to 2025", after_set.year == 2025);
    TEST_ASSERT("month updated to 12", after_set.month == 12);
    TEST_ASSERT("day updated to 25", after_set.day == 25);
    ESP_LOGI(TAG, "After set_time: %04d-%02d-%02d %02d:%02d:%02d",
             after_set.year, after_set.month, after_set.day,
             after_set.hour, after_set.minute, after_set.second);

    /* ── Test 3: Invalid args rejected ──────────────────────── */
    ESP_LOGI(TAG, "--- Test 3: Invalid argument rejection ---");
    TEST_ASSERT("hour=25 rejected",     rtc_set_time(25,0,0,1,1,2026) != ESP_OK);
    TEST_ASSERT("minute=60 rejected",   rtc_set_time(0,60,0,1,1,2026) != ESP_OK);
    TEST_ASSERT("month=13 rejected",    rtc_set_time(0,0,0,1,13,2026) != ESP_OK);
    TEST_ASSERT("NULL pointer rejected",rtc_get_time(NULL) != ESP_OK);

    /* ── Test 4: rtc_set_hms() ───────────────────────────────── */
    ESP_LOGI(TAG, "--- Test 4: rtc_set_hms() (keep date, change time) ---");
    ret = rtc_set_hms(12, 34, 56);
    TEST_ASSERT("rtc_set_hms returns ESP_OK", ret == ESP_OK);

    watch_time_t after_hms;
    rtc_get_time(&after_hms);
    TEST_ASSERT("hour updated to 12", after_hms.hour == 12);
    TEST_ASSERT("minute updated to 34", after_hms.minute == 34);
    TEST_ASSERT("second updated to 56", after_hms.second == 56);
    /* Date must be unchanged from Test 2 */
    TEST_ASSERT("date preserved (day=25)", after_hms.day == 25);
    TEST_ASSERT("date preserved (month=12)", after_hms.month == 12);
    TEST_ASSERT("date preserved (year=2025)", after_hms.year == 2025);
    ESP_LOGI(TAG, "After set_hms: %04d-%02d-%02d %02d:%02d:%02d",
             after_hms.year, after_hms.month, after_hms.day,
             after_hms.hour, after_hms.minute, after_hms.second);

    /* ── Test 5: Continuous read — 30 seconds ────────────────── */
    ESP_LOGI(TAG, "--- Test 5: Continuous read for 30 s ---");
    ESP_LOGI(TAG, "Watch for seconds incrementing. Should not skip or repeat.");
    for (int i = 0; i < 30; i++) {
        watch_time_t tick;
        rtc_get_time(&tick);
        ESP_LOGI(TAG, "Tick %2d: %02d:%02d:%02d",
                 i + 1, tick.hour, tick.minute, tick.second);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* ── Summary ─────────────────────────────────────────────── */
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " RTC test complete.");
    ESP_LOGI(TAG, " If PASS count = 15 and all ticks incremented,");
    ESP_LOGI(TAG, " the RTC driver is ready for time_service.");
    ESP_LOGI(TAG, "================================================");

    /* Idle — keep printing time every 5 s */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        watch_time_t idle;
        rtc_get_time(&idle);
        ESP_LOGI(TAG, "Idle: %02d:%02d:%02d  heap=%lu",
                 idle.hour, idle.minute, idle.second,
                 (unsigned long)esp_get_free_heap_size());
    }
}
