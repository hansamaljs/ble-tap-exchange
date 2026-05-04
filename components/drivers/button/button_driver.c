/**
 * ================================================================
 * button_driver.c
 * Four-button GPIO input driver — Implementation
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 *
 * HOW THIS WORKS
 * --------------
 * 1. button_driver_init() configures four GPIOs as inputs with
 *    internal pull-ups. Buttons connect pin to GND when pressed
 *    (active-low). Falling edge = button pressed.
 *
 * 2. One ISR handler per button. ISR is kept MINIMAL — it only
 *    reads which button fired, grabs a timestamp, and sends a
 *    button_id_t to the output queue. Zero logic in ISR.
 *
 * 3. Debounce and long-press detection live in input_task (tasks.c).
 *    That runs in normal task context where vTaskDelay() is safe.
 *
 * ISR RULES (DO NOT VIOLATE)
 * --------------------------
 *   - ISR marked IRAM_ATTR — must live in RAM, not flash
 *   - Only use *FromISR() variants of FreeRTOS calls
 *   - No ESP_LOG, no malloc, no blocking calls
 *   - Keep it under ~20 instructions
 * ================================================================
 */

#include "button_driver.h"
#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BTN";

/* ── Private state ──────────────────────────────────────────────
 * These are set once in button_driver_init() and read from ISRs.
 * Declared volatile because ISRs read them from interrupt context.
 */
static volatile QueueHandle_t s_output_queue = NULL;

/* Map of button index → GPIO pin number.
 * Indexed by button_id_t: BTN_SELECT=0, BTN_UP=1, BTN_DOWN=2, BTN_BACK=3
 */
static const int s_btn_pins[4] = {
    PIN_BTN_SELECT,   /* BTN_SELECT = 0 */
    PIN_BTN_UP,       /* BTN_UP     = 1 */
    PIN_BTN_DOWN,     /* BTN_DOWN   = 2 */
    PIN_BTN_BACK,     /* BTN_BACK   = 3 */
};

/* ── ISR handler ────────────────────────────────────────────────
 * One ISR handles all four buttons.
 * arg is set to (void*)(uintptr_t)button_id at gpio_isr_handler_add().
 *
 * CRITICAL: This function runs in interrupt context.
 * Keep it as short as physically possible.
 */
static void IRAM_ATTR btn_isr_handler(void *arg)
{
    button_id_t btn_id = (button_id_t)(uintptr_t)arg;

    BaseType_t higher_prio_woken = pdFALSE;
    xQueueSendFromISR(s_output_queue, &btn_id, &higher_prio_woken);

    /* If a higher-priority task was waiting for this queue item,
     * yield to it immediately instead of returning to whatever
     * the CPU was doing when the interrupt fired. */
    portYIELD_FROM_ISR(higher_prio_woken);
}

/* ── Public API ─────────────────────────────────────────────────*/

esp_err_t button_driver_init(QueueHandle_t output_queue)
{
    if (!output_queue) {
        ESP_LOGE(TAG, "output_queue is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    s_output_queue = output_queue;

    /* ── 1. Configure all four GPIOs ─────────────────────────── */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_BTN_SELECT) |
                        (1ULL << PIN_BTN_UP)     |
                        (1ULL << PIN_BTN_DOWN)   |
                        (1ULL << PIN_BTN_BACK),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* Internal pull-up: idle = HIGH */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,      /* Falling edge = button press */
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ── 2. Install the GPIO ISR service ─────────────────────── */
    /* gpio_install_isr_service() must be called once before any
     * gpio_isr_handler_add(). ESP_ERR_INVALID_STATE means it is
     * already installed — that is fine, not an error. */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ── 3. Attach one ISR per button ────────────────────────── */
    for (int i = 0; i < 4; i++) {
        ret = gpio_isr_handler_add(s_btn_pins[i],
                                   btn_isr_handler,
                                   (void *)(uintptr_t)i);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "gpio_isr_handler_add GPIO%d failed: %s",
                     s_btn_pins[i], esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "Button driver init OK");
    ESP_LOGI(TAG, "  SELECT=GPIO%d  UP=GPIO%d  DOWN=GPIO%d  BACK=GPIO%d",
             PIN_BTN_SELECT, PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_BACK);
    ESP_LOGI(TAG, "  Debounce=%dms  LongPress=%dms (handled in input_task)",
             BTN_DEBOUNCE_MS, BTN_LONG_PRESS_MS);

    return ESP_OK;
}

void button_driver_deinit(void)
{
    for (int i = 0; i < 4; i++) {
        gpio_isr_handler_remove(s_btn_pins[i]);
        gpio_reset_pin(s_btn_pins[i]);
    }
    s_output_queue = NULL;
    ESP_LOGI(TAG, "Button driver deinit OK");
}
