/**
 * ================================================================
 * ssd1306_driver.c
 * SSD1306 0.96" I2C OLED Display Driver — Implementation
 * ================================================================
 * Target  : ESP32-C3 Super Mini, ESP-IDF 5.4.3
 *
 * HOW TO READ THIS FILE
 * ---------------------
 * This file is organized into four sections that match the header:
 *
 *  Section 1 — Private internals (struct, helpers, I2C helpers)
 *  Section 2 — Lifecycle (init, deinit)
 *  Section 3 — Hardware controls (contrast, invert, sleep)
 *  Section 4 — Framebuffer drawing primitives
 *  Section 5 — Flush (the only part that touches hardware after init)
 *
 * The ONLY two things in this file that touch I2C hardware:
 *   - ssd1306_init()         (sends startup command sequence)
 *   - ssd1306_flush() family (sends framebuffer pages)
 *   - ssd1306_set_*()        (sends single control commands)
 *
 * Everything else is pure RAM manipulation.
 * ================================================================
 */

#include "ssd1306_driver.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "SSD1306";

/* ================================================================
 * SECTION 1 — PRIVATE INTERNALS
 * ================================================================ */

/**
 * The private device struct.
 * Callers only ever see ssd1306_handle_t (a pointer to this).
 * The internals are completely hidden from the rest of the firmware.
 */
struct ssd1306_dev_t {
    i2c_master_dev_handle_t i2c_dev;

    /**
     * Framebuffer: 8 pages x 128 columns = 1,024 bytes.
     *
     * fb[page][col] — each byte represents 8 vertical pixels.
     * Bit 0 (LSB) = topmost pixel of the page.
     * Bit 7 (MSB) = bottommost pixel of the page.
     *
     * Page 0 covers y=0..7, page 1 covers y=8..15, ..., page 7 covers y=56..63.
     *
     * This layout matches the SSD1306's native page-addressing format,
     * so we can send a page directly to the display with zero conversion.
     */
    uint8_t fb[SSD1306_PAGES][SSD1306_WIDTH];

    /**
     * Dirty flags: one bit per page.
     * Bit N is set when page N has been modified since the last flush.
     * ssd1306_draw_pixel() sets the bit for the affected page.
     * ssd1306_flush() clears bits as it sends pages.
     */
    uint8_t dirty;
};


/* ── I2C helpers ──────────────────────────────────────────────
 * Two private functions that are the only things touching the wire.
 * All public draw functions go through RAM. Only these two go to hardware.
 */

/**
 * Send a single command byte to the SSD1306.
 * The 0x00 prefix tells the controller "next byte is a command".
 */
static esp_err_t s_send_cmd(struct ssd1306_dev_t *dev, uint8_t cmd)
{
    uint8_t buf[2] = { 0x00, cmd };
    return i2c_master_transmit(dev->i2c_dev, buf, 2, 50);
}

/**
 * Send a block of data bytes to the SSD1306 GDDRAM (pixel memory).
 * The 0x40 prefix tells the controller "following bytes are display data".
 * We prepend 0x40 to the data in a local buffer (max 129 bytes — 1 prefix + 128 page).
 */
static esp_err_t s_send_data(struct ssd1306_dev_t *dev,
                              const uint8_t *data, size_t len)
{
    /* One prefix byte + page data. Max = 1 + 128 = 129 bytes. Stack safe. */
    uint8_t buf[SSD1306_WIDTH + 1];
    buf[0] = 0x40;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(dev->i2c_dev, buf, len + 1, 100);
}

/**
 * Mark a specific page as dirty.
 * Called by every draw function after it modifies the framebuffer.
 */
static inline void s_mark_page_dirty(struct ssd1306_dev_t *dev, uint8_t page)
{
    dev->dirty |= (1u << page);
}


/* ================================================================
 * SECTION 2 — LIFECYCLE
 * ================================================================ */

ssd1306_handle_t ssd1306_init(i2c_master_bus_handle_t bus)
{
    /* Allocate the private struct from heap */
    struct ssd1306_dev_t *dev = calloc(1, sizeof(struct ssd1306_dev_t));
    if (!dev) {
        ESP_LOGE(TAG, "calloc failed — out of memory");
        return NULL;
    }

    /* Register SSD1306 as a device on the I2C bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SSD1306_I2C_ADDR,
        .scl_speed_hz    = 400000,   /* 400 kHz fast mode */
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        free(dev);
        return NULL;
    }

    /*
     * SSD1306 startup command sequence.
     *
     * The SSD1306 powers up in an undefined state. This sequence
     * configures every register we need before turning the display on.
     * Values come from the SSD1306 application note (Adafruit reference).
     *
     * We send them one at a time so we can catch and log any failure.
     */
    const uint8_t init_cmds[] = {
        0xAE,        /* [1]  Display OFF — configure while off              */
        0xD5, 0x80,  /* [2]  Oscillator freq / clock divide = default       */
        0xA8, 0x3F,  /* [3]  Multiplex ratio = 63 (for 64 row display)      */
        0xD3, 0x00,  /* [4]  Display offset = 0                             */
        0x40,        /* [5]  Display start line = 0                         */
        0x8D, 0x14,  /* [6]  Charge pump ON — required when VCC from 3.3V   */
        0x20, 0x00,  /* [7]  Memory addressing mode = horizontal            */
        0xA1,        /* [8]  Segment remap — col 127 → SEG0 (flip X)        */
        0xC8,        /* [9]  COM scan direction — remapped (correct orientation) */
        0xDA, 0x12,  /* [10] COM pins hardware config — 64 row alternate     */
        0x81, 0xCF,  /* [11] Contrast = 207 (0xCF) — good default           */
        0xD9, 0xF1,  /* [12] Pre-charge period                              */
        0xDB, 0x40,  /* [13] VCOMH deselect level                           */
        0xA4,        /* [14] Output follows RAM content (not all-on)        */
        0xA6,        /* [15] Normal display (not inverted)                  */
        0xAF,        /* [16] Display ON                                     */
    };

    for (int i = 0; i < (int)sizeof(init_cmds); i++) {
        ret = s_send_cmd(dev, init_cmds[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Init sequence failed at byte %d: %s",
                     i, esp_err_to_name(ret));
            i2c_master_bus_rm_device(dev->i2c_dev);
            free(dev);
            return NULL;
        }
    }

    /* Clear framebuffer and flush — display starts blank */
    ssd1306_clear(dev);
    ret = ssd1306_flush_all(dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initial flush failed: %s", esp_err_to_name(ret));
        /* Non-fatal — display is initialised, just not cleared visually */
    }

    ESP_LOGI(TAG, "SSD1306 initialised. Framebuffer: %d bytes. "
             "Dirty flags: 8-bit bitmask.",
             (int)sizeof(dev->fb));
    return dev;
}

void ssd1306_deinit(ssd1306_handle_t dev)
{
    if (!dev) return;
    ssd1306_set_sleep(dev, true);
    i2c_master_bus_rm_device(dev->i2c_dev);
    free(dev);
}


/* ================================================================
 * SECTION 3 — HARDWARE CONTROLS
 * These bypass the framebuffer and go directly to the chip.
 * ================================================================ */

void ssd1306_set_contrast(ssd1306_handle_t dev, uint8_t contrast)
{
    s_send_cmd(dev, 0x81);
    s_send_cmd(dev, contrast);
}

void ssd1306_set_invert(ssd1306_handle_t dev, bool invert)
{
    /* 0xA6 = normal, 0xA7 = inverted */
    s_send_cmd(dev, invert ? 0xA7 : 0xA6);
}

void ssd1306_set_sleep(ssd1306_handle_t dev, bool sleep)
{
    /* 0xAE = display off, 0xAF = display on */
    s_send_cmd(dev, sleep ? 0xAE : 0xAF);
}


/* ================================================================
 * SECTION 4 — FRAMEBUFFER DRAWING PRIMITIVES
 *
 * Rule: every function here ONLY modifies dev->fb[][] and
 * calls s_mark_page_dirty(). No I2C. No exceptions.
 * ================================================================ */

void ssd1306_clear(ssd1306_handle_t dev)
{
    memset(dev->fb, 0x00, sizeof(dev->fb));
    dev->dirty = 0xFF;   /* All 8 pages dirty */
}

void ssd1306_fill(ssd1306_handle_t dev)
{
    memset(dev->fb, 0xFF, sizeof(dev->fb));
    dev->dirty = 0xFF;
}

void ssd1306_draw_pixel(ssd1306_handle_t dev,
                        uint8_t x, uint8_t y, bool on)
{
    /* Bounds check — silently ignore out-of-range coordinates */
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) return;

    uint8_t page = y / 8;        /* which page (0..7)             */
    uint8_t bit  = y % 8;        /* which bit within the page byte */

    if (on) {
        dev->fb[page][x] |=  (1u << bit);
    } else {
        dev->fb[page][x] &= ~(1u << bit);
    }

    s_mark_page_dirty(dev, page);
}

void ssd1306_draw_hline(ssd1306_handle_t dev,
                        uint8_t x, uint8_t y, uint8_t w, bool on)
{
    /*
     * Optimised: a horizontal line stays in one page if y%8 == same.
     * We still call draw_pixel for simplicity and correctness.
     * If you need extreme speed, you can directly manipulate fb[][] here.
     * For our use case (128px wide max), this loop is fast enough.
     */
    uint8_t x_end = (uint8_t)((uint16_t)x + w > SSD1306_WIDTH
                               ? SSD1306_WIDTH : x + w);
    for (uint8_t i = x; i < x_end; i++) {
        ssd1306_draw_pixel(dev, i, y, on);
    }
}

void ssd1306_draw_vline(ssd1306_handle_t dev,
                        uint8_t x, uint8_t y, uint8_t h, bool on)
{
    uint8_t y_end = (uint8_t)((uint16_t)y + h > SSD1306_HEIGHT
                               ? SSD1306_HEIGHT : y + h);
    for (uint8_t i = y; i < y_end; i++) {
        ssd1306_draw_pixel(dev, x, i, on);
    }
}

void ssd1306_draw_rect(ssd1306_handle_t dev,
                       uint8_t x, uint8_t y,
                       uint8_t w, uint8_t h,
                       bool fill, bool on)
{
    if (fill) {
        /* Filled: draw horizontal lines top to bottom */
        uint8_t y_end = (uint8_t)((uint16_t)y + h > SSD1306_HEIGHT
                                   ? SSD1306_HEIGHT : y + h);
        for (uint8_t row = y; row < y_end; row++) {
            ssd1306_draw_hline(dev, x, row, w, on);
        }
    } else {
        /* Outline only: four sides */
        ssd1306_draw_hline(dev, x,         y,         w, on);  /* top    */
        ssd1306_draw_hline(dev, x,         y + h - 1, w, on);  /* bottom */
        ssd1306_draw_vline(dev, x,         y,         h, on);  /* left   */
        ssd1306_draw_vline(dev, x + w - 1, y,         h, on);  /* right  */
    }
}

uint8_t ssd1306_draw_text(ssd1306_handle_t dev,
                          uint8_t x, uint8_t y,
                          const char *str,
                          const ssd1306_font_t *font,
                          bool on)
{
    if (!str || !font || !font->data) return x;

    uint8_t col_bytes = (font->height + 7) / 8;  /* bytes per column */

    while (*str) {
        char c = *str++;

        /* Skip characters outside the font's range */
        if (c < font->first_char || c > font->last_char) {
            x += font->width + font->char_spacing;
            continue;
        }

        /* Stop if this character would start off-screen */
        if (x >= SSD1306_WIDTH) break;

        /* Pointer to this character's glyph data in flash */
        uint32_t glyph_offset = (uint32_t)(c - font->first_char)
                                 * font->width * col_bytes;
        const uint8_t *glyph = font->data + glyph_offset;

        /*
         * Draw each column of the glyph.
         *
         * The glyph data is stored column-major:
         *   glyph[col * col_bytes + row_byte]
         *
         * col_bytes = 1 for fonts up to 8px tall (one byte covers 8 rows)
         * col_bytes = 2 for fonts 9-16px tall, etc.
         *
         * Bit 0 (LSB) of the first byte = topmost pixel of the column.
         */
        for (uint8_t col = 0; col < font->width; col++) {
            if (x + col >= SSD1306_WIDTH) break;  /* clip at right edge */

            for (uint8_t row_byte = 0; row_byte < col_bytes; row_byte++) {
                uint8_t bits = glyph[col * col_bytes + row_byte];

                for (uint8_t bit = 0; bit < 8; bit++) {
                    uint8_t py = y + row_byte * 8 + bit;

                    /* Clip at bottom edge and font height */
                    if (py >= SSD1306_HEIGHT) break;
                    if (py >= y + font->height) break;

                    bool pixel_on = (bits >> bit) & 1;

                    /* on=true:  1-bits → white, 0-bits → black (normal)
                     * on=false: 1-bits → black, 0-bits → white (inverted) */
                    ssd1306_draw_pixel(dev, x + col, py,
                                       on ? pixel_on : !pixel_on);
                }
            }
        }

        x += font->width + font->char_spacing;
    }

    return x;  /* caller can chain: x = draw_text(...); x = draw_text(x,...); */
}

void ssd1306_draw_bitmap(ssd1306_handle_t dev,
                         uint8_t x, uint8_t y,
                         uint8_t w, uint8_t h,
                         const uint8_t *bitmap,
                         bool transparent)
{
    if (!bitmap) return;

    uint8_t col_bytes = (h + 7) / 8;

    for (uint8_t col = 0; col < w; col++) {
        if (x + col >= SSD1306_WIDTH) break;

        for (uint8_t row_byte = 0; row_byte < col_bytes; row_byte++) {
            uint8_t bits = bitmap[col * col_bytes + row_byte];

            for (uint8_t bit = 0; bit < 8; bit++) {
                uint8_t py = y + row_byte * 8 + bit;

                if (py >= SSD1306_HEIGHT) break;
                if (py >= y + h)          break;

                bool pixel_on = (bits >> bit) & 1;

                if (transparent && !pixel_on) {
                    /* Transparent mode: 0 pixels do not overwrite background */
                    continue;
                }

                ssd1306_draw_pixel(dev, x + col, py, pixel_on);
            }
        }
    }
}


/* ================================================================
 * SECTION 5 — FLUSH
 *
 * This is the only section that communicates with the SSD1306
 * hardware after init. Everything above builds a picture in RAM.
 * This section sends that picture to the glass.
 * ================================================================ */

/**
 * Internal function: send one page to the display.
 *
 * A page is one row of 8 pixels tall, 128 pixels wide.
 * We tell the controller "write to page N, starting at column 0"
 * then send 128 bytes of pixel data.
 *
 * The SSD1306 in horizontal addressing mode auto-advances to the
 * next page after each row, but we set the address explicitly for
 * each page to avoid any state dependency.
 */
static esp_err_t s_flush_page(struct ssd1306_dev_t *dev, uint8_t page)
{
    esp_err_t ret;

    /* Set column address: start=0, end=127 */
    ret = s_send_cmd(dev, 0x21);
    if (ret != ESP_OK) return ret;
    ret = s_send_cmd(dev, 0x00);
    if (ret != ESP_OK) return ret;
    ret = s_send_cmd(dev, 0x7F);
    if (ret != ESP_OK) return ret;

    /* Set page address: start=page, end=page (just this one page) */
    ret = s_send_cmd(dev, 0x22);
    if (ret != ESP_OK) return ret;
    ret = s_send_cmd(dev, page);
    if (ret != ESP_OK) return ret;
    ret = s_send_cmd(dev, page);
    if (ret != ESP_OK) return ret;

    /* Send the 128 bytes of pixel data for this page */
    ret = s_send_data(dev, dev->fb[page], SSD1306_WIDTH);
    return ret;
}

esp_err_t ssd1306_flush(ssd1306_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->dirty == 0) return ESP_OK;  /* Nothing changed — skip all I2C */

    esp_err_t last_err = ESP_OK;

    for (uint8_t page = 0; page < SSD1306_PAGES; page++) {
        if (!(dev->dirty & (1u << page))) {
            continue;  /* This page is clean — skip it */
        }

        esp_err_t ret = s_flush_page(dev, page);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Flush failed on page %d: %s", page, esp_err_to_name(ret));
            last_err = ret;
            /* Continue flushing other pages even if one fails */
        }

        /* Clear the dirty bit for this page */
        dev->dirty &= ~(1u << page);
    }

    return last_err;
}

esp_err_t ssd1306_flush_all(ssd1306_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;

    /* Mark all pages dirty, then flush normally */
    ssd1306_mark_dirty(dev);
    return ssd1306_flush(dev);
}

void ssd1306_mark_dirty(ssd1306_handle_t dev)
{
    if (dev) {
        dev->dirty = 0xFF;
    }
}
