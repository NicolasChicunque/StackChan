# StackChan ESP32-S3 Hardware, Audio, Wi-Fi & USB-C Gotchas

Battle-tested engineering rules from the StackChan Community Suite (`M5Stack CoreS3` / `ESP32-S3`, ESP-IDF 6.1).

## 1. Bluetooth Classic Does Not Exist on ESP32-S3
- The ESP32-S3 only has **Bluetooth 5.0 LE (BLE)** — no Bluetooth Classic (BR/EDR) radio and no BLE Audio ISO (LC3).
- Classic Bluetooth A2DP speaker libraries (`ESP32-A2DP`) only work on original ESP32 boards (Basic/Fire/Core2), never CoreS3.
- For external speaker / audio sink features on StackChan, use **Wi-Fi** (WebSocket/HTTP/UDP) and/or **USB-C OTG (UAC2)**.

## 2. FreeRTOS Tick Rate (`CONFIG_FREERTOS_HZ = 100`)
- One FreeRTOS tick is **10 ms**.
- **Critical trap**: `pdMS_TO_TICKS(8)` (or any value `< 10`) evaluates to **`0` ticks**! Calling `vTaskDelay(0)` from a task with priority `> 1` never blocks, starves `main` (LVGL UI freezes on boot/open), and triggers `task_wdt` on `IDLE1`.
- **Rule**: Always guard short delays with `vTaskDelay(std::max<TickType_t>(2, pdMS_TO_TICKS(20)))` (or `>= 1` tick minimum), and keep background audio tasks at priority `3` rather than `5`.

## 3. Audio Codec (AW88298) & Silence Detection
- Native codec sample rate is **`24000 Hz` mono, 16-bit signed PCM** (`AUDIO_OUTPUT_SAMPLE_RATE = 24000` in `hal/board/config.h`).
- Allocate large PCM ring buffers in **PSRAM** (`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`, e.g., `96 KB` = 2 s @ 24 kHz).
- Feed `Board::GetInstance().GetAudioCodec()->OutputData(chunk)` in `20 ms` chunks (`480` samples) for smooth I2S DMA, and throttle FFT computation to every ~40 ms (matching 20–25 FPS UI updates).
- **Silence gating for servos & RGB**: Hosts like Windows UAC2 keep streaming continuous zero-filled (`0x0000`) PCM packets even when media is paused. Never drive dance/LEDs purely from packet arrival (`_last_rx_ms`). Compute sample peak amplitude (`abs(sample) > 150`) to maintain a separate `_last_sound_ms` timestamp so StackChan stops dancing, turns off LEDs, and returns to home pose cleanly when paused.

## 4. Real-Time Wi-Fi Audio Streaming
- ESP-IDF defaults to **Wi-Fi Modem Sleep (`WIFI_PS_MIN_MODEM`)**, which puts the radio to sleep for 100–300 ms DTIM intervals and causes severe stuttering in live WebSocket/UDP audio.
- **Rules for smooth Wi-Fi streaming**:
  1. Call `esp_wifi_set_ps(WIFI_PS_NONE);` when entering live Wi-Fi streaming, and restore `esp_wifi_set_ps(WIFI_PS_MIN_MODEM);` on stop/close.
  2. Enable `TCP_NODELAY` on WebSocket sockets (`setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))`) to eliminate 200 ms delayed-ACK stalls.
  3. Enable `CONFIG_HTTPD_WS_SUPPORT=y` in `sdkconfig.defaults`.
  4. Use a **250 ms pre-buffer** (`6000` samples) and ~`1 s` max jitter headroom in Wi-Fi mode, small browser frames (`1024` samples ≈ 2 KB, avoiding TCP fragmentation), a static receive buffer in `handle_ws` (no per-packet `malloc/free`), and a `GainNode` at `0` in Web Audio API so the browser does not echo locally.
  5. Always create Wi-Fi / LWIP / `httpd` tasks with **internal SRAM** stacks (never PSRAM stacks for Wi-Fi/TLS tasks).

## 5. USB-C OTG Audio Class (UAC2)
- Component: `espressif/usb_device_uac: ^1.3.2` (compatible with ESP-IDF 6.1).
- **One-shot on-demand init**: The ESP32-S3 shares a single USB-C PHY between USB-Serial-JTAG (`COM3` flashing/monitor) and USB-OTG. Never call `uac_device_init()` at boot — call it **only when the user explicitly selects USB-C mode**, using a global static trampoline guarded by a mutex so re-entering the app works cleanly and closing/switching modes safely detaches.
- **Windows 10/11 compatibility**:
  - Use **2 channels stereo @ 48 kHz, 16-bit** (`CONFIG_UAC_SPEAKER_CHANNEL_NUM=2`, `CONFIG_UAC_MIC_CHANNEL_NUM=0`, `CONFIG_UAC_SAMPLE_RATE=48000`). Windows often refuses to select 1-channel mono UAC2 speakers.
  - Downsample 48 kHz stereo → 24 kHz mono on the fly with `(L0 + R0 + L1 + R1) >> 2`.
  - Set `CONFIG_UAC_TUSB_PRODUCT="StackChan Speaker"` and ensure string descriptor index `4` in `usb_descriptors.c` uses `CONFIG_UAC_TUSB_PRODUCT` instead of `"usb uac"`. Bump `CONFIG_UAC_TUSB_PID` (e.g., `0x8001`) whenever changing channel count or descriptors so Windows clears cached audio endpoint metadata.

## 6. ESP-IDF 6.1 Build & Flash on Windows
- Environment paths on this machine:
  - `IDF_PATH=C:\Users\Nicolas\xs-dev\esp-idf-v6.1`
  - `IDF_TOOLS_PATH=C:\Users\Nicolas\xs-dev\espressif-tools-6.1`
- Avoid PowerShell script execution policy issues by invoking via `cmd /c` (note: no trailing space before `&&` on `set`):
  ```powershell
  cmd /c "set IDF_TOOLS_PATH=C:\Users\Nicolas\xs-dev\espressif-tools-6.1&& call C:\Users\Nicolas\xs-dev\esp-idf-v6.1\export.bat && cd /d d:\StackChan\StackChan\firmware && idf.py -p COM3 app-flash"
  ```
- Use `app-flash` for fast diff-flashing (~1–12 s) when `partitions.csv` and `assets` have not changed.
- Note that `Settings::GetInt(...)` returns `int32_t` (`long int` on Xtensa), so write `std::clamp<int>(s.GetInt(...), lo, hi)` explicitly to avoid template deduction errors.
