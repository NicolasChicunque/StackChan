# StackChan Desktop UI Simulator (LVGL 9.5 + SDL2)

Interactive **320×240** desktop simulator for **StackChan Community Suite**, using the exact same **LVGL 9.5** library (`firmware/managed_components/lvgl__lvgl`) and fonts/widgets as the ESP32-S3 firmware.

Iterate on LVGL screens, buttons, layouts, QR codes, and status bar offsets in **~2 seconds** without compiling or flashing the physical robot.

## Quick Start (Windows)

```powershell
# Launch interactive simulator (Launcher + Avatar + Sandbox + Setup)
.\firmware\sim\run_sim.ps1

# Boot directly into your UI Sandbox screen
.\firmware\sim\run_sim.ps1 -Sandbox

# Bigger window (3x = 960x720)
.\firmware\sim\run_sim.ps1 -Zoom 3
```

> **First run** downloads prebuilt `SDL2 2.30.11` (~3 MB) and compiles LVGL 9.5 once (~20–30 s). Subsequent edits to `sim_sandbox.cpp` rebuild in **~1–2 seconds**.

## Keyboard Shortcuts

| Key | Action |
| :--- | :--- |
| **Mouse Click / Drag** | Simulates CoreS3 capacitive touchscreen |
| **`H`** | Navigate back to the 3×3 Launcher (`HOME`) |
| **`M`** | Toggle Top Status Bar between `Fixed` (26 px offset) and `AutoHide` (0 px offset) |
| **`1` – `6`** | Switch Classic Avatar emotions (`Calm`, `Happy`, `Sleepy`, `Thinking`, `Shy`, `Excited`) |
| **`ESC`** | Quit simulator |

## Prototyping Your Own App UI

1. Edit [`sim_sandbox.cpp`](sim_sandbox.cpp) inside `create_sandbox_screen(lv_obj_t* parent)`.
2. You have the exact same helpers and theme palette as real StackChan apps (`sim_ui_helpers.h`):
   - `community::COL_BG`, `COL_CARD`, `COL_TEAL`, `COL_AMBER`, `COL_GREEN`, `COL_TEXT`, `COL_DIM`, etc.
   - `community::style_box(obj, bg, radius)` and `community::make_btn(...)`
   - `view::get_status_bar_top_offset()` (test both `Fixed` and `AutoHide` by pressing `M`!)
   - All firmware fonts: `lv_font_montserrat_12`, `14`, `16`, `20`, `24` + `lv_qrcode_create(...)`
3. Run `.\firmware\sim\run_sim.ps1 -Sandbox` to preview immediately.
4. Once the layout looks right, copy the LVGL widget code into your `firmware/main/apps/app_<name>/` class.

## Linux / macOS

Install `libsdl2-dev` (Ubuntu/Debian) or `brew install sdl2` (macOS), then:

```bash
cmake -S firmware/sim -B firmware/sim/build -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/sim/build -j
./firmware/sim/build/stackchan_sim --zoom 2
```
