---
name: stackchan-app-dev
description: >-
  Standard architecture, LVGL UI design guide, and ESP32-S3 hardware/audio/Wi-Fi/USB rules
  for creating, editing, or debugging applications in the StackChan Community Suite firmware
  (ESP-IDF 6.1, Mooncake, LVGL 9.5, M5Stack CoreS3). Make sure to use this skill whenever the
  user asks to create a new StackChan app, modify an existing app's UI or behavior, integrate
  the top status bar or avatar, add audio/Wi-Fi/USB-C/servo/RGB features, or build and flash
  StackChan firmware, even if they don't explicitly mention "skill" or "design guide".
---

# StackChan Community Suite App Development

Use this skill to build and maintain consistent, rock-solid applications for **StackChan** (`firmware/` on ESP32-S3 / M5Stack CoreS3, ESP-IDF 6.1, Mooncake, LVGL 9.5).

## Bundled References (Read as Needed)

- **[references/ui-design-guide.md](references/ui-design-guide.md)**: Read before creating or editing any LVGL screen, status bar, avatar, color palette, button layout, or user-facing strings.
- **[references/hardware-gotchas.md](references/hardware-gotchas.md)**: Read before writing FreeRTOS tasks, audio/FFT/I2S code, Wi-Fi/WebSocket/UDP streaming, USB-C OTG (UAC2), servo/RGB reactions, or ESP-IDF 6.1 build/flash commands.

## Standard Workflow for a New App

### 1. Plan & Scope
- Check available OTA flash headroom before adding large assets or dependencies (`ota_0` partition is `0x4f0000` ≈ 4.9 MB in `firmware/partitions.csv`).
- If two ideas share >70% of their core engine (e.g., audio playback + FFT + avatar + servos across Wi-Fi and USB-C), prefer **one unified app with a clear mode switcher** rather than duplicating flash footprint and launcher icons.
- Keep all user-facing UI text, Web UIs, and USB device/interface descriptors in **English** (consistent across the entire suite).

### 2. Create the App Folder (`firmware/main/apps/app_<name>/`)
Every app inherits from `mooncake::AppAbility` and implements:
- **Constructor**: Set `setAppInfo().name` (short uppercase, e.g. `"SPEAKER"`), `setAppInfo().icon` (loaded via `assets::get_image(...)`), and `setAppInfo().userData` (pointer to static theme color `uint32_t`).
- **`onCreate()`**: Lightweight logging only; defer heavy allocation to `onOpen()`.
- **`onOpen()`**:
  1. Load preferences from NVS (`Settings s("app_<name>", false)`).
  2. Configure hardware (servos/LEDs/audio) and allocate large buffers in **PSRAM** (`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` with internal RAM fallback).
  3. Build LVGL UI under `LvglLockGuard lock;` and install the shared chrome:
     ```cpp
     view::create_home_indicator([&]() { close(); }, THEME_COL, COL_BG);
     view::create_status_bar(THEME_COL, COL_BG);
     ```
  4. Start background tasks/services.
- **`onRunning()`**:
  - **ALWAYS** update the shared top bar and home indicator inside `LvglLockGuard lock;` every loop so the user's custom dropdown status bar (Home + Volume) responds to top-edge swipe gestures:
    ```cpp
    {
        LvglLockGuard lock;
        view::update_home_indicator();
        view::update_status_bar();
        // Throttled app UI refresh (e.g., every 50 ms)
    }
    ```
  - Call `GetStackChan().update();` at the end of `onRunning()`.
- **`onClose()`**:
  - Disconnect any `GetHAL()` signal connections (`onHeadPetGesture`, `onImuMotionEvent`).
  - Stop background tasks cleanly and wait for exit flags before freeing buffers.
  - Turn off RGB LEDs (`GetHAL().showRgbColor(0, 0, 0)`), return servos to home, and save NVS prefs.
  - Destroy chrome and UI under `LvglLockGuard`:
    ```cpp
    view::destroy_home_indicator();
    view::destroy_status_bar();
    destroy_ui();
    ```

### 3. Register Modularly in the Build System
Register every new app behind a CMake toggle so users can build either the full suite or standalone packs:
1. **`firmware/main/CMakeLists.txt`**:
   - Add `option(STACKCHAN_ENABLE_APP_<NAME> "Include <Description> app" ON)` (or `OFF` for Labs pack).
   - Add `if(NOT STACKCHAN_ENABLE_APP_<NAME>) list(FILTER STACK_CHAN_SOURCES EXCLUDE REGEX "/apps/app_<name>/") endif()`.
   - Export `STACKCHAN_ENABLE_APP_<NAME>=$<BOOL:${STACKCHAN_ENABLE_APP_<NAME>}>` in `target_compile_definitions`.
2. **`firmware/main/apps/apps.h`**: Include `"app_<name>/app_<name>.h"` guarded by `#if STACKCHAN_ENABLE_APP_<NAME>`.
3. **`firmware/main/main.cpp`**: Call `GetMooncake().installApp(std::make_unique<App<Name>>());` guarded by `#if STACKCHAN_ENABLE_APP_<NAME>`. *(Do not modify `hal_toggles` bitmask `visible_mask_v4` unless explicitly requested; unregistered apps remain visible in the launcher by default).*
4. **`firmware/README.md`**: Add the new app to the pack table and Built-in Apps list.

### 4. Pre-Flash Checklist
Before declaring an app ready, verify every item:
- [ ] All UI labels, status badges, Web UIs, and USB strings are in **English**.
- [ ] UI uses `community_helpers.h` palette/helpers and respects `view::get_status_bar_top_offset()` in both `Fixed` and `AutoHide` modes.
- [ ] `view::update_home_indicator()` and `view::update_status_bar()` are called inside `onRunning()` under `LvglLockGuard`.
- [ ] Avatar rendering strictly follows the global setting `stackchan::avatar::getAvatarStyle()` (never add a local avatar switcher inside an app).
- [ ] Every FreeRTOS `vTaskDelay(...)` in polling/idle loops waits at least `std::max<TickType_t>(2, pdMS_TO_TICKS(20))` (FreeRTOS runs at `100 Hz`, so `< 10 ms` evaluates to `0` ticks and starves `IDLE1` / LVGL).
- [ ] Audio-reactive dance/LEDs gate on real signal energy (`peak > 150`), not just packet arrival, so paused USB/Wi-Fi streams stay still.
- [ ] `idf.py build` succeeds cleanly and leaves comfortable free space in `ota_0`.
