/**
 * ================================================================
 * app_config.h  —  Rev 2.0
 * All hardware constants, pin assignments, and tuning parameters
 * ================================================================
 * Rule: no magic numbers anywhere else in the codebase.
 *       Every constant lives here.
 *
 * When hardware changes for iteration 2, change this file only.
 * Nothing above it needs editing.
 * ================================================================
 */

#pragma once

/* ================================================================
 * GPIO pin assignments
 * ================================================================ */
#define PIN_I2C_SDA         5
#define PIN_I2C_SCL         6

/* Buttons — active-low, internal pull-up enabled in button_driver */
#define PIN_BTN_SELECT      9    /* BOOT pin — safe after reset        */
#define PIN_BTN_UP         10
#define PIN_BTN_DOWN        3
#define PIN_BTN_BACK        4

/* Buzzer — passive piezo, driven via LEDC PWM */
#define PIN_BUZZER         20

/* ================================================================
 * I2C configuration
 * ================================================================ */
#define I2C_PORT            I2C_NUM_0
#define I2C_FREQ_HZ         400000   /* 400 kHz fast mode */
#define SSD1306_I2C_ADDR    0x3C

/* ================================================================
 * Display
 * ================================================================ */
#define DISPLAY_WIDTH      128
#define DISPLAY_HEIGHT      64

/* ================================================================
 * Button tuning
 * ================================================================ */
#define BTN_DEBOUNCE_MS     50    /* ms to wait after edge before confirming */
#define BTN_LONG_PRESS_MS  800   /* ms held before BTN_EVT_LONG_PRESS fires  */
#define BTN_POLL_MS         50   /* poll interval while tracking hold time   */

/* ================================================================
 * Buzzer LEDC hardware config
 * ================================================================ */
#define BUZZER_LEDC_TIMER      LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL    LEDC_CHANNEL_0
#define BUZZER_LEDC_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_RESOLUTION LEDC_TIMER_13_BIT   /* 13-bit = 0..8191 */
#define BUZZER_DUTY_50PCT      4096                /* 50% of 8191 */

/* ================================================================
 * FreeRTOS task priorities
 * Higher number = higher priority.
 * Idle task runs at 0 — never go below 1.
 * ================================================================ */
#define TASK_PRIO_BLE       5    /* Highest — BLE timing is critical  */
#define TASK_PRIO_INPUT     4    /* Fast button response              */
#define TASK_PRIO_UI        3    /* Display refresh                   */
#define TASK_PRIO_ALARM     3    /* Same as UI — both user-facing     */
#define TASK_PRIO_TIME      2    /* Background tick                   */

/* ================================================================
 * FreeRTOS task stack sizes (bytes)
 * Measure with uxTaskGetStackHighWaterMark(), then shrink.
 * ================================================================ */
#define STACK_BLE           6144
#define STACK_INPUT         4096
#define STACK_UI            4096
#define STACK_ALARM         2048
#define STACK_TIME          2048

/* ================================================================
 * Queue depths
 * ================================================================ */
#define QUEUE_INPUT_DEPTH   10
#define QUEUE_BLE_DEPTH      5

/* ================================================================
 * Watchdog
 * ================================================================ */
#define WATCHDOG_TIMEOUT_MS 5000

/* ================================================================
 * TapShare BLE — scanning and advertising
 * ================================================================ */

/* How long to scan+advertise before giving up (no peer found) */
#define TAPSHARE_SCAN_TIMEOUT_MS    5000

/* Minimum RSSI to consider a peer valid.
 * -65 dBm ≈ watches within ~30 cm. Raise toward 0 to tighten. */
#define TAPSHARE_RSSI_THRESHOLD     (-65)

/* Maximum difference in tap timestamps to be considered the same tap.
 * Both watches must have triggered within this window of each other. */
#define TAPSHARE_TIME_WINDOW_S       5

/* How long to show "Saved: [name]" on screen after a successful share */
#define TAPSHARE_DISPLAY_MS          2000

/* BLE advertising interval — 100 ms gives fast discovery (~200 ms avg) */
#define BLE_ADV_INTERVAL_MS          100

/* ================================================================
 * TapShare NVS contact storage
 * ================================================================ */

/* Number of contact slots stored in NVS.
 * Stored in FIFO order. When full, oldest slot is overwritten.
 * Changing this value requires erasing NVS (idf.py erase-flash). */
#define TAPSHARE_MAX_CONTACTS        5

/* NVS namespace — all BLE_Tap keys live here */
#define TAPSHARE_NVS_NAMESPACE       "BLE_Tap"

/* ================================================================
 * TapShare BLE advertising manufacturer data
 * Manufacturer data embeds the tap timestamp and a magic byte
 * so the scanner can extract it without a GATT connection.
 * ================================================================ */
#define TAPSHARE_ADV_MAGIC           0xC0   /* Identifies BLE_Tap advert   */
#define TAPSHARE_MFR_ID              0xFFFF /* Unregistered manufacturer ID */
