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
| **Community Suite** *(default)* | `-DSTACKCHAN_ENABLE_APP_MUSIC=ON -DSTACKCHAN_ENABLE_APP_ALARM=ON -DSTACKCHAN_ENABLE_APP_GEMINI=ON -DSTACKCHAN_ENABLE_APP_MCP=ON` |
| **Community Labs Pack (12 Apps)** | Set the 4 flags above to `OFF` and enable `FX`, `HEALTH`, `GAMES`, `DECK`, `NFC`, `HOME`, `CALENDAR`, `ENGLISH`, `TRAKT`, `FILES`, `COMPANION`, `SENTINEL` |
| **Standalone App** | Enable only one `STACKCHAN_ENABLE_APP_<NAME>=ON` and set the rest to `OFF` |

Example building **Music Player** standalone:

```bash
idf.py -DSTACKCHAN_ENABLE_APP_MUSIC=ON -DSTACKCHAN_ENABLE_APP_ALARM=OFF -DSTACKCHAN_ENABLE_APP_GEMINI=OFF -DSTACKCHAN_ENABLE_APP_MCP=OFF build
```

## Built-in Apps

- **Stock M5Stack:** `AI.AGENT`, `AVATAR`, `ESPNOW.REMOTE`, `APP.CENTER` *(with Community/Official store toggle & SD cache)*, `EZDATA`, `DANCE`, `SETUP`
- **Community Suite:** `MUSIC` (SD MP3/WAV + Internet Radio + Web/QR Remote), `ALARM` (Alarm/Timer/Stopwatch + RTC + SD MP3 ringtones + Nightstand), `GEMINI` (Direct Gemini Live API + Auto-Attach MCP bridge), `MCP` (Legacy HTTP bridge)
- **Community Labs Pack:** `FX`, `HEALTH`, `GAMES`, `DECK`, `NFC`, `HOME`, `CALENDAR`, `ENGLISH`, `TRAKT`, `FILES`, `COMPANION`, `SENTINEL`

## Host-Side Unit Tests

Motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```
