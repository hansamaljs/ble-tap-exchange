/**
 * ================================================================
 * ble_driver.h  —  Rev 2.0
 * NimBLE radio driver — advertising, scanning, GAP, GATT server
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3, NimBLE
 *
 * WHAT THIS DRIVER DOES
 * ---------------------
 * - Initialises the NimBLE stack (HCI + host)
 * - Manages BLE advertising (Peripheral/Broadcaster role)
 *   Advertising packet embeds tap timestamp in manufacturer data
 *   so the peer can read it without a GATT connection
 * - Manages BLE scanning (Central/Observer role)
 *   Scanner applies a 4-gate filter before calling ble_gap_connect():
 *     Gate 1 — UUID match   : advert contains TAPSHARE_SERVICE_UUID
 *     Gate 2 — RSSI check   : rssi >= TAPSHARE_RSSI_THRESHOLD
 *     Gate 3 — Time window  : |peer_tap_ts - own_tap_ts| <= TAPSHARE_TIME_WINDOW_S
 *     Gate 4 — Role decision: lower tap_ts = Central; equal → MAC tiebreaker
 *   All 4 gates are checked before ble_gap_connect() is called.
 *   A rejected peer never causes a connection.
 * - Hosts the GATT server with 3 readable+writable characteristics:
 *     TAPSHARE_CHR_NAME_UUID  — own profile name
 *     TAPSHARE_CHR_PHONE_UUID — own profile phone
 *     TAPSHARE_CHR_TITLE_UUID — own profile title
 * - After connection (Central role): reads peer's 3 characteristics,
 *   then writes own 3 characteristics.
 *   Order: read-all-first, then write-all.
 *   If any read fails, writes are skipped — symmetric failure.
 * - Sends ble_event_t structs to the output queue for every state change.
 *   The queue is owned by ble_task. ble_service processes the events.
 *
 * WHAT THIS DRIVER DOES NOT DO
 * ----------------------------
 * - Does not know what TapShare is
 * - Does not manage state machine (ble_service.c does that)
 * - Does not write to NVS
 * - Does not update the display or buzzer
 *
 * LAYER RULE
 * ----------
 * ble_driver.c is at the driver layer. It knows NimBLE.
 * ble_service.c is at the service layer. It knows TapShare.
 * They communicate only through ble_event_t structs via the queue.
 *
 * UUID NOTE
 * ---------
 * Replace the placeholder byte arrays in ble_driver.c with real
 * UUID v4 values generated at: https://www.uuidgenerator.net/
 * Four UUIDs are needed:
 *   TAPSHARE_SVC_UUID     — the service
 *   TAPSHARE_CHR_NAME_UUID  — name characteristic
 *   TAPSHARE_CHR_PHONE_UUID — phone characteristic
 *   TAPSHARE_CHR_TITLE_UUID — title characteristic
 * ================================================================
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_events.h"

/* ================================================================
 * SECTION 1 — LIFECYCLE
 * ================================================================ */

/**
 * @brief  Initialise the NimBLE stack and GATT server.
 *
 * Sets up HCI transport, NimBLE host, GAP and GATT services.
 * Registers the TapShare service UUID and 3 profile characteristics.
 * Does NOT start advertising or scanning — call
 * ble_driver_start_share_mode() for that.
 *
 * @param  event_queue  Queue to receive ble_event_t structs.
 *                      Typically g_ble_event_queue from tasks.h.
 * @return ESP_OK on success.
 *
 * @note   Must be called after nvs_flash_init() (done in app_main).
 *         Call once from ble_task before nimble_port_freertos_init().
 */
esp_err_t ble_driver_init(QueueHandle_t event_queue);

/**
 * @brief  Stop NimBLE and release resources.
 */
void ble_driver_deinit(void);

/* ================================================================
 * SECTION 2 — SHARE MODE CONTROL
 * (Called by ble_service when state machine transitions)
 * ================================================================ */

/**
 * @brief  Start simultaneous advertising + scanning.
 *
 * Advertising packet includes TAPSHARE_SERVICE_UUID and embeds
 * tap_ts in the manufacturer data field. The peer's scanner
 * extracts this timestamp for the time-window gate check.
 *
 * Scanning filters for TAPSHARE_SERVICE_UUID. All 4 gates are
 * checked in the scan callback before ble_gap_connect().
 *
 * @param  tap_ts  Tap timestamp in seconds (from rtc_get_time epoch).
 *                 Used for time-window matching and role decision.
 * @return ESP_OK on success.
 */
esp_err_t ble_driver_start_share_mode(uint32_t tap_ts);

/**
 * @brief  Stop advertising and scanning.
 *         Call when entering IDLE or CONNECTING state.
 */
void ble_driver_stop_share_mode(void);

/* ================================================================
 * SECTION 3 — GATT EXCHANGE
 * ================================================================ */

/**
 * @brief  Begin GATT exchange as Central.
 *
 * Reads peer's name, phone, title characteristics in sequence.
 * If all 3 reads succeed, writes own name, phone, title to peer.
 * Sends BLE_EVT_EXCHANGE_COMPLETE or BLE_EVT_EXCHANGE_FAILED
 * to the event queue when done.
 *
 * READ-FIRST CONTRACT: If any read fails, writes are not attempted.
 * This ensures failure is symmetric — neither watch saves partial data.
 *
 * @param  conn_handle  Connection handle from BLE_EVT_CONNECTED.
 * @param  own_profile  Our own profile to write to the peer.
 * @return ESP_OK if exchange was started (result comes via queue).
 */
esp_err_t ble_driver_start_gatt_exchange(uint16_t conn_handle,
                                          const profile_data_t *own_profile);

/* ================================================================
 * SECTION 4 — CONNECTION MANAGEMENT
 * ================================================================ */

/**
 * @brief  Disconnect the current BLE connection.
 * @param  conn_handle  Connection handle to terminate.
 */
void ble_driver_disconnect(uint16_t conn_handle);

/**
 * @brief  Returns true if a BLE connection is currently active.
 */
bool ble_driver_is_connected(void);

/* ================================================================
 * SECTION 5 — OWN PROFILE (GATT server read source)
 * ================================================================ */

/**
 * @brief  Set the profile the GATT server will serve to peers.
 *
 * Called by ble_service_init() at startup with data loaded from NVS.
 * The GATT access_cb reads from this internal buffer when a peer
 * performs a characteristic READ operation.
 *
 * @param  profile  Profile to serve. Copied internally.
 */
void ble_driver_set_my_profile(const profile_data_t *profile);
