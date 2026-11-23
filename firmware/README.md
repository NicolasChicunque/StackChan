
## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```

## Built-in Apps

- `AI.AGENT` – Xiaozhi voice AI assistant
- `AVATAR` – Remote avatar / WebSocket / video call mode
- `ESPNOW.REMOTE` – ESP-NOW wireless controller
- `APP.CENTER` – Online firmware/app launcher
- `EZDATA` – EzData cloud integration
- `DANCE` – BLE dance controller
- `MUSIC` – MicroSD Music Player, Internet Radio & Web Remote (`.wav` & `.mp3` from `/sd`, live Wi-Fi MP3 radio streaming, phone/PC Web Remote with QR code and Wi-Fi MP3 upload, rhythm RGB LEDs & servo sway, NVS-persisted settings)
- `GEMINI` – Direct Gemini Live API voice assistant (`WSS` over Wi-Fi, hands-free VAD, voice & pitch settings, free-tier API key via `/sd/gemini.txt` + NVS)
- `SETUP` – Device settings, hardware tests, and app visibility toggles
