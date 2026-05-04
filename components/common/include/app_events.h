/**
 * ================================================================
 * app_events.h  —  Rev 2.0
 * All event types used across the firmware in one place
 * ================================================================
 * Rule: every event type, enum, and shared struct lives here.
 *       No duplicate type definitions anywhere else.
 *
 * All inter-task messages are built from these types.
 * ================================================================
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Define ble_addr_t locally so app_events.h doesn't force the entire NimBLE
 * stack into every component that includes it (e.g. button, display).
 * The layout matches NimBLE exactly; the guard prevents a redefinition error
 * when a component also includes host/ble_hs.h directly. */
#ifndef H_BLE_
typedef struct {
    uint8_t type;
    uint8_t val[6];
} ble_addr_t;
#endif

/* ================================================================
 * Button events
 * Sent by input_task into g_input_queue.
 * ================================================================ */
typedef enum {
    BTN_SELECT = 0,
    BTN_UP,
    BTN_DOWN,
    BTN_BACK,
    BTN_NONE,
} button_id_t;

typedef enum {
    BTN_EVT_PRESS = 0,      /* Released before BTN_LONG_PRESS_MS  */
    BTN_EVT_LONG_PRESS,     /* Held for >= BTN_LONG_PRESS_MS      */
    BTN_EVT_RELEASE,        /* Fired on every release             */
} button_event_type_t;

typedef struct {
    button_id_t         id;
    button_event_type_t type;
    uint32_t            timestamp_ms;   /* esp_timer_get_time() / 1000 */
} button_event_t;

/* ================================================================
 * Profile data
 * Exchanged over BLE and stored in NVS.
 * ================================================================ */
#define PROFILE_NAME_MAX    32
#define PROFILE_PHONE_MAX   16
#define PROFILE_TITLE_MAX   64

typedef struct {
    char name[PROFILE_NAME_MAX];
    char phone[PROFILE_PHONE_MAX];
    char title[PROFILE_TITLE_MAX];
} profile_data_t;

/* ================================================================
 * Contact entry — one saved contact in the addressbook.
 * Stored as an NVS blob (120 bytes per slot).
 * ================================================================ */
typedef struct {
    char     name[PROFILE_NAME_MAX];    /* 32 bytes */
    char     phone[PROFILE_PHONE_MAX];  /* 16 bytes */
    char     title[PROFILE_TITLE_MAX];  /* 64 bytes */
    uint32_t saved_at_epoch;            /*  4 bytes — POSIX time of save */
    uint8_t  version;                   /*  1 byte  — struct version tag */
    uint8_t  _pad[3];                   /*  3 bytes — alignment          */
} contact_entry_t;                      /* = 120 bytes total */

/* ================================================================
 * BLE share state machine
 * Lives in ble_service.c. Exposed here so ui_service can read it.
 * ================================================================ */
typedef enum {
    BLE_STATE_IDLE = 0,     /* Radio off. Default state.                   */
    BLE_STATE_SHARING,      /* Advertising + scanning. Waiting for peer.   */
    BLE_STATE_CONNECTING,   /* Peer found. Initiating GAP connect.         */
    BLE_STATE_EXCHANGING,   /* Connected. Reading + writing GATT chars.    */
    BLE_STATE_DONE,         /* Exchange complete. Showing result on screen. */
    BLE_STATE_FAILED,       /* Exchange failed. Showing error. Auto-idle.  */
} ble_share_state_t;

/* ================================================================
 * BLE events
 * Sent by ble_driver into g_ble_event_queue for ble_service to process.
 * ================================================================ */
typedef enum {
    BLE_EVT_PEER_FOUND = 0,     /* Peer Cylonix watch found in scan results */
    BLE_EVT_CONNECTED,          /* GAP connection established               */
    BLE_EVT_EXCHANGE_COMPLETE,  /* All 3 GATT reads + 3 writes succeeded    */
    BLE_EVT_EXCHANGE_FAILED,    /* Any GATT op failed                       */
    BLE_EVT_DISCONNECTED,       /* GAP disconnection event                  */
    BLE_EVT_SCAN_TIMEOUT,       /* Scan ran for TAPSHARE_SCAN_TIMEOUT_MS    */
} ble_event_type_t;

typedef struct {
    ble_event_type_t type;
    ble_addr_t       peer_addr;         /* Valid for PEER_FOUND and CONNECTED */
    int8_t           rssi;              /* Valid for PEER_FOUND only          */
    uint32_t         peer_tap_ts;       /* Peer tap timestamp, extracted from
                                         * manufacturer data in advert packet */
    profile_data_t   peer_profile;      /* Valid for EXCHANGE_COMPLETE only   */
    uint16_t         conn_handle;       /* Valid for CONNECTED and onwards     */
} ble_event_t;

/* ================================================================
 * Time event group bits
 * Used with xEventGroupSetBits() by time_task.
 * Other tasks wait on these bits to know when time has advanced.
 * ================================================================ */
#define TIME_EVT_SECOND_TICK    (1 << 0)   /* Fires every second      */
#define TIME_EVT_MINUTE_TICK    (1 << 1)   /* Fires every minute      */
#define TIME_EVT_ALARM_TRIGGER  (1 << 2)   /* Alarm time has arrived  */
