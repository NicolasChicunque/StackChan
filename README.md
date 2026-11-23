## MicroSD Music Player (`MUSIC`), Internet Radio & Gemini Live — Custom Firmware

This fork adds a native **MicroSD Music Player + Internet Radio + Web Remote (`MUSIC`)** and a direct **Gemini Live (`GEMINI`)** voice assistant to the StackChan ESP-IDF v6.1 firmware launcher alongside `AI.AGENT`, `AVATAR`, `DANCE`, and `SETUP`:

- **Offline audio playback**: Plays `.wav` (PCM 8/16-bit, mono/stereo) and `.mp3` files from a FAT32 microSD card mounted at `/sd` (recursive scan up to 3 levels deep, PSRAM-backed track index supporting 1,000+ songs).
- **Internet Radio over Wi-Fi**: Streams live HTTP/HTTPS MP3 radio stations directly inside `MUSIC -> Settings -> RADIO` with a 96 KB PSRAM prefetch ring buffer. Includes built-in stations and supports custom `Name|URL` entries in `/sd/radio.txt`.
- **Web Remote, QR Code & Wi-Fi MP3 Upload**: Open `MUSIC -> Settings -> WEB/QR` to launch a local web server (`http://<device-ip>/`) with a scannable QR code. From any phone or PC browser on the same Wi-Fi, search/play SD tracks, control transport/volume/lights/motion, play or add radio stations, and upload `.mp3`/`.wav` files directly to `/sd/`.
- **Minimalist touch UI**: Top status header (track/station title, battery indicator, touch-seekable progress bar, elapsed/total time, volume), animated avatar with floating musical notes, and a clean 6-button control bar (`VOL-`, `Prev`, `Play/Pause`, `Next`, `VOL+`, `Settings`).
- **Persistent preferences**: Volume, playback toggles, head calibration, and the last played track are automatically saved to NVS and restored on next launch.
- **Menu visibility toggle**: Can be shown or hidden from `SETUP -> Apps -> App visibility`.

> **Quick start**: Format a microSD card as FAT32, copy your `.mp3` or `.wav` files to the card, insert it into CoreS3, build & flash `firmware/` with ESP-IDF v6.1 (see [firmware/README.md](firmware/README.md)), and open **MUSIC** from the launcher.

### Personal Branch Customization (`custom/naruto`)

This branch combines `feat/music-player`, `feat/music-wifi`, and `feat/gemini-live`, and adds a custom 7-state Naruto pixel-art sprite set (`Calm`, `Thinking`, `Happy`, `Sleepy`, `Shy`, `Smug`, `Pouty`) cached in PSRAM for smooth voice playback, with a live NVS-persisted **Classic / Naruto** avatar toggle in both `SETUP -> Device -> Avatar` and `MUSIC -> Settings` (synced across `AI.AGENT`, `GEMINI`, and `MUSIC`).

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

### Gemini Live Voice Assistant (`GEMINI`)

The firmware also includes a built-in **Gemini Live (`GEMINI`)** app that connects directly over Wi-Fi (`WSS`) to the Google Gemini Live API (`gemini-3.8-live`) with no external server required:

- **Direct speech-to-speech**: Streams 16 kHz PCM microphone audio with hands-free adaptive VAD (auto-sends after a natural pause) and plays back 24 kHz PCM voice replies with configurable voice & pitch, live captions, animated face, and RGB status LEDs.
- **Simple API key setup**: Save a free Google AI Studio API key in `/sd/gemini.txt` on a FAT32 microSD card (automatically cached in NVS on first open; optional second line overrides the model ID).
- **Menu visibility toggle**: Can be shown or hidden from `SETUP -> Apps -> App visibility`.

> ⚠️ Do not forcibly rotate any movable parts connected to the motors by hand when you are unsure whether the motors are powered and under control, as this may cause hardware damage. 

- Purchase link: [M5Stack Official Store](https://shop.m5stack.com/products/stackchan-kawaii-co-created-open-source-ai-desktop-robot) | [淘宝 Taobao](https://item.taobao.com/item.htm?id=1042238294510)

- Product document page: [English](https://docs.m5stack.com/en/StackChan) | [日本語](https://docs.m5stack.com/ja/StackChan) | [中文](https://docs.m5stack.com/zh_CN/StackChan)

- Board support package: https://github.com/m5stack/StackChan-BSP

Thank you to the contributors of the StackChan community, especially: 

| ![](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/avatar_stack_chan.jpg) | ![](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/avatar_takao.jpg) |
| -------------------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| [@stack_chan](https://x.com/stack_chan)                                          | [@mongonta555](https://x.com/mongonta555)                                   |
| Shinya Ishikawa                                                                  | Takao Akaki                                                                 |
