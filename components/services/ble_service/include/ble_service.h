/**
 * ================================================================
 * ble_service.h  —  Rev 2.0
 * TapShare business logic — state machine and NVS contact storage
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 *
 * WHAT THIS SERVICE DOES
 * ----------------------
 * - Owns the TapShare state machine (BLE_STATE_* states)
 * - Triggers share mode when SELECT long-press arrives
 * - Processes raw ble_event_t events from ble_driver
 * - On EXCHANGE_COMPLETE: saves contact to NVS immediately
 *   (no user confirmation required — save is automatic)
 * - Notifies ui_task of state changes via g_ble_event_queue
 * - Manages NVS contact addressbook (up to TAPSHARE_MAX_CONTACTS)
 *   using FIFO slot rotation when full
 * - Loads own profile from NVS at init; writes defaults on first boot
 *
 * WHAT THIS SERVICE DOES NOT DO
 * ------------------------------
 * - Does not call NimBLE directly (ble_driver.c does that)
 * - Does not update the display (ui_task does that)
 * - Does not know about GATT attribute handles
 *
 * OWN PROFILE — PROTOTYPE APPROACH
 * ---------------------------------
 * At first boot, ble_service_init() checks NVS for "my_name".
 * If absent, it writes MY_PROFILE_NAME/PHONE/TITLE from test_config.h.
 * This is the prototype approach. In production, the companion phone
 * app writes the profile via a dedicated BLE GATT characteristic.
 *
 * CONTACT STORAGE DESIGN
 * ----------------------
 * Up to TAPSHARE_MAX_CONTACTS slots in NVS. FIFO rotation when full.
 * Each slot has a validity flag written BEFORE and AFTER the blob
 * to protect against power-loss mid-write.
 *
 * NVS key layout (namespace: "cylonix"):
 *   my_name, my_phone, my_title   — own profile
 *   ts_count                      — number of contacts saved (U8)
 *   ts_next                       — next slot to write (U8, FIFO index)
 *   ts_valid_N (N=0..MAX-1)       — slot validity flag (U8, 0=invalid)
 *   ts_slot_N  (N=0..MAX-1)       — contact_entry_t blob (120 bytes)
 * ================================================================
 */

#pragma once

#include "esp_err.h"
#include "app_events.h"

/* ================================================================
 * SECTION 1 — LIFECYCLE
 * ================================================================ */

/**
 * @brief  Initialise ble_service.
 *
 * - Opens NVS under "cylonix" namespace
 * - Loads own profile; writes test_config.h defaults if absent
 * - Loads contact metadata (ts_count, ts_next)
 * - Calls ble_driver_set_my_profile() so GATT server is ready
 * - Sets state machine to BLE_STATE_IDLE
 *
 * @return ESP_OK on success.
 *
 * @note   Call BEFORE ble_driver_init(). Own profile must be in the
 *         GATT server before NimBLE starts advertising.
 */
esp_err_t ble_service_init(void);

/* ================================================================
 * SECTION 2 — EVENT INPUT (called by ble_task and input_task)
 * ================================================================ */

/**
 * @brief  Process a raw BLE event from ble_driver.
 *
 * Called by ble_task every time it dequeues a ble_event_t.
 * Drives state machine transitions based on event type.
 *
 * @param  evt  Pointer to the event. Not modified.
 */
void ble_service_on_ble_event(const ble_event_t *evt);

/**
 * @brief  Process a button event that is relevant to BLE.
 *
 * Called by ui_task or ble_task when a button event arrives.
 * SELECT long-press → triggers share mode.
 * SELECT long-press again while SHARING → cancels share mode.
 *
 * @param  evt  Button event from g_input_queue.
 */
void ble_service_on_button_event(const button_event_t *evt);

/* ================================================================
 * SECTION 3 — STATE QUERY
 * ================================================================ */

/**
 * @brief  Get the current TapShare state machine state.
 *         Thread-safe (reads a single volatile enum).
 */
ble_share_state_t ble_service_get_state(void);

/**
 * @brief  Get the tap timestamp set when share mode was last triggered.
 *         Used by ble_driver_start_share_mode().
 */
uint32_t ble_service_get_tap_timestamp(void);

/* ================================================================
 * SECTION 4 — OWN PROFILE MANAGEMENT
 * ================================================================ */

/**
 * @brief  Get this watch's own profile.
 *
 * @param  out  Pointer to fill. Must not be NULL.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no profile in NVS.
 */
esp_err_t ble_service_get_my_profile(profile_data_t *out);

/**
 * @brief  Update this watch's own profile.
 *
 * Writes to NVS and calls ble_driver_set_my_profile() so the
 * GATT server immediately serves the updated data.
 *
 * In prototype: called once at init from test_config.h values.
 * In production: called by the companion phone app via BLE GATT write.
 *
 * @param  profile  New profile data. Copied internally.
 * @return ESP_OK on success.
 */
esp_err_t ble_service_set_my_profile(const profile_data_t *profile);

/* ================================================================
 * SECTION 5 — CONTACT ADDRESSBOOK
 * ================================================================ */

/**
 * @brief  Get a saved contact by slot index.
 *
 * @param  slot_index  0 to (ts_count - 1).
 * @param  out         Pointer to fill.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if slot is empty.
 */
esp_err_t ble_service_get_contact(uint8_t slot_index, contact_entry_t *out);

/**
 * @brief  Get the number of contacts currently saved.
 */
uint8_t ble_service_get_contact_count(void);

/**
 * @brief  Delete a contact slot.
 *
 * Sets ts_valid_N = 0 and decrements ts_count.
 * Remaining slots do not shift — the slot becomes reusable.
 *
 * @param  slot_index  Index of slot to delete.
 * @return ESP_OK on success.
 */
esp_err_t ble_service_delete_contact(uint8_t slot_index);
