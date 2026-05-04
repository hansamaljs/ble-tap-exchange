/**
 * ================================================================
 * tests/05_integration/main/main.c
 * Multi-Driver Integration Test
 * ================================================================
 * Tests all four drivers working together:
 *   - OLED display (ssd1306)
 *   - Buttons (button_driver)
 *   - Buzzer (buzzer_driver)
 *   - RTC (rtc_driver)
 *
 * WHAT IT DOES
 * ------------
 * 1. WiFi NTP sync → sets RTC → displays time on OLED
 * 2. Ticks every second — time updates on display
 * 3. Each button press:
 *    - Plays the correct sound (click, nav_up, nav_down, cancel)
 *    - Shows which button was pressed on the OLED
 * 4. Long-press SELECT → plays long_press sound, inverts display
 * 5. This simulates the core watch_face + input flow
 *
 * This test proves all three new drivers can run simultaneously
 * without interfering with each other or the display driver.
 *
 * WIRING — ALL PERIPHERALS
 * ------------------------
 *   SSD1306 SDA → GPIO5   SCL → GPIO6
 *   SELECT  btn → GPIO9   UP  → GPIO10
 *   DOWN    btn → GPIO3   BACK→ GPIO4
 *   Buzzer (+)  → 100Ω   → GPIO20
 * ================================================================
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "ssd1306_driver.h"
#include "font_5x7.h"
#include "font_digits_large.h"
#include "button_driver.h"
#include "buzzer_driver.h"
#include "rtc_driver.h"
#include "wifi_sync.h"
#include "app_events.h"
#include "app_config.h"

#include "../../../test_config.h"

static const char *TAG = "INTG_TEST";

/* ── Globals shared between tasks ───────────────────────────── */
static ssd1306_handle_t s_oled = NULL;
static QueueHandle_t    s_raw_btn_queue = NULL;
static volatile bool    s_display_inverted = false;
static char             s_last_btn_label[24] = "Press a button...";

/* ── Draw the current watch face onto the OLED ──────────────── */
static void draw_watch_face(void)
{
    watch_time_t t;
    rtc_get_time(&t);

    ssd1306_clear(s_oled);

    /* Status bar */
    char status[32];
    snprintf(status, sizeof(status), "%04d-%02d-%02d",
             t.year, t.month, t.day);
    ssd1306_draw_text(s_oled, 2, 0, status, &font_5x7, true);
    ssd1306_draw_hline(s_oled, 0, 9, 128, true);

    /* Large time digits — centred */
    char time_str[8];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", t.hour, t.minute);
    /* 5 chars × 13px = 65px. Start at (128-65)/2 = 31 */
    ssd1306_draw_text(s_oled, 31, 14, time_str, &font_digits_large, true);

    /* Seconds — small font, right aligned */
    char sec_str[8];
    snprintf(sec_str, sizeof(sec_str), ":%02d", t.second);
    ssd1306_draw_text(s_oled, 100, 34, sec_str, &font_5x7, true);

    /* Bottom divider + last button label */
    ssd1306_draw_hline(s_oled, 0, 54, 128, true);
    ssd1306_draw_text(s_oled, 2, 56, s_last_btn_label, &font_5x7, true);

    ssd1306_flush(s_oled);
}

/* ── Button name helper ──────────────────────────────────────── */
static const char *btn_name(button_id_t id) {
    switch (id) {
        case BTN_SELECT: return "SELECT";
        case BTN_UP:     return "UP";
        case BTN_DOWN:   return "DOWN";
        case BTN_BACK:   return "BACK";
        default:         return "???";
    }
}

/* ── Handle a confirmed button event ────────────────────────── */
static void handle_button(button_id_t id, button_event_type_t type)
{
    if (type == BTN_EVT_LONG_PRESS) {
        ESP_LOGI(TAG, "LONG PRESS: %s", btn_name(id));
        snprintf(s_last_btn_label, sizeof(s_last_btn_label),
                 "LONG: %s", btn_name(id));

        if (id == BTN_SELECT) {
            buzzer_long_press();
            /* Toggle display invert as a visual demo */
            s_display_inverted = !s_display_inverted;
            ssd1306_set_invert(s_oled, s_display_inverted);
        } else {
            buzzer_long_press();
        }
    } else {
        ESP_LOGI(TAG, "PRESS: %s", btn_name(id));
        snprintf(s_last_btn_label, sizeof(s_last_btn_label),
                 "BTN: %s", btn_name(id));

        switch (id) {
            case BTN_SELECT: buzzer_confirm();   break;
            case BTN_UP:     buzzer_nav_up();    break;
            case BTN_DOWN:   buzzer_nav_down();  break;
            case BTN_BACK:   buzzer_cancel();    break;
            default:         buzzer_click();     break;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " Integration Test — All Drivers Together");
    ESP_LOGI(TAG, " Display + Buttons + Buzzer + RTC");
    ESP_LOGI(TAG, "================================================");

    /* ── NVS ─────────────────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ── I2C bus ─────────────────────────────────────────────── */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source               = I2C_CLK_SRC_DEFAULT,
        .i2c_port                 = I2C_NUM_0,
        .scl_io_num               = PIN_I2C_SCL,
        .sda_io_num               = PIN_I2C_SDA,
        .glitch_ignore_cnt        = 7,
        .flags.enable_internal_pullup = false,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    /* ── OLED ────────────────────────────────────────────────── */
    s_oled = ssd1306_init(i2c_bus);
    if (!s_oled) {
        ESP_LOGE(TAG, "ssd1306_init failed");
        return;
    }
    ssd1306_clear(s_oled);
    ssd1306_draw_text(s_oled, 2, 20, "Syncing time...", &font_5x7, true);
    ssd1306_flush(s_oled);

    /* ── Buzzer ──────────────────────────────────────────────── */
    ESP_ERROR_CHECK(buzzer_driver_init());

    /* ── RTC ─────────────────────────────────────────────────── */
    ESP_ERROR_CHECK(rtc_driver_init());

    /* ── WiFi NTP ────────────────────────────────────────────── */
    ret = wifi_sync(TEST_WIFI_SSID, TEST_WIFI_PASS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NTP sync OK");
        buzzer_confirm();   /* Play confirm sound when time is ready */
    } else {
        ESP_LOGW(TAG, "NTP sync failed — running without accurate time");
        /* Manually set a demo time so display is not frozen at epoch */
        rtc_set_time(10, 30, 0, 29, 4, 2026);
        buzzer_cancel();
    }

    /* ── Buttons ─────────────────────────────────────────────── */
    s_raw_btn_queue = xQueueCreate(QUEUE_INPUT_DEPTH, sizeof(button_id_t));
    ESP_ERROR_CHECK(button_driver_init(s_raw_btn_queue));

    /* ── Draw initial watch face ─────────────────────────────── */
    draw_watch_face();
    ESP_LOGI(TAG, "Running. Press buttons to interact.");

    /* ── Main loop ───────────────────────────────────────────── */
    TickType_t last_tick    = xTaskGetTickCount();
    TickType_t last_refresh = last_tick;

    while (1) {
        /* Check for button press (non-blocking peek) */
        button_id_t raw_id;
        if (xQueueReceive(s_raw_btn_queue, &raw_id, pdMS_TO_TICKS(50)) == pdPASS) {

            /* Debounce */
            vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
            static const int pin_map[4] = {
                PIN_BTN_SELECT, PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_BACK
            };
            int pin = pin_map[raw_id];
            if (gpio_get_level(pin) != 0) continue;  /* bounce */

            /* Track hold for long-press */
            uint32_t press_ms = (uint32_t)(esp_timer_get_time() / 1000);
            bool long_fired = false;

            while (gpio_get_level(pin) == 0) {
                vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
                uint32_t held = (uint32_t)(esp_timer_get_time() / 1000) - press_ms;
                if (!long_fired && held >= BTN_LONG_PRESS_MS) {
                    handle_button(raw_id, BTN_EVT_LONG_PRESS);
                    long_fired = true;
                }
            }
            if (!long_fired) {
                handle_button(raw_id, BTN_EVT_PRESS);
            }

            /* Drain stale ISR queue entries */
            button_id_t discard;
            while (xQueueReceive(s_raw_btn_queue, &discard, 0) == pdPASS) {}

            /* Refresh display immediately after button action */
            draw_watch_face();
            last_refresh = xTaskGetTickCount();
        }

        /* Refresh display every second regardless */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_refresh) >= pdMS_TO_TICKS(1000)) {
            draw_watch_face();
            last_refresh = now;
        }
    }
}
