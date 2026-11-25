# Reinstalar el servidor MCP de StackChan (modo charla)

Guía por si se formatea el computador. El servidor es un fork local de
`https://github.com/migratorywhale/stackchan-mcp` con ajustes propios (Piper TTS local,
Faster-Whisper local, conversación en una sola llamada).

## 1. Requisitos
- Windows con Python 3.11+ y `uv` (`pip install uv`)
- FFmpeg (`winget install Gyan.FFmpeg`)
- Git y llave SSH de GitHub
- StackChan con firmware de la rama `custom/naruto` de este repositorio

## 2. Clonar y aplicar los ajustes
```powershell
mkdir D:\StackChan -Force; cd D:\StackChan
git clone git@github.com:migratorywhale/stackchan-mcp.git
cd stackchan-mcp
git checkout 7673c4c9cb93a342824f94c2a91432e4cdbafcf0
git apply --whitespace=nowarn ..\StackChan\host\stackchan-mcp\stackchan-mcp-local.patch
Copy-Item ..\StackChan\host\stackchan-mcp\env.template .env
uv sync
uv pip install piper-tts faster-whisper numpy edge-tts
```
(El parche está en `host/stackchan-mcp/stackchan-mcp-local.patch`; el commit base es `7673c4c`.)

## 3. Voz local (Piper)
Descargar `es_MX-claude-high.onnx` y `es_MX-claude-high.onnx.json` desde
`https://huggingface.co/rhasspy/piper-voices/tree/main/es/es_MX/claude/high` a
`D:\StackChan\stackchan-mcp\models\piper\`.

## 4. Configurar `.env`
Editar `.env`:
- `STACKCHAN_IP` = IP que muestra la app MCP en el dispositivo
- `TTS_ENGINE="piper"`, `STACKCHAN_ASR_ENGINE="whisper"`, `STACKCHAN_WHISPER_MODEL="small"`
- `STACKCHAN_PIPER_MODEL` = ruta al `.onnx`
- Las claves (Fish, ElevenLabs, tokens) quedan vacías; no se versionan.

## 5. Registrar en Antigravity
Copiar el bloque `stackchan` de `antigravity_mcp_config.json` a
`C:\Users\<usuario>\.gemini\config\mcp_config.json` y reiniciar Antigravity.

## 6. Regla de modo charla
Copiar `modo-charla.rule.md` a la carpeta de reglas de Antigravity.

## 7. Firmware
Compilar con ESP-IDF 6.1 (`idf >=6.1.0,<6.2.0`) y flashear:
```powershell
Set-ExecutionPolicy -Scope Process Bypass
$env:IDF_PATH='C:\Users\<usuario>\xs-dev\esp-idf-v6.1'
$env:IDF_TOOLS_PATH='C:\Users\<usuario>\xs-dev\espressif-tools-6.1'
. "$env:IDF_PATH\export.ps1"
cd D:\StackChan\StackChan\firmware
ninja -C build
python -m esptool --chip esp32s3 -p COM3 -b 460800 write_flash 0x20000 build\stack-chan.bin
```

## 8. Probar
Abrir la app `MCP` en StackChan, verificar con `stackchan_status` y decir "modo charla".
