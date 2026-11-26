# StackChan Community — Project Roadmap

Prioritized, public-first engineering roadmap for the StackChan modular firmware (`feat/community-suite`) and `stackchan-mcp` voice bridge.

> **Guiding Principle:** *Keep it simple.* Every app and improvement is designed to be public, clean, copyright-free, and modular so any StackChan owner can install single apps or packs via `APP.CENTER` or M5Burner.

---

## Completed Milestones

- **Custom Community App Store (`APP.CENTER` + GitHub Releases CI/CD):** Live at `store-v1.2.0` with **Community / Official** toggle, HTTPS cert bundle + 302 redirect support, modular per-app CMake flags (`STACKCHAN_ENABLE_APP_*`), and both `*-ota.bin` (Wi-Fi) and `*-merged.bin` (M5Burner) artifacts.
- **MicroSD Personalization (`/sd/avatars/` & `/sd/config/`):** Abstract custom LVGL `.bin` sprite loading from MicroSD (see [`docs/CUSTOM_AVATARS.md`](./CUSTOM_AVATARS.md)) with classic vector face fallback, plus organized `/sd/music/`, `/sd/config/`, and `/sd/apps/` folders.
- **Unified Top Status Bar & App Visibility:** Global `Home` + info + `[- VOL +]` top bar and global-only Avatar Style setting in `SETUP`.

---

## 1. Gemini Live API Real-Time Voice Bridge (`AppMcp` + `stackchan-mcp`) — **NEXT UP (P0)**

**Goal:** Drop casual `FREE`-tier voice latency from ~4–5 s (HTTP WAV + Whisper + Interactions API + local TTS) to **`< 1 s`** with natural native voice and real-time interruption (*barge-in*), while keeping **0 IDE quota** for everyday chat and preserving escalation to **`AGENT $`**.

- **Complementary + Primary/Fallback:** Keep the existing HTTP WAV + Faster-Whisper + Interactions API + local Piper/Edge TTS pipeline intact; add `gemini-live` WebSocket mode in parallel and keep the other as automatic fallback.
- **ESP32-S3 Firmware (`AppMcp`):** Add low-latency bidirectional PCM streaming (16 kHz / 24 kHz) while keeping `IDLE` and `WAITING` mic safeguards.

---

## 2. Offline MicroSD App Cache (`APP.CENTER` -> `/sd/apps/`) — **P1**

- Optionally cache downloaded `.bin` firmwares in `/sd/apps/` and add an **`SD Card`** tab in `APP.CENTER` for 5-second offline app switching (M5Launcher-style) without Wi-Fi.
- Full store specification: [`docs/CUSTOM_APP_STORE_ROADMAP.md`](./CUSTOM_APP_STORE_ROADMAP.md).

---

## 3. Hardware Inputs & Sensor Brainstorming (Upgrading Current & Future Apps) — **P1**

Make better use of everything built into the CoreS3 + StackChan body so apps feel alive and effortless without touching tiny UI buttons:

| Hardware Sensor / Input | Ideas for Existing Apps | Ideas for Future Apps |
| --- | --- | --- |
| **3-Zone Capacitive Head Touch** | `MUSIC`: tap = play/pause, swipe L/R = prev/next track. `ALARM`: pet = snooze / wake from Night. `MCP`: long-press = push-to-talk. | `Presence/Breaks`: pet to acknowledge a water/stretch break. `Companion`: petting raises happiness/affection. |
| **NFC Reader / Writer** | Tap an NFC tag to launch a specific radio station, playlist, alarm profile, or switch `APP.CENTER` app. | `NFC Studio`: scan/clone/write tags, trigger Smart Home scenes or Stream Deck macros by placing a card/figure on the robot. |
| **Dual Microphones (Voice Dictation)** | Voice-set alarms (`"Alarm at 7:30"`), voice search songs/stations in `MUSIC`, dictate quick notes/calendar events. | `English Teacher`: pronunciation & conversation drills. `Board Games`: voice-announced dice/rules. |
| **0.3 MP Camera + Proximity / Ambient Light** | Auto-dim screen in dark rooms (`ALARM` Night mode auto-trigger). | `Presence Monitor`: detect desk presence for posture/active-break & water reminders. `File Explorer`: quick photo capture to `/sd/`. |
| **9-Axis IMU (Shake / Tilt)** | `ALARM`: shake to dismiss. `MUSIC`: shake to shuffle next track. | `Board Games`: physically shake StackChan to roll virtual dice! |
| **Servos + 12 RGB LEDs + IR Blaster** | Visual timers (LED ring countdown), mood lighting synced to music/status. | `Smart Home / TV`: use the built-in IR transmitter to control TVs/ACs! `Companion`: autonomous idle glances and emotional postures. |

---

## 4. Future Public Apps Backlog (*Keep It Simple*) — **P2**

All future apps will live as isolated modular folders in `feat/community-suite` (compiled on demand via CMake flags so we never hit the 5.17 MB OTA partition limit):

### Quick Wins (Simple, High Utility on ESP32-S3)
1. **Currency Converter (`FX`):** Live USD/COP (and major currencies) exchange rate viewer using a free public JSON API + simple touch keypad converter.
2. **Presence Monitor & Active Breaks (`HEALTH`):** Desk presence & timer-based reminders to stretch, rest eyes (20-20-20 rule), and **drink water** — using proximity/ambient sensor + gentle head nod + LED pulse.
3. **Board Game Companion (`GAMES`):** Shake-to-roll dice (D4/D6/D8/D10/D12/D20), turn timer, life/score counter, and optional voice-assisted tabletop referee.
4. **Stream Deck (`DECK`):** Customizable 6–8 button macro pad sending HTTP/WebSocket/BLE shortcuts to PC (OBS, mute mic, media, IDE tasks).
5. **NFC Studio (`NFC`):** Read tag UID/NDEF records, write simple text/URL tags, and bind tags to local StackChan actions or webhooks.

### Medium Scope (Connected Services & Productivity)
6. **Smart Home & IR TV Remote (`HOME`):** Control TVs/ACs directly via StackChan's built-in **IR transmitter**, plus simple webhook / Home Assistant / Google Home triggers over Wi-Fi.
7. **Google Calendar (`CALENDAR`):** Glanceable daily agenda, next-meeting countdown, and gentle pre-meeting chime/head-nod alerts.
8. **English Language Teacher (`ENGLISH`):** Short daily speaking & vocabulary sessions with Duolingo-style **streaks & habit tracking** (scoped specifically to this app to keep it focused).
9. **Trakt.tv Tracker (`TRAKT`):** Check "Up Next" episodes, upcoming movie/show releases, and quick mark-as-watched via Trakt API.
10. **File Explorer (`FILES`):** Browse MicroSD folders (`/sd/music`, `/sd/avatars`, `/sd/config`), preview `.jpg`/`.bmp` images, manage files, and optionally sync with **Google Drive** *(note: smooth full-motion video playback on ESP32-S3 is limited to low-FPS MJPEG/animated GIFs, so focus on images/files first)*.

### Creative & Exploratory
11. **Autonomous Companion (`COMPANION` — Low-Maintenance Tamagotchi):** An autonomous desktop buddy with its **own lightweight personality, mood, daily routine, and evolving mini-story** — designed to be *unattended* (never stressful or needy like a classic Tamagotchi), with simple local state/rules (plus optional lightweight local/free chat that feels distinct from the utility `GEMINI` assistant).
12. **CI/CD & Unity Build Sentinel (`SENTINEL` — Exploratory):** Ambient build-status monitor (GitHub Actions / Unity Cloud Build webhooks) using RGB LEDs, happy/sad expressions, and audio alerts when a build passes or breaks.
