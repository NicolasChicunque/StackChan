---
name: stackchanchat
description: >-
  Token-efficient voice conversation loop with StackChan using the direct Gemini
  Interactions API (local Piper / Edge TTS + local Faster-Whisper ASR with
  automatic language detection). Activate when the user invokes /stackchanchat,
  or asks to start voice chat with StackChan. Runs in a background process and
  escalates back to the main agent only when coding, repo edits, builds, or
  heavy technical tasks are requested.
---

# StackChan Voice Chat (`/stackchanchat`)

Run voice conversations with StackChan outside the Antigravity context window
by launching `scripts/stackchan_chat.py` in the background. It uses local
Faster-Whisper ASR (locked to the operating system's language by default,
switching only if the user explicitly asks to change languages, or honoring
`--lang`), local Piper / Edge TTS, and the direct Gemini Interactions API
(`gemini-3.5-flash-lite` by default) so Antigravity IDE quota is preserved for
engineering work.

## Procedure

1. **Preflight**: Call `stackchan_status` once. If it times out, retry once and
   tell the user to open the `MCP` app on StackChan.

2. **Resolve arguments** (optional):
   - `--lang`: defaults to `auto` (resolves to the OS language at startup and
     stays locked unless the user explicitly asks to switch languages); pass
     an explicit ISO code such as `--lang es` or `--lang en` if requested.
   - `--model`: defaults to `gemini-3.5-flash-lite`; pass `--model gemini-4-hydrogen`
     if the user requests higher reasoning during voice chat.

3. **Launch the background loop** with `run_command` (`IsDaemon=true`):
   ```powershell
   cd D:\StackChan\stackchan-mcp; uv run python -u scripts/stackchan_chat.py --lang auto 2>&1
   ```
   - Tell the user the voice loop is active and stop calling tools while it runs.

4. **Handle escalation**: When the user asks for technical work, the script
   prints `[ESCALATED] Task: <summary>` and exits. Execute the requested work
   with the main agent's tools:
   - **If you need user confirmation or a follow-up answer**: launch a
     single-turn background listener with **no timeout** (`IsDaemon=true`,
     `WaitMsBeforeAsync=500`) so the user never hits a 60s timeout while
     reading or thinking:
     ```powershell
     cd D:\StackChan\stackchan-mcp; uv run python -u scripts/stackchan_chat.py --lang auto --wait-reply --say "<concise spoken question>" 2>&1
     ```
     Then stop calling tools and wait: as soon as the user replies by voice,
     the script prints `[USER_REPLY (<lang>)] <text>` and exits, waking you up
     automatically with full context.
   - **Once the technical work is complete**: speak a concise summary via
     `stackchan_say` and immediately re-launch
     `scripts/stackchan_chat.py --lang auto --no-greeting` so StackChan stays
     listening in silent standby without repeating the greeting.

5. **Handle end**: When the user ends the session, the script exits cleanly;
   confirm briefly in chat.
