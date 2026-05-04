/**
 * ================================================================
 * ble_driver.c  —  Rev 2.0
 * NimBLE radio driver — Implementation
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3, NimBLE
 *
 * SECTIONS
 * --------
 * 1. Private state and UUID definitions
 * 2. GATT table and access callback
 * 3. GATT client — chained async read then write
 * 4. GAP event handler
 * 5. Scan event callback (4-gate filter + role decision)
 * 6. Advertising helpers
 * 7. NimBLE host task and on_sync callback
 * 8. Public API
 *
 * UUID PLACEHOLDER NOTE
 * ---------------------
 * The UUIDs below use real UUID v4 values from uuidgenerator.net.
 * Each UUID is a 16-byte little-endian array.
 * ================================================================
 */

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_driver.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "BLE_DRV";

/* ================================================================
 * SECTION 1 — Private state and UUID definitions
 * ================================================================ */

/* ── UUID v4 values ── */
/* Service UUID: 3f01ca00-838d-47a1-b989-e14f368838b5 */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xb5,0x38,0x88,0x36, 0x4f,0xe1, 0x89,0xb9,
    0xa1,0x47, 0x8d,0x83, 0x00,0xca,0x01,0x3f
);

/* Name characteristic UUID: b9c30a8b-7ef3-4525-9977-d5d504ce3f13 */
static const ble_uuid128_t s_chr_name_uuid = BLE_UUID128_INIT(
    0x13,0x3f,0xce,0x04, 0xd5,0xd5, 0x77,0x99,
    0x25,0x45, 0xf3,0x7e, 0x8b,0x0a,0xc3,0xb9
);

/* Phone characteristic UUID: 459dff70-04c3-4779-8523-6a13bc8412fb */
static const ble_uuid128_t s_chr_phone_uuid = BLE_UUID128_INIT(
    0xfb,0x12,0x84,0xbc, 0x13,0x6a, 0x23,0x85,
    0x79,0x47, 0xc3,0x04, 0x70,0xff,0x9d,0x45
);

/* Title characteristic UUID: 3675ea16-c6ec-488c-a421-8db1252d564e */
static const ble_uuid128_t s_chr_title_uuid = BLE_UUID128_INIT(
    0x4e,0x56,0x2d,0x25, 0xb1,0x8d, 0x21,0xa4,
    0x8c,0x48, 0xec,0xc6, 0x16,0xea,0x75,0x36
);

/* Characteristic value handles — filled by NimBLE at registration */
static uint16_t s_chr_name_handle  = 0;
static uint16_t s_chr_phone_handle = 0;
static uint16_t s_chr_title_handle = 0;

/* Private state */
static QueueHandle_t  s_event_queue  = NULL;
static profile_data_t s_my_profile;          /* Own profile, served to peers */
static profile_data_t s_peer_profile;        /* Peer profile, built during exchange */
static bool           s_connected    = false;
static uint32_t       s_my_tap_ts    = 0;    /* Set by ble_driver_start_share_mode() */
static uint8_t        s_own_addr[6];         /* Our BLE MAC, read at on_sync */

/* GATT exchange read stage tracker */
typedef enum {
    GATT_STAGE_READ_NAME = 0,
    GATT_STAGE_READ_PHONE,
    GATT_STAGE_READ_TITLE,
    GATT_STAGE_WRITE_NAME,
    GATT_STAGE_WRITE_PHONE,
    GATT_STAGE_WRITE_TITLE,
    GATT_STAGE_DONE,
} gatt_stage_t;

static gatt_stage_t  s_gatt_stage    = GATT_STAGE_READ_NAME;
static uint16_t      s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static profile_data_t s_exchange_own; /* Own profile copy for GATT write phase */

/* Helper: send a ble_event_t to the service layer */
static void send_event(ble_event_t *evt)
{
    if (s_event_queue) {
        xQueueSend(s_event_queue, evt, 0);
    }
}

/* ================================================================
 * SECTION 2 — GATT table and access callback
 * ================================================================ */

/**
 * GATT access callback — called by NimBLE for READ and WRITE ops.
 * One callback handles all three characteristics.
 * READ: copy from s_my_profile into the response om.
 * WRITE: NimBLE calls this when Central writes its profile to us.
 *        We store it directly in s_peer_profile fields.
 */
static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *val = NULL;
        if (attr_handle == s_chr_name_handle)  val = s_my_profile.name;
        else if (attr_handle == s_chr_phone_handle) val = s_my_profile.phone;
        else if (attr_handle == s_chr_title_handle) val = s_my_profile.title;
        if (!val) return BLE_ATT_ERR_ATTR_NOT_FOUND;
        int rc = os_mbuf_append(ctxt->om, val, (uint16_t)strlen(val) + 1);
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;

    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        char buf[PROFILE_TITLE_MAX];
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
        buf[len] = '\0';

        if (attr_handle == s_chr_name_handle)
            strncpy(s_peer_profile.name,  buf, PROFILE_NAME_MAX - 1);
        else if (attr_handle == s_chr_phone_handle)
            strncpy(s_peer_profile.phone, buf, PROFILE_PHONE_MAX - 1);
        else if (attr_handle == s_chr_title_handle)
            strncpy(s_peer_profile.title, buf, PROFILE_TITLE_MAX - 1);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &s_chr_name_uuid.u,
                .access_cb = gatt_access_cb,
                .val_handle= &s_chr_name_handle,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid      = &s_chr_phone_uuid.u,
                .access_cb = gatt_access_cb,
                .val_handle= &s_chr_phone_handle,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid      = &s_chr_title_uuid.u,
                .access_cb = gatt_access_cb,
                .val_handle= &s_chr_title_handle,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            { 0 }  /* Terminator */
        },
    },
    { 0 }  /* Terminator */
};

/* ================================================================
 * SECTION 3 — GATT client: chained async read then write
 * ================================================================ */

/* Forward declarations */
static void gatt_read_next(uint16_t conn_handle);
static void gatt_write_next(uint16_t conn_handle);

static int gatt_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGE(TAG, "GATT read failed at stage %d: status=%d",
                 s_gatt_stage, error->status);
        ble_event_t evt = { .type = BLE_EVT_EXCHANGE_FAILED,
                            .conn_handle = conn_handle };
        send_event(&evt);
        return 0;
    }

    /* Extract the attribute value into the appropriate field */
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    char buf[PROFILE_TITLE_MAX];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    ble_hs_mbuf_to_flat(attr->om, buf, len, NULL);
    buf[len] = '\0';

    switch (s_gatt_stage) {
        case GATT_STAGE_READ_NAME:
            strncpy(s_peer_profile.name, buf, PROFILE_NAME_MAX - 1);
            ESP_LOGI(TAG, "Read peer name: %s", s_peer_profile.name);
            s_gatt_stage = GATT_STAGE_READ_PHONE;
            break;
        case GATT_STAGE_READ_PHONE:
            strncpy(s_peer_profile.phone, buf, PROFILE_PHONE_MAX - 1);
            ESP_LOGI(TAG, "Read peer phone: %s", s_peer_profile.phone);
            s_gatt_stage = GATT_STAGE_READ_TITLE;
            break;
        case GATT_STAGE_READ_TITLE:
            strncpy(s_peer_profile.title, buf, PROFILE_TITLE_MAX - 1);
            ESP_LOGI(TAG, "Read peer title: %s", s_peer_profile.title);
            s_gatt_stage = GATT_STAGE_WRITE_NAME;
            break;
        default:
            break;
    }

    gatt_read_next(conn_handle);
    return 0;
}

static void gatt_read_next(uint16_t conn_handle)
{
    uint16_t handle = 0;
    switch (s_gatt_stage) {
        case GATT_STAGE_READ_NAME:  handle = s_chr_name_handle;  break;
        case GATT_STAGE_READ_PHONE: handle = s_chr_phone_handle; break;
        case GATT_STAGE_READ_TITLE: handle = s_chr_title_handle; break;
        case GATT_STAGE_WRITE_NAME: /* reads done, start writes */
            gatt_write_next(conn_handle);
            return;
        default:
            return;
    }
    ble_gattc_read(conn_handle, handle, gatt_read_cb, NULL);
}

static int gatt_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGE(TAG, "GATT write failed at stage %d: status=%d",
                 s_gatt_stage, error->status);
        ble_event_t evt = { .type = BLE_EVT_EXCHANGE_FAILED,
                            .conn_handle = conn_handle };
        send_event(&evt);
        return 0;
    }

    switch (s_gatt_stage) {
        case GATT_STAGE_WRITE_NAME:
            ESP_LOGI(TAG, "Wrote own name to peer");
            s_gatt_stage = GATT_STAGE_WRITE_PHONE;
            break;
        case GATT_STAGE_WRITE_PHONE:
            ESP_LOGI(TAG, "Wrote own phone to peer");
            s_gatt_stage = GATT_STAGE_WRITE_TITLE;
            break;
        case GATT_STAGE_WRITE_TITLE:
            ESP_LOGI(TAG, "Wrote own title to peer — exchange complete");
            s_gatt_stage = GATT_STAGE_DONE;
            /* All 6 ops succeeded — report to service layer */
            {
                ble_event_t evt = {
                    .type        = BLE_EVT_EXCHANGE_COMPLETE,
                    .conn_handle = conn_handle,
                    .peer_profile= s_peer_profile,
                };
                send_event(&evt);
            }
            return 0;
        default:
            return 0;
    }
    gatt_write_next(conn_handle);
    return 0;
}

static void gatt_write_next(uint16_t conn_handle)
{
    uint16_t  handle = 0;
    const char *val  = NULL;
    uint16_t   len   = 0;

    switch (s_gatt_stage) {
        case GATT_STAGE_WRITE_NAME:
            handle = s_chr_name_handle;
            val    = s_exchange_own.name;
            len    = (uint16_t)strlen(val) + 1;
            break;
        case GATT_STAGE_WRITE_PHONE:
            handle = s_chr_phone_handle;
            val    = s_exchange_own.phone;
            len    = (uint16_t)strlen(val) + 1;
            break;
        case GATT_STAGE_WRITE_TITLE:
            handle = s_chr_title_handle;
            val    = s_exchange_own.title;
            len    = (uint16_t)strlen(val) + 1;
            break;
        default:
            return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(val, len);
    if (!om) {
        ESP_LOGE(TAG, "ble_hs_mbuf_from_flat OOM");
        ble_event_t evt = { .type = BLE_EVT_EXCHANGE_FAILED,
                            .conn_handle = conn_handle };
        send_event(&evt);
        return;
    }
    ble_gattc_write(conn_handle, handle, om, gatt_write_cb, NULL);
}

/* ================================================================
 * SECTION 4 — GAP event handler
 * ================================================================ */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    ble_event_t evt;
    memset(&evt, 0, sizeof(evt));

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected   = true;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "GAP connected — handle=%d", s_conn_handle);
            evt.type        = BLE_EVT_CONNECTED;
            evt.conn_handle = s_conn_handle;
            send_event(&evt);
        } else {
            ESP_LOGW(TAG, "GAP connect failed: status=%d",
                     event->connect.status);
            evt.type = BLE_EVT_EXCHANGE_FAILED;
            send_event(&evt);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "GAP disconnected — reason=%d",
                 event->disconnect.reason);
        s_connected   = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* If the exchange already completed, ble_service will disconnect on
         * its own after processing BLE_EVT_EXCHANGE_COMPLETE.  Sending
         * BLE_EVT_DISCONNECTED here would race with that event and cause a
         * spurious FAILED transition before the contact is saved. */
        if (s_gatt_stage != GATT_STAGE_DONE) {
            evt.type = BLE_EVT_DISCONNECTED;
            send_event(&evt);
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ================================================================
 * SECTION 5 — Scan event callback (4-gate filter + role decision)
 * ================================================================ */

static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_hs_adv_fields fields;
    int rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                      event->disc.length_data);
    if (rc != 0) return 0;

    /* ── Gate 1: UUID match ─────────────────────────────────── */
    bool uuid_match = false;
    for (int i = 0; i < fields.num_uuids128; i++) {
        if (ble_uuid_cmp(&fields.uuids128[i].u, &s_svc_uuid.u) == 0) {
            uuid_match = true;
            break;
        }
    }
    if (!uuid_match) return 0;

    /* ── Gate 2: RSSI check ─────────────────────────────────── */
    int8_t rssi = event->disc.rssi;
    if (rssi < TAPSHARE_RSSI_THRESHOLD) {
        ESP_LOGD(TAG, "Peer found but RSSI too weak: %d dBm", rssi);
        return 0;
    }

    /* ── Extract tap_ts from manufacturer data ──────────────── */
    /* Manufacturer data layout: [magic(1)] [tap_ts(4)] little-endian */
    uint32_t peer_tap_ts = 0;
    if (fields.mfg_data_len >= 5 &&
        fields.mfg_data[0] == TAPSHARE_ADV_MAGIC) {
        memcpy(&peer_tap_ts, &fields.mfg_data[1], sizeof(uint32_t));
    } else {
        ESP_LOGD(TAG, "Peer has no valid manufacturer data");
        return 0;
    }

    /* ── Gate 3: Time window check ──────────────────────────── */
    int32_t ts_diff = (int32_t)peer_tap_ts - (int32_t)s_my_tap_ts;
    if (ts_diff < 0) ts_diff = -ts_diff;
    if ((uint32_t)ts_diff > TAPSHARE_TIME_WINDOW_S) {
        ESP_LOGD(TAG, "Peer tap timestamp outside window: diff=%" PRId32 "s", ts_diff);
        return 0;
    }

    /* ── Gate 4: Role decision ──────────────────────────────── */
    /* Lower tap_ts becomes Central and calls ble_gap_connect().
     * Equal timestamps: compare MAC addresses lexicographically.
     * Both watches run this same logic independently and reach
     * the same role assignment deterministically. */
    bool i_am_central;
    if (s_my_tap_ts < peer_tap_ts) {
        i_am_central = true;
    } else if (s_my_tap_ts > peer_tap_ts) {
        i_am_central = false;
    } else {
        /* Equal timestamps — MAC tiebreaker */
        i_am_central = (memcmp(s_own_addr, event->disc.addr.val, 6) < 0);
    }

    if (!i_am_central) {
        ESP_LOGI(TAG, "Role: PERIPHERAL — peer will connect to us");
        return 0;  /* Wait for them to connect; our GATT server is ready */
    }

    ESP_LOGI(TAG, "Role: CENTRAL — all 4 gates passed. Connecting...");
    ESP_LOGI(TAG, "  RSSI=%d  peer_ts=%lu  my_ts=%lu",
             rssi, (unsigned long)peer_tap_ts, (unsigned long)s_my_tap_ts);

    /* Stop scanning before connecting — C3 cannot scan during connection */
    ble_gap_disc_cancel();

    /* Stop advertising before connecting */
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }

    /* Initiate GAP connection */
    struct ble_gap_conn_params conn_params = {
        .scan_itvl          = 0x0010,
        .scan_window        = 0x0010,
        .itvl_min           = BLE_GAP_INITIAL_CONN_ITVL_MIN,
        .itvl_max           = BLE_GAP_INITIAL_CONN_ITVL_MAX,
        .latency            = 0,
        .supervision_timeout= 0x0100,
        .min_ce_len         = 0x0010,
        .max_ce_len         = 0x0300,
    };
    rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr,
                         30000, &conn_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
    }

    return 0;
}

/* ================================================================
 * SECTION 6 — Advertising helpers
 * ================================================================ */

static esp_err_t start_advertising(uint32_t tap_ts)
{
    /* Build manufacturer data: [magic(1)][tap_ts(4)] */
    uint8_t mfr_data[5];
    mfr_data[0] = TAPSHARE_ADV_MAGIC;
    memcpy(&mfr_data[1], &tap_ts, sizeof(uint32_t));

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags               = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128            = &s_svc_uuid;
    fields.num_uuids128        = 1;
    fields.uuids128_is_complete= 1;
    fields.mfg_data            = mfr_data;
    fields.mfg_data_len        = sizeof(mfr_data);

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    /* Scan response: device name */
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name            = (uint8_t *)"cylonix-watch";
    rsp_fields.name_len        = 13;
    rsp_fields.name_is_complete= 1;
    ble_gap_adv_rsp_set_fields(&rsp_fields);

    /* Advertising parameters */
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode  = BLE_GAP_CONN_MODE_UND;  /* Connectable */
    adv_params.disc_mode  = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Advertising started — tap_ts=%lu", (unsigned long)tap_ts);
    return ESP_OK;
}

static esp_err_t start_scanning(void)
{
    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.passive       = 0;   /* Active scan: send SCAN_REQ for scan response */
    disc_params.filter_policy = 0;   /* Accept all */
    disc_params.itvl          = 80;  /* 50 ms */
    disc_params.window        = 80;  /* 50 ms (100% duty cycle during share mode) */

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                          &disc_params, scan_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Scanning started");
    return ESP_OK;
}

/* ================================================================
 * SECTION 7 — NimBLE host task and on_sync callback
 * ================================================================ */

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();  /* Blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static void on_sync_cb(void)
{
    /* Read our own public BLE address — used for MAC tiebreaker */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, s_own_addr, NULL);
        ESP_LOGI(TAG, "BLE address: %02X:%02X:%02X:%02X:%02X:%02X",
                 s_own_addr[5], s_own_addr[4], s_own_addr[3],
                 s_own_addr[2], s_own_addr[1], s_own_addr[0]);
    }
}

/* ================================================================
 * SECTION 8 — Public API
 * ================================================================ */

esp_err_t ble_driver_init(QueueHandle_t event_queue)
{
    if (!event_queue) return ESP_ERR_INVALID_ARG;
    s_event_queue = event_queue;

    memset(&s_my_profile,   0, sizeof(s_my_profile));
    memset(&s_peer_profile, 0, sizeof(s_peer_profile));

    /* NimBLE init sequence — order is fixed */
    nimble_port_init();

    /* Register GAP and GATT base services */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("cylonix-watch");

    /* Register our GATT service table */
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* Set sync callback — fires when NimBLE is ready */
    ble_hs_cfg.sync_cb = on_sync_cb;

    /* Start NimBLE's internal FreeRTOS task */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE driver init OK");
    return ESP_OK;
}

void ble_driver_deinit(void)
{
    nimble_port_stop();
}

esp_err_t ble_driver_start_share_mode(uint32_t tap_ts)
{
    s_my_tap_ts   = tap_ts;
    s_gatt_stage  = GATT_STAGE_READ_NAME;
    memset(&s_peer_profile, 0, sizeof(s_peer_profile));

    esp_err_t ret = start_advertising(tap_ts);
    if (ret != ESP_OK) return ret;
    return start_scanning();
}

void ble_driver_stop_share_mode(void)
{
    if (ble_gap_disc_active())  ble_gap_disc_cancel();
    if (ble_gap_adv_active())   ble_gap_adv_stop();
    ESP_LOGI(TAG, "Share mode stopped");
}

esp_err_t ble_driver_start_gatt_exchange(uint16_t conn_handle,
                                          const profile_data_t *own_profile)
{
    if (!own_profile) return ESP_ERR_INVALID_ARG;

    s_conn_handle = conn_handle;
    s_gatt_stage  = GATT_STAGE_READ_NAME;
    memset(&s_peer_profile, 0, sizeof(s_peer_profile));
    memcpy(&s_exchange_own, own_profile, sizeof(profile_data_t));

    ESP_LOGI(TAG, "Starting GATT exchange on conn_handle=%d", conn_handle);
    gatt_read_next(conn_handle);
    return ESP_OK;
}

void ble_driver_disconnect(uint16_t conn_handle)
{
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

bool ble_driver_is_connected(void)
{
    return s_connected;
}

void ble_driver_set_my_profile(const profile_data_t *profile)
{
    if (profile) {
        memcpy(&s_my_profile, profile, sizeof(profile_data_t));
    }
}
