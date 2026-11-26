# Custom Avatars via MicroSD (`/sd/avatars/`)

StackChan Community Suite ships with the classic vector StackChan face by default, and optionally lets you drop your own custom character sprites onto the MicroSD card — without recompiling the firmware.

## 1. Folder & File Names

Create a folder named `avatars` at the root of your MicroSD card (`/sd/avatars/`) and place any of the following LVGL v9 `.bin` sprite files inside:

| Filename | Used for |
| --- | --- |
| `avatar_calm.bin` | Neutral / idle expression (`AI.Agent`, `MCP`, `Alarm`) |
| `avatar_thinking.bin` | Listening / thinking (`AI.Agent`, `MCP`) |
| `avatar_happy.bin` | Happy / speaking (`AI.Agent`, `MCP`, `Alarm` ringing) |
| `avatar_sleepy.bin` | Sleepy / night mode (`AI.Agent`, `Alarm`, `Screensaver`) |
| `avatar_shy.bin` | Sad / shy (`AI.Agent`, `MCP`) |
| `avatar_smug.bin` | Smug / confident (`AI.Agent`, `MCP`, `Screensaver`) |
| `avatar_pouty.bin` | Angry / pouty (`AI.Agent`, `MCP`, `Alarm` ringing) |
| `avatar_calm_music.bin` | Idle expression in `Music`, `Gemini`, `Alarm`, `Screensaver` |
| `avatar_happy_music.bin` | Active/singing expression in `Music`, `Gemini`, `Alarm`, `Screensaver` |

If a file is missing (or no MicroSD card is inserted), StackChan automatically falls back to the classic vector face.

## 2. How to Create `.bin` Sprites from PNG

Use the official **LVGL Image Converter** (v9) or `LVGLImage.py` from LVGL v9:

- **Recommended size:** `160×160` to `220×220` px (transparent PNG or matching dark background).
- **Color format:** `RGB565A8` (with transparency) or `RGB565` (opaque).
- **Output format:** `Binary (.bin)` with standard LVGL v9 `lv_image_header_t` header.

Example with LVGL v9's converter script:

```bash
python LVGLImage.py --ofmt BIN --cf RGB565A8 -o ./out my_calm.png
```

Then rename the generated `.bin` files to the names in the table above, copy them to `/sd/avatars/` on your MicroSD card, and select **Custom** in **`SETUP → Avatar Style`**.
