/**
 * ================================================================
 * tests/02_button/main/main.c
 * Button Driver Test
 * ================================================================
 * Tests all four buttons. For each button press, prints:
 *   - Which button
 *   - Whether it was a short press or long press
 *   - Time held in ms
 *
 * HOW TO READ THE OUTPUT
 * ----------------------
 * Serial monitor (idf.py monitor) will show:
 *   I (1234) BTN_TEST: SELECT SHORT PRESS (held 120 ms)
 *   I (1234) BTN_TEST: SELECT LONG PRESS  (held 812 ms)
 *   I (1234) BTN_TEST: UP    SHORT PRESS  (held 58 ms)
 *   etc.
 *
 * TEST CHECKLIST
 * --------------
 * [ ] All 4 buttons generate events when pressed
 * [ ] Short press fires BTN_EVT_PRESS (release before 800 ms)
 * [ ] Long press fires BTN_EVT_LONG_PRESS (held >= 800 ms)
 * [ ] BTN_EVT_PRESS is NOT also fired after a long press
 * [ ] No phantom events (button held down should not repeat)
 * [ ] Rapid pressing multiple buttons does not cause crashes
 * [ ] SELECT button works safely (GPIO9 boot pin)
 *
 * WIRING (matches app_config.h)
 * ------------------------------
 *   SELECT button: GPIO9  → GND when pressed
 *   UP     button: GPIO10 → GND when pressed
 *   DOWN   button: GPIO3  → GND when pressed
 *   BACK   button: GPIO4  → GND when pressed
 *   (Internal pull-ups enabled — no external resistors needed)
 * ================================================================
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "button_driver.h"
#include "app_events.h"
#include "app_config.h"

static const char *TAG = "BTN_TEST";

/* Queue for raw button_id_t from the ISR */
static QueueHandle_t s_raw_queue = NULL;

/* Human-readable button names */
static const char *btn_name(button_id_t id) {
    switch (id) {
        case BTN_SELECT: return "SELECT";
        case BTN_UP:     return "UP    ";
        case BTN_DOWN:   return "DOWN  ";
        case BTN_BACK:   return "BACK  ";
        default:         return "UNKNWN";
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " Button Driver Test — ESP-IDF 5.4.3");
    ESP_LOGI(TAG, " Debounce=%dms  LongPress=%dms",
             BTN_DEBOUNCE_MS, BTN_LONG_PRESS_MS);
    ESP_LOGI(TAG, " Press each button. Watch for PRESS vs LONG_PRESS.");
    ESP_LOGI(TAG, "================================================");

    /* Create the raw queue — ISR sends button_id_t here */
    s_raw_queue = xQueueCreate(QUEUE_INPUT_DEPTH, sizeof(button_id_t));
    if (!s_raw_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return;
    }

    /* Init the button driver — attaches ISR handlers */
    esp_err_t ret = button_driver_init(s_raw_queue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "button_driver_init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Button driver init OK. Press any button...");

    /* ── Main loop: debounce + long-press detection ─────────── */
    while (1) {
        button_id_t raw_id;

        /* Block until an ISR sends a button ID (falling edge detected) */
        if (xQueueReceive(s_raw_queue, &raw_id, portMAX_DELAY) != pdPASS) {
            continue;
        }

        /* ── Step 1: Debounce ─────────────────────────────────
         * Wait BTN_DEBOUNCE_MS then re-read the GPIO.
         * If still low, the press is real. If high, it was bounce.
         */
        vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));

        static const int pin_map[4] = {
            PIN_BTN_SELECT, PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_BACK
        };
        int pin = pin_map[raw_id];

        if (gpio_get_level(pin) != 0) {
            /* Pin is back HIGH — it was contact bounce, not a real press */
            ESP_LOGD(TAG, "%s bounce ignored", btn_name(raw_id));
            continue;
        }

        /* Real press confirmed. Record start time. */
        uint32_t press_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool long_press_fired = false;

        /* ── Step 2: Track hold time ──────────────────────────
         * Poll every BTN_POLL_MS. When pin goes high, button released.
         * If still low at BTN_LONG_PRESS_MS, fire LONG_PRESS immediately.
         */
        while (gpio_get_level(pin) == 0) {
            vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));

            uint32_t now_ms     = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t held_ms    = now_ms - press_start_ms;

            if (!long_press_fired && held_ms >= BTN_LONG_PRESS_MS) {
                /* Long press threshold reached — fire immediately */
                ESP_LOGI(TAG, "%s LONG PRESS  (held %lu ms)",
                         btn_name(raw_id), (unsigned long)held_ms);
                long_press_fired = true;
                /* Don't break — wait for release to send RELEASE event */
            }
        }

        /* Button released */
        uint32_t released_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t total_held  = released_ms - press_start_ms;

        if (!long_press_fired) {
            /* Short press — fire PRESS on release */
            ESP_LOGI(TAG, "%s SHORT PRESS (held %lu ms)",
                     btn_name(raw_id), (unsigned long)total_held);
        }

        /* Always log RELEASE event */
        ESP_LOGD(TAG, "%s RELEASE", btn_name(raw_id));

        /* Drain any ISR queue entries that arrived while we were polling
         * (e.g. bounce pulses during hold) */
        button_id_t discard;
        while (xQueueReceive(s_raw_queue, &discard, 0) == pdPASS) {
            ESP_LOGD(TAG, "Discarded stale queue entry during hold");
        }
    }
}
