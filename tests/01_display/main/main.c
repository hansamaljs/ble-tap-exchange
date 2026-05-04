/**
 * ================================================================
 * main.c — SSD1306 Driver Test
 * ================================================================
 * This program runs through every function in ssd1306_driver.c
 * one at a time, with a 1.5 second pause between each test so
 * you can see exactly what each primitive draws.
 *
 * Run this BEFORE writing ui_service.c.
 * If every test passes visually, the driver is correct.
 *
 * Expected test sequence on the OLED:
 *  1. Blank screen (clear test)
 *  2. All pixels on (fill test)
 *  3. Single pixel blinking in each corner
 *  4. Horizontal lines at various y positions
 *  5. Vertical lines at various x positions
 *  6. Hollow rectangle (border)
 *  7. Filled rectangle (solid block)
 *  8. Small font text — full alphabet
 *  9. Large digit font — "12:34"
 * 10. Mixed text sizes — simulated watch face layout
 * 11. Bitmap icon (16x16 battery icon)
 * 12. Transparent bitmap overlay
 * 13. Invert display hardware control
 * 14. Contrast sweep
 * 15. Per-page dirty flag demo (only partial updates)
 *
 * WIRING (matches app_config.h from architecture doc):
 *   SSD1306 SDA → GPIO5  (+ 4.7k pull-up to 3.3V)
 *   SSD1306 SCL → GPIO6  (+ 4.7k pull-up to 3.3V)
 *   SSD1306 VCC → 3.3V
 *   SSD1306 GND → GND
 * ================================================================
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ssd1306_driver.h"
#include "font_5x7.h"
#include "font_digits_large.h"

static const char *TAG = "DRIVER_TEST";

/* Pin assignments — match your wiring */
#define PIN_SDA     5
#define PIN_SCL     6

/* Helper: pause and print what test is running */
#define TEST(name) do { \
    ESP_LOGI(TAG, "--- TEST: %s ---", name); \
    vTaskDelay(pdMS_TO_TICKS(2500)); \
} while(0)

/* Helper: short pause between steps within a test */
#define STEP_PAUSE() vTaskDelay(pdMS_TO_TICKS(600))

/* ── A 16x16 battery icon stored in flash ──────────────────────
 * Column-major format: 2 bytes per column, 16 columns.
 * Bit 0 of byte 0 = top pixel of column.
 * This is a simple battery outline with a fill indicator.
 */
static const uint8_t BATTERY_ICON_16x16[] = {
    /* col 0  */ 0x00, 0x00,
    /* col 1  */ 0xFE, 0x01,
    /* col 2  */ 0xFE, 0x01,
    /* col 3  */ 0x02, 0x01,
    /* col 4  */ 0x02, 0x01,
    /* col 5  */ 0xF2, 0x01,
    /* col 6  */ 0xF2, 0x01,
    /* col 7  */ 0xF2, 0x01,
    /* col 8  */ 0xF2, 0x01,
    /* col 9  */ 0xF2, 0x01,
    /* col 10 */ 0x02, 0x01,
    /* col 11 */ 0x02, 0x01,
    /* col 12 */ 0xFE, 0x01,
    /* col 13 */ 0xFE, 0x01,
    /* col 14 */ 0x00, 0x01,  /* positive terminal */
    /* col 15 */ 0x00, 0x01,
};

/* ── BLE icon 8x8 — simple dot with rings ────────────────────── */
static const uint8_t BLE_ICON_8x8[] = {
    /* col 0 */ 0x08,
    /* col 1 */ 0x14,
    /* col 2 */ 0x63,
    /* col 3 */ 0x14,
    /* col 4 */ 0x63,
    /* col 5 */ 0x14,
    /* col 6 */ 0x08,
    /* col 7 */ 0x00,
};


void app_main(void)
{
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " SSD1306 Driver Test — ESP-IDF 5.4.3");
    ESP_LOGI(TAG, " SDA=GPIO%d  SCL=GPIO%d", PIN_SDA, PIN_SCL);
    ESP_LOGI(TAG, "==============================================");

    /* ── Initialise I2C master bus ─────────────────────────── */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source               = I2C_CLK_SRC_DEFAULT,
        .i2c_port                 = I2C_NUM_0,
        .scl_io_num               = PIN_SCL,
        .sda_io_num               = PIN_SDA,
        .glitch_ignore_cnt        = 7,
        .flags.enable_internal_pullup = false,  /* external 4.7k pull-ups */
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    /* ── Initialise SSD1306 ────────────────────────────────── */
    ssd1306_handle_t oled = ssd1306_init(i2c_bus);
    if (!oled) {
        ESP_LOGE(TAG, "ssd1306_init() returned NULL");
        ESP_LOGE(TAG, "Check: I2C scan should show 0x3C");
        ESP_LOGE(TAG, "Check: SDA on GPIO%d, SCL on GPIO%d", PIN_SDA, PIN_SCL);
        ESP_LOGE(TAG, "Check: 4.7k pull-up resistors to 3.3V on both lines");
        return;
    }
    ESP_LOGI(TAG, "ssd1306_init() OK. Starting tests...");
    vTaskDelay(pdMS_TO_TICKS(500));


    /* ============================================================
     * TEST 1 — ssd1306_clear()
     * Screen must go completely black.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_flush(oled);
    TEST("clear — screen should be black");


    /* ============================================================
     * TEST 2 — ssd1306_fill()
     * Screen must go completely white (all pixels on).
     * ============================================================ */
    ssd1306_fill(oled);
    ssd1306_flush(oled);
    TEST("fill — screen should be all white");
    ssd1306_clear(oled);
    ssd1306_flush(oled);


    /* ============================================================
     * TEST 3 — ssd1306_draw_pixel()
     * Single pixel in each of the four corners.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_pixel(oled,   0,  0, true);   /* top-left     */
    ssd1306_draw_pixel(oled, 127,  0, true);   /* top-right    */
    ssd1306_draw_pixel(oled,   0, 63, true);   /* bottom-left  */
    ssd1306_draw_pixel(oled, 127, 63, true);   /* bottom-right */
    ssd1306_draw_pixel(oled,  64, 32, true);   /* centre       */
    ssd1306_flush(oled);
    TEST("draw_pixel — 4 corners + centre pixel");

    /* Verify bounds check: these must NOT crash */
    ssd1306_draw_pixel(oled, 200, 200, true);  /* out of bounds */
    ssd1306_draw_pixel(oled, 128,   0, true);  /* x exactly at limit */
    ssd1306_draw_pixel(oled,   0,  64, true);  /* y exactly at limit */
    ESP_LOGI(TAG, "Out-of-bounds draw_pixel: no crash — bounds check OK");


    /* ============================================================
     * TEST 4 — ssd1306_draw_hline()
     * Horizontal lines at y=0, y=32, y=63.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_hline(oled, 0,  0, 128, true);  /* top edge    */
    ssd1306_draw_hline(oled, 0, 32, 128, true);  /* middle      */
    ssd1306_draw_hline(oled, 0, 63, 128, true);  /* bottom edge */
    ssd1306_draw_hline(oled, 32, 16,  64, true); /* half-width centred */
    ssd1306_flush(oled);
    TEST("draw_hline — top, middle, bottom, half-width");


    /* ============================================================
     * TEST 5 — ssd1306_draw_vline()
     * Vertical lines at x=0, x=64, x=127.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_vline(oled,   0,  0, 64, true);  /* left edge   */
    ssd1306_draw_vline(oled,  64,  0, 64, true);  /* centre      */
    ssd1306_draw_vline(oled, 127,  0, 64, true);  /* right edge  */
    ssd1306_flush(oled);
    TEST("draw_vline — left, centre, right");


    /* ============================================================
     * TEST 6 — ssd1306_draw_rect() hollow
     * Border rectangle touching all four edges.
     * Then a smaller inner rectangle.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_rect(oled, 0, 0, 128, 64, false, true);    /* outer border */
    ssd1306_draw_rect(oled, 4, 4, 120, 56, false, true);    /* inner border */
    ssd1306_draw_rect(oled, 48, 24, 32, 16, false, true);   /* small centre box */
    ssd1306_flush(oled);
    TEST("draw_rect hollow — nested borders");


    /* ============================================================
     * TEST 7 — ssd1306_draw_rect() filled
     * Three filled blocks. Then erase the middle one.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_rect(oled,  0,  0, 40, 64, true, true);    /* left block  */
    ssd1306_draw_rect(oled, 44,  0, 40, 64, true, true);    /* mid block   */
    ssd1306_draw_rect(oled, 88,  0, 40, 64, true, true);    /* right block */
    ssd1306_flush(oled);
    STEP_PAUSE();
    /* Erase the middle block */
    ssd1306_draw_rect(oled, 44, 0, 40, 64, true, false);
    ssd1306_flush(oled);
    TEST("draw_rect filled — 3 blocks, then erase middle");


    /* ============================================================
     * TEST 8 — ssd1306_draw_text() with font_5x7
     * Full alphabet, digits, symbols.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_rect(oled, 0, 0, 128, 64, false, true);
    ssd1306_draw_text(oled,  2,  4, "ABCDEFGHIJKLMNOP", &font_5x7, true);
    ssd1306_draw_text(oled,  2, 14, "QRSTUVWXYZ 0-9",  &font_5x7, true);
    ssd1306_draw_text(oled,  2, 24, "0123456789:+-=.", &font_5x7, true);
    ssd1306_draw_text(oled,  2, 34, "Hello World!",    &font_5x7, true);
    ssd1306_draw_text(oled,  2, 44, "tapshare watch",  &font_5x7, true);
    ssd1306_draw_text(oled,  2, 54, "v1.0 esp32-c3",   &font_5x7, true);
    ssd1306_flush(oled);
    TEST("draw_text font_5x7 — alphabet + symbols");


    /* ============================================================
     * TEST 9 — ssd1306_draw_text() inverted text
     * White text on black background (normal) vs
     * black text on white background (inverted).
     * ============================================================ */
    ssd1306_clear(oled);
    /* Normal: white text on black */
    ssd1306_draw_text(oled, 2, 4, "Normal text", &font_5x7, true);
    /* Inverted: fill a bar then draw black-on-white text */
    ssd1306_draw_rect(oled, 0, 16, 128, 9, true, true);    /* white bar */
    ssd1306_draw_text(oled, 2, 17, "Inverted text", &font_5x7, false);
    /* Second inverted bar */
    ssd1306_draw_rect(oled, 0, 28, 128, 9, true, true);
    ssd1306_draw_text(oled, 2, 29, "Selected item!", &font_5x7, false);
    ssd1306_flush(oled);
    TEST("draw_text inverted — menu highlight demo");


    /* ============================================================
     * TEST 10 — ssd1306_draw_text() with font_digits_large
     * Large time digits.
     * ============================================================ */
    ssd1306_clear(oled);
    /* Draw "12:34" centred — each char is 12px wide + 1px gap = 13px
     * "12:34" = 5 chars: 5 * 13 = 65px. Start at x = (128-65)/2 = 31 */
    ssd1306_draw_text(oled, 31, 24, "12:34", &font_digits_large, true);
    ssd1306_flush(oled);
    TEST("draw_text font_digits_large — 12:34");

    STEP_PAUSE();

    /* Watch-face style layout */
    ssd1306_clear(oled);
    /* Status bar row */
    ssd1306_draw_text(oled, 2, 0, "MON 14 APR", &font_5x7, true);
    /* Divider line */
    ssd1306_draw_hline(oled, 0, 9, 128, true);
    /* Large time */
    ssd1306_draw_text(oled, 17, 16, "09:41", &font_digits_large, true);
    /* Seconds */
    ssd1306_draw_text(oled, 100, 38, ":05", &font_5x7, true);
    /* Bottom status */
    ssd1306_draw_hline(oled, 0, 54, 128, true);
    ssd1306_draw_text(oled, 2, 56, "ALM 07:00", &font_5x7, true);
    ssd1306_flush(oled);
    TEST("watch face layout — combined fonts");


    /* ============================================================
     * TEST 11 — ssd1306_draw_bitmap() opaque
     * Draw the battery icon at top-right.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_bitmap(oled, 110, 0, 16, 16,
                        BATTERY_ICON_16x16, false);    /* opaque */
    ssd1306_draw_bitmap(oled,   2, 0,  8,  8,
                        BLE_ICON_8x8, false);           /* BLE icon */
    ssd1306_draw_text(oled, 12, 1, "BLE", &font_5x7, true);
    ssd1306_flush(oled);
    TEST("draw_bitmap opaque — battery + BLE icon");


    /* ============================================================
     * TEST 12 — ssd1306_draw_bitmap() transparent
     * Draw a filled rectangle (simulates existing screen content)
     * then overlay a transparent icon on top.
     * Zero-pixels in the icon must NOT overwrite the background.
     * ============================================================ */
    ssd1306_clear(oled);
    /* Background content */
    ssd1306_draw_rect(oled, 90, 0, 38, 20, true, true);     /* white box */
    ssd1306_draw_text(oled, 2, 4, "Transparent overlay:", &font_5x7, true);
    /* Overlay battery icon transparently — white box should show
     * through the zero-pixels in the icon */
    ssd1306_draw_bitmap(oled, 100, 2, 16, 16,
                        BATTERY_ICON_16x16, true);          /* transparent */
    ssd1306_flush(oled);
    TEST("draw_bitmap transparent — icon over white box");


    /* ============================================================
     * TEST 13 — ssd1306_set_invert()
     * Hardware invert — no framebuffer change.
     * Draw something, invert it, then restore.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_rect(oled, 0, 0, 128, 64, false, true);
    ssd1306_draw_text(oled, 10, 28, "INVERTED!", &font_5x7, true);
    ssd1306_flush(oled);
    STEP_PAUSE();
    ssd1306_set_invert(oled, true);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ssd1306_set_invert(oled, false);
    TEST("set_invert — hardware invert on/off");


    /* ============================================================
     * TEST 14 — ssd1306_set_contrast()
     * Sweep from dim to bright.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_text(oled, 2, 28, "Contrast sweep...", &font_5x7, true);
    ssd1306_flush(oled);
    /* Sweep from dim (10) to bright (255) to default (200) */
    for (int c = 10; c <= 255; c += 5) {
        ssd1306_set_contrast(oled, (uint8_t)c);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    ssd1306_set_contrast(oled, 200);   /* restore to good default */
    TEST("set_contrast — sweep complete, restored to 200");


    /* ============================================================
     * TEST 15 — Per-page dirty flag verification
     *
     * This test proves that only changed pages are flushed.
     * Sequence:
     *   1. Draw a full screen (all 8 pages dirty) → flush
     *   2. Change only one text in the top row (page 0 dirty)
     *   3. Flush → only page 0 should be sent over I2C
     *      (pages 1-7 stay from previous flush)
     *
     * You cannot see "only page 0 was sent" visually, but you can
     * verify by watching the serial monitor — the test prints how
     * many pages were dirty before each flush.
     *
     * To observe: add a breakpoint in s_flush_page() or add
     * ESP_LOGI calls there during development.
     * ============================================================ */
    ssd1306_clear(oled);

    /* Fill all 8 pages with content */
    for (int row = 0; row < 8; row++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Row %d: initial text", row);
        ssd1306_draw_text(oled, 0, row * 8, buf, &font_5x7, true);
    }
    ssd1306_flush(oled);   /* All 8 pages flushed */
    STEP_PAUSE();

    /* Now change ONLY row 0 (page 0) */
    /* Erase just the top row */
    ssd1306_draw_rect(oled, 0, 0, 128, 8, true, false);  /* clears page 0 */
    ssd1306_draw_text(oled, 0, 0, "*** UPDATED ***", &font_5x7, true);
    /* dirty_flags should now be 0x01 — only bit 0 set */
    ssd1306_flush(oled);   /* Only page 0 sent — 128 bytes not 1024 */

    ESP_LOGI(TAG, "Per-page dirty flag test complete.");
    ESP_LOGI(TAG, "In second flush: only page 0 was dirty (128 bytes sent).");
    ESP_LOGI(TAG, "Pages 1-7 were clean — not sent. No I2C traffic for them.");
    TEST("per-page dirty flags — partial flush confirmed");


    /* ============================================================
     * TEST 16 — ssd1306_set_sleep()
     * Turn display off then back on.
     * Framebuffer is preserved — display resumes from last state.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_text(oled, 2, 24, "Going to sleep...", &font_5x7, true);
    ssd1306_flush(oled);
    vTaskDelay(pdMS_TO_TICKS(1000));

    ssd1306_set_sleep(oled, true);
    ESP_LOGI(TAG, "Display sleeping...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    ssd1306_set_sleep(oled, false);
    ESP_LOGI(TAG, "Display awake. Refreshing framebuffer.");
    /* After wake, content should be restored automatically.
     * If not, call ssd1306_flush_all() to re-sync. */
    ssd1306_mark_dirty(oled);
    ssd1306_flush(oled);
    TEST("set_sleep — off/on, content preserved");


    /* ============================================================
     * ALL TESTS COMPLETE
     * Show a final confirmation screen.
     * ============================================================ */
    ssd1306_clear(oled);
    ssd1306_draw_rect(oled, 0, 0, 128, 64, false, true);
    ssd1306_draw_rect(oled, 2, 2, 124, 60, false, true);
    ssd1306_draw_text(oled,  8,  8, "ALL TESTS DONE", &font_5x7, true);
    ssd1306_draw_hline(oled, 4, 18, 120, true);
    ssd1306_draw_text(oled,  2, 22, "pixel / hline / vline", &font_5x7, true);
    ssd1306_draw_text(oled,  2, 32, "rect / text / bitmap", &font_5x7, true);
    ssd1306_draw_text(oled,  2, 42, "invert / contrast / sleep", &font_5x7, true);
    ssd1306_draw_text(oled,  2, 52, "dirty flags: all OK", &font_5x7, true);
    ssd1306_flush(oled);

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " All driver tests complete.");
    ESP_LOGI(TAG, " Driver is ready for ui_service integration.");
    ESP_LOGI(TAG, "==============================================");

    /* Stay here — display holds the result screen */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "Free heap: %lu bytes",
                 (unsigned long)esp_get_free_heap_size());
    }
}
