#include "sim_apps.h"
#include "sim_ui_helpers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

/**
 * Preview of StackChan's real MUSIC app UI (mirroring app_music_player.cpp).
 */
namespace sim {

namespace {

constexpr int W        = 320;
constexpr int H        = 240;
constexpr int BAR_X    = 8;
constexpr int BAR_W    = 304;
constexpr int BAR_H    = 6;
constexpr int STAGE_X  = 8;
constexpr int STAGE_W  = 304;
constexpr int BAR4_Y   = 192;
constexpr int BAR4_H   = 42;
constexpr int BAR4_W   = 71;
constexpr int BAR4_GAP = 6;
constexpr int BAR4_X0  = 9;

constexpr uint32_t COL_BG      = 0x0B0F1A;
constexpr uint32_t COL_CARD    = 0x151B2E;
constexpr uint32_t COL_CARD_HI = 0x232C4A;
constexpr uint32_t COL_ACCENT  = 0xFF4D6D;
constexpr uint32_t COL_TEXT    = 0xE8EEF9;
constexpr uint32_t COL_DIM     = 0x7C89A8;
constexpr uint32_t COL_WHITE   = 0xFFFFFF;
constexpr uint32_t COL_GREEN   = 0x34C759;
constexpr uint32_t COL_PLAY_ON = 0x1F6B3A;

constexpr uint32_t PALETTE[4] = {0xFF3B30, 0xFFCC00, 0x00C7BE, 0x5856D6};

struct DemoTrack {
    const char* title;
    int duration_sec;
};

constexpr DemoTrack TRACKS[] = {
    {"01. Daft Punk - Digital Love.mp3", 298},
    {"02. Lofi Chill Beats - Study.mp3", 184},
    {"03. StackChan Theme Song.wav", 142},
    {"04. Synthwave Night Drive.mp3", 236},
};
constexpr int TRACK_COUNT = static_cast<int>(sizeof(TRACKS) / sizeof(TRACKS[0]));

enum class MusicMode : uint8_t { Main = 0, Settings };

MusicMode s_mode     = MusicMode::Main;
int s_track_idx      = 0;
bool s_playing       = true;
bool s_radio_mode    = false;
int s_loop_mode      = 2;
bool s_shuffle       = false;
bool s_lights        = true;
bool s_move          = true;
float s_elapsed_sec  = 42.0f;
uint32_t s_last_ms   = 0;

lv_obj_t* s_root          = nullptr;
lv_obj_t* s_main_cont     = nullptr;
lv_obj_t* s_settings_cont = nullptr;
lv_obj_t* s_lbl_title     = nullptr;
lv_obj_t* s_bar_fill      = nullptr;
lv_obj_t* s_lbl_elapsed   = nullptr;
lv_obj_t* s_lbl_track_num = nullptr;
lv_obj_t* s_lbl_duration  = nullptr;
lv_obj_t* s_lbl_vol_ver   = nullptr;
lv_obj_t* s_lbl_status    = nullptr;
lv_obj_t* s_btn_play      = nullptr;
lv_obj_t* s_lbl_play      = nullptr;
lv_obj_t* s_mouth         = nullptr;
lv_obj_t* s_note_lbls[4]  = {};
lv_obj_t* s_cfg_lbls[8]   = {};
lv_obj_t* s_cfg_btns[8]   = {};

void style_flat_box(lv_obj_t* obj, uint32_t bg_hex, int radius = 0)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void fmt_mmss(char* buf, size_t n, int sec)
{
    if (sec < 0) sec = 0;
    std::snprintf(buf, n, "%02d:%02d", sec / 60, sec % 60);
}

void refresh_music_ui()
{
    if (!s_root) return;

    if (s_mode == MusicMode::Main) {
        lv_obj_remove_flag(s_main_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_settings_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_main_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_settings_cont, LV_OBJ_FLAG_HIDDEN);
    }

    const auto& tr = TRACKS[s_track_idx];
    if (s_radio_mode) {
        lv_label_set_text(s_lbl_title, "RADIO: Lofi Girl 24/7");
        lv_obj_set_width(s_bar_fill, BAR_W);
        lv_label_set_text(s_lbl_elapsed, "LIVE");
        lv_label_set_text(s_lbl_track_num, "RADIO");
        lv_label_set_text(s_lbl_duration, "WEB");
    } else {
        lv_label_set_text(s_lbl_title, tr.title);
        float ratio = std::clamp(s_elapsed_sec / static_cast<float>(tr.duration_sec), 0.0f, 1.0f);
        lv_obj_set_width(s_bar_fill, static_cast<int>(ratio * BAR_W));
        char b1[16], b2[16];
        fmt_mmss(b1, sizeof(b1), static_cast<int>(s_elapsed_sec));
        fmt_mmss(b2, sizeof(b2), tr.duration_sec);
        lv_label_set_text(s_lbl_elapsed, b1);
        lv_label_set_text_fmt(s_lbl_track_num, "%d/%d", s_track_idx + 1, TRACK_COUNT);
        lv_label_set_text(s_lbl_duration, b2);
    }

    lv_label_set_text_fmt(s_lbl_vol_ver, "VOL %d%%  v4.1", state().volume);
    lv_label_set_text(s_lbl_status, s_playing ? (s_radio_mode ? "STREAMING" : "PLAYING") : "PAUSED");
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(s_playing ? COL_GREEN : COL_DIM), 0);

    lv_label_set_text(s_lbl_play, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(s_playing ? COL_PLAY_ON : COL_CARD), 0);

    const char* loop_names[3] = {"OFF", "1", "ALL"};
    if (s_cfg_lbls[0]) lv_label_set_text_fmt(s_cfg_lbls[0], "LOOP\n%s", loop_names[s_loop_mode]);
    if (s_cfg_lbls[1]) lv_label_set_text_fmt(s_cfg_lbls[1], "SHUFFLE\n%s", s_shuffle ? "ON" : "OFF");
    if (s_cfg_lbls[2]) lv_label_set_text_fmt(s_cfg_lbls[2], "LIGHTS\n%s", s_lights ? "ON" : "OFF");
    if (s_cfg_lbls[3]) lv_label_set_text_fmt(s_cfg_lbls[3], "MOVE\n%s", s_move ? "ON" : "OFF");
    if (s_cfg_lbls[4]) lv_label_set_text(s_cfg_lbls[4], "SD LIST\n(4)");
    if (s_cfg_lbls[5]) lv_label_set_text_fmt(s_cfg_lbls[5], "RADIO\n%s", s_radio_mode ? "ON" : "OFF");
    if (s_cfg_lbls[6]) lv_label_set_text(s_cfg_lbls[6], "WEB/QR\nREMOTE");
    if (s_cfg_lbls[7]) lv_label_set_text(s_cfg_lbls[7], "HEAD\n45 deg");

    auto set_hi = [](lv_obj_t* b, bool on) {
        if (b) lv_obj_set_style_bg_color(b, lv_color_hex(on ? COL_PLAY_ON : COL_CARD), 0);
    };
    set_hi(s_cfg_btns[0], s_loop_mode > 0);
    set_hi(s_cfg_btns[1], s_shuffle);
    set_hi(s_cfg_btns[2], s_lights);
    set_hi(s_cfg_btns[3], s_move);
    set_hi(s_cfg_btns[5], s_radio_mode);
}

}  // namespace

void create_music_preview(lv_obj_t* parent)
{
    destroy_music_preview();
    s_mode    = MusicMode::Main;
    s_last_ms = 0;

    const int top_y    = view::get_status_bar_top_offset();
    const bool is_fix  = (top_y > 0);
    const int title_y  = top_y + 2;
    const int bar_y    = top_y + 22;
    const int time_y   = top_y + 30;
    const int status_y = top_y + 42;
    const int stage_y  = top_y + 55;
    const int stage_h  = 188 - stage_y;

    s_root = lv_obj_create(parent);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, W, H);
    style_flat_box(s_root, COL_BG, 0);

    s_main_cont = lv_obj_create(s_root);
    lv_obj_set_pos(s_main_cont, 0, 0);
    lv_obj_set_size(s_main_cont, W, H);
    style_flat_box(s_main_cont, COL_BG, 0);

    s_lbl_title = lv_label_create(s_main_cont);
    lv_obj_set_pos(s_lbl_title, 10, title_y);
    lv_obj_set_size(s_lbl_title, 300, 20);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(COL_TEXT), 0);

    lv_obj_t* bar_bg = lv_obj_create(s_main_cont);
    lv_obj_set_pos(bar_bg, BAR_X, bar_y);
    lv_obj_set_size(bar_bg, BAR_W, BAR_H);
    style_flat_box(bar_bg, COL_CARD_HI, 3);
    lv_obj_add_flag(bar_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(bar_bg, 6);
    lv_obj_add_event_cb(
        bar_bg,
        [](lv_event_t*) {
            if (s_radio_mode) return;
            lv_point_t p;
            lv_indev_get_point(lv_indev_active(), &p);
            float ratio   = std::clamp(static_cast<float>(p.x - BAR_X) / static_cast<float>(BAR_W), 0.0f, 1.0f);
            s_elapsed_sec = ratio * TRACKS[s_track_idx].duration_sec;
            refresh_music_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    s_bar_fill = lv_obj_create(bar_bg);
    lv_obj_set_pos(s_bar_fill, 0, 0);
    lv_obj_set_size(s_bar_fill, 0, BAR_H);
    style_flat_box(s_bar_fill, COL_ACCENT, 3);
    lv_obj_remove_flag(s_bar_fill, LV_OBJ_FLAG_CLICKABLE);

    s_lbl_elapsed = lv_label_create(s_main_cont);
    lv_obj_set_pos(s_lbl_elapsed, 10, time_y);
    lv_obj_set_style_text_font(s_lbl_elapsed, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_elapsed, lv_color_hex(COL_DIM), 0);

    s_lbl_track_num = lv_label_create(s_main_cont);
    lv_obj_set_pos(s_lbl_track_num, 180, time_y);
    lv_obj_set_width(s_lbl_track_num, 85);
    lv_obj_set_style_text_align(s_lbl_track_num, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_lbl_track_num, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_track_num, lv_color_hex(COL_DIM), 0);

    s_lbl_duration = lv_label_create(s_main_cont);
    lv_obj_set_pos(s_lbl_duration, 268, time_y);
    lv_obj_set_width(s_lbl_duration, 44);
    lv_obj_set_style_text_align(s_lbl_duration, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_lbl_duration, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_duration, lv_color_hex(COL_DIM), 0);

    s_lbl_vol_ver = lv_label_create(s_main_cont);
    lv_obj_set_pos(s_lbl_vol_ver, 10, status_y);
    lv_obj_set_style_text_font(s_lbl_vol_ver, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_vol_ver, lv_color_hex(COL_DIM), 0);

    s_lbl_status = lv_label_create(s_main_cont);
    lv_obj_set_pos(s_lbl_status, 110, status_y);
    lv_obj_set_width(s_lbl_status, 202);
    lv_obj_set_style_text_align(s_lbl_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_12, 0);

    lv_obj_t* stage = lv_obj_create(s_main_cont);
    lv_obj_set_pos(stage, STAGE_X, stage_y);
    lv_obj_set_size(stage, STAGE_W, stage_h);
    style_flat_box(stage, COL_BG, 12);

    lv_obj_t* face_card = lv_obj_create(stage);
    lv_obj_set_size(face_card, 128, is_fix ? 78 : 96);
    lv_obj_align(face_card, LV_ALIGN_CENTER, 0, 0);
    style_flat_box(face_card, COL_CARD, 18);
    lv_obj_set_style_border_width(face_card, 2, 0);
    lv_obj_set_style_border_color(face_card, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_add_flag(face_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        face_card,
        [](lv_event_t*) {
            s_playing = !s_playing;
            refresh_music_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t* eye_l = lv_obj_create(face_card);
    lv_obj_set_size(eye_l, 18, 18);
    lv_obj_align(eye_l, LV_ALIGN_CENTER, -26, -8);
    style_flat_box(eye_l, COL_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(eye_l, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* eye_r = lv_obj_create(face_card);
    lv_obj_set_size(eye_r, 18, 18);
    lv_obj_align(eye_r, LV_ALIGN_CENTER, 26, -8);
    style_flat_box(eye_r, COL_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(eye_r, LV_OBJ_FLAG_CLICKABLE);

    s_mouth = lv_obj_create(face_card);
    lv_obj_set_size(s_mouth, 38, 6);
    lv_obj_align(s_mouth, LV_ALIGN_CENTER, 0, 18);
    style_flat_box(s_mouth, COL_WHITE, 4);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_CLICKABLE);

    const int note_xs[4] = {20, 56, 234, 270};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* n    = lv_label_create(stage);
        s_note_lbls[i] = n;
        lv_label_set_text(n, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(PALETTE[i]), 0);
        lv_obj_set_pos(n, note_xs[i], stage_h / 2 - 10);
    }

    const char* icons[4] = {LV_SYMBOL_PREV, LV_SYMBOL_PAUSE, LV_SYMBOL_NEXT, LV_SYMBOL_SETTINGS};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* btn = lv_button_create(s_main_cont);
        lv_obj_set_pos(btn, BAR4_X0 + i * (BAR4_W + BAR4_GAP), BAR4_Y);
        lv_obj_set_size(btn, BAR4_W, BAR4_H);
        style_flat_box(btn, COL_CARD, 10);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(COL_CARD_HI), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_center(lbl);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
        lv_label_set_text(lbl, icons[i]);

        if (i == 1) {
            s_btn_play = btn;
            s_lbl_play = lbl;
        }

        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                int idx = static_cast<int>(
                    reinterpret_cast<uintptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
                if (idx == 0) {
                    s_track_idx   = (s_track_idx + TRACK_COUNT - 1) % TRACK_COUNT;
                    s_elapsed_sec = 0.0f;
                } else if (idx == 1) {
                    s_playing = !s_playing;
                } else if (idx == 2) {
                    s_track_idx   = (s_track_idx + 1) % TRACK_COUNT;
                    s_elapsed_sec = 0.0f;
                } else if (idx == 3) {
                    s_mode = MusicMode::Settings;
                }
                refresh_music_ui();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    s_settings_cont = lv_obj_create(s_root);
    lv_obj_set_pos(s_settings_cont, 0, 0);
    lv_obj_set_size(s_settings_cont, W, H);
    style_flat_box(s_settings_cont, COL_BG, 0);
    lv_obj_add_flag(s_settings_cont, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* hdr = lv_label_create(s_settings_cont);
    lv_obj_set_pos(hdr, 10, top_y + 4);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(hdr, "< SETTINGS");
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        hdr,
        [](lv_event_t*) {
            s_mode = MusicMode::Main;
            refresh_music_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    struct Spec { int x, y, w, h; };
    constexpr Spec SPECS[8] = {
        {8, 66, 73, 54},  {85, 66, 73, 54},  {162, 66, 73, 54},  {239, 66, 73, 54},
        {8, 126, 73, 54}, {85, 126, 73, 54}, {162, 126, 73, 54}, {239, 126, 73, 54},
    };

    for (int i = 0; i < 8; ++i) {
        lv_obj_t* btn = lv_button_create(s_settings_cont);
        s_cfg_btns[i] = btn;
        lv_obj_set_pos(btn, SPECS[i].x, SPECS[i].y);
        lv_obj_set_size(btn, SPECS[i].w, SPECS[i].h);
        style_flat_box(btn, COL_CARD, 10);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(COL_CARD_HI), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);

        lv_obj_t* lbl = lv_label_create(btn);
        s_cfg_lbls[i] = lbl;
        lv_obj_center(lbl);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);

        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                int idx = static_cast<int>(
                    reinterpret_cast<uintptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
                if (idx == 0) s_loop_mode = (s_loop_mode + 1) % 3;
                else if (idx == 1) s_shuffle = !s_shuffle;
                else if (idx == 2) s_lights = !s_lights;
                else if (idx == 3) s_move = !s_move;
                else if (idx == 4) { s_radio_mode = false; s_mode = MusicMode::Main; }
                else if (idx == 5) { s_radio_mode = !s_radio_mode; s_mode = MusicMode::Main; }
                refresh_music_ui();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* back_btn = community::make_btn(s_settings_cont, 8, 192, 304, 40, "BACK", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        back_btn,
        [](lv_event_t*) {
            s_mode = MusicMode::Main;
            refresh_music_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    refresh_music_ui();
}

void update_music_preview(uint32_t now_ms)
{
    if (!s_root) return;
    if (s_last_ms == 0) s_last_ms = now_ms;
    float dt  = (now_ms - s_last_ms) / 1000.0f;
    s_last_ms = now_ms;

    if (s_playing && !s_radio_mode) {
        s_elapsed_sec += dt;
        if (s_elapsed_sec >= TRACKS[s_track_idx].duration_sec) {
            s_elapsed_sec = 0.0f;
            s_track_idx   = (s_track_idx + 1) % TRACK_COUNT;
        }
    }

    const int top_y   = view::get_status_bar_top_offset();
    const int stage_h = 188 - (top_y + 55);
    for (int i = 0; i < 4; ++i) {
        if (!s_note_lbls[i]) continue;
        if (s_playing) {
            lv_obj_remove_flag(s_note_lbls[i], LV_OBJ_FLAG_HIDDEN);
            int dy = static_cast<int>(std::sin(now_ms * 0.005f + i * 1.3f) * 14.0f);
            lv_obj_set_y(s_note_lbls[i], stage_h / 2 - 10 + dy);
        } else {
            lv_obj_add_flag(s_note_lbls[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_mouth) {
        int mh = s_playing ? (8 + static_cast<int>(std::abs(std::sin(now_ms * 0.012f)) * 10.0f)) : 6;
        lv_obj_set_size(s_mouth, s_playing ? 42 : 34, mh);
    }

    refresh_music_ui();
}

void destroy_music_preview()
{
    if (s_root) {
        lv_obj_delete(s_root);
        s_root          = nullptr;
        s_main_cont     = nullptr;
        s_settings_cont = nullptr;
        s_lbl_title     = nullptr;
        s_bar_fill      = nullptr;
        s_lbl_elapsed   = nullptr;
        s_lbl_track_num = nullptr;
        s_lbl_duration  = nullptr;
        s_lbl_vol_ver   = nullptr;
        s_lbl_status    = nullptr;
        s_btn_play      = nullptr;
        s_lbl_play      = nullptr;
        s_mouth         = nullptr;
        for (auto& n : s_note_lbls) n = nullptr;
        for (auto& l : s_cfg_lbls) l = nullptr;
        for (auto& b : s_cfg_btns) b = nullptr;
    }
}

}  // namespace sim
