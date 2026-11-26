#include "app_home.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <hal/hal.h>

using namespace community;

AppHome::AppHome()
{
    setAppInfo().name         = "HOME";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_TEAL;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppHome::onCreate() {}

void AppHome::load_actions()
{
    _actions.clear();
    std::vector<std::string> lines;
    if (read_sd_config_lines("home.txt", lines, 6)) {
        for (const auto& l : lines) {
            auto sep = l.find('|');
            if (sep == std::string::npos) continue;
            _actions.push_back({trim(l.substr(0, sep)), trim(l.substr(sep + 1))});
        }
    }
    const char* defaults[6] = {"TV PWR", "VOL +", "VOL -", "LIGHTS", "SCENE 1", "NIGHT"};
    while (_actions.size() < 6) {
        _actions.push_back({defaults[_actions.size()], ""});
    }
}

void AppHome::onOpen()
{
    load_actions();
    _pending_idx = -1;
    LvglLockGuard lock;
    build_ui();
    view::create_home_indicator([&]() { close(); }, COL_TEAL, COL_BG);
    view::create_status_bar(COL_TEAL, COL_BG);
}

void AppHome::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    _root         = lv_obj_create(scr);
    lv_obj_set_size(_root, 320, 240);
    style_box(_root, COL_BG, 0);

    const int top_y = view::get_status_bar_top_offset();
    const int y0    = top_y + 8;
    static constexpr uint32_t kCols[6] = {COL_RED, COL_TEAL_DK, COL_TEAL_DK, COL_AMBER, COL_BLUE, COL_PURPLE};

    for (int i = 0; i < 6; ++i) {
        lv_obj_t* b = make_btn(_root, 8 + (i % 3) * 103, y0 + (i / 3) * 72, 97, 64, _actions[i].label.c_str(), kCols[i],
                               COL_WHITE);
        lv_obj_set_user_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            b,
            [](lv_event_t* e) {
                auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
                self->_pending_idx = static_cast<int>(
                    reinterpret_cast<intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
            },
            LV_EVENT_CLICKED, this);
    }

    _lbl_status = lv_label_create(_root);
    lv_obj_set_pos(_lbl_status, 10, 206);
    lv_obj_set_width(_lbl_status, 300);
    lv_obj_set_style_text_align(_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(_lbl_status, "Webhooks / IR: /sd/config/home.txt (Name|URL)");
}

void AppHome::onRunning()
{
    int idx = _pending_idx.exchange(-1);
    if (idx >= 0 && idx < static_cast<int>(_actions.size())) {
        const auto& a = _actions[idx];
        if (a.url.empty()) {
            LvglLockGuard lock;
            lv_label_set_text(_lbl_status, "Add Name|WebhookURL in /sd/config/home.txt");
        } else {
            {
                LvglLockGuard lock;
                lv_label_set_text(_lbl_status, ("Sending " + a.label + "...").c_str());
            }
            GetHAL().startNetwork([&](std::string_view) {});
            std::string resp;
            bool ok = http_request("POST", a.url, resp);
            if (!ok) ok = http_request("GET", a.url, resp);
            LvglLockGuard lock;
            lv_label_set_text(_lbl_status, (a.label + (ok ? " OK" : " Failed")).c_str());
        }
    }
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppHome::onClose()
{
    LvglLockGuard lock;
    view::destroy_home_indicator();
    view::destroy_status_bar();
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}
