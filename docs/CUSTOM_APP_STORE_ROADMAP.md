# Roadmap: Custom StackChan App Store & Public Distribution

> **Status:** Planning (not implemented yet)  
> **Goal:** Distribute custom StackChan apps (`MUSIC`, `ALARM`, `GEMINI`, `MCP`, and future apps) publicly from your own catalog — without relying on M5Stack's backend and while keeping personal customizations (`custom/naruto`) private.

---

## 1. How the Built-in `APP.CENTER` Works Today

Based on [`firmware/main/apps/app_app_center/app_app_center.cpp`](../firmware/main/apps/app_app_center/app_app_center.cpp) and [`firmware/main/hal/hal_app_center.cpp`](../firmware/main/hal/hal_app_center.cpp):

- `Hal::fetchAppList()` performs a single HTTP `GET` to `<server>/stackChan/apps` and parses a JSON array (or `{"code": 0, "data": [...]}`) where each entry has:
  - `name` (or `appName`)
  - `description` (or `appDescription` / `desc`)
  - `iconUrl` (or `appIcon` / `icon`)
  - `firmwareUrl` (or `downloadUrl` / `otaUrl` / `url`)
- `Hal::launchApp(firmwareUrl, onProgress)` downloads a full ESP32-S3 firmware `.bin` over HTTPS OTA (`start_ota_update`) and boots into it.

**Key insight:** No paid backend server is needed. A static `apps.json` hosted on **GitHub Pages** (or `raw.githubusercontent.com`) pointing to `.bin` assets in **GitHub Releases** is 100% compatible with the existing `Hal::fetchAppList()` + OTA loader.

---

## 2. Target Architecture (Zero-Cost Static Store)

```text
[Public Repo / Branches]            [GitHub Actions CI]             [GitHub Pages + Releases]
  feat/music-player          ──┐
  feat/alarm-clock           ──┼──► Build clean ESP-IDF .bin  ───►  Releases: *.bin + SHA256
  feat/gemini-live           ──┤    (no private Naruto assets)      Pages:    apps.json + icons
  feat/mcp-bridge            ──┤
  feat/community-suite (all) ──┘
                                                                               │
                                                                               ▼  HTTPS GET + OTA
                                                                    [StackChan APP.CENTER]
                                                                    Toggle: Official / Custom Store
```

---

## 3. Repository & Branch Separation Strategy

| Branch / Repo | Visibility | Contents |
| --- | --- | --- |
| `main` | Public | Clean upstream M5Stack firmware (`Forairaaaaa/StackChan`) |
| `feat/music-player`, `feat/alarm-clock`, `feat/gemini-live`, `feat/mcp-bridge` | Public | Clean individual apps using stock/public assets only |
| `feat/community-suite` *(planned)* | Public | All-in-one public firmware combining all custom apps without private sprites |
| `custom/naruto` | Private (or personal fork) | Personal build with Naruto sprites and private tweaks |
| `stackchan-mcp` (`master`) | Public | Host MCP server, `/stackchanchat` voice loop, install guide |

---

## 4. Phased Implementation Plan

### Phase 1 — Clean Public Release Branch & Manual Catalog (`MVP`)
- [ ] Create clean `feat/community-suite` branch from `main` merging `MUSIC`, `ALARM`, `GEMINI`, and `MCP` without private Naruto assets.
- [ ] Verify partition size (`stack-chan.bin` fits within the OTA app partition with safety margin).
- [ ] Publish first tagged GitHub Release (`v1.0.0`) with prebuilt `stack-chan.bin` (and optional per-app `.bin` builds).
- [ ] Create static catalog file `store/apps.json` + PNG icons published via GitHub Pages.

### Phase 2 — Custom Store URL Support in Firmware (`APP.CENTER`)
- [ ] Update `firmware/main/hal/hal_app_center.cpp` so `get_app_info_list_url()` supports a configurable catalog URL (persisted in NVS, optional override via `/sd/store.txt`, fallback to official M5Stack URL).
- [ ] Add a simple **Official / Custom Store** toggle in `SETUP` or `APP.CENTER`.
- [ ] Verify HTTPS certificate bundle (`esp_crt_bundle_attach`) works with GitHub Releases redirects (`302` → `objects.githubusercontent.com`).
- [ ] Test end-to-end OTA install and rollback safety on physical CoreS3 hardware.

### Phase 3 — Automated Build & Publishing Pipeline (`CI/CD`)
- [ ] Add a GitHub Actions workflow using ESP-IDF v6.1 Docker image to build `.bin` artifacts on tag push.
- [ ] Auto-generate `apps.json` (version, release notes, firmware URL, binary size) and deploy to GitHub Pages.
- [ ] Optionally add a simple web flasher page (`esp-web-tools`) on the same GitHub Pages site so first-time users can flash the initial firmware from Chrome over USB-C without installing ESP-IDF.

### Phase 4 — Polish & Security Hardening
- [ ] Add SHA-256 verification metadata in `apps.json` and check before applying OTA.
- [ ] Add version display / "installed vs available" badge in `AppListPage`.
- [ ] Document how users switch back to the official M5Stack firmware at any time.

---

## 5. Open Decisions to Revisit Before Phase 1
1. **Distribution granularity:** Offer one combined `Community Suite` firmware (all apps together + `SETUP -> App visibility` toggles), individual per-app `.bin` builds, or both?
2. **First-time onboarding:** Include an `esp-web-tools` browser flasher on GitHub Pages so new users don't need ESP-IDF or `esptool`?
3. **Private branch hosting:** Keep `custom/naruto` in a separate private GitHub repository (mirroring `main` + public branches) since GitHub visibility is per-repository rather than per-branch.
