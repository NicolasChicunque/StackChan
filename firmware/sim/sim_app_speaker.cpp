#include "sim_apps.h"
#include "sim_ui_helpers.h"
#include <algorithm>
#include <cmath>

/**
 * Preview of StackChan's real SPEAKER app UI (mirroring app_speaker.cpp).
 */
namespace sim {

namespace {

constexpr int W        = 320;
constexpr int H        = 240;
constexpr int BAR4_Y   = 192;
constexpr int BAR4_H   = 42;
constexpr int BAR4_W   = 71;
constexpr int BAR4_GAP = 6;
constexpr int BAR4_X0  = 9;

bool s_usb_mode  = false;
bool s_muted     = false;
bool s_dance     = true;
bool s_lights    = true;
bool s_streaming = true;

lv_obj_t* s_root         = nullptr;
lv_obj_t* s_btn_wifi     = nullptr;
lv_obj_t* s_btn_usb      = nullptr;
lv_obj_t* s_lbl_badge    = nullptr;
lv_obj_t* s_badge_box    = nullptr;
lv_obj_t* s_lbl_hint     = nullptr;
lv_obj_t* s_mouth        = nullptr;
lv_obj_t* s_eq_objs[8]   = {};
lv_obj_t* s_btn_mute     = nullptr;
lv_obj_t* s_btn_dance    = nullptr;
lv_obj_t* s_btn_lights   = nullptr;

void refresh_speaker_ui()
{
    if (!s_root) return;

    if (s_btn_wifi) lv_obj_set_style_bg_color(s_btn_wifi, lv_color_hex(!s_usb_mode ? community::COL_TEAL_DK : community::COL_CARD), 0);
    if (s_btn_usb)  lv_obj_set_style_bg_color(s_btn_usb,  lv_color_hex(s_usb_mode ? community::COL_BLUE : community::COL_CARD), 0);

    bool active = s_streaming && !s_muted;
    if (s_badge_box) lv_obj_set_style_bg_color(s_badge_box, lv_color_hex(active ? 0x14532D : community::COL_CARD), 0);
    if (s_lbl_badge) {
        lv_label_set_text(s_lbl_badge, s_muted ? "MUTED" : (s_streaming ? "STREAMING" : "READY"));
        lv_obj_set_style_text_color(s_lbl_badge,
                                    lv_color_hex(s_muted ? community::COL_RED : (s_streaming ? community::COL_GREEN : community::COL_DIM)), 0);
    }

    if (s_lbl_hint) {
        lv_label_set_text(s_lbl_hint, s_usb_mode ? "USB-C OTG: StackChan Speaker (48kHz)" : "Wi-Fi Web: http://192.168.1.50/");
    }

    if (s_btn_mute)   lv_obj_set_style_bg_color(s_btn_mute,   lv_color_hex(s_muted ? community::COL_RED : community::COL_CARD), 0);
    if (s_btn_dance)  lv_obj_set_style_bg_color(s_btn_dance,  lv_color_hex(s_dance ? community::COL_TEAL_DK : community::COL_CARD), 0);
    if (s_btn_lights) lv_obj_set_style_bg_color(s_btn_lights, lv_color_hex(s_lights ? community::COL_PURPLE : community::COL_CARD), 0);
}

}  // namespace

void create_speaker_preview(lv_obj_t* parent)
{
    destroy_speaker_preview();

    const int top_y   = view::get_status_bar_top_offset();
    const bool fix    = (top_y > 0);
    const int mode_y  = top_y + 4;
    const int mode_h  = 28;
    const int stage_y = mode_y + mode_h + 6;
    const int stage_h = (BAR4_Y - 22) - stage_y;

    s_root = lv_obj_create(parent);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, W, H);
    community::style_box(s_root, community::COL_BG, 0);

    // 1. Mode Switcher Bar
    s_btn_wifi = community::make_btn(s_root, 8, mode_y, 88, mode_h, "WI-FI", community::COL_TEAL_DK,
                                     community::COL_WHITE, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        s_btn_wifi,
        [](lv_event_t*) {
            s_usb_mode = false;
            refresh_speaker_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    s_btn_usb = community::make_btn(s_root, 102, mode_y, 88, mode_h, "USB-C", community::COL_CARD,
                                    community::COL_WHITE, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        s_btn_usb,
        [](lv_event_t*) {
            s_usb_mode = true;
            refresh_speaker_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    s_badge_box = lv_obj_create(s_root);
    lv_obj_set_pos(s_badge_box, 200, mode_y);
    lv_obj_set_size(s_badge_box, 112, mode_h);
    community::style_box(s_badge_box, community::COL_CARD, 14);
    lv_obj_add_flag(s_badge_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        s_badge_box,
        [](lv_event_t*) {
            s_streaming = !s_streaming;
            refresh_speaker_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    s_lbl_badge = lv_label_create(s_badge_box);
    lv_obj_center(s_lbl_badge);
    lv_obj_set_style_text_font(s_lbl_badge, &lv_font_montserrat_12, 0);
    lv_obj_remove_flag(s_lbl_badge, LV_OBJ_FLAG_CLICKABLE);

    // 2. Stage Card: Face + 8-bar FFT EQ
    lv_obj_t* stage = lv_obj_create(s_root);
    lv_obj_set_pos(stage, 8, stage_y);
    lv_obj_set_size(stage, 304, stage_h);
    community::style_box(stage, community::COL_CARD, 14);

    const int face_h  = fix ? 58 : 78;
    lv_obj_t* face    = lv_obj_create(stage);
    lv_obj_set_size(face, 136, face_h);
    lv_obj_align(face, LV_ALIGN_TOP_MID, 0, fix ? 6 : 12);
    community::style_box(face, community::COL_BG, 16);

    lv_obj_t* el = lv_obj_create(face);
    lv_obj_set_size(el, 14, 14);
    lv_obj_align(el, LV_ALIGN_TOP_LEFT, 30, fix ? 12 : 18);
    community::style_box(el, community::COL_WHITE, 7);

    lv_obj_t* er = lv_obj_create(face);
    lv_obj_set_size(er, 14, 14);
    lv_obj_align(er, LV_ALIGN_TOP_RIGHT, -30, fix ? 12 : 18);
    community::style_box(er, community::COL_WHITE, 7);

    s_mouth = lv_obj_create(face);
    lv_obj_set_size(s_mouth, 36, 5);
    lv_obj_align(s_mouth, LV_ALIGN_BOTTOM_MID, 0, fix ? -10 : -14);
    community::style_box(s_mouth, community::COL_TEAL, 4);

    constexpr int kEqBarW = 24;
    constexpr int kEqGap  = 10;
    constexpr int kEqX0   = (304 - (8 * kEqBarW + 7 * kEqGap)) / 2;
    for (int i = 0; i < 8; ++i) {
        s_eq_objs[i] = lv_obj_create(stage);
        lv_obj_set_size(s_eq_objs[i], kEqBarW, 4);
        lv_obj_align(s_eq_objs[i], LV_ALIGN_BOTTOM_LEFT, kEqX0 + i * (kEqBarW + kEqGap), -6);
        community::style_box(s_eq_objs[i], community::COL_TEAL, 3);
    }

    s_lbl_hint = lv_label_create(s_root);
    lv_obj_set_pos(s_lbl_hint, 10, BAR4_Y - 18);
    lv_obj_set_size(s_lbl_hint, 300, 16);
    lv_obj_set_style_text_align(s_lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_lbl_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(community::COL_DIM), 0);

    // 3. Bottom 4-Button Control Bar
    s_btn_mute = community::make_btn(s_root, BAR4_X0 + 0 * (BAR4_W + BAR4_GAP), BAR4_Y, BAR4_W, BAR4_H, "MUTE",
                                     community::COL_CARD, community::COL_TEXT, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        s_btn_mute,
        [](lv_event_t*) {
            s_muted = !s_muted;
            refresh_speaker_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    s_btn_dance = community::make_btn(s_root, BAR4_X0 + 1 * (BAR4_W + BAR4_GAP), BAR4_Y, BAR4_W, BAR4_H, "DANCE",
                                      community::COL_CARD, community::COL_TEXT, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        s_btn_dance,
        [](lv_event_t*) {
            s_dance = !s_dance;
            refresh_speaker_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    s_btn_lights = community::make_btn(s_root, BAR4_X0 + 2 * (BAR4_W + BAR4_GAP), BAR4_Y, BAR4_W, BAR4_H, "LIGHTS",
                                       community::COL_CARD, community::COL_TEXT, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        s_btn_lights,
        [](lv_event_t*) {
            s_lights = !s_lights;
            refresh_speaker_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_sim = community::make_btn(s_root, BAR4_X0 + 3 * (BAR4_W + BAR4_GAP), BAR4_Y, BAR4_W, BAR4_H,
                                            "PAUSE/GO", community::COL_CARD, community::COL_TEXT, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        btn_sim,
        [](lv_event_t*) {
            s_streaming = !s_streaming;
            refresh_speaker_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    refresh_speaker_ui();
}

void update_speaker_preview(uint32_t now_ms)
{
    if (!s_root) return;
    const bool active = s_streaming && !s_muted;
    const float t     = now_ms * 0.008f;

    float avg = 0.0f;
    for (int i = 0; i < 8; ++i) {
        if (!s_eq_objs[i]) continue;
        int h = 4;
        if (active) {
            float v = 0.5f + 0.5f * std::sin(t * (1.1f + i * 0.27f) + i * 0.9f);
            h       = 6 + static_cast<int>(v * 26.0f);
            avg += v;
        }
        lv_obj_set_height(s_eq_objs[i], h);
    }

    if (s_mouth) {
        int mh = active ? (5 + static_cast<int>((avg / 8.0f) * 12.0f)) : 5;
        lv_obj_set_size(s_mouth, active ? 40 : 34, mh);
    }
}

void destroy_speaker_preview()
{
    if (s_root) {
        lv_obj_delete(s_root);
        s_root       = nullptr;
        s_btn_wifi   = nullptr;
        s_btn_usb    = nullptr;
        s_lbl_badge  = nullptr;
        s_badge_box  = nullptr;
        s_lbl_hint   = nullptr;
        s_mouth      = nullptr;
        s_btn_mute   = nullptr;
        s_btn_dance  = nullptr;
        s_btn_lights = nullptr;
        for (auto& e : s_eq_objs) e = nullptr;
    }
}

}  // namespace sim
