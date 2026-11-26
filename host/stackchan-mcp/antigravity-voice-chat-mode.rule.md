# Rule: Voice Chat Mode with StackChan

Optional pointer rule if you also want plain phrases to trigger the `charla` skill.
Prefer installing the `charla` skill at `C:\Users\<user>\.gemini\config\skills\charla\SKILL.md`
so `/charla` works out of the box with the token-saving Flash relay workflow.

## When it applies
When the user writes "/charla", "modo charla", "voice chat", "let's talk" or
"sigamos hablando" and the `stackchan` MCP server is available.

## Behavior
- Always follow the `charla` skill: delegate voice turns to a fresh-context `flash`
  subagent with `stackchan_converse` (one call per turn, gesture + face included),
  and escalate back to the main agent only when coding, file edits, builds, or
  heavy technical tasks are requested.
