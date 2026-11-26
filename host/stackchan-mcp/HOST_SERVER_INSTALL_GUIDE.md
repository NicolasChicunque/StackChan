# StackChan MCP Host Server: Install Guide

Use this guide to rebuild the voice chat setup after reformatting the computer.
The host server is a personal fork of `migratorywhale/stackchan-mcp` with local
changes (local Piper TTS, local Faster-Whisper ASR, single-call conversation tool).

## Files in this folder
| File | Purpose |
| --- | --- |
| `HOST_SERVER_INSTALL_GUIDE.md` | This guide |
| `stackchan-mcp-local-changes.patch` | Local changes on top of upstream commit `7673c4c` (only needed if the fork is not available) |
| `stackchan-mcp.env.template` | `.env` template, secrets blanked |
| `antigravity-mcp-config.example.json` | Antigravity MCP server entry |
| `skills/charla/SKILL.md` | Antigravity `/charla` skill (token-saving Flash voice loop + escalation) |
| `antigravity-voice-chat-mode.rule.md` | Optional pointer rule for plain phrases |

## 1. Requirements
- Windows, Python 3.11+, `uv` (`pip install uv`)
- FFmpeg (`winget install Gyan.FFmpeg`)
- Git with a GitHub SSH key
- StackChan flashed with the `custom/naruto` branch firmware of this repository

## 2. Get the server
Clone your own fork (already contains all local changes on `master`):
```powershell
mkdir D:\StackChan -Force; cd D:\StackChan
git clone git@github.com:NicolasChicunque/stackchan-mcp.git
cd stackchan-mcp
git remote add upstream git@github.com:migratorywhale/stackchan-mcp.git
```
Fallback only if the fork is unavailable: clone upstream and apply the patch.
```powershell
git clone git@github.com:migratorywhale/stackchan-mcp.git
cd stackchan-mcp
git checkout 7673c4c9cb93a342824f94c2a91432e4cdbafcf0
git apply --whitespace=nowarn ..\StackChan\host\stackchan-mcp\stackchan-mcp-local-changes.patch
```

## 3. Install dependencies
```powershell
cd D:\StackChan\stackchan-mcp
Copy-Item ..\StackChan\host\stackchan-mcp\stackchan-mcp.env.template .env
uv sync
uv pip install piper-tts faster-whisper numpy edge-tts
```

## 4. Local voice model (Piper)
Download `es_MX-claude-high.onnx` and `es_MX-claude-high.onnx.json` from
`https://huggingface.co/rhasspy/piper-voices/tree/main/es/es_MX/claude/high`
into `D:\StackChan\stackchan-mcp\models\piper\`.

## 5. Configure `.env`
- `STACKCHAN_IP`: IP address shown by the MCP app on the device
- `TTS_ENGINE="piper"`, `STACKCHAN_ASR_ENGINE="whisper"`, `STACKCHAN_WHISPER_MODEL="small"`
- `STACKCHAN_PIPER_MODEL`: full path to the `.onnx` file
- Keys and tokens stay empty and are never committed.

## 6. Register in Antigravity
Copy the `stackchan` block from `antigravity-mcp-config.example.json` into
`C:\Users\<user>\.gemini\config\mcp_config.json`, then restart Antigravity.

## 7. Install the `/charla` skill
Copy the `skills\charla` folder into `C:\Users\<user>\.gemini\config\skills\charla\`:
```powershell
New-Item -ItemType Directory -Force "$env:USERPROFILE\.gemini\config\skills\charla" | Out-Null
Copy-Item D:\StackChan\StackChan\host\stackchan-mcp\skills\charla\SKILL.md "$env:USERPROFILE\.gemini\config\skills\charla\SKILL.md"
```
(Optional) Copy `antigravity-voice-chat-mode.rule.md` into `C:\Users\<user>\.gemini\config\rules\`.

## 8. Build and flash the firmware
Requires ESP-IDF 6.1 (`idf >=6.1.0,<6.2.0`).
```powershell
Set-ExecutionPolicy -Scope Process Bypass
$env:IDF_PATH = 'C:\Users\<user>\xs-dev\esp-idf-v6.1'
$env:IDF_TOOLS_PATH = 'C:\Users\<user>\xs-dev\espressif-tools-6.1'
. "$env:IDF_PATH\export.ps1"
cd D:\StackChan\StackChan\firmware
ninja -C build
python -m esptool --chip esp32s3 -p COM3 -b 460800 write-flash 0x20000 build\stack-chan.bin
```

## 9. Verify
Open the `MCP` app on StackChan, run `stackchan_status`, then say "modo charla".
