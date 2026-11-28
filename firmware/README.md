# StackChan Firmware (ESP-IDF v6.1)

Build and flash instructions for **StackChan Community Suite** on M5Stack CoreS3 (`esp32s3`).

## Prerequisites

- [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/index.html)
- Python 3.10+

## Quick Build & Flash

```bash
cd firmware
python3 ./fetch_repos.py
idf.py set-target esp32s3
idf.py build flash monitor
```

### Generate a Single M5Burner Image (`0x0`)

```bash
idf.py merge-bin -o stackchan-community-suite-merged.bin
```

Flash `stackchan-community-suite-merged.bin` at offset `0x0` with M5Burner or `esptool`.

## Modular App Selection (CMake Flags)

To stay comfortably inside the ESP32-S3 OTA partition limit, custom apps are compiled modularly via CMake options defined in [`main/CMakeLists.txt`](main/CMakeLists.txt):

| Pack | Flags |
| --- | --- |
| **Community Suite** *(default)* | `-DSTACKCHAN_ENABLE_APP_MUSIC=ON -DSTACKCHAN_ENABLE_APP_ALARM=ON -DSTACKCHAN_ENABLE_APP_GEMINI=ON -DSTACKCHAN_ENABLE_APP_MCP=ON -DSTACKCHAN_ENABLE_APP_SPEAKER=ON` |
| **Community Labs Pack (12 Apps)** | Set the 4 flags above to `OFF` and enable `FX`, `HEALTH`, `GAMES`, `DECK`, `NFC`, `HOME`, `CALENDAR`, `ENGLISH`, `TRAKT`, `FILES`, `COMPANION`, `SENTINEL` |
| **Standalone App** | Enable only one `STACKCHAN_ENABLE_APP_<NAME>=ON` and set the rest to `OFF` |

Example building **Music Player** standalone:

```bash
idf.py -DSTACKCHAN_ENABLE_APP_MUSIC=ON -DSTACKCHAN_ENABLE_APP_ALARM=OFF -DSTACKCHAN_ENABLE_APP_GEMINI=OFF -DSTACKCHAN_ENABLE_APP_MCP=OFF build
```

## Built-in Apps

- **Stock M5Stack:** `AI.AGENT`, `AVATAR`, `ESPNOW.REMOTE`, `APP.CENTER` *(with Community/Official store toggle & SD cache)*, `EZDATA`, `DANCE`, `SETUP`
- **Community Suite:** `MUSIC` (SD MP3/WAV + Internet Radio + Web/QR Remote), `ALARM` (Alarm/Timer/Stopwatch + RTC + SD MP3 ringtones + Nightstand), `GEMINI` (Direct Gemini Live API + Auto-Attach MCP bridge), `MCP` (Legacy HTTP bridge), `SPEAKER` (Wi-Fi Web/UDP + USB-C OTG UAC2 External Speaker with FFT lip-sync & dance)
- **Community Labs Pack:** `FX`, `HEALTH`, `GAMES`, `DECK`, `NFC`, `HOME`, `CALENDAR`, `ENGLISH`, `TRAKT`, `FILES`, `COMPANION`, `SENTINEL`

## Fast Local Iteration (Desktop UI Simulator & ccache Builds)

### 1. Desktop UI Simulator (LVGL 9.5 + SDL2, 320×240)

Preview and iterate on LVGL screens, layouts, QR codes, and status bar offsets on Windows in **~2 seconds** without flashing the robot:

```powershell
.\sim\run_sim.ps1            # Launch 3x3 Launcher, Avatar, Setup & Sandbox
.\sim\run_sim.ps1 -Sandbox   # Boot straight into sim_sandbox.cpp
```

See [`sim/README.md`](sim/README.md) for keyboard shortcuts and prototyping tips.

### 2. Accelerated ESP-IDF Builds (`ccache` + Single-App `app-flash`)

Use [`scripts/fast_build.ps1`](scripts/fast_build.ps1) to automatically enable `ccache`, build only the app you are working on in its own build directory, and flash only the app partition (`app-flash` at `921600` baud):

```powershell
.\scripts\fast_build.ps1 -App music -Flash -Port COM3
.\scripts\fast_build.ps1 -Pack suite -Flash -Monitor
.\scripts\fast_build.ps1 -Pack minimal
```

## Host-Side Unit Tests

Motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```
