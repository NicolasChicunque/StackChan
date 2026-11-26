## MicroSD Music Player (`MUSIC`), Internet Radio & Gemini Live — Custom Firmware

This fork adds a native **MicroSD Music Player + Internet Radio + Web Remote (`MUSIC`)** and a direct **Gemini Live (`GEMINI`)** voice assistant to the StackChan ESP-IDF v6.1 firmware launcher alongside `AI.AGENT`, `AVATAR`, `DANCE`, and `SETUP`:

- **Offline audio playback**: Plays `.wav` (PCM 8/16-bit, mono/stereo) and `.mp3` files from a FAT32 microSD card mounted at `/sd` (recursive scan up to 3 levels deep, PSRAM-backed track index supporting 1,000+ songs).
- **Internet Radio over Wi-Fi**: Streams live HTTP/HTTPS MP3 radio stations directly inside `MUSIC -> Settings -> RADIO` with a 96 KB PSRAM prefetch ring buffer. Includes built-in stations and supports custom `Name|URL` entries in `/sd/radio.txt`.
- **Web Remote, QR Code & Wi-Fi MP3 Upload**: Open `MUSIC -> Settings -> WEB/QR` to launch a local web server (`http://<device-ip>/`) with a scannable QR code. From any phone or PC browser on the same Wi-Fi, search/play SD tracks, control transport/volume/lights/motion, play or add radio stations, and upload `.mp3`/`.wav` files directly to `/sd/`.
- **Minimalist touch UI**: Top status header (track/station title, battery indicator, touch-seekable progress bar, elapsed/total time, volume), animated avatar with floating musical notes, and a clean 6-button control bar (`VOL-`, `Prev`, `Play/Pause`, `Next`, `VOL+`, `Settings`).
- **Persistent preferences**: Volume, playback toggles, head calibration, and the last played track are automatically saved to NVS and restored on next launch.
- **Menu visibility toggle**: Can be shown or hidden from `SETUP -> Apps -> App visibility`.

> **Quick start**: Format a microSD card as FAT32, copy your `.mp3` or `.wav` files to the card, insert it into CoreS3, build & flash `firmware/` with ESP-IDF v6.1 (see [firmware/README.md](firmware/README.md)), and open **MUSIC** from the launcher.

### Community Suite, Custom App Store & MicroSD Layout

This public `feat/community-suite` branch combines all custom apps (`MUSIC` + Internet Radio + Web/QR Remote, `ALARM` + Timer/Stopwatch, `GEMINI`, `MCP`), the unified top status bar (**Home** + info + **`[- VOL +]`**), and a **Custom Community App Store** inside `APP.CENTER` (with a **Community / Official** toggle for Wi-Fi HTTPS OTA updates, plus `*-merged.bin` builds in [GitHub Releases](https://github.com/NicolasChicunque/StackChan/releases) ready for M5Burner).

#### MicroSD Card Layout (FAT32, mounted at `/sd`)

| Path on MicroSD | Purpose |
| --- | --- |
| `/sd/avatars/avatar_*.bin` | Optional custom LVGL v9 character sprites (loaded into PSRAM; falls back to the classic StackChan face if omitted). Enable via `SETUP -> Device -> Avatar -> Custom`. Full guide & conversion steps: [docs/CUSTOM_AVATARS.md](docs/CUSTOM_AVATARS.md). |
| `/sd/music/` *(or `/sd/`)* | `.mp3` and `.wav` files for **MUSIC** and **ALARM** ringtones (recursive scan up to 3 levels deep). |
| `/sd/radio.txt` | Optional custom Internet Radio stations (`Name|StreamURL`, one per line). |
| `/sd/gemini.txt` | Google AI Studio API key for **GEMINI** (auto-cached to NVS; optional line 2: model ID, line 3: voice). |
| `/sd/store.txt` | Optional custom JSON catalog URL override for **APP.CENTER** Community Store. |

----

# StackChan Open-Source

<img src="https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/K151_stack_chan_main_pictures_01.webp" width="60%">

Here are StackChan related open-source resources, including source code of the StackChan firmware, remote controller firmware, mobile app (iOS and Android), and server. 

Update of this repo could be a little late than the released firmware and mobile app. 

----

<img src="https://cdn.shopify.com/s/files/1/0056/7689/2250/files/5a589623895f65487717894d9240f6b8.png" width="60%">

**StackChan is a super kawaii AI desktop robot co-created by M5Stack and the user community.** It uses the M5Stack **flagship IoT development kit [CoreS3](https://docs.m5stack.com/en/core/CoreS3)** as its main controller, powered by an ESP32-S3 SoC featuring a 240 MHz dual-core processor, with 16MB Flash and 8MB PSRAM onboard, and supporting Wi-Fi and BLE. The main unit also integrates a 2.0-inch capacitive touch display with a high-strength glass cover, a 0.3 MP camera, a proximity & ambient light sensor, a 9-axis IMU (accelerometer + gyroscope + magnetometer), a microSD card slot, a 1W speaker, dual microphones, and power/reset buttons. 

The **robot body**, connected to the main unit, includes a USB-C interface for power and data, a 550 mAh battery, two feedback servos (360-degree continuous rotation on the horizontal axis and 90-degree movement on the vertical axis), two rows totaling 12 RGB LEDs, infrared transmitter and receiver, a three-zone touch panel, and a full-featured NFC module. 

The **factory firmware** is feature-rich, including an AI Agent, lively and expressive animations, ESP-NOW wireless remote control, and online app downloads. It can connect to a mobile app for video viewing, remote avatar control, and more, and also supports online updates (OTA). The product also supports programming via Arduino, UiFlow2, and other methods, and can connect to various expansion units in the M5Stack ecosystem, making it easy to implement a wide range of custom functions.

### Alarm Clock App (`AppAlarm`)

This branch adds a native **ALARM** launcher app (`firmware/main/apps/app_alarm/`) inspired by Google Clock:

- **Clock face** backed by the onboard PCF8563 RTC, timezone from **SETUP**, and one-tap NTP sync
- **Up to 6 alarms** with hour/minute, weekday schedule (or one-shot), label, and ringtone, persisted in NVS
- **4 synthesized ringtones** (Chime, Classic, Radar, Energetic) plus **MP3 files from the microSD card**, with a progressive volume ramp
- **Background triggering**: a lightweight `AlarmService` task keeps watching the clock while other apps run; it opens ALARM directly from the launcher, or performs a clean warm reboot straight into the ringing screen when another app owns the audio codec
- **Snooze / Stop** with large on-screen buttons, **head pet to snooze**, and **shake to stop**
- **NIGHT** nightstand mode dims the backlight until touched or until an alarm rings

> The device must stay powered on (USB-C or battery) for alarms to ring; a fully powered-off StackChan cannot wake itself. 

### Gemini Live Voice Assistant (`GEMINI`)

The firmware also includes a built-in **Gemini Live (`GEMINI`)** app that connects directly over Wi-Fi (`WSS`) to the Google Gemini Live API (`gemini-3.8-live`) with no external server required:

- **Direct speech-to-speech**: Streams 16 kHz PCM microphone audio with hands-free adaptive VAD (auto-sends after a natural pause) and plays back 24 kHz PCM voice replies with configurable voice & pitch, live captions, animated face, and RGB status LEDs.
- **Simple API key setup**: Save a free Google AI Studio API key in `/sd/gemini.txt` on a FAT32 microSD card (automatically cached in NVS on first open; optional second line overrides the model ID).
- **Menu visibility toggle**: Can be shown or hidden from `SETUP -> Apps -> App visibility`.

### Native MCP Voice & Robot Bridge (`MCP`)

This branch also adds a native **MCP (`AppMcp`)** app (`firmware/main/apps/app_mcp/`) that turns StackChan into a hands-free voice and robot interface for **Antigravity IDE** (see [host/stackchan-mcp/HOST_SERVER_INSTALL_GUIDE.md](host/stackchan-mcp/HOST_SERVER_INSTALL_GUIDE.md)):

![How StackChan MCP Works](docs/stackchan-mcp-flow.svg)

- **How the two paths work**:
  - **Casual chat (free tier)**: `stackchan_chat.py` runs in silent standby on your PC (local Faster-Whisper ASR locked to your OS language + local Piper TTS + `gemini-3.5-flash-lite`) without consuming Antigravity IDE quota.
  - **Engineering escalation**: When you ask to inspect/edit code, run Git/builds, use the camera, or look up live web info, it escalates to your active Antigravity conversation, which executes the task in the right workspace, replies aloud (or asks follow-ups via `stackchan_converse`), and returns StackChan to silent standby.
- **On-device controls (`MCP -> SET`)**: `AUTO` hands-free mode, silence `PAUSE`, `MIC` sensitivity, `VOLUME`, and `MOVE` (voice-synced gentle head motion with priority for explicit server gestures `/nod`, `/shake`, `/move`, `/home`), all saved to NVS.
- **`GEMINI` vs `MCP` at a glance**: Use **`GEMINI`** for standalone casual chat with no PC; use **`MCP`** + `/stackchanchat` when you want StackChan connected to your PC and active Antigravity project.

> ⚠️ Do not forcibly rotate any movable parts connected to the motors by hand when you are unsure whether the motors are powered and under control, as this may cause hardware damage. 

- Purchase link: [M5Stack Official Store](https://shop.m5stack.com/products/stackchan-kawaii-co-created-open-source-ai-desktop-robot) | [淘宝 Taobao](https://item.taobao.com/item.htm?id=1042238294510)

- Product document page: [English](https://docs.m5stack.com/en/StackChan) | [日本語](https://docs.m5stack.com/ja/StackChan) | [中文](https://docs.m5stack.com/zh_CN/StackChan)

- Board support package: https://github.com/m5stack/StackChan-BSP

Thank you to the contributors of the StackChan community, especially: 

| ![](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/avatar_stack_chan.jpg) | ![](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/avatar_takao.jpg) |
| -------------------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| [@stack_chan](https://x.com/stack_chan)                                          | [@mongonta555](https://x.com/mongonta555)                                   |
| Shinya Ishikawa                                                                  | Takao Akaki                                                                 |
