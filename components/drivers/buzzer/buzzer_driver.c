/**
 * ================================================================
 * buzzer_driver.c
 * Passive piezo buzzer driver — Implementation
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 *
 * HOW THIS WORKS
 * --------------
 * The LEDC (LED PWM Controller) peripheral generates a square wave
 * on GPIO20 without any CPU involvement after configuration.
 *
 * To play a tone:
 *   1. Set LEDC timer frequency to the desired Hz
 *   2. Set duty cycle to 50% (BUZZER_DUTY_50PCT)
 *   3. Wait for duration_ms using vTaskDelay()
 *   4. Set duty to 0 (silence)
 *
 * All named sound functions are built from buzzer_play_tone()
 * and buzzer_silence() calls with vTaskDelay() gaps between notes.
 *
 * PROTOTYPE NOTE
 * --------------
 * All sounds use blocking vTaskDelay() between tones. This is
 * acceptable because:
 *   - Sounds are short (< 600 ms total)
 *   - Callers (ui_service, ble_service, alarm_task) can tolerate
 *     the brief blocking since they just played a sound anyway
 *   - For production, replace with esp_timer non-blocking sequences
 * TODO: Non-blocking tone sequencer for production build
 * ================================================================
 */

#include "buzzer_driver.h"
#include "app_config.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "BUZZER";

/* ── Private helpers ────────────────────────────────────────────*/

/**
 * Update the LEDC timer to a new frequency and restart.
 * Called inside buzzer_play_tone() for each note.
 */
static esp_err_t s_set_frequency(uint32_t freq_hz)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode      = BUZZER_LEDC_SPEED_MODE,
        .timer_num       = BUZZER_LEDC_TIMER,
        .duty_resolution = BUZZER_LEDC_RESOLUTION,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    return ledc_timer_config(&timer_conf);
}

/* ================================================================
 * SECTION 1 — LIFECYCLE
 * ================================================================ */

esp_err_t buzzer_driver_init(void)
{
    /* Configure LEDC timer at a default frequency.
     * Actual frequency is set per-tone in buzzer_play_tone(). */
    ledc_timer_config_t timer_conf = {
        .speed_mode      = BUZZER_LEDC_SPEED_MODE,
        .timer_num       = BUZZER_LEDC_TIMER,
        .duty_resolution = BUZZER_LEDC_RESOLUTION,
        .freq_hz         = 1000,    /* Default — overridden per tone */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure LEDC channel — GPIO and duty */
    ledc_channel_config_t channel_conf = {
        .speed_mode = BUZZER_LEDC_SPEED_MODE,
        .channel    = BUZZER_LEDC_CHANNEL,
        .timer_sel  = BUZZER_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PIN_BUZZER,
        .duty       = 0,    /* Silent on start */
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&channel_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ledc_fade_func_install(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_fade_func_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Buzzer driver init OK. GPIO=%d, LEDC CH%d, 13-bit res",
             PIN_BUZZER, BUZZER_LEDC_CHANNEL);
    return ESP_OK;
}

void buzzer_driver_deinit(void)
{
    buzzer_silence();
    ledc_stop(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL, 0);
    gpio_reset_pin(PIN_BUZZER);
    ESP_LOGI(TAG, "Buzzer driver deinit OK");
}

/* ================================================================
 * SECTION 2 — LOW-LEVEL
 * ================================================================ */

esp_err_t buzzer_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    /* Update timer to desired frequency */
    esp_err_t ret = s_set_frequency(freq_hz);
    if (ret != ESP_OK) return ret;

    /* Set 50% duty cycle — maximum volume for passive piezo */
    ret = ledc_set_duty_and_update(BUZZER_LEDC_SPEED_MODE,
                                    BUZZER_LEDC_CHANNEL,
                                    BUZZER_DUTY_50PCT,
                                    0);
    if (ret != ESP_OK) return ret;

    /* Hold for requested duration */
    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    /* Silence */
    buzzer_silence();
    return ESP_OK;
}

void buzzer_silence(void)
{
    ledc_set_duty_and_update(BUZZER_LEDC_SPEED_MODE,
                              BUZZER_LEDC_CHANNEL,
                              0, 0);
}

/* ── Internal gap helper ────────────────────────────────────────
 * Short silence between notes in a multi-tone sound.
 */
static inline void s_gap(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ================================================================
 * SECTION 3 — BUTTON FEEDBACK SOUNDS
 * ================================================================ */

void buzzer_click(void)
{
    /* 1200 Hz, 25 ms — crisp tactile click */
    buzzer_play_tone(1200, 25);
}

void buzzer_long_press(void)
{
    /* 800 Hz → 1200 Hz, 30 ms each — rising two-tone
     * "Something bigger just happened" */
    buzzer_play_tone(800,  30);
    s_gap(20);
    buzzer_play_tone(1200, 30);
}

void buzzer_nav_up(void)
{
    /* 1400 Hz, 20 ms — slightly higher pitch = going up */
    buzzer_play_tone(1400, 20);
}

void buzzer_nav_down(void)
{
    /* 900 Hz, 20 ms — lower pitch = going down */
    buzzer_play_tone(900, 20);
}

void buzzer_confirm(void)
{
    /* 1000 Hz + 1500 Hz, 60 ms each — rising double-tone = success */
    buzzer_play_tone(1000, 60);
    s_gap(20);
    buzzer_play_tone(1500, 60);
}

void buzzer_cancel(void)
{
    /* 600 Hz, 80 ms — single low tone = abort */
    buzzer_play_tone(600, 80);
}

/* ================================================================
 * SECTION 4 — TAPSHARE BLE SOUNDS
 * ================================================================ */

void buzzer_ble_scanning(void)
{
    /* 500 Hz, 40 ms — single quiet pulse.
     * Caller repeats this once per ~1000 ms to create heartbeat. */
    buzzer_play_tone(500, 40);
}

void buzzer_ble_connected(void)
{
    /* 800 → 1200 → 1600 Hz, 50 ms each — bright rising chord.
     * "I found you." Distinct from any button sound. */
    buzzer_play_tone(800,  50);
    s_gap(20);
    buzzer_play_tone(1200, 50);
    s_gap(20);
    buzzer_play_tone(1600, 50);
}

void buzzer_tap_detected(void)
{
    /* 2× (1800 Hz 30 ms + 2200 Hz 30 ms) — double rising chirp.
     * High-frequency, energetic. "The physical tap was confirmed." */
    buzzer_play_tone(1800, 30);
    s_gap(15);
    buzzer_play_tone(2200, 30);
    s_gap(30);
    buzzer_play_tone(1800, 30);
    s_gap(15);
    buzzer_play_tone(2200, 30);
}

void buzzer_data_sent(void)
{
    /* 1600 → 2000 → 2400 Hz, 40 ms each — fast ascending sweep.
     * Data moving outward = pitch rises. */
    buzzer_play_tone(1600, 40);
    s_gap(10);
    buzzer_play_tone(2000, 40);
    s_gap(10);
    buzzer_play_tone(2400, 40);
}

void buzzer_data_received(void)
{
    /* 2400 → 2000 → 1600 Hz, 40 ms each — fast descending sweep.
     * Mirror of data_sent. Data coming in = pitch falls.
     * Together they sound like an exchange. */
    buzzer_play_tone(2400, 40);
    s_gap(10);
    buzzer_play_tone(2000, 40);
    s_gap(10);
    buzzer_play_tone(1600, 40);
}

void buzzer_share_saved(void)
{
    /* 1000 → 1500 → 2000 → 2500 Hz, 35 ms each — triumphant 4-note rise.
     * The most satisfying sound in the system.
     * Used for BOTH manual save and auto-save (no distinction needed). */
    buzzer_play_tone(1000, 35);
    s_gap(15);
    buzzer_play_tone(1500, 35);
    s_gap(15);
    buzzer_play_tone(2000, 35);
    s_gap(15);
    buzzer_play_tone(2500, 45);   /* last note slightly longer */
}

void buzzer_share_discarded(void)
{
    /* 400 Hz, 120 ms — single low thud. Final. No ambiguity. */
    buzzer_play_tone(400, 120);
}

void buzzer_ble_timeout(void)
{
    /* 600 → 400 Hz, 80 ms each — drooping two-tone.
     * Something ended unexpectedly. */
    buzzer_play_tone(600, 80);
    s_gap(20);
    buzzer_play_tone(400, 80);
}

void buzzer_autosave_warning(void)
{
    /* Two quiet ticks at T=5s — "3 seconds left to act."
     * Subtle, not startling. 1200 Hz, 20 ms × 2, 200 ms apart. */
    buzzer_play_tone(1200, 20);
    s_gap(200);
    buzzer_play_tone(1200, 20);
}

/* ================================================================
 * SECTION 5 — ALARM SOUNDS
 * ================================================================ */

void buzzer_alarm_ring(void)
{
    /* 3× (2000 Hz 150 ms + 100 ms silence) + 400 ms final gap.
     * Total: ~850 ms per call. Loop in alarm_task until dismissed.
     * Loud, rhythmic, impossible to ignore. */
    for (int i = 0; i < 3; i++) {
        buzzer_play_tone(2000, 150);
        s_gap(100);
    }
    s_gap(400);  /* Pause between ring cycles */
}

void buzzer_alarm_snooze(void)
{
    /* 1200 → 800 Hz, 80 ms each — descending double.
     * Stepping back = lower pitch. */
    buzzer_play_tone(1200, 80);
    s_gap(20);
    buzzer_play_tone(800, 80);
}

void buzzer_alarm_dismiss(void)
{
    /* 400 Hz, 60 ms — single quiet low beep.
     * Just enough to confirm dismiss was registered. */
    buzzer_play_tone(400, 60);
}
