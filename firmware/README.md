
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

## Alarm Clock App (`AppAlarm`)

- **Screens**: Clock (`ALARMS` | `NIGHT` | `SET` | `EXIT`), Alarm list (toggle ON/OFF, `+ NEW`), Editor (time, weekdays, tag, tone, `TEST`, `SD MP3`), Settings (snooze `1–30 min`, max volume, `SYNC NTP`, `TEST RING`), and Ringing (`SNOOZE` / `STOP`).
- **Background service**: `AlarmService` (`alarm_service.cpp`) is started from `main.cpp` and polls the system clock every 500 ms. Alarms and snooze state are stored in the `alarm_clock` NVS namespace.
- **SD ringtones**: any `.mp3` file on the microSD card (up to 2 folder levels deep). If the card cannot be read when an alarm fires, the Chime tone is used as a fallback.
