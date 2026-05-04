/**
 * ================================================================
 * ssd1306_driver.h
 * SSD1306 0.96" I2C OLED Display Driver
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 * Display : SSD1306 128x64 monochrome OLED, I2C address 0x3C
 *
 * WHAT THIS DRIVER DOES
 * ---------------------
 * Provides pixel-level drawing primitives and framebuffer management.
 * All draw functions write into a RAM framebuffer only.
 * ssd1306_flush() is the ONLY function that communicates with the
 * display hardware over I2C. Call it once after all your drawing.
 *
 * WHAT THIS DRIVER DOES NOT DO
 * -----------------------------
 * - Does not know about watch faces, menus, or animations
 * - Does not own or manage fonts (caller passes font pointer)
 * - Does not schedule redraws or handle events
 * - Does not know what the pixels represent
 *
 * USAGE PATTERN
 * -------------
 *   ssd1306_handle_t oled = ssd1306_init(i2c_bus);
 *
 *   ssd1306_clear(oled);
 *   ssd1306_draw_rect(oled, 0, 0, 128, 64, false);
 *   ssd1306_draw_text(oled, 10, 20, "Hello", &font_5x7);
 *   ssd1306_flush(oled);     // <-- one I2C transfer, done
 *
 * MEMORY
 * ------
 * Framebuffer: 1,024 bytes (128 cols x 8 pages, 1 bit per pixel)
 * Dirty flags : 1 byte (one bit per page)
 * Handle struct: ~24 bytes
 * Total RAM   : ~1,050 bytes
 * ================================================================
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* ================================================================
 * Display dimensions — do not change these
 * ================================================================ */
#define SSD1306_WIDTH        128
#define SSD1306_HEIGHT        64
#define SSD1306_PAGES          8    /* 64 / 8 bits = 8 pages       */
#define SSD1306_I2C_ADDR    0x3C   /* Default address, ADDR pin=GND */

/* ================================================================
 * Font descriptor
 * Fonts are stored as const arrays in flash. The driver receives
 * a pointer and reads glyph bytes directly from flash — zero RAM.
 *
 * Layout: fixed-width glyphs stored sequentially.
 * Glyph for character 'c' starts at:
 *   data + (c - first_char) * bytes_per_glyph
 *
 * Each glyph is 'width' columns wide, each column is one byte
 * representing 8 vertical pixels (LSB = topmost pixel).
 * If height > 8, each column uses ceil(height/8) bytes, top first.
 * ================================================================ */
typedef struct {
    uint8_t        width;           /* pixels wide per glyph          */
    uint8_t        height;          /* pixels tall (up to 64)         */
    uint8_t        char_spacing;    /* pixels between chars (usually 1)*/
    uint8_t        first_char;      /* lowest ASCII code in this font  */
    uint8_t        last_char;       /* highest ASCII code in this font */
    const uint8_t *data;            /* glyph data in flash             */
} ssd1306_font_t;

/* ================================================================
 * Opaque device handle
 * Callers receive a pointer to the internal struct but cannot
 * see inside it. All internals are private to ssd1306_driver.c.
 * ================================================================ */
typedef struct ssd1306_dev_t *ssd1306_handle_t;

/* ================================================================
 * SECTION 1 — LIFECYCLE
 * ================================================================ */

/**
 * @brief  Initialise the SSD1306 display.
 *
 * - Registers the SSD1306 as a device on the given I2C bus
 * - Sends the standard SSD1306 startup command sequence
 * - Clears the framebuffer and flushes (display starts blank)
 *
 * @param  bus   I2C master bus handle from i2c_new_master_bus()
 * @return       Device handle on success, NULL on failure
 *
 * @note   Call once at startup before any other ssd1306_* function.
 */
ssd1306_handle_t ssd1306_init(i2c_master_bus_handle_t bus);

/**
 * @brief  Free the device handle and remove from I2C bus.
 *         Call when shutting down or reinitialising.
 */
void ssd1306_deinit(ssd1306_handle_t dev);

/* ================================================================
 * SECTION 2 — HARDWARE CONTROLS
 * These send commands directly to the SSD1306 and take effect
 * immediately. They do NOT interact with the framebuffer.
 * ================================================================ */

/**
 * @brief  Set display contrast (brightness).
 * @param  contrast  0 = dim, 255 = maximum brightness
 */
void ssd1306_set_contrast(ssd1306_handle_t dev, uint8_t contrast);

/**
 * @brief  Invert all pixels on the display.
 *         ON pixels become OFF, OFF pixels become ON.
 *         Does not modify the framebuffer — a second call restores normal.
 * @param  invert  true = inverted, false = normal
 */
void ssd1306_set_invert(ssd1306_handle_t dev, bool invert);

/**
 * @brief  Put display to sleep (panel off) or wake it up.
 *         Framebuffer is preserved during sleep.
 *         Waking resumes from the last flushed state.
 * @param  sleep  true = off, false = on
 */
void ssd1306_set_sleep(ssd1306_handle_t dev, bool sleep);

/* ================================================================
 * SECTION 3 — FRAMEBUFFER OPERATIONS
 * All functions below write into the RAM framebuffer only.
 * Nothing is sent to the display until ssd1306_flush() is called.
 *
 * Coordinate system:
 *   x = 0 (left) to 127 (right)
 *   y = 0 (top)  to  63 (bottom)
 *
 * Out-of-bounds coordinates are silently clamped — no crash.
 * ================================================================ */

/**
 * @brief  Clear the entire framebuffer to black (all pixels off).
 *         Marks all 8 pages dirty.
 */
void ssd1306_clear(ssd1306_handle_t dev);

/**
 * @brief  Fill the entire framebuffer to white (all pixels on).
 *         Marks all 8 pages dirty.
 */
void ssd1306_fill(ssd1306_handle_t dev);

/**
 * @brief  Set or clear a single pixel.
 * @param  x    Column, 0–127
 * @param  y    Row, 0–63
 * @param  on   true = pixel on (white), false = pixel off (black)
 */
void ssd1306_draw_pixel(ssd1306_handle_t dev,
                        uint8_t x, uint8_t y, bool on);

/**
 * @brief  Draw a horizontal line.
 * @param  x    Start column
 * @param  y    Row
 * @param  w    Width in pixels
 * @param  on   true = on, false = off
 */
void ssd1306_draw_hline(ssd1306_handle_t dev,
                        uint8_t x, uint8_t y, uint8_t w, bool on);

/**
 * @brief  Draw a vertical line.
 * @param  x    Column
 * @param  y    Start row
 * @param  h    Height in pixels
 * @param  on   true = on, false = off
 */
void ssd1306_draw_vline(ssd1306_handle_t dev,
                        uint8_t x, uint8_t y, uint8_t h, bool on);

/**
 * @brief  Draw a rectangle (hollow or filled).
 * @param  x, y   Top-left corner
 * @param  w, h   Width and height
 * @param  fill   true = filled rectangle, false = outline only
 * @param  on     true = draw on, false = draw off (erase)
 */
void ssd1306_draw_rect(ssd1306_handle_t dev,
                       uint8_t x, uint8_t y,
                       uint8_t w, uint8_t h,
                       bool fill, bool on);

/**
 * @brief  Render a text string into the framebuffer.
 *
 * Characters outside the display bounds are clipped.
 * Newline (\n) is not handled — caller manages line breaks.
 *
 * @param  x      Start column for the first character
 * @param  y      Top row for the text
 * @param  str    Null-terminated ASCII string
 * @param  font   Pointer to font descriptor (const, in flash)
 * @param  on     true = white text on black, false = black on white
 * @return        x position immediately after the last character drawn.
 *                Use this to chain multiple ssd1306_draw_text() calls
 *                on the same line without manual width calculation.
 */
uint8_t ssd1306_draw_text(ssd1306_handle_t dev,
                          uint8_t x, uint8_t y,
                          const char *str,
                          const ssd1306_font_t *font,
                          bool on);

/**
 * @brief  Draw a bitmap (icon, sprite frame) from flash.
 *
 * The bitmap is stored in column-major format: byte[col * ceil(h/8) + row_byte].
 * This matches the SSD1306's native page format for efficient copying.
 *
 * @param  x, y       Top-left corner of the bitmap
 * @param  w, h       Bitmap dimensions in pixels
 * @param  bitmap     Pointer to bitmap data in flash (const uint8_t array)
 * @param  transparent  If true, pixels with value 0 do NOT overwrite
 *                      the framebuffer (transparent background).
 *                      If false, 0-pixels draw as black (opaque).
 */
void ssd1306_draw_bitmap(ssd1306_handle_t dev,
                         uint8_t x, uint8_t y,
                         uint8_t w, uint8_t h,
                         const uint8_t *bitmap,
                         bool transparent);

/* ================================================================
 * SECTION 4 — FLUSH
 * ================================================================ */

/**
 * @brief  Send the framebuffer to the display over I2C.
 *
 * ONLY dirty pages are sent (per-page dirty flag system).
 * A page is 128 bytes wide x 8 pixels tall. There are 8 pages.
 * If only page 2 changed (time digits), only 128 bytes are sent.
 * If the whole screen changed, all 1,024 bytes are sent.
 *
 * This is the ONLY function in the driver that writes to hardware
 * after init. Call it once after all your draw operations.
 *
 * @return  ESP_OK on success, ESP_FAIL if I2C write failed.
 */
esp_err_t ssd1306_flush(ssd1306_handle_t dev);

/**
 * @brief  Force flush of ALL pages regardless of dirty flags.
 *         Use after a full screen clear or when bringing display
 *         out of sleep to guarantee the display matches the framebuffer.
 */
esp_err_t ssd1306_flush_all(ssd1306_handle_t dev);

/**
 * @brief  Mark all pages dirty without changing framebuffer content.
 *         Call this before the next flush if you know the display
 *         content is out of sync (e.g., after waking from sleep).
 */
void ssd1306_mark_dirty(ssd1306_handle_t dev);
