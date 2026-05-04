# Architecture

## 1. Overview

This firmware runs on an ESP32-C3 Super Mini and implements a contact-exchange protocol for a wrist-worn device. Two people simultaneously long-press a button and their watches exchange name, phone number, and job title over Bluetooth Low Energy. The exchange is automatic: no pairing, no app, no screen interaction beyond the initial button press.

The engineering challenge is coordination without prior communication. Two independent devices must detect co-activation within a shared time window, negotiate connection roles without exchanging any data beforehand, complete a bidirectional GATT data exchange, and persist the result safely to flash — all without central infrastructure and in under 10 seconds. The protocol must reject nearby watches whose buttons were pressed at a different time, handle the case where both devices press at exactly the same millisecond, and ensure that a power loss at any point during the flash write does not leave corrupted data.

The current repository state covers all hardware drivers, the complete BLE protocol, and nine incremental bring-up tests. The FreeRTOS task integration and UI screens are the next milestone.

---

## 2. Firmware Architecture

### 2.1 Four-layer model

The firmware is structured in four layers: Hardware → Drivers → Services → App.

**Hardware** is the physical peripherals: SSD1306 OLED, button GPIOs, LEDC buzzer, internal RTC, NimBLE radio.

**Drivers** (`ssd1306_driver`, `button_driver`, `buzzer_driver`, `rtc_driver`, `ble_driver`) wrap each peripheral in a C API. A driver knows its peripheral and nothing above it. `ble_driver` knows NimBLE; it does not know what TapShare is. `ssd1306_driver` knows I2C and framebuffers; it does not know what a watch face is. Each driver exposes an opaque handle so callers cannot reach into its internals.

**Services** (`ble_service`, `time_service`, `ui_service`) own business logic. `ble_service` owns the TapShare state machine and NVS contact storage. It calls `ble_driver` but never touches NimBLE directly. Services communicate with each other only through the event types defined in `app_events.h` — no service includes another service's header.

**App** (`tasks.c`, `main.c`) creates the FreeRTOS tasks, allocates queues and event groups, and connects services to each other. It is the only layer that knows the full system topology.

The payoff of this layering is that v2 hardware changes are isolated to single files. Swapping the SSD1306 for a GC9A01 round display requires a new `ssd1306_driver.c` implementing the same header contract — `ui_service` is unchanged. Replacing the internal RTC with a DS3231 external oscillator requires editing `rtc_driver.c` only. Adding a companion phone app that writes the user's profile requires adding one GATT characteristic to `ble_driver` and calling the existing `ble_service_set_my_profile()` API — no state machine changes needed.

### 2.2 FreeRTOS task design

| Task | Priority | Stack | Responsibility |
|------|----------|-------|----------------|
| `ble_task` | 5 (highest) | 6144 B | Dequeues `ble_event_t` from `g_ble_event_queue`, calls `ble_service_on_ble_event()` |
| `input_task` | 4 | 4096 B | Dequeues `button_event_t` from `g_input_queue`, dispatches to service layer |
| `ui_task` | 3 | 4096 B | Redraws display on `TIME_EVT_SECOND_TICK` or BLE state change |
| `alarm_task` | 3 | 2048 B | Monitors `TIME_EVT_ALARM_TRIGGER` event group bit |
| `time_task` | 2 (lowest) | 2048 B | Calls `rtc_get_time()` every second, sets event group bits |

`ble_task` runs at priority 5 because the NimBLE HCI transport layer is time-sensitive: missed HCI responses cause connection parameter negotiation to fail or connection supervision timeouts to fire. All other application tasks can be preempted by `ble_task` without correctness issues.

The golden pattern for every task is: initialise once before the loop, then block indefinitely on a queue or event group inside `while(1)`. No task polls or sleeps with a fixed delay. `time_task` is the only task that uses `vTaskDelay` — it sleeps for 1000 ms between RTC reads, which is its intended period.

Tasks communicate exclusively through `xQueueSend` / `xQueueReceive` and `xEventGroupSetBits` / `xEventGroupWaitBits`. There are no direct function calls between tasks. This means a task can be replaced or reordered without auditing call chains, and the queue provides automatic backpressure if a consumer falls behind.

### 2.3 Memory layout

The ESP32-C3 has 400 KB of SRAM. The dominant consumer is the NimBLE stack.

| Item | Size | Notes |
|------|------|-------|
| NimBLE host + HCI | 85–100 KB | Configured for 1 simultaneous connection, all 4 roles |
| Task stacks (5 tasks) | ~18 KB | Sum of `STACK_BLE` + `STACK_INPUT` + `STACK_UI` + `STACK_ALARM` + `STACK_TIME` |
| SSD1306 framebuffer | 1 KB | 128 × 64 px, 1 bit/pixel, plus 8-bit dirty flag byte |
| Queues + event groups | < 1 KB | Two queues (depths 10 and 5), one event group |
| NVS working buffers | < 2 KB | Profile strings + `contact_entry_t` (120 bytes) on stack during save |
| Available heap | ~280 KB | Remaining for NimBLE mbuf pool, lwIP (if WiFi used), user allocations |
| **Total SRAM** | **400 KB** | |

LVGL was evaluated for the display and rejected. LVGL's frame buffer and widget tree require 80–150 KB of RAM depending on configuration. Combined with NimBLE's 85–100 KB, the two together would leave under 150 KB of headroom on a 400 KB device — insufficient once lwIP, task stacks, and NimBLE's mbuf pool are accounted for. The custom `ssd1306_driver` uses a 1 KB framebuffer and a per-page dirty flag that limits I2C traffic to only the pages that changed. For a 128×64 monochrome display with text-only content, this is sufficient and costs roughly 1/100th the RAM of LVGL.

---

## 3. BLE Protocol Design

### 3.1 Why NimBLE

| | NimBLE | Bluedroid |
|---|---|---|
| RAM usage | ~85–100 KB | ~150–200 KB |
| ESP32-C3 support | Full | Partial |
| Simultaneous Central + Peripheral | Yes | Limited |
| ESP-IDF 5.x status | Recommended | Legacy |
| API style | Callback-based, C | Java-derived, verbose |

NimBLE is Espressif's recommended BLE stack for new ESP-IDF 5.x projects on ESP32-C3. Its RAM footprint is roughly half that of Bluedroid, and it supports all four BLE roles (Central, Peripheral, Observer, Broadcaster) simultaneously on a single device — which is a hard requirement for the TapShare protocol.

### 3.2 Dual-role simultaneous operation

Both watches run as Peripheral and Central at the same time during share mode. `ble_driver_start_share_mode()` calls `start_advertising()` followed immediately by `start_scanning()`. Neither device knows in advance whether it will be the one to initiate the connection — that depends on the tap timestamp comparison, which requires seeing the peer's advertisement first.

This symmetry is why both roles must be active before the role decision is made. If only one device advertised while the other scanned, they would need to agree on roles before pressing the button, which defeats the purpose of a simultaneous-press protocol.

When the 4-gate filter determines that the local device is Central, scanning and advertising are both stopped immediately before calling `ble_gap_connect()`. On ESP32-C3, the radio cannot scan while a connection is being established. `ble_gap_disc_cancel()` is called first, then `ble_gap_adv_stop()` if advertising is still active. Failing to stop both before `ble_gap_connect()` causes an `BLE_HS_EALREADY` or radio conflict error.

### 3.3 Tap timestamp in advertising packet — collision detection

The core problem: two watches that happen to be in the same room must not accidentally connect to each other's share events unless they were pressed at the same time. RSSI proximity alone is insufficient — a watch 20 cm away on a desk from a separate social interaction would pass an RSSI gate.

The solution encodes the tap timestamp directly into the BLE advertising packet. When `ble_service_on_button_event()` fires on a long-press, it calls `gettimeofday()` and records the current POSIX time as `s_tap_ts` (a `uint32_t` in seconds). This value is passed to `ble_driver_start_share_mode(tap_ts)`, which builds a 5-byte manufacturer-specific data field:

```c
uint8_t mfr_data[5];
mfr_data[0] = TAPSHARE_ADV_MAGIC;          /* 0xC0 — identifies Cylonix advert */
memcpy(&mfr_data[1], &tap_ts, sizeof(uint32_t)); /* little-endian epoch seconds */
```

This field is included in every advertising packet. Any scanner that sees this advertisement can extract the peer's tap timestamp from the raw GAP discovery event — no connection required. The time-window gate (`|peer_tap_ts - own_tap_ts| <= 5`) runs entirely from advertisement data.

The magic byte `0xC0` is essential: the manufacturer-specific data AD type in BLE is a general-purpose field used by many devices. Without the magic byte, the scanner would attempt to parse arbitrary bytes from other devices' manufacturer data as timestamps, producing false positives.

### 3.4 The 4-gate scan filter

All four gates run sequentially in `scan_event_cb()` for every discovered advertisement. A failure at any gate returns immediately without proceeding to the next.

| Gate | Check | Threshold | Fail action | Rationale |
|------|-------|-----------|-------------|-----------|
| 1 — UUID | Advertisement contains TapShare service UUID | Exact match | Discard silently | Filters all non-TapShare BLE devices |
| 2 — RSSI | `rssi >= TAPSHARE_RSSI_THRESHOLD` | `-65 dBm` | Log and discard | −65 dBm ≈ 30 cm empirical; rejects watches in the same room but not adjacent |
| 3 — Time window | `|peer_tap_ts - own_tap_ts| <= 5` | 5 seconds | Log and discard | 5 s >> typical human reaction-time difference (< 200 ms); << time between separate social interactions |
| 4 — Role decision | `own_tap_ts < peer_tap_ts` → Central; equal → MAC tiebreaker | `memcmp` of 6-byte MAC | Peripheral waits; Central connects | Deterministic, zero network cost, no additional message exchange |

The −65 dBm threshold was determined empirically with the prototype hardware at approximately 30 cm separation. It can be adjusted in `app_config.h` by changing `TAPSHARE_RSSI_THRESHOLD`. Moving it toward 0 dBm tightens the required proximity; moving it more negative loosens it.

The 5-second window is deliberately generous. Two people tapping simultaneously will differ by less than 200 ms in practice. The 5-second bound is large enough to absorb clock drift and any FreeRTOS scheduling jitter, while being short enough to distinguish separate social interactions (which are typically minutes apart).

### 3.5 Deterministic role assignment

```c
bool i_am_central;
if (s_my_tap_ts < peer_tap_ts) {
    i_am_central = true;
} else if (s_my_tap_ts > peer_tap_ts) {
    i_am_central = false;
} else {
    /* Equal timestamps — MAC tiebreaker */
    i_am_central = (memcmp(s_own_addr, event->disc.addr.val, 6) < 0);
}
```

Both devices run this identical code with the same inputs: their own tap timestamp (known locally) and the peer's tap timestamp (extracted from the advertisement). They reach the same role assignment independently, instantaneously, and without any additional message exchange. The MAC addresses are already in memory — `s_own_addr` is populated at NimBLE sync time, and `event->disc.addr.val` comes from the GAP discovery event.

The Peripheral device does not call `ble_gap_connect()`. It simply returns from `scan_event_cb()` and waits. Its GATT server is already registered and serving its own profile, ready to accept the incoming connection from the Central.

### 3.6 GATT table and read-first contract

The TapShare GATT service exposes three characteristics, each with `READ | WRITE` flags:

| Characteristic | Content | Max size |
|----------------|---------|----------|
| Name | Contact name string | 32 bytes |
| Phone | Phone number string | 16 bytes |
| Title | Job title string | 64 bytes |

Three separate characteristics are used rather than a single blob so that partial failures are precisely located. If the name write fails, the service layer knows exactly what was and was not delivered.

The read-first contract means the Central reads all three of the Peripheral's characteristics before writing any of its own:

```
Central → read Name  → Peripheral
Central ← Name value ← Peripheral
Central → read Phone → Peripheral
Central ← Phone val  ← Peripheral
Central → read Title → Peripheral
Central ← Title val  ← Peripheral
Central → write Name  → Peripheral
Central → write Phone → Peripheral
Central → write Title → Peripheral
```

If any of the three reads fails, `BLE_EVT_EXCHANGE_FAILED` is posted immediately and the write phase is skipped. This ensures that if the exchange fails mid-way, neither device saves a partial contact. Without the read-first contract, a failure after the Central's writes but before its reads would result in the Peripheral saving the Central's contact while the Central saved nothing — an asymmetric half-exchange.

The exchange is implemented as a chained async callback sequence in `gatt_read_cb()` → `gatt_write_cb()` using the `gatt_stage_t` enum to track position. The NimBLE callbacks fire on the NimBLE host task; results are posted to `g_ble_event_queue` for processing on `ble_task`.

### 3.7 NVS power-loss safe writes

Each contact slot uses a validity flag stored separately from the data blob. The write sequence for slot N is:

```
1. nvs_set_u8(nvs, "ts_valid_N", 0)        // mark INVALID before writing
2. nvs_commit(nvs)                           // flush to flash NOW
3. nvs_set_blob(nvs, "ts_slot_N", &entry)   // write 120-byte contact blob
4. nvs_set_u8(nvs, "ts_valid_N", 1)         // mark VALID after write
5. nvs_commit(nvs)                           // final flush
```

The intermediate commit at step 2 is the critical element. Without it, step 1's write to the validity flag may still be in NVS's internal write buffer when power is lost during step 3. With the intermediate commit, the following power-loss scenarios are safe:

- Power lost before step 1: slot remains in previous valid state — no change.
- Power lost between steps 1 and 2: validity flag is in the write buffer; on reboot NVS may or may not have flushed it, but step 2 ensures it reaches flash before the data write begins.
- Power lost between steps 2 and 3: validity flag is 0 (invalid), data blob is stale or absent — slot is skipped on next boot.
- Power lost between steps 3 and 4: data is written but validity flag is still 0 — slot is skipped on next boot.
- Power lost between steps 4 and 5: validity flag write may be lost; slot may or may not be valid on next boot, but data blob is intact.
- Normal completion: both the data and the validity flag are committed — slot is valid.

On boot, `ble_service_init()` reads `ts_valid_N` for each slot and skips any with value 0. Corrupted or incomplete writes are invisible to the application.

---

## 4. Driver Design Decisions

**SSD1306.** A custom driver was written rather than integrating LVGL (see Section 2.3 for the RAM analysis). The framebuffer is 1,024 bytes — 128 columns × 8 pages, 1 bit per pixel. An 8-bit dirty flag tracks which of the 8 pages has changed since the last flush. `ssd1306_flush()` iterates pages and sends only those with the dirty bit set, so a one-second time update that changes only the seconds digits sends 128 bytes (one page) rather than 1,024 bytes. The driver exposes an opaque `ssd1306_handle_t` pointer so callers cannot reach into the internal struct.

**Button.** The ISR fires on falling edge and posts a minimal event to a FreeRTOS queue. No debounce logic runs in the ISR — the ISR is intentionally minimal to minimise time in interrupt context. Debounce (50 ms window, configured in `app_config.h` as `BTN_DEBOUNCE_MS`) and long-press detection (800 ms threshold, `BTN_LONG_PRESS_MS`) run in the `input_task`, which reads the queue and tracks hold time using `esp_timer_get_time()`.

**Buzzer.** The LEDC hardware PWM peripheral drives the passive piezo directly. LEDC runs its own hardware timer independent of the CPU, so tone frequency and duration are exact regardless of FreeRTOS scheduling jitter. A software PWM approach using GPIO toggle in a task would produce audible jitter on a 1 kHz FreeRTOS tick. The buzzer driver uses a sound map: named sounds are defined as frequency + duration pairs; the driver plays them sequentially with duty cycle set to `BUZZER_DUTY_50PCT` (4096 out of 8191) during tone and 0 between tones.

**RTC.** The v1 prototype intentionally uses the internal ESP32-C3 RTC rather than an external DS3231. The engineering question being validated in v1 is the BLE tap-exchange protocol, not timekeeping precision. The `rtc_driver` API (`rtc_get_time()`, `rtc_set_time()`) is designed to be time-source-agnostic: callers use `watch_time_t` structs and are unaware of whether the time source is the internal oscillator or an external I2C clock. Swapping to DS3231 for v2 requires implementing `rtc_driver.c` against the same header — no service or task code changes.

---

## 5. Test Strategy

Each test adds exactly one new component on top of the components verified by all prior tests. A failure in test N is unambiguously attributed to the component added in test N, because everything below it was already confirmed working.

| Test | New component verified |
|------|----------------------|
| 01 | `ssd1306_driver` — I2C init, framebuffer, flush |
| 02 | `button_driver` — ISR, queue, debounce, long-press |
| 03 | `buzzer_driver` — LEDC init, sound map |
| 04 | `rtc_driver` — time read/write, optional NTP sync |
| 05 | All four non-BLE drivers together — cross-driver interference |
| 06 | `ble_driver` Peripheral role — advertising packet content |
| 07 | `ble_driver` Central role — 4-gate scan filter (two boards) |
| 08 | GAP connection establishment — role assignment, MAC tiebreaker (two boards) |
| 09 | Full TapShare protocol — `ble_service`, NVS safe write, FIFO rotation (two boards) |

Test 09 is the current stable state of the repository. All nine tests pass.

---

## 6. Future work

### v1 UI (next planned upload)

The following screens are designed and will be implemented as the next milestone:

- **Watch face** — `HH:MM:SS` and date, updated on `TIME_EVT_SECOND_TICK`, large digit font
- **Sharing mode** — triggered by SELECT long-press; displays scan animation while in `BLE_STATE_SHARING`
- **Share result** — displays saved contact name for 4 seconds after `BLE_STATE_DONE`, then auto-returns to watch face
- **Contact book** — MODE button switches to paginated contact list from NVS
- **Contact detail** — long-press SELECT on a contact book entry shows full name, phone, and title

### v2 hardware

| Component | Change | What stays identical |
|-----------|--------|----------------------|
| LIS3DH accelerometer | Detect wrist tap gesture; replace button long-press trigger | `ble_service_on_button_event()` API unchanged — accelerometer driver posts same `button_event_t` |
| DS3231 external RTC | Higher timekeeping accuracy (±2 ppm vs. ±150 ppm internal) | `rtc_driver.h` API unchanged; callers unaffected |
| GC9A01 round display | 240×240 colour round OLED replaces 128×64 monochrome | `ui_service` rewritten for new resolution; `ble_service` and all drivers unchanged |
| Companion phone app | Writes user profile over BLE; removes `test_config.h` dependency | `ble_service_set_my_profile()` already exists; new GATT write characteristic added to `ble_driver` |

### Out of scope for v1

`alarm_service` and `stopwatch_service` are fully implemented in the repository but not yet integrated into the task layer. Both services depend on a reliable real-time clock with alarm interrupt support, which is not available on the internal ESP32-C3 RTC. Task-layer integration and activation are deferred to v2 when the DS3231 is added.
