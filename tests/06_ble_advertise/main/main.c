/**
 * ================================================================
 * tests/06_ble_advertise/main/main.c
 * Tier 1 BLE Test — Single board, nRF Connect verification
 * ================================================================
 * WHAT THIS TEST PROVES
 * ---------------------
 * - NimBLE stack initialises correctly
 * - "cylonix-watch" appears in BLE scanner apps (nRF Connect)
 * - TapShare service UUID is present in advertising packet
 * - GATT server serves all 3 profile characteristics correctly
 * - After disconnect, watch re-advertises within 500 ms
 * - Manufacturer data embeds tap_ts correctly
 *
 * HOW TO USE
 * ----------
 * 1. Copy test_config.h.template to test_config.h in this folder
 * 2. Fill in MY_PROFILE_NAME/PHONE/TITLE
 * 3. idf.py build flash monitor
 * 4. Open nRF Connect on phone
 * 5. Scan → find "cylonix-watch"
 * 6. Connect → navigate to Unknown Service (your custom UUID)
 * 7. Read each characteristic → should show your profile data
 * 8. Disconnect → watch should re-advertise within 500 ms
 *
 * EXPECTED SERIAL OUTPUT
 * ----------------------
 *   [BLE_TEST] Own profile: Chamod / +94771234567 / Firmware Dev
 *   [BLE_TEST] Advertising started — tap_ts=1234567
 *   [BLE_DRV]  BLE address: XX:XX:XX:XX:XX:XX
 *   [BLE_TEST] Status: ADVERTISING | Connected: NO
 *   ... (every 5s)
 *   [BLE_DRV]  GAP connected — handle=1
 *   [BLE_TEST] Status: CONNECTED | Connected: YES
 *   [BLE_DRV]  GAP disconnected — reason=19
 *   [BLE_TEST] Re-advertising after disconnect
 * ================================================================
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "app_events.h"
#include "ble_driver.h"
#include "ble_service.h"

/* Include test profile config */
#ifdef __has_include
  #if __has_include("test_config.h")
    #include "test_config.h"
  #else
    #define MY_PROFILE_NAME  "Test Board A"
    #define MY_PROFILE_PHONE "+94000000000"
    #define MY_PROFILE_TITLE "BLE Test"
  #endif
#endif

static const char *TAG = "BLE_TEST";

/* Queue required by ble_driver but we don't process events here */
static QueueHandle_t s_ble_queue;

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Test 06: BLE Advertise (Single Board)  ");
    ESP_LOGI(TAG, "========================================");

    /* 1. NVS — required before BLE */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 2. Create BLE event queue */
    s_ble_queue = xQueueCreate(QUEUE_BLE_DEPTH, sizeof(ble_event_t));
    configASSERT(s_ble_queue);

    /* 3. Init ble_service first — loads/writes profile to NVS */
    ret = ble_service_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble_service_init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 4. Init ble_driver — starts NimBLE stack */
    ret = ble_driver_init(s_ble_queue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble_driver_init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Give NimBLE a moment to sync and read MAC address */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 5. Log own profile */
    profile_data_t profile;
    ble_service_get_my_profile(&profile);
    ESP_LOGI(TAG, "Own profile: %s / %s / %s",
             profile.name, profile.phone, profile.title);

    /* 6. Start advertising — use current time as tap_ts */
    uint32_t fake_tap_ts = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    ret = ble_driver_start_share_mode(fake_tap_ts);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble_driver_start_share_mode failed");
        return;
    }

    ESP_LOGI(TAG, "Advertising started — tap_ts=%lu", (unsigned long)fake_tap_ts);
    ESP_LOGI(TAG, "Open nRF Connect on your phone and scan for 'cylonix-watch'");
    ESP_LOGI(TAG, "Connect, read characteristics, verify profile data.");

    /* 7. Status loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        bool connected = ble_driver_is_connected();
        ESP_LOGI(TAG, "Status: %s | Connected: %s",
                 connected ? "CONNECTED" : "ADVERTISING",
                 connected ? "YES" : "NO");

        /* Drain BLE events and log them */
        ble_event_t evt;
        while (xQueueReceive(s_ble_queue, &evt, 0) == pdPASS) {
            switch (evt.type) {
            case BLE_EVT_CONNECTED:
                ESP_LOGI(TAG, "EVENT: GAP connected — handle=%d",
                         evt.conn_handle);
                break;
            case BLE_EVT_DISCONNECTED:
                ESP_LOGI(TAG, "EVENT: GAP disconnected");
                ESP_LOGI(TAG, "Re-advertising after disconnect");
                ble_driver_start_share_mode(fake_tap_ts);
                break;
            default:
                ESP_LOGI(TAG, "EVENT: type=%d", evt.type);
                break;
            }
        }
    }
}
