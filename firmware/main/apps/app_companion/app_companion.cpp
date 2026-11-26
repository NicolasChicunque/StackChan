#include "app_companion.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <esp_random.h>
#include <hal/hal.h>
#include <settings.h>
#include <stackchan/stackchan.h>
#include <algorithm>
#include <cstdio>

using namespace community;

namespace {
constexpr const char* kNvsNs     = "app_buddy";
constexpr const char* kStories[] = {
    "Day 1: I woke up on your desk and counted 12 RGB LEDs. Nice place!",
    "Day 2: I practiced nodding smoothly while you were busy coding.",
    "Day 3: I wonder what songs are hidden inside /sd/music/ today.",
    "Day 4: Petting my head charges my mood faster than USB-C!",
    "Day 5: Quiet desk buddy mode: I'll just chill here and keep you company.",
    "Day 6: I dreamed about rolling a natural 20 in a tabletop game.",
};
constexpr int kStoryCount = static_cast<int>(sizeof(kStories) / sizeof(kStories[0]));
}  // namespace

AppCompanion::AppCompanion()
{
    setAppInfo().name         = "BUDDY";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_PURPLE;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppCompanion::onCreate() {}

void AppCompanion::onOpen()
{
    {
        Settings s(kNvsNs, false);
        _bond      = std::clamp<int>(s.GetInt("bond", 50), 0, 100);
        _curiosity = std::clamp<int>(s.GetInt("curiosity", 70), 0, 100);
        _story_idx = s.GetInt("story", 0) % kStoryCount;
    }
    _head_pet    = false;
    _next_glance = GetHAL().millis() + 4000;

    _head_conn = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture g) {
        if (g == HeadPetGesture::Press || g == HeadPetGesture::SwipeForward) {
            _head_pet = true;
        }
    });

    GetHAL().showRgbColor(90, 40, 160);
    LvglLockGuard lock;
    build_ui();
    refresh_ui();
    view::create_home_indicator([&]() { close(); }, COL_PURPLE, COL_BG);
    view::create_status_bar(COL_PURPLE, COL_BG);
}

void AppCompanion::interact_pet()
{
    _bond      = std::min(100, _bond + 5);
    _curiosity = std::min(100, _curiosity + 3);
    Settings s(kNvsNs, true);
    s.SetInt("bond", _bond);
    s.SetInt("curiosity", _curiosity);
    GetHAL().showRgbColor(40, 220, 140);
    GetStackChan().motion().moveWithSpeed(0, 180, 450);
    refresh_ui();
}

void AppCompanion::next_story()
{
    _story_idx = (_story_idx + 1) % kStoryCount;
    Settings s(kNvsNs, true);
    s.SetInt("story", _story_idx);
    refresh_ui();
}

void AppCompanion::build_ui()
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

    _lbl_mood = lv_label_create(card);
    lv_obj_set_pos(_lbl_mood, 12, 8);
    lv_obj_set_style_text_font(_lbl_mood, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_mood, lv_color_hex(COL_PURPLE), 0);

    _lbl_story = lv_label_create(card);
    lv_obj_set_pos(_lbl_story, 12, 34);
    lv_obj_set_width(_lbl_story, 280);
    lv_label_set_long_mode(_lbl_story, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_story, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_story, lv_color_hex(COL_WHITE), 0);

    _lbl_stats = lv_label_create(card);
    lv_obj_align(_lbl_stats, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    lv_obj_set_style_text_font(_lbl_stats, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_stats, lv_color_hex(COL_TEAL), 0);

    lv_obj_t* b_pet = make_btn(_root, 8, 182, 148, 42, "PET BUDDY", COL_PURPLE, COL_WHITE);
    lv_obj_add_event_cb(
        b_pet, [](lv_event_t* e) { static_cast<AppCompanion*>(lv_event_get_user_data(e))->interact_pet(); },
        LV_EVENT_CLICKED, this);

    lv_obj_t* b_story = make_btn(_root, 164, 182, 148, 42, "DIARY NEXT", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        b_story, [](lv_event_t* e) { static_cast<AppCompanion*>(lv_event_get_user_data(e))->next_story(); },
        LV_EVENT_CLICKED, this);
}

void AppCompanion::refresh_ui()
{
    if (!_lbl_mood) return;
    const char* mood = (_bond >= 80) ? "Mood: Super Happy" : (_bond >= 50 ? "Mood: Chill & Curious" : "Mood: Cozy");
    lv_label_set_text(_lbl_mood, mood);
    lv_label_set_text(_lbl_story, kStories[_story_idx]);
    char sb[64];
    std::snprintf(sb, sizeof(sb), "Bond: %d%%   |   Curiosity: %d%%   (Pet head!)", _bond, _curiosity);
    lv_label_set_text(_lbl_stats, sb);
}

void AppCompanion::onRunning()
{
    uint32_t now = GetHAL().millis();
    if (_head_pet.exchange(false)) {
        LvglLockGuard lock;
        interact_pet();
    }
    if (now >= _next_glance) {
        _next_glance = now + 7000 + (esp_random() % 6000);
        int yaw      = static_cast<int>(esp_random() % 300) - 150;
        GetStackChan().motion().moveWithSpeed(yaw, 120, 350);
    }
    GetStackChan().update();
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppCompanion::onClose()
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
