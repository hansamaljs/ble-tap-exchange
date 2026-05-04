/**
 * ================================================================
 * tests/09_ble_tapshare/main/main.c
 * Tier 3 Integration Test — Full TapShare end-to-end
 * ================================================================
 * WHAT THIS TEST PROVES
 * ---------------------
 * - Full flow: button long-press → share mode → BLE connect →
 *   GATT exchange → NVS save → display result → IDLE
 * - OLED displays correct state at every transition
 * - Buzzer fires correct sound at each stage
 * - NVS save is atomic (power cycle test)
 * - Contact count increments correctly
 * - After 5 exchanges, slot rotation works (FIFO wraps)
 * - Two boards can exchange each other's profiles correctly
 *
 * HOW TO USE
 * ----------
 * 1. Copy test_config.h.template → test_config.h
 *    Board A: MY_PROFILE_NAME = "Alice"
 *    Board B: MY_PROFILE_NAME = "Bob"
 * 2. Flash to both boards.
 * 3. Hold SELECT on both boards simultaneously (long press = 800ms).
 *    Both should start scanning (buzzer scanning pulse).
 * 4. They find each other → connect → exchange → save.
 *    Board A serial: shows received Bob profile
 *    Board B serial: shows received Alice profile
 *    OLED: shows "SAVED: [name]" for 2 seconds
 *    Buzzer: plays share_saved melody
 * 5. Power cycle both boards. Press SELECT long-press again.
 *    Previous contact should show on display (NVS persisted).
 *
 * NVS PERSISTENCE TEST
 * --------------------
 * After a successful exchange, note the contact count on serial.
 * Power cycle both boards (unplug USB).
 * Run this test again. The contact count should be the same as before
 * power cycle. This proves NVS write committed to flash correctly.
 *
 * SLOT ROTATION TEST
 * ------------------
 * Repeat the tap exchange 6 times total (more than TAPSHARE_MAX_CONTACTS=5).
 * On the 6th exchange, serial should show "slot 0" being overwritten
 * and ts_next wrapping back to 1.
 * ================================================================
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "app_events.h"
#include "ssd1306_driver.h"
#include "font_5x7.h"
#include "button_driver.h"
#include "buzzer_driver.h"
#include "rtc_driver.h"
#include "ble_driver.h"
#include "ble_service.h"

static const char *TAG = "BLE_09";

static QueueHandle_t   s_input_queue;
static QueueHandle_t   s_ble_queue;
static ssd1306_handle_t s_display;

/* ── Display helpers ──────────────────────────────────────────── */
static void display_state(const char *line1, const char *line2,
                           const char *line3)
{
    ssd1306_clear(s_display);
    if (line1) ssd1306_draw_text(s_display, 0, 0,  line1, &font_5x7, true);
    if (line2) ssd1306_draw_text(s_display, 0, 16, line2, &font_5x7, true);
    if (line3) ssd1306_draw_text(s_display, 0, 32, line3, &font_5x7, true);
    ssd1306_flush(s_display);
}

static void display_contact_saved(const char *name)
{
    char line2[32];
    snprintf(line2, sizeof(line2), "%.20s", name);
    ssd1306_clear(s_display);
    ssd1306_draw_text(s_display, 0, 0,  "SAVED!", &font_5x7, true);
    ssd1306_draw_text(s_display, 0, 16, line2,   &font_5x7, true);
    ssd1306_draw_text(s_display, 0, 48, "Going to idle...", &font_5x7, true);
    ssd1306_flush(s_display);
}

static void display_contacts_summary(void)
{
    uint8_t count = ble_service_get_contact_count();
    ssd1306_clear(s_display);
    ssd1306_draw_text(s_display, 0, 0,  "NVS Contacts:", &font_5x7, true);
    char buf[24];
    snprintf(buf, sizeof(buf), "%u / %u slots", count, TAPSHARE_MAX_CONTACTS);
    ssd1306_draw_text(s_display, 0, 16, buf, &font_5x7, true);

    /* Show first saved contact name if any */
    if (count > 0) {
        contact_entry_t entry;
        if (ble_service_get_contact(0, &entry) == ESP_OK) {
            snprintf(buf, sizeof(buf), "[0]: %.16s", entry.name);
            ssd1306_draw_text(s_display, 0, 32, buf, &font_5x7, true);
        }
    }
    ssd1306_flush(s_display);
}

/* ── State name helper ────────────────────────────────────────── */
static const char *state_name(ble_share_state_t s)
{
    switch (s) {
    case BLE_STATE_IDLE:       return "IDLE";
    case BLE_STATE_SHARING:    return "SHARING";
    case BLE_STATE_CONNECTING: return "CONNECTING";
    case BLE_STATE_EXCHANGING: return "EXCHANGING";
    case BLE_STATE_DONE:       return "SAVED";
    case BLE_STATE_FAILED:     return "FAILED";
    default:                   return "?";
    }
}

/* ── BLE event processing task ────────────────────────────────── */
static void ble_proc_task(void *pv)
{
    ble_event_t evt;
    ble_share_state_t last_state = BLE_STATE_IDLE;

    while (1) {
        /* Process BLE events */
        if (xQueueReceive(s_ble_queue, &evt, pdMS_TO_TICKS(50)) == pdPASS) {
            ble_service_on_ble_event(&evt);
        }

        /* Detect and react to state changes */
        ble_share_state_t st = ble_service_get_state();
        if (st != last_state) {
            ESP_LOGI(TAG, "State → %s", state_name(st));

            switch (st) {
            case BLE_STATE_SHARING:
                display_state("TAPSHARE", "Scanning...", "Hold SELECT to cancel");
                buzzer_ble_scanning();
                break;

            case BLE_STATE_CONNECTING:
                display_state("TAPSHARE", "Connecting...", NULL);
                buzzer_ble_connected();
                break;

            case BLE_STATE_EXCHANGING:
                display_state("TAPSHARE", "Exchanging...", NULL);
                buzzer_tap_detected();
                break;

            case BLE_STATE_DONE: {
                /* Read most recent saved contact for display */
                uint8_t count = ble_service_get_contact_count();
                if (count > 0) {
                    uint8_t last_slot = (count <= TAPSHARE_MAX_CONTACTS) ?
                                        count - 1 : TAPSHARE_MAX_CONTACTS - 1;
                    contact_entry_t entry;
                    if (ble_service_get_contact(last_slot, &entry) == ESP_OK) {
                        display_contact_saved(entry.name);
                        ESP_LOGI(TAG, "Saved contact: %s / %s / %s",
                                 entry.name, entry.phone, entry.title);
                    }
                }
                buzzer_share_saved();
                break;
            }

            case BLE_STATE_FAILED:
                display_state("TAPSHARE", "Exchange failed.", "Try again.");
                buzzer_ble_timeout();
                break;

            case BLE_STATE_IDLE:
                display_contacts_summary();
                break;

            default:
                break;
            }

            last_state = st;
        }
    }
}

/* ── Input task ───────────────────────────────────────────────── */
static void input_task(void *pv)
{
    button_driver_init(s_input_queue);

    static const int pin_map[4] = {
        PIN_BTN_SELECT, PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_BACK,
    };

    button_id_t raw_id;
    while (1) {
        if (xQueueReceive(s_input_queue, &raw_id, portMAX_DELAY) == pdPASS) {
            vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));

            uint32_t press_start = (uint32_t)(esp_timer_get_time() / 1000);
            bool long_press      = false;

            /* Poll until long-press threshold or button released */
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
                uint32_t held = (uint32_t)(esp_timer_get_time() / 1000) - press_start;
                if (held >= BTN_LONG_PRESS_MS) {
                    long_press = true;
                    break;
                }
                if (gpio_get_level(pin_map[raw_id]) == 1) {
                    break;  /* released before threshold */
                }
            }

            button_event_t evt = {
                .id           = raw_id,
                .type         = long_press ? BTN_EVT_LONG_PRESS : BTN_EVT_PRESS,
                .timestamp_ms = press_start,
            };

            if (long_press) {
                buzzer_long_press();
                ESP_LOGI(TAG, "Long press: BTN=%d", raw_id);
            } else {
                buzzer_click();
            }

            /* Route to BLE service for SELECT long-press */
            ble_service_on_button_event(&evt);
        }
    }
}

/* ── app_main ─────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Test 09: Full TapShare Integration     ");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Hold SELECT (800ms) on both boards to share contacts.");

    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 2. I2C bus */
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .i2c_port              = I2C_PORT,
        .scl_io_num            = PIN_I2C_SCL,
        .sda_io_num            = PIN_I2C_SDA,
        .glitch_ignore_cnt     = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    /* 3. Display */
    s_display = ssd1306_init(i2c_bus);
    configASSERT(s_display);
    display_state("Cylonix", "TapShare v2", "Initialising...");

    /* 4. Buzzer */
    buzzer_driver_init();
    buzzer_confirm();

    /* 5. RTC */
    rtc_driver_init();

    /* 6. Queues */
    s_input_queue = xQueueCreate(QUEUE_INPUT_DEPTH, sizeof(button_id_t));
    s_ble_queue   = xQueueCreate(QUEUE_BLE_DEPTH,   sizeof(ble_event_t));
    configASSERT(s_input_queue);
    configASSERT(s_ble_queue);

    /* 7. BLE service (loads profile from NVS, sets up GATT server profile) */
    ret = ble_service_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble_service_init failed");
        display_state("ERROR", "ble_service", "init failed");
        return;
    }

    /* 8. BLE driver (starts NimBLE stack) */
    ret = ble_driver_init(s_ble_queue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble_driver_init failed");
        display_state("ERROR", "ble_driver", "init failed");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    /* 9. Show profile and contact count on startup */
    profile_data_t myp;
    ble_service_get_my_profile(&myp);
    ESP_LOGI(TAG, "Own profile: %s / %s / %s",
             myp.name, myp.phone, myp.title);
    ESP_LOGI(TAG, "Saved contacts: %d", ble_service_get_contact_count());

    display_contacts_summary();

    /* 10. Print all existing contacts to serial */
    uint8_t count = ble_service_get_contact_count();
    if (count > 0) {
        ESP_LOGI(TAG, "── Existing contacts in NVS ──");
        for (int i = 0; i < TAPSHARE_MAX_CONTACTS; i++) {
            contact_entry_t entry;
            if (ble_service_get_contact(i, &entry) == ESP_OK) {
                ESP_LOGI(TAG, "  [%d] %s / %s / %s (saved_at=%lu)",
                         i, entry.name, entry.phone, entry.title,
                         (unsigned long)entry.saved_at_epoch);
            }
        }
        ESP_LOGI(TAG, "──────────────────────────────");
    }

    /* 11. Start tasks */
    xTaskCreate(input_task,   "input",   STACK_INPUT, NULL, TASK_PRIO_INPUT, NULL);
    xTaskCreate(ble_proc_task,"ble_proc",STACK_BLE,   NULL, TASK_PRIO_BLE,   NULL);

    ESP_LOGI(TAG, "All tasks running. Hold SELECT to share.");
    ESP_LOGI(TAG, "Hold SELECT again while sharing to cancel.");
}
