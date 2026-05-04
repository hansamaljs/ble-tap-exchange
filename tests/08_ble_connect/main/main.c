/**
 * ================================================================
 * tests/08_ble_connect/main/main.c
 * Tier 2 BLE Test — Two boards, connection + role decision
 * ================================================================
 * WHAT THIS TEST PROVES
 * ---------------------
 * - Both boards can advertise and scan simultaneously (dual role)
 * - Role decision is deterministic: lower tap_ts → Central
 * - MAC tiebreaker works when timestamps match
 * - GAP connection established from Central to Peripheral
 * - Connection state machine transitions log correctly
 * - Scan timeout fires correctly when no peer (single board test)
 * - Both boards recover to a clean state after disconnect
 *
 * HOW TO USE (two boards)
 * ----------------------
 * 1. Flash this same firmware to both Board A and Board B.
 *    Fill test_config.h with different names for each board.
 * 2. Open two serial monitor windows (one per board).
 * 3. Reset both boards within a few seconds of each other.
 *    Both will auto-trigger share mode 3 seconds after boot.
 * 4. Watch the role decision in the logs.
 * 5. Confirm CONNECTING state on the Central board.
 * 6. Confirm full GATT exchange (read peer + write own) on both boards.
 * 7. Confirm contact saved to NVS on both boards.
 *
 * HOW TO USE (single board — timeout test)
 * ----------------------------------------
 * Flash one board. It will activate share mode, find no peer,
 * and show "Scan timeout" after TAPSHARE_SCAN_TIMEOUT_MS.
 * Then it returns to IDLE.
 *
 * EXPECTED SERIAL OUTPUT (Central board):
 *   [BLE_08] Auto-triggering share mode in 3s...
 *   [BLE_SVC] State: IDLE → SHARING
 *   [BLE_DRV] Advertising started — tap_ts=1716800000
 *   [BLE_DRV] Scanning started
 *   [BLE_DRV] Role: CENTRAL — all 4 gates passed. Connecting...
 *   [BLE_SVC] State: SHARING → CONNECTING
 *   [BLE_DRV] GAP connected — handle=1
 *   [BLE_SVC] State: CONNECTING → EXCHANGING
 *   [BLE_08]  GATT exchange not implemented in this test — disconnecting
 *   [BLE_SVC] State: EXCHANGING → IDLE
 * ================================================================
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "app_config.h"
#include "app_events.h"
#include "ble_driver.h"
#include "ble_service.h"

static const char *TAG = "BLE_08";

static QueueHandle_t s_ble_queue;

static const char *state_name(ble_share_state_t s)
{
    switch (s) {
    case BLE_STATE_IDLE:       return "IDLE";
    case BLE_STATE_SHARING:    return "SHARING";
    case BLE_STATE_CONNECTING: return "CONNECTING";
    case BLE_STATE_EXCHANGING: return "EXCHANGING";
    case BLE_STATE_DONE:       return "DONE";
    case BLE_STATE_FAILED:     return "FAILED";
    default:                   return "UNKNOWN";
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Test 08: BLE Connect + Role Decision   ");
    ESP_LOGI(TAG, "========================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_ble_queue = xQueueCreate(QUEUE_BLE_DEPTH, sizeof(ble_event_t));
    configASSERT(s_ble_queue);

    /* Driver must init first — it zeros s_my_profile internally.
     * Service init loads the NVS profile and then sets it via
     * ble_driver_set_my_profile, so it must run after driver init. */
    ret = ble_driver_init(s_ble_queue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble_driver_init failed");
        return;
    }

    ret = ble_service_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble_service_init failed");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    /* Auto-trigger share mode after 3s so both boards start near-simultaneously */
    ESP_LOGI(TAG, "Auto-triggering share mode in 3 seconds...");
    ESP_LOGI(TAG, "Reset both boards within this window for matching timestamps.");
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Simulate a SELECT long-press to trigger share mode */
    button_event_t btn = {
        .id           = BTN_SELECT,
        .type         = BTN_EVT_LONG_PRESS,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    ble_service_on_button_event(&btn);

    ESP_LOGI(TAG, "Share mode activated. State: %s",
             state_name(ble_service_get_state()));
    ESP_LOGI(TAG, "Tap TS: %lu", (unsigned long)ble_service_get_tap_timestamp());

    /* Main event loop */
    ble_share_state_t last_state = BLE_STATE_SHARING;
    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;

    while (1) {
        /* Process real BLE events first so state is up-to-date */
        ble_event_t evt;
        while (xQueueReceive(s_ble_queue, &evt, 0) == pdPASS) {
            if (evt.type == BLE_EVT_CONNECTED) {
                conn_handle = evt.conn_handle;
            } else if (evt.type == BLE_EVT_DISCONNECTED) {
                conn_handle = BLE_HS_CONN_HANDLE_NONE;
            }
            ble_service_on_ble_event(&evt);
        }

        vTaskDelay(pdMS_TO_TICKS(100));

        /* Log state changes */
        ble_share_state_t current = ble_service_get_state();
        if (current != last_state) {
            ESP_LOGI(TAG, "State changed → %s", state_name(current));
            last_state = current;

            if (current == BLE_STATE_EXCHANGING) {
                ESP_LOGI(TAG, "GATT exchange in progress...");
            }

            if (current == BLE_STATE_IDLE) {
                ESP_LOGI(TAG, "Returned to IDLE. Test complete.");
                uint8_t count = ble_service_get_contact_count();
                ESP_LOGI(TAG, "Contact count in NVS: %d", count);
                for (uint8_t i = 0; i < count; i++) {
                    contact_entry_t entry;
                    if (ble_service_get_contact(i, &entry) == ESP_OK) {
                        ESP_LOGI(TAG, "  [%u] name=%-20s phone=%-16s title=%s",
                                 i, entry.name, entry.phone, entry.title);
                    }
                }
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Test 08 complete.");
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
