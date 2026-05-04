/**
 * ================================================================
 * tests/03_buzzer/main/main.c
 * Buzzer Driver Test
 * ================================================================
 * Plays every named sound in sequence with a 1.5 s pause between
 * each. Listen and verify each sound matches the spec.
 *
 * EXPECTED SOUND SEQUENCE (listen and tick each off)
 * ---------------------------------------------------
 * [ ] 1.  click()           — short crisp click (25 ms)
 * [ ] 2.  long_press()      — rising two-tone (800→1200 Hz)
 * [ ] 3.  nav_up()          — brief high tick (1400 Hz)
 * [ ] 4.  nav_down()        — brief low tick (900 Hz)
 * [ ] 5.  confirm()         — double rising tone (1000→1500 Hz)
 * [ ] 6.  cancel()          — single low tone (600 Hz)
 * [ ] 7.  ble_scanning()    — quiet pulse (500 Hz, repeated 3×)
 * [ ] 8.  ble_connected()   — bright chord (800→1200→1600 Hz)
 * [ ] 9.  tap_detected()    — double rising chirp (1800→2200 Hz ×2)
 * [ ] 10. data_sent()       — fast ascending sweep (1600→2000→2400)
 * [ ] 11. data_received()   — fast descending sweep (2400→2000→1600)
 * [ ] 12. share_saved()     — triumphant 4-note rise (the big one)
 * [ ] 13. share_discarded() — single low thud (400 Hz, 120 ms)
 * [ ] 14. ble_timeout()     — drooping two-tone (600→400 Hz)
 * [ ] 15. autosave_warning()— two quiet ticks
 * [ ] 16. alarm_ring()      — 3-beep cycle (2000 Hz × 3), 2 cycles
 * [ ] 17. alarm_snooze()    — descending double (1200→800 Hz)
 * [ ] 18. alarm_dismiss()   — single quiet low beep (400 Hz, 60 ms)
 *
 * WIRING
 * ------
 *   Passive piezo (+) → 100Ω resistor → GPIO20
 *   Passive piezo (-) → GND
 *
 * NOTE: Active buzzers (the ones with electronics inside) will
 * only click at the frequency boundaries. You need a PASSIVE
 * piezo — it should sound like a plain speaker element.
 * ================================================================
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "buzzer_driver.h"

static const char *TAG = "BZR_TEST";

/* Pause between tests — long enough to hear each distinctly */
#define GAP_MS 1500

#define TEST_SOUND(label, call) do {              \
    ESP_LOGI(TAG, "--- %s ---", label);           \
    call;                                          \
    vTaskDelay(pdMS_TO_TICKS(GAP_MS));            \
} while(0)

void app_main(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " Buzzer Driver Test — ESP-IDF 5.4.3");
    ESP_LOGI(TAG, " GPIO%d via 100 ohm series resistor", 20);
    ESP_LOGI(TAG, " Listen and verify each sound matches the spec.");
    ESP_LOGI(TAG, "================================================");

    /* Initialise the buzzer driver */
    esp_err_t ret = buzzer_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "buzzer_driver_init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Buzzer init OK. Starting sound tests in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* ── LOW-LEVEL TEST ──────────────────────────────────────── */
    ESP_LOGI(TAG, "=== LOW-LEVEL: buzzer_play_tone() ===");
    ESP_LOGI(TAG, "440 Hz (concert A) for 500 ms");
    buzzer_play_tone(440, 500);
    vTaskDelay(pdMS_TO_TICKS(GAP_MS));

    ESP_LOGI(TAG, "1000 Hz for 300 ms");
    buzzer_play_tone(1000, 300);
    vTaskDelay(pdMS_TO_TICKS(GAP_MS));

    ESP_LOGI(TAG, "2000 Hz for 300 ms");
    buzzer_play_tone(2000, 300);
    vTaskDelay(pdMS_TO_TICKS(GAP_MS));

    /* ── BUTTON FEEDBACK ─────────────────────────────────────── */
    ESP_LOGI(TAG, "=== BUTTON FEEDBACK SOUNDS ===");
    TEST_SOUND("1. click()       — crisp short click",         buzzer_click());
    TEST_SOUND("2. long_press()  — rising two-tone",           buzzer_long_press());
    TEST_SOUND("3. nav_up()      — brief high tick",           buzzer_nav_up());
    TEST_SOUND("4. nav_down()    — brief low tick",            buzzer_nav_down());
    TEST_SOUND("5. confirm()     — rising double-beep",        buzzer_confirm());
    TEST_SOUND("6. cancel()      — single low tone",           buzzer_cancel());

    /* ── TAPSHARE BLE SEQUENCE ───────────────────────────────── */
    ESP_LOGI(TAG, "=== TAPSHARE BLE SOUNDS ===");

    /* Scanning pulse — repeat 3× to simulate the heartbeat effect */
    ESP_LOGI(TAG, "--- 7. ble_scanning() x3 — heartbeat pulse ---");
    for (int i = 0; i < 3; i++) {
        buzzer_ble_scanning();
        vTaskDelay(pdMS_TO_TICKS(960));  /* ~1 pulse per second */
    }
    vTaskDelay(pdMS_TO_TICKS(GAP_MS));

    TEST_SOUND("8.  ble_connected()    — bright rising chord",     buzzer_ble_connected());
    TEST_SOUND("9.  tap_detected()     — double chirp",            buzzer_tap_detected());
    TEST_SOUND("10. data_sent()        — ascending sweep",         buzzer_data_sent());
    TEST_SOUND("11. data_received()    — descending sweep",        buzzer_data_received());
    TEST_SOUND("12. share_saved()      — TRIUMPHANT 4-note rise!", buzzer_share_saved());
    TEST_SOUND("13. share_discarded()  — low thud",                buzzer_share_discarded());
    TEST_SOUND("14. ble_timeout()      — drooping two-tone",       buzzer_ble_timeout());
    TEST_SOUND("15. autosave_warning() — two quiet ticks",         buzzer_autosave_warning());

    /* ── AUTO-SAVE SEQUENCE DEMO ─────────────────────────────── */
    ESP_LOGI(TAG, "=== AUTO-SAVE SEQUENCE (5s warning → 3s → save) ===");
    ESP_LOGI(TAG, "Simulating: data received → 5s → warning → 3s → auto-save");
    buzzer_data_received();
    vTaskDelay(pdMS_TO_TICKS(5000));   /* 5 s silence (user viewing contact) */
    buzzer_autosave_warning();         /* T=5s warning */
    vTaskDelay(pdMS_TO_TICKS(3000));   /* 3 s more */
    buzzer_share_saved();              /* T=8s auto-save */
    vTaskDelay(pdMS_TO_TICKS(GAP_MS));

    /* ── ALARM SOUNDS ────────────────────────────────────────── */
    ESP_LOGI(TAG, "=== ALARM SOUNDS ===");

    /* Ring 2 full cycles to show the repeating pattern */
    ESP_LOGI(TAG, "--- 16. alarm_ring() x2 — full alarm loop demo ---");
    buzzer_alarm_ring();
    buzzer_alarm_ring();
    vTaskDelay(pdMS_TO_TICKS(GAP_MS));

    TEST_SOUND("17. alarm_snooze()  — descending double",  buzzer_alarm_snooze());
    TEST_SOUND("18. alarm_dismiss() — quiet low confirm",  buzzer_alarm_dismiss());

    /* ── DONE ────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " All 18 sounds played.");
    ESP_LOGI(TAG, " If any sound was missing or wrong, check:");
    ESP_LOGI(TAG, "   - Passive (not active) piezo connected");
    ESP_LOGI(TAG, "   - 100 ohm series resistor on GPIO%d", 20);
    ESP_LOGI(TAG, "   - LEDC_CHANNEL_0 not in use by another peripheral");
    ESP_LOGI(TAG, "================================================");

    /* Repeat the TapShare full sequence every 30s for extended testing */
    int cycle = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        cycle++;
        ESP_LOGI(TAG, "Repeat cycle %d — TapShare sequence", cycle);
        vTaskDelay(pdMS_TO_TICKS(500));
        buzzer_ble_connected();
        vTaskDelay(pdMS_TO_TICKS(1000));
        buzzer_tap_detected();
        vTaskDelay(pdMS_TO_TICKS(500));
        buzzer_data_sent();
        vTaskDelay(pdMS_TO_TICKS(300));
        buzzer_data_received();
        vTaskDelay(pdMS_TO_TICKS(500));
        buzzer_share_saved();
    }
}
