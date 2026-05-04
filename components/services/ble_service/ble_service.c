/**
 * ================================================================
 * ble_service.c  —  Rev 2.0
 * TapShare business logic — Implementation
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 *
 * SECTIONS
 * --------
 * 1. Private state and NVS key helpers
 * 2. NVS contact storage (safe write sequence)
 * 3. Own profile NVS load/save
 * 4. State machine (set_state, transition helpers)
 * 5. Scan timeout esp_timer callback
 * 6. Public API
 * ================================================================
 */

#include "host/ble_hs.h"
#include "ble_service.h"
#include "ble_driver.h"
#include "rtc_driver.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

/* Include test_config.h for first-boot profile defaults.
 * This file is gitignored — copy from test_config.h.template. */
#ifdef __has_include
  #if __has_include("test_config.h")
    #include "test_config.h"
  #else
    #define MY_PROFILE_NAME  "Cylonix User"
    #define MY_PROFILE_PHONE "+00000000000"
    #define MY_PROFILE_TITLE "TapShare"
  #endif
#else
  #define MY_PROFILE_NAME  "Cylonix User"
  #define MY_PROFILE_PHONE "+00000000000"
  #define MY_PROFILE_TITLE "TapShare"
#endif

static const char *TAG = "BLE_SVC";

/* ================================================================
 * SECTION 1 — Private state and NVS key helpers
 * ================================================================ */

static nvs_handle_t     s_nvs;
static ble_share_state_t s_state      = BLE_STATE_IDLE;
static uint32_t          s_tap_ts     = 0;
static uint8_t           s_contact_count = 0;
static uint8_t           s_next_slot  = 0;
static profile_data_t    s_my_profile;
static esp_timer_handle_t s_scan_timer = NULL;
static uint16_t           s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

/* Build NVS key strings for per-slot keys.
 * Keys must be <= 15 chars for NVS.
 * Format: "ts_valid_N" and "ts_slot_N" where N is 0-4.
 */
static void make_valid_key(uint8_t slot, char *out)  /* out must be >=13 chars */
{
    snprintf(out, 13, "ts_valid_%u", slot);
}

static void make_slot_key(uint8_t slot, char *out)   /* out must be >=12 chars */
{
    snprintf(out, 12, "ts_slot_%u", slot);
}

/* ================================================================
 * SECTION 2 — NVS contact storage (safe write sequence)
 * ================================================================
 *
 * Safe write sequence for each slot:
 *   1. Write ts_valid_N = 0  (mark invalid BEFORE writing data)
 *   2. nvs_commit()           (flush to flash NOW)
 *   3. Write ts_slot_N blob   (write full contact_entry_t)
 *   4. Write ts_valid_N = 1  (mark valid AFTER data is written)
 *   5. nvs_commit()           (final flush)
 *
 * This ensures that a power loss between steps 3-4 leaves the slot
 * flagged as invalid. On next boot, invalid slots are skipped.
 * Without the intermediate commit at step 2, step 1's write might
 * not reach flash before the power loss.
 */
static esp_err_t save_contact_to_nvs(const contact_entry_t *entry)
{
    char valid_key[13];
    char slot_key[12];
    uint8_t slot = s_next_slot;

    make_valid_key(slot, valid_key);
    make_slot_key(slot,  slot_key);

    /* Step 1+2: Mark slot invalid BEFORE writing data */
    uint8_t invalid = 0;
    esp_err_t ret = nvs_set_u8(s_nvs, valid_key, invalid);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "NVS: set valid=0 failed"); return ret; }
    ret = nvs_commit(s_nvs);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "NVS: commit(1) failed"); return ret; }

    /* Step 3: Write the contact blob */
    ret = nvs_set_blob(s_nvs, slot_key, entry, sizeof(contact_entry_t));
    if (ret != ESP_OK) { ESP_LOGE(TAG, "NVS: set blob failed"); return ret; }

    /* Step 4+5: Mark slot valid AFTER data is written */
    uint8_t valid = 1;
    ret = nvs_set_u8(s_nvs, valid_key, valid);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "NVS: set valid=1 failed"); return ret; }
    ret = nvs_commit(s_nvs);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "NVS: commit(2) failed"); return ret; }

    /* Update metadata */
    s_next_slot = (s_next_slot + 1) % TAPSHARE_MAX_CONTACTS;
    if (s_contact_count < TAPSHARE_MAX_CONTACTS) s_contact_count++;

    nvs_set_u8(s_nvs, "ts_count", s_contact_count);
    nvs_set_u8(s_nvs, "ts_next",  s_next_slot);
    nvs_commit(s_nvs);

    ESP_LOGI(TAG, "Contact saved to slot %u: %s", slot, entry->name);
    return ESP_OK;
}

/* ================================================================
 * SECTION 3 — Own profile NVS load/save
 * ================================================================ */

static esp_err_t load_or_init_profile(void)
{
    char buf[PROFILE_TITLE_MAX];
    size_t len = sizeof(buf);
    esp_err_t ret = nvs_get_str(s_nvs, "my_name", buf, &len);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot — write defaults from test_config.h */
        ESP_LOGI(TAG, "No profile in NVS. Writing defaults from test_config.h");
        strncpy(s_my_profile.name,  MY_PROFILE_NAME,  PROFILE_NAME_MAX  - 1);
        strncpy(s_my_profile.phone, MY_PROFILE_PHONE, PROFILE_PHONE_MAX - 1);
        strncpy(s_my_profile.title, MY_PROFILE_TITLE, PROFILE_TITLE_MAX - 1);

        nvs_set_str(s_nvs, "my_name",  s_my_profile.name);
        nvs_set_str(s_nvs, "my_phone", s_my_profile.phone);
        nvs_set_str(s_nvs, "my_title", s_my_profile.title);
        nvs_commit(s_nvs);
        return ESP_OK;
    }

    if (ret != ESP_OK) return ret;

    /* Profile exists — load all three fields */
    strncpy(s_my_profile.name, buf, PROFILE_NAME_MAX - 1);

    len = sizeof(buf);
    nvs_get_str(s_nvs, "my_phone", buf, &len);
    strncpy(s_my_profile.phone, buf, PROFILE_PHONE_MAX - 1);

    len = sizeof(buf);
    nvs_get_str(s_nvs, "my_title", buf, &len);
    strncpy(s_my_profile.title, buf, PROFILE_TITLE_MAX - 1);

    ESP_LOGI(TAG, "Own profile loaded: %s / %s / %s",
             s_my_profile.name, s_my_profile.phone, s_my_profile.title);
    return ESP_OK;
}

/* ================================================================
 * SECTION 4 — State machine
 * ================================================================ */

static void set_state(ble_share_state_t new_state)
{
    static const char *state_names[] = {
        "IDLE", "SHARING", "CONNECTING", "EXCHANGING", "DONE", "FAILED"
    };
    ESP_LOGI(TAG, "State: %s → %s",
             state_names[s_state], state_names[new_state]);
    s_state = new_state;
}

static void enter_idle(void)
{
    ble_driver_stop_share_mode();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_driver_disconnect(s_conn_handle);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    if (s_scan_timer) {
        esp_timer_stop(s_scan_timer);
    }
    set_state(BLE_STATE_IDLE);
}

static void enter_failed(void)
{
    set_state(BLE_STATE_FAILED);
    /* Auto-recover to IDLE after display time */
    vTaskDelay(pdMS_TO_TICKS(TAPSHARE_DISPLAY_MS));
    enter_idle();
}

/* ================================================================
 * SECTION 5 — Scan timeout callback
 * ================================================================ */

static void scan_timeout_cb(void *arg)
{
    ESP_LOGW(TAG, "Scan timeout — no peer found");
    ble_event_t evt = { .type = BLE_EVT_SCAN_TIMEOUT };
    /* Post to ourselves via event handler */
    ble_service_on_ble_event(&evt);
}

/* ================================================================
 * SECTION 6 — Public API
 * ================================================================ */

esp_err_t ble_service_init(void)
{
    /* Open NVS namespace */
    esp_err_t ret = nvs_open(TAPSHARE_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Load own profile */
    ret = load_or_init_profile();
    if (ret != ESP_OK) return ret;

    /* Push own profile to GATT server */
    ble_driver_set_my_profile(&s_my_profile);

    /* Load contact metadata */
    nvs_get_u8(s_nvs, "ts_count", &s_contact_count);
    nvs_get_u8(s_nvs, "ts_next",  &s_next_slot);

    ESP_LOGI(TAG, "Contact addressbook: %u/%u slots used",
             s_contact_count, TAPSHARE_MAX_CONTACTS);

    /* Create scan timeout timer (one-shot) */
    esp_timer_create_args_t timer_args = {
        .callback = scan_timeout_cb,
        .name     = "scan_timeout",
    };
    ret = esp_timer_create(&timer_args, &s_scan_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    set_state(BLE_STATE_IDLE);
    ESP_LOGI(TAG, "BLE service init OK");
    return ESP_OK;
}

void ble_service_on_ble_event(const ble_event_t *evt)
{
    if (!evt) return;

    switch (evt->type) {

    case BLE_EVT_CONNECTED:
        if (s_state == BLE_STATE_CONNECTING || s_state == BLE_STATE_SHARING) {
            s_conn_handle = evt->conn_handle;
            set_state(BLE_STATE_EXCHANGING);
            /* Start GATT exchange immediately on connect */
            ble_driver_start_gatt_exchange(evt->conn_handle, &s_my_profile);
        }
        break;

    case BLE_EVT_EXCHANGE_COMPLETE:
        if (s_state == BLE_STATE_EXCHANGING) {
            /* Build contact entry */
            contact_entry_t entry;
            memset(&entry, 0, sizeof(entry));
            strncpy(entry.name,  evt->peer_profile.name,  PROFILE_NAME_MAX  - 1);
            strncpy(entry.phone, evt->peer_profile.phone, PROFILE_PHONE_MAX - 1);
            strncpy(entry.title, evt->peer_profile.title, PROFILE_TITLE_MAX - 1);
            entry.saved_at_epoch = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            entry.version        = 1;

            /* Save immediately to NVS */
            esp_err_t ret = save_contact_to_nvs(&entry);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Contact saved: %s | %s | %s",
                         entry.name, entry.phone, entry.title);
                set_state(BLE_STATE_DONE);
                /* Display "saved" for TAPSHARE_DISPLAY_MS, then IDLE */
                vTaskDelay(pdMS_TO_TICKS(TAPSHARE_DISPLAY_MS));
            } else {
                ESP_LOGE(TAG, "NVS save failed");
                set_state(BLE_STATE_FAILED);
                vTaskDelay(pdMS_TO_TICKS(TAPSHARE_DISPLAY_MS));
            }

            /* Disconnect and return to IDLE — ready for next tap */
            ble_driver_disconnect(s_conn_handle);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            set_state(BLE_STATE_IDLE);
        }
        break;

    case BLE_EVT_EXCHANGE_FAILED:
        ESP_LOGW(TAG, "Exchange failed");
        enter_failed();
        break;

    case BLE_EVT_DISCONNECTED:
        /* If we were in EXCHANGING when disconnect fired unexpectedly */
        if (s_state == BLE_STATE_EXCHANGING) {
            ESP_LOGW(TAG, "Unexpected disconnect during exchange");
            enter_failed();
        }
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        break;

    case BLE_EVT_SCAN_TIMEOUT:
        if (s_state == BLE_STATE_SHARING) {
            ESP_LOGW(TAG, "No peer found within timeout");
            enter_failed();
        }
        break;

    case BLE_EVT_PEER_FOUND:
        /* Peer found in scan — driver handles connection (4-gate filter).
         * We transition to CONNECTING to track progress. */
        if (s_state == BLE_STATE_SHARING) {
            set_state(BLE_STATE_CONNECTING);
        }
        break;

    default:
        break;
    }
}

void ble_service_on_button_event(const button_event_t *evt)
{
    if (!evt) return;

    if (evt->id == BTN_SELECT && evt->type == BTN_EVT_LONG_PRESS) {
        if (s_state == BLE_STATE_IDLE) {
            /* Get current RTC timestamp as tap trigger time */
            watch_time_t wt;
            rtc_get_time(&wt);
            struct timeval tv;
            gettimeofday(&tv, NULL);
            s_tap_ts = (uint32_t)tv.tv_sec;

            ESP_LOGI(TAG, "Share mode activated — tap_ts=%lu",
                     (unsigned long)s_tap_ts);

            set_state(BLE_STATE_SHARING);
            ble_driver_start_share_mode(s_tap_ts);

            /* Start scan timeout timer */
            esp_timer_start_once(s_scan_timer,
                                 (uint64_t)TAPSHARE_SCAN_TIMEOUT_MS * 1000ULL);

        } else if (s_state == BLE_STATE_SHARING) {
            ESP_LOGI(TAG, "Share mode cancelled by user");
            enter_idle();
        }
    }
}

ble_share_state_t ble_service_get_state(void)
{
    return s_state;
}

uint32_t ble_service_get_tap_timestamp(void)
{
    return s_tap_ts;
}

esp_err_t ble_service_get_my_profile(profile_data_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memcpy(out, &s_my_profile, sizeof(profile_data_t));
    return ESP_OK;
}

esp_err_t ble_service_set_my_profile(const profile_data_t *profile)
{
    if (!profile) return ESP_ERR_INVALID_ARG;

    memcpy(&s_my_profile, profile, sizeof(profile_data_t));

    /* Write to NVS */
    nvs_set_str(s_nvs, "my_name",  s_my_profile.name);
    nvs_set_str(s_nvs, "my_phone", s_my_profile.phone);
    nvs_set_str(s_nvs, "my_title", s_my_profile.title);
    nvs_commit(s_nvs);

    /* Update GATT server to serve new data immediately */
    ble_driver_set_my_profile(&s_my_profile);

    ESP_LOGI(TAG, "Own profile updated: %s", s_my_profile.name);
    return ESP_OK;
}

esp_err_t ble_service_get_contact(uint8_t slot_index, contact_entry_t *out)
{
    if (!out || slot_index >= TAPSHARE_MAX_CONTACTS)
        return ESP_ERR_INVALID_ARG;

    char valid_key[13];
    char slot_key[12];
    make_valid_key(slot_index, valid_key);
    make_slot_key(slot_index,  slot_key);

    uint8_t valid = 0;
    esp_err_t ret = nvs_get_u8(s_nvs, valid_key, &valid);
    if (ret != ESP_OK || valid == 0) return ESP_ERR_NOT_FOUND;

    size_t len = sizeof(contact_entry_t);
    ret = nvs_get_blob(s_nvs, slot_key, out, &len);
    if (ret != ESP_OK) return ESP_ERR_NOT_FOUND;

    return ESP_OK;
}

uint8_t ble_service_get_contact_count(void)
{
    return s_contact_count;
}

esp_err_t ble_service_delete_contact(uint8_t slot_index)
{
    if (slot_index >= TAPSHARE_MAX_CONTACTS) return ESP_ERR_INVALID_ARG;

    char valid_key[13];
    make_valid_key(slot_index, valid_key);

    nvs_set_u8(s_nvs, valid_key, 0);
    if (s_contact_count > 0) s_contact_count--;
    nvs_set_u8(s_nvs, "ts_count", s_contact_count);
    nvs_commit(s_nvs);

    ESP_LOGI(TAG, "Contact slot %u deleted", slot_index);
    return ESP_OK;
}
