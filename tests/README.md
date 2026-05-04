# Test Suite

Nine standalone ESP-IDF projects for incremental hardware bring-up. Each test validates exactly one new component on top of previously verified components. Flash one test at a time; each is a complete `idf.py` project with its own `CMakeLists.txt` and `sdkconfig`.

---

## Setup (do once)

```sh
cp tests/test_config.h.template tests/test_config.h
# Fill in: MY_PROFILE_NAME, MY_PROFILE_PHONE, MY_PROFILE_TITLE
# Optionally: WIFI_SSID, WIFI_PASS (needed only for test 04 NTP sync)
```

`test_config.h` is gitignored and never committed. Tests that require a profile (06–09) fall back to `"Cylonix User"` if the file is absent, but BLE exchange tests will exchange that placeholder name.

## Flash any test

```sh
cd tests/<test-dir>
idf.py set-target esp32c3 build
idf.py -p <PORT> flash monitor
```

---

## Test index

### 01 — Display

**Component under test:** `ssd1306_driver`

**Depends on:** Nothing (first test)

**What it does:** Initialises the I2C bus at 400 kHz and the SSD1306 at address `0x3C`. Draws a static text screen using `font_5x7`, then cycles through several demo frames — filled rectangle, inverted display, pixel-level drawing — to exercise each drawing primitive. Calls `ssd1306_flush()` after each frame to send only dirty pages over I2C.

**Pass criteria:** Display shows text without glitches; serial log shows no `I2C` errors; framebuffer round-trip matches expected output on screen.

---

### 02 — Button

**Component under test:** `button_driver`

**Depends on:** Test 01 (I2C bus init pattern verified)

**What it does:** Configures all four GPIO inputs (SELECT, UP, DOWN, BACK) with active-low logic and internal pull-ups. An ISR fires on falling edge and posts a raw event to a FreeRTOS queue. The task reads the queue, applies a 50 ms debounce window, and classifies each gesture as `BTN_EVT_PRESS` (released before 800 ms) or `BTN_EVT_LONG_PRESS` (held for ≥ 800 ms). Logs each event to the serial monitor.

**Pass criteria:** Clean edge detection with no spurious events during sustained hold; `BTN_EVT_LONG_PRESS` fires exactly once per gesture; rapid taps produce one `BTN_EVT_PRESS` each.

---

### 03 — Buzzer

**Component under test:** `buzzer_driver`

**Depends on:** Test 02 (task/queue pattern verified)

**What it does:** Initialises LEDC timer 0 and channel 0 on GPIO 20 at 13-bit resolution. Plays a sequence of three named sounds from the sound map — boot chime, success melody, error tone — with defined frequencies and durations. Between sounds the LEDC duty is set to zero to silence the buzzer.

**Pass criteria:** Audible tones match the expected pattern and sequence; no distortion or continuous tone (stuck duty); serial log confirms each sound name before it plays.

---

### 04 — RTC

**Component under test:** `rtc_driver`

**Depends on:** Test 03 (all non-BLE drivers verified)

**What it does:** Calls `rtc_set_time()` with a known epoch, then reads back the time in a 1-second loop via `rtc_get_time()` and prints the delta. If `test_config.h` contains valid WiFi credentials, the test also connects to WiFi and syncs the clock via SNTP, then reads back the updated time.

**Note:** The v1 prototype uses the internal ESP32-C3 RTC. Precision is intentionally not the engineering question being validated in v1; the `rtc_driver` API is designed to swap in a DS3231 for v2 without changing callers.

**Pass criteria:** RTC increments correctly between reads (±0 drift in a 10-second window); if WiFi credentials are present, NTP sets the clock and the post-sync read shows the correct wall-clock time.

---

### 05 — Integration

**Component under test:** All four non-BLE drivers together

**Depends on:** Tests 01–04

**What it does:** Runs a minimal clock UI on a single board: the OLED displays the current time updated every second from the RTC, button presses trigger buzzer tones (SELECT = boot chime, UP/DOWN = short beep), and the display redraws only the changed time region using dirty-page flush. Verifies that all four drivers coexist without I2C arbitration errors or priority inversion.

**Pass criteria:** All peripherals respond correctly under simultaneous activity; no I2C bus busy errors; display updates at 1 Hz without glitches; buzzer fires on button press without blocking the display task.

---

### 06 — BLE Advertise

**Component under test:** `ble_driver` — Peripheral/Broadcaster role

**Depends on:** Test 05

**What it does:** Calls `ble_driver_start_share_mode()` with a fixed tap timestamp. The driver builds a BLE advertising packet containing the TapShare service UUID and a 5-byte manufacturer data field: magic byte `0xC0` followed by the `uint32_t` tap timestamp in little-endian. The watch advertises at 100 ms interval continuously. Use a phone BLE scanner app (e.g. nRF Connect) to inspect the raw advertisement.

**Pass criteria:** Advertisement is visible on a nearby BLE scanner; the service UUID matches the TapShare UUID; the manufacturer data field contains the correct magic byte and timestamp value matching the one logged to serial.

---

### 07 — BLE Scan

**Component under test:** `ble_driver` — Central/Observer role and 4-gate filter

**Depends on:** Test 06 (a second board running test 06 is required as the advertising peer)

**Requires:** Two boards — one running test 06 (advertiser), one running test 07 (scanner).

**What it does:** Starts an active BLE scan and passes every discovered advertisement through the 4-gate filter. For each advertisement that carries the TapShare UUID, the driver logs all four gate results: UUID match, RSSI value vs. the `-65 dBm` threshold, timestamp delta vs. the 5-second window, and the role decision outcome. Advertisements that fail a gate are logged with the specific gate that rejected them. The 4-gate filter log is the primary verification artifact for this test.

**Pass criteria:** The board running test 06 appears in the scan results; all four gate results are printed per advertisement; an advertisement from test 06 placed within ~30 cm passes gates 1–3 and triggers the role decision log; moving the test 06 board beyond ~1 metre causes gate 2 (RSSI) to fail and log the rejection.

---

### 08 — BLE Connect

**Component under test:** `ble_driver` — GAP connection establishment and role assignment

**Depends on:** Test 07

**Requires:** Two boards, both flashed with test 08.

**What it does:** Both boards simultaneously call `ble_driver_start_share_mode()` with identical tap timestamps to exercise the MAC address tiebreaker path. After the 4-gate filter runs, the board with the lower MAC address becomes Central, calls `ble_gap_connect()`, and the scan and advertise are stopped before the connection is initiated. On connect, both boards log their role, the connection handle, peer address, and RSSI. The connection is terminated immediately after parameters are logged.

**Role consistency check:** Run this test five times in sequence. The same physical board must always be assigned Central on every run — the MAC tiebreaker is deterministic. If roles reverse between runs, the tiebreaker logic has a bug.

**Pass criteria:** GAP connection established within 5 seconds; both boards log their assigned role; connection parameters (handle, peer address, RSSI) are printed; clean disconnect with no assert or watchdog timeout; Central role is assigned to the same board on all five runs.

---

### 09 — BLE TapShare

**Component under test:** Complete TapShare protocol — `ble_driver` + `ble_service` + NVS

**Depends on:** Test 08

**Requires:** Two boards, both flashed with test 09, each with a distinct profile in their own `test_config.h`.

**What it does:** Runs the full end-to-end TapShare flow. Both boards simultaneously long-press SELECT. Each records its tap timestamp, calls `ble_driver_start_share_mode()`, and enters `BLE_STATE_SHARING`. The 4-gate filter fires on both scanners; the Central board calls `ble_gap_connect()`. On connect, the GATT exchange sequence runs: Central reads peer Name, Phone, and Title characteristics in sequence, then writes its own three values back. On `BLE_EVT_EXCHANGE_COMPLETE`, `ble_service` writes the contact to NVS using the double-commit safe sequence and transitions to `BLE_STATE_DONE`. The contact is immediately read back from NVS and printed to serial.

Three key verifications:

1. **Contact data integrity** — the name, phone, and title printed by each board must exactly match the profile configured in the other board's `test_config.h`.
2. **NVS persistence across power cycle** — power-cycle both boards after the exchange; on next boot, `ble_service_get_contact(0, &entry)` must return `ESP_OK` and the same contact data.
3. **FIFO slot rotation at the 6th exchange** — after five successful exchanges (`TAPSHARE_MAX_CONTACTS = 5`), perform a sixth. The slot index wraps to 0 and the oldest contact is overwritten. Verify via serial log that `s_next_slot` wraps and slot 0 contains the newest contact.

**Pass criteria:** Both boards reach `BLE_STATE_DONE`; contact data on each board matches the peer's profile exactly; NVS read-back after power cycle returns the correct contact; slot rotation occurs at the 6th exchange.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| `I2C: bus busy` at boot | SDA/SCL shorted or pull-up resistors missing |
| `NVS: not found` on first boot | Expected — profile is written automatically on first boot |
| BLE test stuck in `BLE_STATE_SHARING` | Tap timestamps more than 5 s apart; press buttons within 1 s of each other |
| Test 07 shows gate 2 failing for nearby device | RSSI threshold too tight for the environment; check `TAPSHARE_RSSI_THRESHOLD` in `app_config.h` |
| Display blank after flash | Wrong I2C address — check `SSD1306_I2C_ADDR` in `app_config.h` (default `0x3C`) |
| Roles swap between test 08 runs | Possible bug in MAC tiebreaker; check `memcmp(s_own_addr, event->disc.addr.val, 6)` direction |
| `idf.py: IDF_PATH not set` | Run `. $IDF_PATH/export.sh` first |
| NVS data missing after reflash | `idf.py flash` does not erase NVS; use `idf.py erase-flash` then reflash to reset |
