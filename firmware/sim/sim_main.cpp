#include "sim_apps.h"
#include "sim_ui_helpers.h"
#include <SDL.h>
#include <lvgl.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace sim {

namespace {

constexpr int SCREEN_W     = 320;
constexpr int SCREEN_H     = 240;
constexpr int STATUS_BAR_H = 26;

enum class ActiveApp : uint8_t {
    Launcher = 0,
    Music,
    Alarm,
    Gemini,
    Speaker,
};

SimState s_state;
ActiveApp s_active      = ActiveApp::Launcher;
lv_obj_t* s_status_bar  = nullptr;
lv_obj_t* s_status_vol  = nullptr;
lv_obj_t* s_status_info = nullptr;
lv_obj_t* s_launcher    = nullptr;

void build_active();

void update_status_bar_text()
{
    if (s_status_vol) {
        lv_label_set_text_fmt(s_status_vol, "%d%%", s_state.volume);
    }
    if (s_status_info) {
        lv_label_set_text_fmt(s_status_info, "%s  BAT %d%%", s_state.wifi_connected ? "WiFi" : "OFF",
                              s_state.battery_pct);
    }
}

void rebuild_status_bar()
{
    if (s_status_bar) {
        lv_obj_delete(s_status_bar);
        s_status_bar  = nullptr;
        s_status_vol  = nullptr;
        s_status_info = nullptr;
    }

    if (!s_state.status_bar_fixed) {
        return;
    }

    s_status_bar = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_status_bar, 0, 0);
    lv_obj_set_size(s_status_bar, SCREEN_W, STATUS_BAR_H);
    community::style_box(s_status_bar, 0x101626, 0);

    lv_obj_t* btn_home =
        community::make_btn(s_status_bar, 4, 3, 52, 20, "HOME", community::COL_CARD_HI, community::COL_WHITE,
                            &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        btn_home, [](lv_event_t*) { navigate_home(); }, LV_EVENT_CLICKED, nullptr);

    s_status_info = lv_label_create(s_status_bar);
    lv_obj_align(s_status_info, LV_ALIGN_CENTER, -14, 0);
    lv_obj_set_style_text_font(s_status_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_info, lv_color_hex(community::COL_DIM), 0);

    lv_obj_t* btn_minus =
        community::make_btn(s_status_bar, 224, 3, 24, 20, "-", community::COL_CARD_HI, community::COL_WHITE,
                            &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        btn_minus,
        [](lv_event_t*) {
            s_state.volume = std::max(0, s_state.volume - 10);
            update_status_bar_text();
        },
        LV_EVENT_CLICKED, nullptr);

    s_status_vol = lv_label_create(s_status_bar);
    lv_obj_set_pos(s_status_vol, 250, 5);
    lv_obj_set_width(s_status_vol, 38);
    lv_obj_set_style_text_align(s_status_vol, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status_vol, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_vol, lv_color_hex(community::COL_TEXT), 0);

    lv_obj_t* btn_plus =
        community::make_btn(s_status_bar, 290, 3, 24, 20, "+", community::COL_CARD_HI, community::COL_WHITE,
                            &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        btn_plus,
        [](lv_event_t*) {
            s_state.volume = std::min(100, s_state.volume + 10);
            update_status_bar_text();
        },
        LV_EVENT_CLICKED, nullptr);

    update_status_bar_text();
}

void destroy_all_views()
{
    destroy_music_preview();
    destroy_alarm_preview();
    destroy_gemini_preview();
    destroy_speaker_preview();
    if (s_launcher) {
        lv_obj_delete(s_launcher);
        s_launcher = nullptr;
    }
}

void build_launcher()
{
    const int top = status_bar_top_offset();
    s_launcher    = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(s_launcher, 0, top);
    lv_obj_set_size(s_launcher, SCREEN_W, SCREEN_H - top);
    community::style_box(s_launcher, community::COL_BG, 0);

    lv_obj_t* hdr = lv_label_create(s_launcher);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(community::COL_DIM), 0);
    lv_label_set_text(hdr, "STACKCHAN COMMUNITY SUITE");

    struct AppTile {
        const char* title;
        const char* sub;
        uint32_t bg;
        ActiveApp app;
    };

    static const AppTile tiles[4] = {
        {LV_SYMBOL_AUDIO "  MUSIC",   "SD MP3 & Radio",   0xB91C45, ActiveApp::Music},
        {LV_SYMBOL_BELL "  ALARM",    "Clock & Timer",    0xB45309, ActiveApp::Alarm},
        {LV_SYMBOL_CHARGE "  GEMINI", "Live Voice + MCP", 0x2563EB, ActiveApp::Gemini},
        {LV_SYMBOL_VOLUME_MAX "  SPEAKER", "Wi-Fi & USB-C", 0x0F766E, ActiveApp::Speaker},
    };

    constexpr int tile_w = 144;
    constexpr int tile_h = 76;
    constexpr int gap_x  = 12;
    constexpr int gap_y  = 12;
    constexpr int x0     = 10;
    const int y0         = 34;

    for (int i = 0; i < 4; ++i) {
        const int col = i % 2;
        const int row = i / 2;
        const int x   = x0 + col * (tile_w + gap_x);
        const int y   = y0 + row * (tile_h + gap_y);

        lv_obj_t* btn = lv_button_create(s_launcher);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, tile_w, tile_h);
        community::style_box(btn, tiles[i].bg, 14);
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<uintptr_t>(tiles[i].app)));
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                s_active = static_cast<ActiveApp>(
                    reinterpret_cast<uintptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
                build_active();
            },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_t* l1 = lv_label_create(btn);
        lv_obj_align(l1, LV_ALIGN_CENTER, 0, -10);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l1, lv_color_hex(community::COL_WHITE), 0);
        lv_label_set_text(l1, tiles[i].title);
        lv_obj_remove_flag(l1, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* l2 = lv_label_create(btn);
        lv_obj_align(l2, LV_ALIGN_CENTER, 0, 14);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l2, lv_color_hex(0xE2E8F0), 0);
        lv_label_set_text(l2, tiles[i].sub);
        lv_obj_remove_flag(l2, LV_OBJ_FLAG_CLICKABLE);
    }
}

void build_active()
{
    destroy_all_views();
    lv_obj_t* scr = lv_screen_active();
    switch (s_active) {
        case ActiveApp::Launcher: build_launcher(); break;
        case ActiveApp::Music:    create_music_preview(scr); break;
        case ActiveApp::Alarm:    create_alarm_preview(scr); break;
        case ActiveApp::Gemini:   create_gemini_preview(scr); break;
        case ActiveApp::Speaker:  create_speaker_preview(scr); break;
    }
}

void tick_active(uint32_t now_ms)
{
    switch (s_active) {
        case ActiveApp::Music:   update_music_preview(now_ms); break;
        case ActiveApp::Alarm:   update_alarm_preview(now_ms); break;
        case ActiveApp::Gemini:  update_gemini_preview(now_ms); break;
        case ActiveApp::Speaker: update_speaker_preview(now_ms); break;
        default: break;
    }
}

}  // namespace

SimState& state()
{
    return s_state;
}

void navigate_home()
{
    s_active = ActiveApp::Launcher;
    build_active();
}

void set_status_bar_fixed(bool fixed)
{
    s_state.status_bar_fixed = fixed;
    rebuild_status_bar();
    build_active();
}

int status_bar_top_offset()
{
    return s_state.status_bar_fixed ? STATUS_BAR_H : 0;
}

}  // namespace sim

int main(int argc, char* argv[])
{
    float zoom        = 2.0f;
    uint32_t smoke_ms = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
            zoom = std::clamp(std::strtof(argv[++i], nullptr), 1.0f, 4.0f);
        } else if (std::strcmp(argv[i], "--smoke-ms") == 0 && i + 1 < argc) {
            smoke_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return SDL_GetTicks(); });

    lv_display_t* disp = lv_sdl_window_create(320, 240);
    if (!disp) {
        return 1;
    }
    lv_sdl_window_set_zoom(disp, zoom);
    lv_sdl_window_set_title(disp, "StackChan Desktop Simulator (320x240)");
    if (SDL_Window* win = lv_sdl_window_get_window(disp)) {
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_ShowWindow(win);
        SDL_RaiseWindow(win);
    }
    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();

    sim::set_status_bar_fixed(true);

    const uint32_t start_ms = SDL_GetTicks();
    bool running            = true;

    while (running) {
        const uint32_t now_ms = SDL_GetTicks();

        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        static bool prev_h = false, prev_m = false, prev_esc = false;
        const bool cur_h   = keys[SDL_SCANCODE_H] != 0;
        const bool cur_m   = keys[SDL_SCANCODE_M] != 0;
        const bool cur_esc = keys[SDL_SCANCODE_ESCAPE] != 0;

        if (cur_h && !prev_h) {
            sim::navigate_home();
        }
        if (cur_m && !prev_m) {
            sim::set_status_bar_fixed(!sim::state().status_bar_fixed);
        }
        if (cur_esc && !prev_esc) {
            running = false;
        }
        prev_h   = cur_h;
        prev_m   = cur_m;
        prev_esc = cur_esc;

        sim::tick_active(now_ms);
        lv_timer_handler();

        if (smoke_ms > 0 && (now_ms - start_ms) >= smoke_ms) {
            break;
        }

        SDL_Delay(5);
    }

    lv_sdl_quit();
    return 0;
}
