#include "app_games.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <esp_random.h>
#include <hal/hal.h>
#include <stackchan/stackchan.h>
#include <cstdio>

using namespace community;

namespace {
constexpr int kDieSides[6] = {4, 6, 8, 10, 12, 20};
}

AppGames::AppGames()
{
    setAppInfo().name         = "GAMES";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_PURPLE;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppGames::onCreate() {}

void AppGames::onOpen()
{
    _shake    = false;
    _imu_conn = GetHAL().onImuMotionEvent.connect([this](ImuMotionEvent e) {
        if (e == ImuMotionEvent::Shake) {
            _shake = true;
        }
    });

    LvglLockGuard lock;
    build_ui();
    refresh_ui();
    view::create_home_indicator([&]() { close(); }, COL_PURPLE, COL_BG);
    view::create_status_bar(COL_PURPLE, COL_BG);
}

void AppGames::roll_dice()
{
    int sides  = kDieSides[_die_idx];
    _last_roll = static_cast<int>(esp_random() % static_cast<uint32_t>(sides)) + 1;
    GetHAL().showRgbColor((_last_roll == sides) ? 0 : 120, (_last_roll == sides) ? 255 : 40, 220);
    GetStackChan().motion().moveWithSpeed((_last_roll % 2 == 0) ? 120 : -120, 100, 600);
    refresh_ui();
}

void AppGames::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    _root         = lv_obj_create(scr);
    lv_obj_set_size(_root, 320, 240);
    style_box(_root, COL_BG, 0);

    const int top_y = view::get_status_bar_top_offset();

    lv_obj_t* card = lv_obj_create(_root);
    lv_obj_set_pos(card, 8, top_y + 6);
    lv_obj_set_size(card, 304, 130 - (top_y > 0 ? 12 : 0));
    style_box(card, COL_CARD, 14);

    _lbl_die = lv_label_create(card);
    lv_obj_set_pos(_lbl_die, 14, 10);
    lv_obj_set_style_text_font(_lbl_die, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_die, lv_color_hex(COL_PURPLE), 0);

    _lbl_val = lv_label_create(card);
    lv_obj_align(_lbl_val, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_text_font(_lbl_val, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lbl_val, lv_color_hex(COL_WHITE), 0);

    _lbl_score = lv_label_create(card);
    lv_obj_align(_lbl_score, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_font(_lbl_score, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_score, lv_color_hex(COL_AMBER), 0);

    lv_obj_t* b_roll = make_btn(_root, 8, 182, 86, 42, "ROLL", COL_PURPLE, COL_WHITE);
    lv_obj_add_event_cb(
        b_roll, [](lv_event_t* e) { static_cast<AppGames*>(lv_event_get_user_data(e))->roll_dice(); },
        LV_EVENT_CLICKED, this);

    lv_obj_t* b_die = make_btn(_root, 98, 182, 66, 42, "DIE", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        b_die,
        [](lv_event_t* e) {
            auto* self     = static_cast<AppGames*>(lv_event_get_user_data(e));
            self->_die_idx = (self->_die_idx + 1) % 6;
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* b_p1 = make_btn(_root, 168, 182, 68, 42, "P1 +1", COL_TEAL_DK, COL_WHITE);
    lv_obj_add_event_cb(
        b_p1,
        [](lv_event_t* e) {
            auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
            self->_scores[0]++;
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* b_p2 = make_btn(_root, 240, 182, 72, 42, "P2 +1", COL_BLUE, COL_WHITE);
    lv_obj_add_event_cb(
        b_p2,
        [](lv_event_t* e) {
            auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
            self->_scores[1]++;
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);
}

void AppGames::refresh_ui()
{
    if (!_lbl_val) return;
    char d[32], v[32], s[64];
    std::snprintf(d, sizeof(d), "D%d  (Shake to roll!)", kDieSides[_die_idx]);
    std::snprintf(v, sizeof(v), "[ %d ]", _last_roll);
    std::snprintf(s, sizeof(s), "Score   P1: %d   |   P2: %d", _scores[0], _scores[1]);
    lv_label_set_text(_lbl_die, d);
    lv_label_set_text(_lbl_val, v);
    lv_label_set_text(_lbl_score, s);
}

void AppGames::onRunning()
{
    uint32_t now = GetHAL().millis();
    if (_shake.exchange(false) && (now - _last_roll_ms) >= 900) {
        _last_roll_ms = now;
        LvglLockGuard lock;
        roll_dice();
    }
    GetStackChan().update();
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppGames::onClose()
{
    if (_imu_conn >= 0) {
        GetHAL().onImuMotionEvent.disconnect(_imu_conn);
        _imu_conn = -1;
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
