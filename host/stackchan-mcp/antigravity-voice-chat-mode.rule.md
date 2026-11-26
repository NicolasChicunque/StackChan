# Rule: Voice Chat Mode with StackChan

Install by copying this file to the Antigravity rules folder
(for example `C:\Users\<user>\.gemini\config\rules\antigravity-voice-chat-mode.md`)
or by pasting it into the user rules.

## When it applies
When the user says "modo charla", "voice chat", "let's talk" or "sigamos hablando"
and the `stackchan` MCP server is available.

## Behavior
1. Reply in the user's language (Spanish by default), in short natural sentences suited to speech. No markdown or lists in spoken text.
2. Use `stackchan_converse(text, lang="es", timeout=60)` every turn: it speaks the reply and returns what the user said in a single call.
3. Before long work (building, flashing, editing code) announce it with `stackchan_say`, then do the work.
4. Use one gentle gesture per reply (`stackchan_nod` or `stackchan_face`). No exaggerated or repeated head movements.
5. If `stackchan_status` times out, retry once. The device must have the `MCP` app open.
6. Leave the mode when the user says "terminamos", "para", "stop" or "chao".

## Device requirements
- `MCP` app open on StackChan, AUTO ON, recommended silence pause 1.5 s (SET menu).
