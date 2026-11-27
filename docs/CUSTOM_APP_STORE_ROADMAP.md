# Custom StackChan App Store — Architecture

> **Status:** Implemented & Live (`APP.CENTER` Community Store + GitHub Actions CI/CD)

## Overview

StackChan's built-in **`APP.CENTER`** downloads a JSON catalog and flashes ESP32-S3 firmware images over HTTPS OTA without requiring a paid backend server.

```text
[GitHub Repo (main)]          [GitHub Actions CI]             [GitHub Releases + Raw]
  Modular CMake flags   ───►  ESP-IDF v6.1 Docker    ───►     *-ota.bin (Wi-Fi OTA)
  STACKCHAN_ENABLE_APP_*      builds packs & apps             *-merged.bin (M5Burner)
                                                              store/apps.json (catalog)
                                                                       │
                                                                       ▼ HTTPS GET + OTA
                                                              [StackChan APP.CENTER]
                                                              Community / Official / SD
```

## Key Components

| Component | Location | Role |
| --- | --- | --- |
| **Catalog** | [`store/apps.json`](../store/apps.json) | Static JSON list of packs and standalone apps (`name`, `version`, `description`, `firmwareUrl`). |
| **Device Loader** | [`firmware/main/hal/hal_app_center.cpp`](../firmware/main/hal/hal_app_center.cpp) | Fetches `store/apps.json` (or custom URL from `/sd/config/store.txt`), handles HTTPS 302 redirects, flashes OTA, and supports `/sd/apps/` offline caching. |
| **Modular Build** | [`firmware/main/CMakeLists.txt`](../firmware/main/CMakeLists.txt) | Per-app `STACKCHAN_ENABLE_APP_*` flags so every firmware stays well under the 5.17 MB OTA partition limit. |
| **CI/CD Pipeline** | [`.github/workflows/release-app-store.yml`](../.github/workflows/release-app-store.yml) | Builds all packs/apps on tag push (`store-v*`), generates `SHA256SUMS.txt`, and publishes a GitHub Release. |

## Returning to Official Firmware

Switch the toggle in **`APP.CENTER`** from **Community** to **Official** to install M5Stack's official apps over Wi-Fi, or re-flash the factory firmware using M5Burner.
