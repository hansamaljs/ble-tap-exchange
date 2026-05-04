/**
 * ================================================================
 * wifi_sync.c
 * One-shot WiFi NTP time synchronisation — Implementation
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 *
 * SEQUENCE
 * --------
 *  1. nvs_flash_init()           — required by WiFi stack
 *  2. esp_netif_init()           — TCP/IP stack
 *  3. esp_wifi_init/config/start — connect to AP
 *  4. Wait for WIFI_EVENT_STA_GOT_IP (max 10 s)
 *  5. esp_sntp_init()            — start NTP client
 *  6. Wait for SNTP_SYNC_STATUS_COMPLETED (max 10 s)
 *  7. setenv("TZ", ...)          — configure local timezone
 *  8. Disconnect WiFi            — no longer needed
 *
 * TIMEZONE
 * --------
 * Default: "IST-5:30" (Sri Lanka / India Standard Time, UTC+5:30)
 * Change WIFI_SYNC_TZ_STRING below for your location.
 * POSIX TZ format: name offset (e.g. "EST5", "CET-1", "IST-5:30")
 * ================================================================
 */

#include "wifi_sync.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>

static const char *TAG = "WIFI_SYNC";

/* Change this to your local timezone (POSIX TZ format) */
#define WIFI_SYNC_TZ_STRING   "IST-5:30"   /* Sri Lanka / India UTC+5:30 */

/* NTP server — pool.ntp.org is reliable worldwide */
#define WIFI_SYNC_NTP_SERVER  "pool.ntp.org"

/* Timeouts */
#define WIFI_CONNECT_TIMEOUT_MS   10000
#define SNTP_SYNC_TIMEOUT_MS      10000

/* ── Private event group ────────────────────────────────────────*/
static EventGroupHandle_t s_wifi_event_group = NULL;
#define BIT_CONNECTED   (1 << 0)
#define BIT_FAILED      (1 << 1)

static void s_wifi_event_handler(void *arg, esp_event_base_t base,
                                  int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected — not retrying");
        xEventGroupSetBits(s_wifi_event_group, BIT_FAILED);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, BIT_CONNECTED);
    }
}

esp_err_t wifi_sync(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Starting WiFi NTP sync (SSID: %s)", ssid);

    /* ── 1. NVS (may already be done in app_main — idempotent) ── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ── 2. TCP/IP stack ────────────────────────────────────────*/
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* ── 3. WiFi init ───────────────────────────────────────────*/
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                s_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                s_wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    /* No authmode threshold — accept WPA2, WPA2/WPA3 mixed, and WPA3-SAE.
     * Setting a specific threshold (e.g. WPA2_PSK) blocks WPA3-SAE networks,
     * which Android 12+ phone hotspots use by default. */
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    /* PMF required by WPA3 and WPA2/WPA3 transition-mode APs. capable=true
     * negotiates it when the AP requires it; required=false keeps WPA2-only
     * APs working. */
    wifi_cfg.sta.pmf_cfg.capable  = true;
    wifi_cfg.sta.pmf_cfg.required = false;
    /* WPA3-SAE PWE: BOTH = accept hunt-and-peck and H2E methods.
     * Needed on ESP-IDF 5.x for WPA3-SAE to work at all. */
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* ── 4. Wait for IP ─────────────────────────────────────────*/
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        BIT_CONNECTED | BIT_FAILED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)
    );

    if (!(bits & BIT_CONNECTED)) {
        ESP_LOGE(TAG, "WiFi connect failed or timed out after %d ms",
                  WIFI_CONNECT_TIMEOUT_MS);
        esp_wifi_stop();
        esp_wifi_deinit();
        vEventGroupDelete(s_wifi_event_group);
        return ESP_FAIL;
    }

    /* ── 5. Start SNTP ──────────────────────────────────────────*/
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, WIFI_SYNC_NTP_SERVER);
    esp_sntp_init();
    ESP_LOGI(TAG, "Waiting for NTP sync from %s ...", WIFI_SYNC_NTP_SERVER);

    /* ── 6. Wait for NTP sync ───────────────────────────────────*/
    uint32_t waited_ms = 0;
    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
        if (waited_ms >= SNTP_SYNC_TIMEOUT_MS) {
            ESP_LOGE(TAG, "NTP sync timed out after %d ms", SNTP_SYNC_TIMEOUT_MS);
            esp_sntp_stop();
            esp_wifi_disconnect();
            esp_wifi_stop();
            esp_wifi_deinit();
            vEventGroupDelete(s_wifi_event_group);
            return ESP_FAIL;
        }
    }

    /* ── 7. Set timezone ────────────────────────────────────────*/
    setenv("TZ", WIFI_SYNC_TZ_STRING, 1);
    tzset();

    /* Log the synced time */
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "NTP sync OK. Local time: %04d-%02d-%02d %02d:%02d:%02d (%s)",
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon  + 1,
             timeinfo.tm_mday,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec,
             WIFI_SYNC_TZ_STRING);

    /* ── 8. Disconnect WiFi — not needed anymore ────────────────*/
    esp_sntp_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();
    /* Note: do NOT call esp_wifi_deinit() here — it also tears down
     * the event loop which may be needed by BLE stack later. */

    vEventGroupDelete(s_wifi_event_group);
    ESP_LOGI(TAG, "WiFi disconnected. RTC is now set.");
    return ESP_OK;
}
