#include "app_health.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <settings.h>
#include <stackchan/stackchan.h>
#include <algorithm>
#include <cstdio>

using namespace community;

namespace {
constexpr const char* kNvsNs = "app_health";
}

AppHealth::AppHealth()
{
    setAppInfo().name         = "HEALTH";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_TEAL;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppHealth::onCreate() {}

void AppHealth::onOpen()
{
    {
        Settings s(kNvsNs, false);
        _break_min  = std::clamp<int>(s.GetInt("break_min", 30), 10, 90);
        _water_cups = std::max<int>(0, s.GetInt("water_cups", 0));
    }
    _alert_active = false;
    _next_due_ms  = GetHAL().millis() + static_cast<uint32_t>(_break_min) * 60000U;
    _head_pet     = false;

    _head_conn = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture g) {
        if (g == HeadPetGesture::Press || g == HeadPetGesture::SwipeForward) {
            _head_pet = true;
        }
    });

    GetHAL().showRgbColor(0, 24, 24);
    LvglLockGuard lock;
    build_ui();
    refresh_ui();
    view::create_home_indicator([&]() { close(); }, COL_TEAL, COL_BG);
    view::create_status_bar(COL_TEAL, COL_BG);
}

void AppHealth::log_water()
{
    _water_cups++;
    Settings s(kNvsNs, true);
    s.SetInt("water_cups", _water_cups);
    ack_break();
}

void AppHealth::ack_break()
{
    _alert_active = false;
    _next_due_ms  = GetHAL().millis() + static_cast<uint32_t>(_break_min) * 60000U;
    GetHAL().showRgbColor(0, 28, 16);
    refresh_ui();
}

void AppHealth::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    _root         = lv_obj_create(scr);
    lv_obj_set_size(_root, 320, 240);
    style_box(_root, COL_BG, 0);

    const int top_y = view::get_status_bar_top_offset();

    lv_obj_t* card = lv_obj_create(_root);
    lv_obj_set_pos(card, 8, top_y + 6);
    lv_obj_set_size(card, 304, 132 - (top_y > 0 ? 12 : 0));
    style_box(card, COL_CARD, 14);

    _lbl_prompt = lv_label_create(card);
    lv_obj_set_pos(_lbl_prompt, 12, 10);
    lv_obj_set_width(_lbl_prompt, 280);
    lv_obj_set_style_text_font(_lbl_prompt, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_prompt, lv_color_hex(COL_TEAL), 0);

    _lbl_timer = lv_label_create(card);
    lv_obj_set_pos(_lbl_timer, 12, 42);
    lv_obj_set_style_text_font(_lbl_timer, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lbl_timer, lv_color_hex(COL_WHITE), 0);

    _lbl_water = lv_label_create(card);
    lv_obj_align(_lbl_water, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    lv_obj_set_style_text_font(_lbl_water, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_water, lv_color_hex(COL_AMBER), 0);

    lv_obj_t* btn_water = make_btn(_root, 8, 182, 110, 42, "+1 WATER", COL_BLUE, COL_WHITE);
    lv_obj_add_event_cb(
        btn_water, [](lv_event_t* e) { static_cast<AppHealth*>(lv_event_get_user_data(e))->log_water(); },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_int = make_btn(_root, 124, 182, 92, 42, "INTERVAL", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_int,
        [](lv_event_t* e) {
            auto* self       = static_cast<AppHealth*>(lv_event_get_user_data(e));
            self->_break_min = (self->_break_min >= 60) ? 15 : (self->_break_min + 15);
            Settings s(kNvsNs, true);
            s.SetInt("break_min", self->_break_min);
            self->ack_break();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_rst = make_btn(_root, 222, 182, 90, 42, "RESET", COL_CARD, COL_DIM);
    lv_obj_add_event_cb(
        btn_rst,
        [](lv_event_t* e) {
            auto* self        = static_cast<AppHealth*>(lv_event_get_user_data(e));
            self->_water_cups = 0;
            Settings s(kNvsNs, true);
            s.SetInt("water_cups", 0);
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);
}

void AppHealth::refresh_ui()
{
    if (!_lbl_timer) return;
    uint32_t now = GetHAL().millis();
    uint32_t rem = (_next_due_ms > now) ? (_next_due_ms - now) / 1000U : 0;
    char tb[32], wb[48];
    std::snprintf(tb, sizeof(tb), "%02u:%02u", static_cast<unsigned>(rem / 60), static_cast<unsigned>(rem % 60));
    std::snprintf(wb, sizeof(wb), "Water today: %d cups  |  Every %dm", _water_cups, _break_min);
    lv_label_set_text(_lbl_prompt, _alert_active ? "TIME TO STRETCH & DRINK! (Pet head)" : "Active Break & Water");
    lv_obj_set_style_text_color(_lbl_prompt, lv_color_hex(_alert_active ? COL_AMBER : COL_TEAL), 0);
    lv_label_set_text(_lbl_timer, tb);
    lv_label_set_text(_lbl_water, wb);
}

void AppHealth::onRunning()
{
    uint32_t now = GetHAL().millis();
    if (_head_pet.exchange(false)) {
        LvglLockGuard lock;
        log_water();
    }
    if (!_alert_active && now >= _next_due_ms) {
        _alert_active = true;
        GetHAL().showRgbColor(255, 140, 0);
        GetStackChan().motion().moveWithSpeed(0, 200, 400);
        LvglLockGuard lock;
        refresh_ui();
    }
    if (now - _last_ui_ms >= 500) {
        _last_ui_ms = now;
        LvglLockGuard lock;
        refresh_ui();
    }
    GetStackChan().update();
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppHealth::onClose()
{
    if (_head_conn >= 0) {
        GetHAL().onHeadPetGesture.disconnect(_head_conn);
        _head_conn = -1;
    }
    GetHAL().showRgbColor(0, 0, 0);
    LvglLockGuard lock;
    view::destroy_home_indicator();
    view::destroy_status_bar();
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}
