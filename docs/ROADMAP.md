# StackChan Community Suite — Roadmap

Public-first engineering roadmap for the StackChan modular firmware (`main` branch) and the optional `stackchan-mcp` voice bridge.

> **Guiding Principle:** *Keep it simple.* Every app is built to be modular, clean, and easy to install via **`APP.CENTER`** or **M5Burner**.

---

## Completed Milestones

- **Custom Community App Store (`APP.CENTER` + CI/CD):** Live via [GitHub Releases](https://github.com/NicolasChicunque/StackChan/releases) with **Community / Official** toggle, HTTPS cert bundle + 302 redirect handling, modular CMake flags (`STACKCHAN_ENABLE_APP_*`), `*-ota.bin` (Wi-Fi) and `*-merged.bin` (M5Burner) artifacts, and `/sd/apps/` offline SD caching.
- **Unified `GEMINI` + Auto-Attach MCP Bridge:** Native ESP32-S3 Gemini Live WebSocket voice assistant with dual-avatar UI (`FREE` / `AGENT $`), voice & pitch selector with live preview, `pause_listening` voice mute tool, on-screen MCP onboarding, and seamless handoff to Antigravity IDE.
- **3×3 Scrollable Launcher & Unified Top Status Bar:** Clean scrollable app grid, global `Home` + info + `[- VOL +]` top bar, and global Avatar Style setting in `SETUP`.
- **MicroSD Personalization (`/sd/avatars/` & `/sd/config/`):** Custom LVGL v9 `.bin` sprite support ([`docs/CUSTOM_AVATARS.md`](CUSTOM_AVATARS.md)) with classic vector face fallback, plus organized `/sd/music/`, `/sd/config/`, and `/sd/apps/` folders.
- **Community Labs Pack (12 Apps):** Modular pack available in `APP.CENTER` (`FX`, `HEALTH`, `GAMES`, `DECK`, `NFC`, `HOME`, `CALENDAR`, `ENGLISH`, `TRAKT`, `FILES`, `COMPANION`, `SENTINEL`).

---

## Next Up & Future Ideas

### 1. Store & Distribution Polish
- **SHA-256 OTA Verification:** Verify checksum metadata from `store/apps.json` before applying OTA updates.
- **Installed vs. Available Badges:** Show currently installed version alongside available updates in `APP.CENTER`.
- **Optional Web Flasher (`esp-web-tools`):** One-click Chrome USB-C flasher via GitHub Pages as a complement to M5Burner.

### 2. Deeper Hardware Sensor Integration
Make more use of the CoreS3 + StackChan body sensors across existing and Labs apps:

| Sensor / Input | Planned Enhancements |
| --- | --- |
| **3-Zone Capacitive Head Touch** | `MUSIC`: tap = play/pause, swipe L/R = prev/next. `COMPANION`: petting interaction. |
| **NFC Reader / Writer** | Tap NFC tags to launch radio stations, playlists, alarms, or Smart Home webhooks. |
| **Camera + Ambient Light** | Auto-dim backlight in dark rooms (`ALARM` Night mode auto-trigger), desk presence detection in `HEALTH`. |
| **9-Axis IMU** | Shake-to-shuffle in `MUSIC` and richer gesture triggers in `GAMES`. |
| **IR Blaster & RGB LEDs** | Expanded IR remote profiles in `HOME` and ambient timer/notification LED feedback. |
