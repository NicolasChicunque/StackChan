#include "app_english.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <settings.h>
#include <cstdio>
#include <ctime>

using namespace community;

namespace {
constexpr const char* kNvsNs = "app_english";
struct FlashCard {
    const char* en;
    const char* es;
};
constexpr FlashCard kCards[] = {
    {"Keep it simple!", "Mantenlo simple!"},
    {"Could you repeat that, please?", "Podrias repetir eso, por favor?"},
    {"I am working on a robotics project.", "Estoy trabajando en un proyecto de robotica."},
    {"Let's take a quick break.", "Tomemos una pausa rapida."},
    {"That makes total sense.", "Eso tiene todo el sentido."},
    {"What's on the schedule today?", "Que hay en la agenda hoy?"},
    {"I'll get back to you shortly.", "Te respondo en un momento."},
    {"Good job! Keep the streak alive!", "Buen trabajo! Manten viva la racha!"},
};
constexpr int kCardCount = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
}  // namespace

AppEnglish::AppEnglish()
{
    setAppInfo().name         = "ENGLISH";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_GREEN;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppEnglish::onCreate() {}

void AppEnglish::onOpen()
{
    {
        Settings s(kNvsNs, false);
        _streak   = s.GetInt("streak", 0);
        _xp       = s.GetInt("xp", 0);
        _last_day = s.GetInt("last_day", 0);
        _card_idx = s.GetInt("card_idx", 0) % kCardCount;
    }
    _show_hint = false;

    LvglLockGuard lock;
    build_ui();
    refresh_ui();
    view::create_home_indicator([&]() { close(); }, COL_GREEN, COL_BG);
    view::create_status_bar(COL_GREEN, COL_BG);
}

void AppEnglish::complete_card()
{
    _xp += 10;
    _card_idx  = (_card_idx + 1) % kCardCount;
    _show_hint = false;

    time_t now  = std::time(nullptr);
    int today   = static_cast<int>(now / 86400);
    if (today > 19000) {
        if (_last_day == 0 || today > _last_day) {
            _streak   = (_last_day > 0 && today == _last_day + 1) ? (_streak + 1) : 1;
            _last_day = today;
        }
    } else if (_streak == 0) {
        _streak = 1;
    }

    Settings s(kNvsNs, true);
    s.SetInt("streak", _streak);
    s.SetInt("xp", _xp);
    s.SetInt("last_day", _last_day);
    s.SetInt("card_idx", _card_idx);

    GetHAL().showRgbColor(0, 180, 60);
    refresh_ui();
}

void AppEnglish::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    _root         = lv_obj_create(scr);
    lv_obj_set_size(_root, 320, 240);
    style_box(_root, COL_BG, 0);

    const int top_y = view::get_status_bar_top_offset();

    lv_obj_t* card = lv_obj_create(_root);
    lv_obj_set_pos(card, 8, top_y + 6);
    lv_obj_set_size(card, 304, 134 - (top_y > 0 ? 12 : 0));
    style_box(card, COL_CARD, 14);

    _lbl_streak = lv_label_create(card);
    lv_obj_set_pos(_lbl_streak, 12, 8);
    lv_obj_set_style_text_font(_lbl_streak, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_streak, lv_color_hex(COL_AMBER), 0);

    _lbl_en = lv_label_create(card);
    lv_obj_set_pos(_lbl_en, 12, 34);
    lv_obj_set_width(_lbl_en, 280);
    lv_label_set_long_mode(_lbl_en, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_en, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_en, lv_color_hex(COL_WHITE), 0);

    _lbl_es = lv_label_create(card);
    lv_obj_align(_lbl_es, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    lv_obj_set_width(_lbl_es, 280);
    lv_label_set_long_mode(_lbl_es, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_es, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_es, lv_color_hex(COL_TEAL), 0);

    lv_obj_t* b_hint = make_btn(_root, 8, 182, 148, 42, "TRANSLATE", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        b_hint,
        [](lv_event_t* e) {
            auto* self       = static_cast<AppEnglish*>(lv_event_get_user_data(e));
            self->_show_hint = !self->_show_hint;
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* b_done = make_btn(_root, 164, 182, 148, 42, "GOT IT (+10XP)", COL_GREEN, COL_BG);
    lv_obj_add_event_cb(
        b_done, [](lv_event_t* e) { static_cast<AppEnglish*>(lv_event_get_user_data(e))->complete_card(); },
        LV_EVENT_CLICKED, this);
}

void AppEnglish::refresh_ui()
{
    if (!_lbl_en) return;
    char sb[64];
    std::snprintf(sb, sizeof(sb), "Streak: %d day(s)  |  %d XP", _streak, _xp);
    lv_label_set_text(_lbl_streak, sb);
    lv_label_set_text(_lbl_en, kCards[_card_idx].en);
    lv_label_set_text(_lbl_es, _show_hint ? kCards[_card_idx].es : "Say it aloud, then tap TRANSLATE");
}

void AppEnglish::onRunning()
{
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppEnglish::onClose()
{
    GetHAL().showRgbColor(0, 0, 0);
    LvglLockGuard lock;
    view::destroy_home_indicator();
    view::destroy_status_bar();
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}
