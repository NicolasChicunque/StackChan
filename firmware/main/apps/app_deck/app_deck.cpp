#include "app_deck.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <hal/hal.h>

using namespace community;

AppDeck::AppDeck()
{
    setAppInfo().name         = "DECK";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_BLUE;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppDeck::onCreate() {}

void AppDeck::load_macros()
{
    _macros.clear();
    std::vector<std::string> lines;
    static constexpr uint32_t kCols[6] = {COL_BLUE, COL_TEAL_DK, COL_PURPLE, COL_AMBER, COL_GREEN, COL_RED};
    if (read_sd_config_lines("deck.txt", lines, 6)) {
        for (size_t i = 0; i < lines.size(); ++i) {
            auto sep = lines[i].find('|');
            if (sep == std::string::npos) continue;
            _macros.push_back({trim(lines[i].substr(0, sep)), trim(lines[i].substr(sep + 1)), kCols[i % 6]});
        }
    }
    while (_macros.size() < 6) {
        size_t i = _macros.size();
        _macros.push_back({"MACRO " + std::to_string(i + 1), "", kCols[i % 6]});
    }
}

void AppDeck::onOpen()
{
    load_macros();
    _pending_idx = -1;
    LvglLockGuard lock;
    build_ui();
    view::create_home_indicator([&]() { close(); }, COL_BLUE, COL_BG);
    view::create_status_bar(COL_BLUE, COL_BG);
}

void AppDeck::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    _root         = lv_obj_create(scr);
    lv_obj_set_size(_root, 320, 240);
    style_box(_root, COL_BG, 0);

    const int top_y = view::get_status_bar_top_offset();
    const int y0    = top_y + 8;

    for (int i = 0; i < 6; ++i) {
        int col = i % 3;
        int row = i / 3;
        lv_obj_t* b = make_btn(_root, 8 + col * 103, y0 + row * 72, 97, 64, _macros[i].label.c_str(), _macros[i].color,
                               COL_WHITE);
        lv_obj_set_user_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            b,
            [](lv_event_t* e) {
                auto* self = static_cast<AppDeck*>(lv_event_get_user_data(e));
                int idx    = static_cast<int>(
                    reinterpret_cast<intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
                self->_pending_idx = idx;
            },
            LV_EVENT_CLICKED, this);
    }

    _lbl_status = lv_label_create(_root);
    lv_obj_set_pos(_lbl_status, 10, 206);
    lv_obj_set_width(_lbl_status, 300);
    lv_obj_set_style_text_align(_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(_lbl_status, "Config: /sd/config/deck.txt (Label|URL)");
}

void AppDeck::onRunning()
{
    int idx = _pending_idx.exchange(-1);
    if (idx >= 0 && idx < static_cast<int>(_macros.size())) {
        const auto& m = _macros[idx];
        if (m.url.empty()) {
            LvglLockGuard lock;
            lv_label_set_text(_lbl_status, "Add Label|http://... in /sd/config/deck.txt");
        } else {
            {
                LvglLockGuard lock;
                lv_label_set_text(_lbl_status, ("Triggering " + m.label + "...").c_str());
            }
            GetHAL().startNetwork([&](std::string_view) {});
            std::string resp;
            bool ok = http_request("POST", m.url, resp);
            if (!ok) ok = http_request("GET", m.url, resp);
            LvglLockGuard lock;
            lv_label_set_text(_lbl_status, (m.label + (ok ? ": OK!" : ": Failed")).c_str());
        }
    }
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppDeck::onClose()
{
    LvglLockGuard lock;
    view::destroy_home_indicator();
    view::destroy_status_bar();
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}
