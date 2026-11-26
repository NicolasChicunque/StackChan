#include "app_nfc.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <hal/hal.h>

using namespace community;

AppNfc::AppNfc()
{
    setAppInfo().name         = "NFC";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_AMBER;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppNfc::onCreate() {}

void AppNfc::load_bindings()
{
    read_sd_config_lines("nfc.txt", _bindings, 8);
}

void AppNfc::onOpen()
{
    load_bindings();
    LvglLockGuard lock;
    build_ui();
    view::create_home_indicator([&]() { close(); }, COL_AMBER, COL_BG);
    view::create_status_bar(COL_AMBER, COL_BG);
}

void AppNfc::build_ui()
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

    _lbl_status = lv_label_create(card);
    lv_obj_set_pos(_lbl_status, 12, 10);
    lv_obj_set_width(_lbl_status, 280);
    lv_obj_set_style_text_font(_lbl_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(COL_AMBER), 0);
    lv_label_set_text(_lbl_status, "NFC Studio (Tag Bindings)");

    _lbl_list = lv_label_create(card);
    lv_obj_set_pos(_lbl_list, 12, 38);
    lv_obj_set_width(_lbl_list, 280);
    lv_label_set_long_mode(_lbl_list, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_list, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_list, lv_color_hex(COL_TEXT), 0);

    if (_bindings.empty()) {
        lv_label_set_text(_lbl_list, "Place UID|ActionURL lines in /sd/config/nfc.txt\nTap SCAN to probe onboard NFC.");
    } else {
        std::string txt;
        for (size_t i = 0; i < _bindings.size() && i < 4; ++i) {
            txt += _bindings[i] + "\n";
        }
        lv_label_set_text(_lbl_list, txt.c_str());
    }

    lv_obj_t* b_scan = make_btn(_root, 8, 182, 148, 42, "SCAN TAG", COL_AMBER, COL_BG);
    lv_obj_add_event_cb(
        b_scan,
        [](lv_event_t* e) {
            auto* self = static_cast<AppNfc*>(lv_event_get_user_data(e));
            lv_label_set_text(self->_lbl_status, "Hold NFC tag near base...");
            GetHAL().showRgbColor(255, 160, 0);
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* b_rel = make_btn(_root, 164, 182, 148, 42, "RELOAD SD", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        b_rel,
        [](lv_event_t* e) {
            auto* self = static_cast<AppNfc*>(lv_event_get_user_data(e));
            self->load_bindings();
            lv_label_set_text(self->_lbl_status, self->_bindings.empty() ? "No /sd/config/nfc.txt" : "Bindings reloaded");
        },
        LV_EVENT_CLICKED, this);
}

void AppNfc::onRunning()
{
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppNfc::onClose()
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
