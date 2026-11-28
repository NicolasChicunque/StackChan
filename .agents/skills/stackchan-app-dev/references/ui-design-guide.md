# StackChan UI Design Guide (320×240 LVGL 9.5)

Follow these standards so every app in the StackChan Community Suite looks and behaves like part of one cohesive operating system.

## 1. Screen & Chrome Layout

- **Resolution**: `320 × 240` px.
- **Language**: **English only** for all on-screen text, status badges, hints, web portals, and USB/BLE device names.
- **Shared Top Status Bar & Home Indicator**:
  - The user configures the top bar globally as either `Fixed` (`38 px` reserved at top) or `AutoHide` (`0 px` reserved, revealed by swiping down from the top edge).
  - Always query `const int top_y = view::get_status_bar_top_offset();` and position your top content at `top_y + 4` (never hardcode `y = 0` for interactive controls).
  - Create chrome in `onOpen()` *after* building your root screen:
    ```cpp
    view::create_home_indicator([&]() { close(); }, THEME_COL, COL_BG);
    view::create_status_bar(THEME_COL, COL_BG);
    ```
  - **Mandatory in `onRunning()`**: Call both `view::update_home_indicator();` and `view::update_status_bar();` inside `LvglLockGuard` every cycle. Without these calls, the top-edge swipe gesture (`StatuBarGesture`) will not detect swipes and the user cannot access Home or Volume controls.
  - Destroy both in `onClose()` (`view::destroy_home_indicator(); view::destroy_status_bar();`).

## 2. Color Palette & Helpers (`apps/common/community_helpers.h`)

Always include `<apps/common/community_helpers.h>` and use `using namespace community;` instead of inventing ad-hoc colors:

| Token | Hex | Usage |
| --- | --- | --- |
| `COL_BG` | `0x0B0F1A` | Full-screen app background |
| `COL_CARD` | `0x151B2E` | Content cards, inactive buttons, stage panels |
| `COL_CARD_HI` | `0x232C4A` | Secondary action buttons, progress tracks |
| `COL_TEAL` / `COL_TEAL_DK` | `0x2DD4BF` / `0x0F766E` | Primary accent (Wi-Fi, active states, highlights) |
| `COL_BLUE` | `0x3B82F6` | Secondary mode accent (e.g., USB-C mode) |
| `COL_GREEN` | `0x22C55E` | Connected / active / success |
| `COL_AMBER` | `0xF59E0B` | Warnings / highlights |
| `COL_RED` | `0xEF4444` | Muted / stop / destructive / error |
| `COL_TEXT` / `COL_WHITE` | `0xE2E8F0` / `0xFFFFFF` | Primary labels and active button text |
| `COL_DIM` | `0x94A3B8` | Subtitles, hints, inactive labels |

Use `style_box(obj, bg, radius)` and `make_btn(parent, x, y, w, h, txt, bg, fg, font)` to guarantee flat, non-scrollable, borderless cards with consistent corner radii (`10–14 px` for cards/buttons, `14 px` for pills).

## 3. Standard Vertical Regions

Divide the `320 × 240` screen cleanly into three bands:
1. **Header / Mode Bar** (`y = top_y + 4`, `h = 28`):
   - Mode switcher pills (e.g., `[ WI-FI ] [ USB-C ]`) or app title on the left (`x = 8`), plus a compact status badge pill on the right (`w ≈ 110`, right-aligned to `x = 312`).
2. **Center Stage** (`y = header_bottom + 6` to `174`):
   - Main visual card (`x = 8, w = 304`, radius `14`) containing the Avatar, live visualization (EQ bars / waveform / clock / metrics), and a single-line hint/telemetry label at `y = 174` (`12 px` `COL_DIM`).
   - Adapt inner heights when `top_y > 0` (`Fixed` status bar mode) so nothing clips at the bottom.
3. **Bottom 4-Button Control Bar** (`y = 192`, `h = 42`, `BAR4_W = 71`, `BAR4_GAP = 6`, `BAR4_X0 = 9`):
   - Matches `AppMusicPlayer` and `AppSpeaker`: four thumb-friendly buttons (`9 + 4*71 + 3*6 = 311 <= 320`).
   - Do not duplicate Volume `-`/`+` or Home buttons in the bottom bar — they already live in the shared top status bar.

## 4. Typography & Sub-Views

- Use Montserrat built-in fonts (`&lv_font_montserrat_12`, `14`, `16`, `20`, `24`).
- Sub-views (e.g., Settings, QR/Info, Lists) use a clickable `< TITLE` header at `x = 10, y = top_y + 6` in `16 px` accent color to return to the main view.
- For QR codes, place `lv_qrcode_create` inside a white rounded container (`COL_WHITE`, `radius = 10`, `5 px` quiet-zone padding) so phone cameras scan reliably in dark themes.

## 5. Avatar Standard (Global Setting Only & IP-Safe Naming)

- **Never** add a per-app avatar switcher or store a local avatar preference in NVS. The avatar style is controlled globally from the main **SETUP / Settings** screen.
- **IP / Trademark Hygiene**: Never use copyrighted character or brand names in code identifiers, comments, UI labels, asset filenames, or documentation. Always use generic terms such as `Custom` (`AvatarStyle::Custom`), `avatar`, `_img_avatar_calm`, `_img_avatar_happy`, or `custom_avatar`.
- Always query:
  ```cpp
  bool show_custom_avatar = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Custom &&
                             _img_avatar_calm.data != nullptr);
  ```
- When `show_custom_avatar` is `false`, render the Classic StackChan Face (clean eyes with natural blinking + dynamic mouth/lip-sync).
- When `show_custom_avatar` is `true`, display `avatar_calm_music.bin` / `avatar_happy_music.bin` scaled proportionally to fit both `Fixed` and `AutoHide` stage heights.
