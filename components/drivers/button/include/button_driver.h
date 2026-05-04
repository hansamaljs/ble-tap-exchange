/**
 * ================================================================
 * button_driver.h
 * Four-button GPIO input driver
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 *
 * WHAT THIS DRIVER DOES
 * ---------------------
 * - Configures four GPIO inputs with internal pull-up resistors
 * - Installs ISR handlers that fire on falling edges (button press)
 * - ISR sends the raw button_id into a FreeRTOS queue immediately
 * - Debounce and long-press detection happen in input_task (tasks.c),
 *   NOT in this driver — ISRs stay tiny
 *
 * WHAT THIS DRIVER DOES NOT DO
 * ----------------------------
 * - Does not know what any button means in any screen context
 * - Does not handle long-press timing (input_task does that)
 * - Does not know about menus, alarms, stopwatches, or BLE
 *
 * USAGE PATTERN
 * -------------
 *   // In input_task:
 *   button_driver_init(g_input_queue);
 *
 *   button_id_t raw_id;
 *   while (1) {
 *       xQueueReceive(g_input_queue, &raw_id, portMAX_DELAY);
 *       // debounce + long-press logic here in the task
 *   }
 *
 * WARNING — GPIO9 (SELECT) is the ESP32-C3 boot/STRAP pin.
 * Do NOT hold SELECT pressed while powering on or pressing RESET.
 * Doing so will enter ROM download mode instead of starting firmware.
 * Normal button use after boot is completely safe.
 * ================================================================
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_events.h"

/**
 * @brief  Initialise all four button GPIOs and attach ISR handlers.
 *
 * Configures GPIOs as inputs with internal pull-up resistors enabled.
 * Installs a falling-edge ISR on each pin. When a button is pressed,
 * the ISR sends a button_id_t into output_queue.
 *
 * @param  output_queue  Queue to receive button_id_t values.
 *                       Must be created before calling this.
 *                       Typically g_input_queue from tasks.h.
 * @return ESP_OK on success, or an esp_err_t code on failure.
 *
 * @note   Call once from input_task before entering the task loop.
 */
esp_err_t button_driver_init(QueueHandle_t output_queue);

/**
 * @brief  Remove ISR handlers and reset GPIOs.
 *         Call during shutdown or when reinitialising.
 */
void button_driver_deinit(void);
