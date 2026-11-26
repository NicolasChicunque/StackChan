# StackChan MCP — Project Roadmap

Prioritized engineering roadmap for the StackChan custom firmware (`custom/naruto`) and `stackchan-mcp` voice bridge.

---

## 1. Gemini Live API Real-Time Voice Bridge (`AppMcp` + `stackchan-mcp`) — **NEXT UP (P0)**

**Goal:** Drop casual `FREE`-tier voice latency from ~4–5 s (HTTP WAV + Whisper + Interactions API + local TTS) to **`< 1 s`** with natural native voice and real-time interruption (*barge-in*), while keeping **0 IDE quota** for everyday chat and preserving escalation to **`AGENT $`** (Antigravity / Claude Code / Codex).

### Why
- Official Google AI Studio pricing confirms **Gemini Live API includes a Free Tier** (audio input & output free of charge within rate/session limits).
- Eliminates the 1.5 s silence pause + full-utterance WAV upload + local ASR + TTS round-trip on the `FREE` path.

### Design Principle (Complementary + Primary/Fallback)
- **Non-destructive:** Do **not** replace or break the current working HTTP WAV + Faster-Whisper + Interactions API + local Piper/Edge TTS flow.
- **Complementary modes:** Ship Gemini Live API as a parallel/selectable mode alongside the current bridge.
- **Best-wins priority with fallback:** After real-world testing, set whichever mode performs better (latency, stability, voice quality, escalation reliability) as the **primary** mode, and keep the other as an automatic **fallback / backup**.

### Architecture Plan
1. **PC Bridge (`stackchan-mcp`):**
   - Add a `gemini-live` voice loop mode using `google-genai` Live WebSocket API (native audio in/out + live transcription + `escalate_to_engineer` tool/function call).
   - Keep `POST /agent_mode` (`free` / `engineer` / `idle`) and `--context` handoff intact so the dual avatars (`● FREE` / `● AGENT $`) and coding-agent escalations work seamlessly.
   - Retain current Whisper + Piper/Edge + Interactions API mode as an offline-ASR/fallback option.
2. **ESP32-S3 Firmware (`AppMcp`):**
   - Add low-latency bidirectional PCM streaming (WebSocket or UDP PCM frames, 16 kHz / 24 kHz) between `AppMcp` and the PC bridge while in `FREE` Live mode.
   - Support hardware-friendly half-duplex / barge-in handling and keep `WAITING` (mic OFF) during `AGENT $` escalations.
   - Keep `IDLE` (mic OFF until `/stackchanchat` connects) and non-clickable stage safeguards.

---

## 2. Custom Static App Store (GitHub Pages + Releases) — **P1**

- Zero-backend app catalog reusing the existing `APP.CENTER` (`Hal::fetchAppList()` + HTTPS OTA).
- Full specification: [`docs/CUSTOM_APP_STORE_ROADMAP.md`](./CUSTOM_APP_STORE_ROADMAP.md).

---

## 3. Dual-Avatar Polish & Public Branch Readiness — **P2**

- Optional second distinct character sprite pack for the right `● AGENT $` slot (checking `assets` partition budget).
- Upstream/public `main` branch packaging using the classic dual StackChan face fallback (zero dependency on custom Naruto assets).
