#pragma once
/**
 * Desktop-compatible StackChan UI helpers & simulated top status bar.
 * Mirrors firmware/main/apps/common/community_helpers.h and status_bar.h
 * so LVGL screens written in sim_sandbox.cpp transfer directly to ESP32-S3 apps.
 */
#include <lvgl.h>
#include <cstdint>
#include <functional>
#include <string>

namespace community {

inline constexpr uint32_t COL_BG      = 0x0B0F1A;
inline constexpr uint32_t COL_CARD    = 0x151B2E;
inline constexpr uint32_t COL_CARD_HI = 0x232C4A;
inline constexpr uint32_t COL_TEAL    = 0x2DD4BF;
inline constexpr uint32_t COL_TEAL_DK = 0x0F766E;
inline constexpr uint32_t COL_AMBER   = 0xF59E0B;
inline constexpr uint32_t COL_GREEN   = 0x22C55E;
inline constexpr uint32_t COL_BLUE    = 0x3B82F6;
inline constexpr uint32_t COL_PURPLE  = 0x8B5CF6;
inline constexpr uint32_t COL_RED     = 0xEF4444;
inline constexpr uint32_t COL_TEXT    = 0xE2E8F0;
inline constexpr uint32_t COL_DIM     = 0x94A3B8;
inline constexpr uint32_t COL_WHITE   = 0xFFFFFF;

inline void style_box(lv_obj_t* obj, uint32_t bg, int radius = 10)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

inline lv_obj_t* make_btn(lv_obj_t* parent, int x, int y, int w, int h, const char* txt, uint32_t bg,
                          uint32_t fg = COL_WHITE, const lv_font_t* font = &lv_font_montserrat_14)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    style_box(b, bg, 10);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, txt);
    return b;
}

}  // namespace community

namespace sim {

enum class Emotion : uint8_t {
    Calm = 0,
    Happy,
    Sleepy,
    Thinking,
    Shy,
    Excited,
};

struct SimState {
    int volume            = 70;
    int battery_pct       = 88;
    bool wifi_connected   = true;
    int servo_yaw_deg     = 0;   // -90..+90
    int servo_pitch_deg   = 0;   // -30..+45
    uint32_t rgb_hex      = 0x000000;
    Emotion emotion       = Emotion::Calm;
    bool status_bar_fixed = true;
};

SimState& state();
void navigate_home();
void set_status_bar_fixed(bool fixed);
int status_bar_top_offset();

}  // namespace sim

namespace view {

enum class StatusBarMode : uint8_t {
    Fixed    = 0,
    AutoHide = 1,
};

inline StatusBarMode get_status_bar_mode()
{
    return sim::state().status_bar_fixed ? StatusBarMode::Fixed : StatusBarMode::AutoHide;
}

inline int get_status_bar_top_offset()
{
    return sim::status_bar_top_offset();
}

}  // namespace view
