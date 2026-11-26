# Rule: Voice Chat Mode with StackChan

Optional pointer rule if you also want plain phrases to trigger the `stackchanchat` skill.
Prefer installing the `stackchanchat` skill at `C:\Users\<user>\.gemini\config\skills\stackchanchat\SKILL.md`
so `/stackchanchat` works out of the box with the token-saving background voice loop.

## When it applies
When the user writes "/stackchanchat", "modo charla", "voice chat", "let's talk" or
"sigamos hablando" and the `stackchan` MCP server is available.

## Behavior
- Always follow the `stackchanchat` skill: launch `scripts/stackchan_chat.py --lang auto`
  in the background (local Piper/Edge TTS + Faster-Whisper ASR + direct Gemini Interactions API),
  and escalate back to the main agent only when coding, file edits, builds, live web lookups,
  or heavy technical tasks are requested.
