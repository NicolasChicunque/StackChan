#include "sim_apps.h"
#include "sim_ui_helpers.h"
#include <cstdio>
#include <ctime>

/**
 * Preview of StackChan's real ALARM app UI (mirroring app_alarm.cpp).
 */
namespace sim {

namespace {

constexpr int W = 320;
constexpr int H = 240;

enum class AlarmScreen : uint8_t { Clock = 0, List, Timer };

AlarmScreen s_screen  = AlarmScreen::Clock;
bool s_night_mode     = false;
bool s_alarm_on[2]    = {true, false};
bool s_timer_running  = false;
float s_timer_sec     = 300.0f;  // 05:00
uint32_t s_last_ms    = 0;

lv_obj_t* s_root      = nullptr;
lv_obj_t* s_scr_clock = nullptr;
lv_obj_t* s_scr_list  = nullptr;
lv_obj_t* s_scr_timer = nullptr;
lv_obj_t* s_stage     = nullptr;
lv_obj_t* s_lbl_time  = nullptr;
lv_obj_t* s_lbl_sec   = nullptr;
lv_obj_t* s_lbl_date  = nullptr;
lv_obj_t* s_lbl_next  = nullptr;
lv_obj_t* s_btn_night = nullptr;
lv_obj_t* s_eye_l     = nullptr;
lv_obj_t* s_eye_r     = nullptr;
lv_obj_t* s_lbl_tmr   = nullptr;
lv_obj_t* s_btn_tmr   = nullptr;
lv_obj_t* s_btn_alm[2] = {};

void refresh_alarm_ui()
{
    if (!s_root) return;

    auto set_vis = [](lv_obj_t* obj, bool show) {
        if (!obj) return;
        if (show) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    };
    set_vis(s_scr_clock, s_screen == AlarmScreen::Clock);
    set_vis(s_scr_list, s_screen == AlarmScreen::List);
    set_vis(s_scr_timer, s_screen == AlarmScreen::Timer);

    // Real PC local clock
    std::time_t t = std::time(nullptr);
    std::tm tm_now{};
#ifdef _WIN32
    localtime_s(&tm_now, &t);
#else
    localtime_r(&t, &tm_now);
#endif

    char buf_hm[16], buf_s[16], buf_d[32];
    std::snprintf(buf_hm, sizeof(buf_hm), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    std::snprintf(buf_s, sizeof(buf_s), ":%02d", tm_now.tm_sec);
    std::strftime(buf_d, sizeof(buf_d), "%a, %b %d", &tm_now);

    if (s_lbl_time) lv_label_set_text(s_lbl_time, buf_hm);
    if (s_lbl_sec) lv_label_set_text(s_lbl_sec, buf_s);
    if (s_lbl_date) lv_label_set_text(s_lbl_date, buf_d);

    if (s_stage) {
        lv_obj_set_style_bg_color(s_stage, lv_color_hex(s_night_mode ? 0x05070D : community::COL_CARD), 0);
    }
    if (s_lbl_time) {
        lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(s_night_mode ? community::COL_AMBER : community::COL_WHITE), 0);
    }
    if (s_eye_l && s_eye_r) {
        int eh = s_night_mode ? 4 : 16;
        lv_obj_set_size(s_eye_l, 16, eh);
        lv_obj_set_size(s_eye_r, 16, eh);
    }
    if (s_btn_night) {
        lv_obj_set_style_bg_color(s_btn_night, lv_color_hex(s_night_mode ? community::COL_AMBER : community::COL_CARD), 0);
    }
    if (s_lbl_next) {
        if (s_alarm_on[0]) lv_label_set_text(s_lbl_next, LV_SYMBOL_BELL " Next: 07:00  Wake Up (Mon-Fri)");
        else if (s_alarm_on[1]) lv_label_set_text(s_lbl_next, LV_SYMBOL_BELL " Next: 08:30  Weekend");
        else lv_label_set_text(s_lbl_next, "No active alarms");
    }

    for (int i = 0; i < 2; ++i) {
        if (s_btn_alm[i]) {
            lv_obj_set_style_bg_color(s_btn_alm[i],
                                      lv_color_hex(s_alarm_on[i] ? community::COL_TEAL_DK : community::COL_CARD_HI), 0);
            if (lv_obj_t* l = lv_obj_get_child(s_btn_alm[i], 0)) {
                lv_label_set_text(l, s_alarm_on[i] ? "ON" : "OFF");
            }
        }
    }

    if (s_lbl_tmr) {
        int sec = static_cast<int>(s_timer_sec);
        if (sec < 0) sec = 0;
        lv_label_set_text_fmt(s_lbl_tmr, "%02d:%02d", sec / 60, sec % 60);
    }
    if (s_btn_tmr) {
        lv_obj_set_style_bg_color(s_btn_tmr, lv_color_hex(s_timer_running ? community::COL_RED : community::COL_GREEN), 0);
        if (lv_obj_t* l = lv_obj_get_child(s_btn_tmr, 0)) {
            lv_label_set_text(l, s_timer_running ? "PAUSE" : "START");
        }
    }
}

}  // namespace

void create_alarm_preview(lv_obj_t* parent)
{
    destroy_alarm_preview();
    s_screen  = AlarmScreen::Clock;
    s_last_ms = 0;

    const int top_y         = view::get_status_bar_top_offset();
    const bool is_fix       = (top_y > 0);
    const int clock_stage_y = is_fix ? (top_y + 4) : 10;
    const int clock_stage_h = 180 - clock_stage_y;

    s_root = lv_obj_create(parent);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, W, H);
    community::style_box(s_root, community::COL_BG, 0);

    /* ======================== 1. CLOCK SCREEN ======================== */
    s_scr_clock = lv_obj_create(s_root);
    lv_obj_set_pos(s_scr_clock, 0, 0);
    lv_obj_set_size(s_scr_clock, W, H);
    community::style_box(s_scr_clock, community::COL_BG, 0);

    s_stage = lv_obj_create(s_scr_clock);
    lv_obj_set_pos(s_stage, 8, clock_stage_y);
    lv_obj_set_size(s_stage, 304, clock_stage_h);
    community::style_box(s_stage, community::COL_CARD, 14);
    lv_obj_set_style_border_width(s_stage, 1, 0);
    lv_obj_set_style_border_color(s_stage, lv_color_hex(community::COL_CARD_HI), 0);

    s_lbl_time = lv_label_create(s_stage);
    lv_obj_set_pos(s_lbl_time, 14, 24);
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(community::COL_WHITE), 0);

    s_lbl_sec = lv_label_create(s_stage);
    lv_obj_set_pos(s_lbl_sec, 90, 32);
    lv_obj_set_style_text_font(s_lbl_sec, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_sec, lv_color_hex(community::COL_TEAL), 0);

    s_lbl_date = lv_label_create(s_stage);
    lv_obj_set_pos(s_lbl_date, 14, 58);
    lv_obj_set_style_text_font(s_lbl_date, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_date, lv_color_hex(community::COL_DIM), 0);

    lv_obj_t* face_card = lv_obj_create(s_stage);
    lv_obj_set_size(face_card, 118, 86);
    lv_obj_align(face_card, LV_ALIGN_RIGHT_MID, -14, -10);
    community::style_box(face_card, community::COL_BG, 14);
    lv_obj_set_style_border_width(face_card, 2, 0);
    lv_obj_set_style_border_color(face_card, lv_color_hex(community::COL_TEAL_DK), 0);

    s_eye_l = lv_obj_create(face_card);
    lv_obj_set_size(s_eye_l, 16, 16);
    lv_obj_align(s_eye_l, LV_ALIGN_CENTER, -24, -8);
    community::style_box(s_eye_l, community::COL_WHITE, LV_RADIUS_CIRCLE);

    s_eye_r = lv_obj_create(face_card);
    lv_obj_set_size(s_eye_r, 16, 16);
    lv_obj_align(s_eye_r, LV_ALIGN_CENTER, 24, -8);
    community::style_box(s_eye_r, community::COL_WHITE, LV_RADIUS_CIRCLE);

    lv_obj_t* mouth = lv_obj_create(face_card);
    lv_obj_set_size(mouth, 34, 6);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 16);
    community::style_box(mouth, community::COL_WHITE, 4);

    s_lbl_next = lv_label_create(s_stage);
    lv_obj_align(s_lbl_next, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_width(s_lbl_next, 286);
    lv_obj_set_style_text_align(s_lbl_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_lbl_next, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_next, lv_color_hex(community::COL_AMBER), 0);

    lv_obj_t* btn_alarms = community::make_btn(s_scr_clock, 8, 186, 88, 42, LV_SYMBOL_BELL " ALARMS",
                                               community::COL_TEAL_DK, community::COL_WHITE, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        btn_alarms,
        [](lv_event_t*) {
            s_screen = AlarmScreen::List;
            refresh_alarm_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_timer = community::make_btn(s_scr_clock, 102, 186, 72, 42, "TIMER", community::COL_CARD,
                                              community::COL_AMBER, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        btn_timer,
        [](lv_event_t*) {
            s_screen = AlarmScreen::Timer;
            refresh_alarm_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    s_btn_night = community::make_btn(s_scr_clock, 180, 186, 66, 42, "NIGHT", community::COL_CARD,
                                      community::COL_TEXT, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        s_btn_night,
        [](lv_event_t*) {
            s_night_mode = !s_night_mode;
            refresh_alarm_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_set = community::make_btn(s_scr_clock, 252, 186, 60, 42, LV_SYMBOL_SETTINGS " SET",
                                            community::COL_CARD, community::COL_TEXT, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        btn_set,
        [](lv_event_t*) {
            s_screen = AlarmScreen::List;
            refresh_alarm_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    /* ======================== 2. ALARMS LIST ========================= */
    s_scr_list = lv_obj_create(s_root);
    lv_obj_set_pos(s_scr_list, 0, 0);
    lv_obj_set_size(s_scr_list, W, H);
    community::style_box(s_scr_list, community::COL_BG, 0);
    lv_obj_add_flag(s_scr_list, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* list_hdr = lv_label_create(s_scr_list);
    lv_obj_set_pos(list_hdr, 12, top_y + 8);
    lv_obj_set_style_text_font(list_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(list_hdr, lv_color_hex(community::COL_TEAL), 0);
    lv_label_set_text(list_hdr, "< ALARMS");
    lv_obj_add_flag(list_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        list_hdr,
        [](lv_event_t*) {
            s_screen = AlarmScreen::Clock;
            refresh_alarm_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    const char* titles[2] = {"07:00  Wake Up (Mon-Fri)", "08:30  Weekend"};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* row = lv_obj_create(s_scr_list);
        lv_obj_set_pos(row, 10, top_y + 38 + i * 54);
        lv_obj_set_size(row, 300, 46);
        community::style_box(row, community::COL_CARD, 10);

        lv_obj_t* l = lv_label_create(row);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(community::COL_TEXT), 0);
        lv_label_set_text(l, titles[i]);

        s_btn_alm[i] = community::make_btn(row, 236, 8, 54, 30, "ON", community::COL_TEAL_DK, community::COL_WHITE,
                                           &lv_font_montserrat_12);
        lv_obj_set_user_data(s_btn_alm[i], reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(
            s_btn_alm[i],
            [](lv_event_t* e) {
                int idx = static_cast<int>(
                    reinterpret_cast<uintptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
                s_alarm_on[idx] = !s_alarm_on[idx];
                refresh_alarm_ui();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* btn_back1 = community::make_btn(s_scr_list, 10, 190, 300, 38, "BACK TO CLOCK", community::COL_CARD_HI);
    lv_obj_add_event_cb(
        btn_back1,
        [](lv_event_t*) {
            s_screen = AlarmScreen::Clock;
            refresh_alarm_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    /* ======================== 3. TIMER SCREEN ======================== */
    s_scr_timer = lv_obj_create(s_root);
    lv_obj_set_pos(s_scr_timer, 0, 0);
    lv_obj_set_size(s_scr_timer, W, H);
    community::style_box(s_scr_timer, community::COL_BG, 0);
    lv_obj_add_flag(s_scr_timer, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* tmr_hdr = lv_label_create(s_scr_timer);
    lv_obj_set_pos(tmr_hdr, 12, top_y + 8);
    lv_obj_set_style_text_font(tmr_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tmr_hdr, lv_color_hex(community::COL_AMBER), 0);
    lv_label_set_text(tmr_hdr, "< COUNTDOWN TIMER");
    lv_obj_add_flag(tmr_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        tmr_hdr,
        [](lv_event_t*) {
            s_screen = AlarmScreen::Clock;
            refresh_alarm_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    s_lbl_tmr = lv_label_create(s_scr_timer);
    lv_obj_align(s_lbl_tmr, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_text_font(s_lbl_tmr, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_tmr, lv_color_hex(community::COL_WHITE), 0);

    s_btn_tmr = community::make_btn(s_scr_timer, 12, 186, 142, 42, "START", community::COL_GREEN);
    lv_obj_add_event_cb(
        s_btn_tmr,
        [](lv_event_t*) {
            s_timer_running = !s_timer_running;
            refresh_alarm_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_rst = community::make_btn(s_scr_timer, 166, 186, 142, 42, "RESET 05:00", community::COL_CARD_HI);
    lv_obj_add_event_cb(
        btn_rst,
        [](lv_event_t*) {
            s_timer_running = false;
            s_timer_sec     = 300.0f;
            refresh_alarm_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    refresh_alarm_ui();
}

void update_alarm_preview(uint32_t now_ms)
{
    if (!s_root) return;
    if (s_last_ms == 0) s_last_ms = now_ms;
    float dt  = (now_ms - s_last_ms) / 1000.0f;
    s_last_ms = now_ms;

    if (s_timer_running && s_timer_sec > 0.0f) {
        s_timer_sec -= dt;
        if (s_timer_sec <= 0.0f) {
            s_timer_sec     = 0.0f;
            s_timer_running = false;
        }
    }
    refresh_alarm_ui();
}

void destroy_alarm_preview()
{
    if (s_root) {
        lv_obj_delete(s_root);
        s_root      = nullptr;
        s_scr_clock = nullptr;
        s_scr_list  = nullptr;
        s_scr_timer = nullptr;
        s_stage     = nullptr;
        s_lbl_time  = nullptr;
        s_lbl_sec   = nullptr;
        s_lbl_date  = nullptr;
        s_lbl_next  = nullptr;
        s_btn_night = nullptr;
        s_eye_l     = nullptr;
        s_eye_r     = nullptr;
        s_lbl_tmr   = nullptr;
        s_btn_tmr   = nullptr;
        s_btn_alm[0] = nullptr;
        s_btn_alm[1] = nullptr;
    }
}

}  // namespace sim
