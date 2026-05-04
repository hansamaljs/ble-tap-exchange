/**
 * ================================================================
 * tests/07_ble_scan/main/main.c
 * Tier 1 BLE Test — Scan only, 4-gate filter verification
 * ================================================================
 * WHAT THIS TEST PROVES
 * ---------------------
 * - BLE scanning works on the ESP32-C3
 * - UUID filter correctly identifies Cylonix adverts
 * - RSSI gate correctly filters weak signals
 * - Tap timestamp extracted correctly from manufacturer data
 * - Time-window gate logic works as expected
 * - Non-Cylonix BLE devices are silently ignored
 *
 * HOW TO USE
 * ----------
 * Run test 06 on Board A (advertising).
 * Run this test on Board B (scanning).
 * Place boards at different distances to see RSSI gate in action.
 *
 * EXPECTED SERIAL OUTPUT (Board B, Board A nearby):
 *   [BLE_SCAN] Scanning started. Looking for Cylonix watches...
 *   [BLE_SCAN] ── Cylonix peer found ──
 *   [BLE_SCAN]   MAC   : XX:XX:XX:XX:XX:XX
 *   [BLE_SCAN]   RSSI  : -42 dBm → PASS (threshold: -65)
 *   [BLE_SCAN]   Tap TS: 1234567 | My TS: 0 | Diff: 1234567s → FAIL (window: 5s)
 *   [BLE_SCAN]   (no connection — time window not matched)
 *
 * Note: With tap_ts=0 on this board, the time window gate will
 * fail unless both boards use matching timestamps. This is correct
 * behaviour — it proves the gate is working. To test a full pass,
 * set MY_FAKE_TAP_TS to match the advertising board's tap_ts.
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
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "app_config.h"
#include "app_events.h"

static const char *TAG = "BLE_SCAN";

/* Set this to the tap_ts of the advertising board to test time-window pass */
#define MY_FAKE_TAP_TS  0   /* Change to match Board A's tap_ts for full pass */

/* TapShare service UUID — must match ble_driver.c */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x01,0x02,0x03,0x04, 0x05,0x06, 0x07,0x08,
    0x09,0x0A, 0x0B,0x0C,0x0D,0x0E,0x0F,0x10
);

static void print_addr(const uint8_t *addr)
{
    ESP_LOGI(TAG, "   MAC   : %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data) != 0) {
        return 0;
    }

    /* Gate 1: UUID match */
    bool uuid_match = false;
    for (int i = 0; i < fields.num_uuids128; i++) {
        if (ble_uuid_cmp(&fields.uuids128[i].u, &s_svc_uuid.u) == 0) {
            uuid_match = true;
            break;
        }
    }
    if (!uuid_match) return 0;  /* Non-Cylonix device — silent ignore */

    ESP_LOGI(TAG, "── Cylonix peer found ──");
    print_addr(event->disc.addr.val);

    /* Gate 2: RSSI */
    int8_t rssi = event->disc.rssi;
    bool rssi_pass = (rssi >= TAPSHARE_RSSI_THRESHOLD);
    ESP_LOGI(TAG, "   RSSI  : %d dBm → %s (threshold: %d)",
             rssi,
             rssi_pass ? "PASS" : "FAIL",
             TAPSHARE_RSSI_THRESHOLD);

    /* Extract tap_ts from manufacturer data */
    uint32_t peer_tap_ts = 0;
    bool mfr_valid = false;
    if (fields.mfg_data_len >= 5 &&
        fields.mfg_data[0] == TAPSHARE_ADV_MAGIC) {
        memcpy(&peer_tap_ts, &fields.mfg_data[1], sizeof(uint32_t));
        mfr_valid = true;
    }

    /* Gate 3: Time window */
    uint32_t my_ts = MY_FAKE_TAP_TS;
    int32_t diff = (int32_t)peer_tap_ts - (int32_t)my_ts;
    if (diff < 0) diff = -diff;
    bool ts_pass = mfr_valid && ((uint32_t)diff <= TAPSHARE_TIME_WINDOW_S);
    ESP_LOGI(TAG, "   Tap TS: %lu | My TS: %lu | Diff: %lds → %s (window: %ds)",
             (unsigned long)peer_tap_ts, (unsigned long)my_ts,
             (long)diff,
             ts_pass ? "PASS" : "FAIL",
             TAPSHARE_TIME_WINDOW_S);

    /* Gate 4 summary */
    bool all_pass = rssi_pass && ts_pass;
    ESP_LOGI(TAG, "   All gates: %s", all_pass ? "PASS → would connect" : "FAIL → ignored");
    ESP_LOGI(TAG, "──────────────────────");

    return 0;
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync_cb(void)
{
    uint8_t own_addr[6];
    ble_hs_util_ensure_addr(0);
    ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, own_addr, NULL);
    ESP_LOGI(TAG, "NimBLE ready. BLE address: %02X:%02X:%02X:%02X:%02X:%02X",
             own_addr[5], own_addr[4], own_addr[3],
             own_addr[2], own_addr[1], own_addr[0]);
    ESP_LOGI(TAG, "MY_FAKE_TAP_TS = %lu", (unsigned long)MY_FAKE_TAP_TS);
    ESP_LOGI(TAG, "Scanning started. Looking for Cylonix watches...");

    struct ble_gap_disc_params disc;
    memset(&disc, 0, sizeof(disc));
    disc.passive       = 0;
    disc.filter_policy = 0;
    disc.itvl          = 80;
    disc.window        = 80;
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc,
                 scan_event_cb, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Test 07: BLE Scan + 4-Gate Filter      ");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Run test 06 on another board to see a Cylonix peer.");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_hs_cfg.sync_cb = on_sync_cb;
    nimble_port_freertos_init(ble_host_task);

    /* Status loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "[Heartbeat] Still scanning...");
    }
}
