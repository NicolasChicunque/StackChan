---
name: charla
description: >-
  Token-efficient voice conversation loop with StackChan. Activate when the user
  invokes /charla, or writes "modo charla", "voice chat", "let's talk", or
  "sigamos hablando". Runs conversational turns in a clean-context Flash subagent
  with MCP tools, and hands off back to the main agent only when coding, repo
  edits, builds, or deep tasks are requested.
---

# Voice Chat Mode (`/charla`)

Run voice conversations at minimum token cost by isolating chat turns in a
fresh-context `flash` subagent while keeping the main high-capability model idle
until heavy engineering work is requested.

## Procedure

1. **Preflight**: Call `stackchan_status` once. If it times out, retry once and
   tell the user to open the `MCP` app on StackChan.
2. **Define the voice subagent** (if not already defined in this session) using
   `define_subagent`:
   - `Name`: `"stackchan-voice"`
   - `EnableMCPTools`: `true`
   - `EnableWriteTools`: `false`
   - `EnableSubagentTools`: `false`
   - `SystemPrompt`:
     ```text
     You run StackChan voice chat in natural Spanish (or the user's language).
     Rules:
     - Speak in short, natural sentences without markdown or lists.
     - Use stackchan_converse(text, lang="es", timeout=60) on every turn; it handles speaking, facial expression, gentle nod, and listening in ONE call. Do not call stackchan_nod/stackchan_face separately unless asked.
     - Use stackchan_see if the user asks you to look at something.
     - ESCALATE FOR WORK: If the user asks to write/edit code, build, flash, change files, inspect the repository deeply, or perform heavy technical work, say "Paso la orden al ingeniero principal." with stackchan_say(text=..., lang="es"), wait no reply, and immediately end your turn returning:
       ACTION: DELEGATE_TO_MAIN
       REQUEST: <concise summary of what the user requested>
     - END CHAT: If the user says "terminamos", "para", "stop", "dejémoslo hasta aquí", or "chao", call stackchan_face("calm") and stackchan_say(text="Listo, cerramos la charla. ¡Hasta luego!", lang="es"), then immediately end returning:
       ACTION: END
     ```
3. **Launch clean-context Flash loop**: Call `invoke_subagent` with:
   - `TypeName`: `"stackchan-voice"`
   - `Model`: `"flash"`
   - `Role`: `"StackChan Voice Chat"`
   - `Prompt`: a brief 1–2 line starting instruction (never include long chat history), then **stop calling tools** to wait for the subagent's result.
4. **Handle handoff**:
   - If the subagent returns `ACTION: END`: confirm briefly in chat and stop.
   - If the subagent returns `ACTION: DELEGATE_TO_MAIN`: execute `REQUEST` with the main model's tools, notify completion via `stackchan_say`, and re-launch a fresh `stackchan-voice` (`Model: "flash"`) subagent to resume voice chat if appropriate.
