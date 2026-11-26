#include "app_calendar.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <sstream>

using namespace community;

AppCalendar::AppCalendar()
{
    setAppInfo().name         = "CALENDAR";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_BLUE;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppCalendar::onCreate() {}

void AppCalendar::onOpen()
{
    std::vector<std::string> lines;
    _ics_url.clear();
    if (read_sd_config_lines("calendar.txt", lines, 1) && !lines.empty()) {
        _ics_url = lines[0];
        _status  = "Tap SYNC to load events";
        _sync_req = true;
    } else {
        _status = "Put Google Calendar .ics URL in /sd/config/calendar.txt";
    }

    LvglLockGuard lock;
    build_ui();
    refresh_ui();
    view::create_home_indicator([&]() { close(); }, COL_BLUE, COL_BG);
    view::create_status_bar(COL_BLUE, COL_BG);
}

void AppCalendar::fetch_calendar()
{
    if (_ics_url.empty()) {
        return;
    }
    GetHAL().startNetwork([&](std::string_view) {});
    std::string body;
    if (!http_request("GET", _ics_url, body)) {
        _status = "Sync failed (check URL/Wi-Fi)";
        return;
    }

    _events.clear();
    std::istringstream iss(body);
    std::string line, cur_sum, cur_dt;
    while (std::getline(iss, line) && _events.size() < 5) {
        line = trim(line);
        if (line == "BEGIN:VEVENT") {
            cur_sum.clear();
            cur_dt.clear();
        } else if (line.rfind("SUMMARY:", 0) == 0) {
            cur_sum = line.substr(8);
        } else if (line.rfind("DTSTART", 0) == 0) {
            auto col = line.find(':');
            if (col != std::string::npos) {
                cur_dt = line.substr(col + 1);
                if (cur_dt.size() >= 8) {
                    cur_dt = cur_dt.substr(4, 2) + "/" + cur_dt.substr(6, 2);
                }
            }
        } else if (line == "END:VEVENT" && !cur_sum.empty()) {
            _events.push_back((cur_dt.empty() ? "" : (cur_dt + " ")) + cur_sum);
        }
    }
    _status = _events.empty() ? "No upcoming events found" : "Google Calendar synced";
}

void AppCalendar::build_ui()
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

    _lbl_hdr = lv_label_create(card);
    lv_obj_set_pos(_lbl_hdr, 12, 8);
    lv_obj_set_width(_lbl_hdr, 280);
    lv_obj_set_style_text_font(_lbl_hdr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_hdr, lv_color_hex(COL_BLUE), 0);

    _lbl_events = lv_label_create(card);
    lv_obj_set_pos(_lbl_events, 12, 34);
    lv_obj_set_width(_lbl_events, 280);
    lv_label_set_long_mode(_lbl_events, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_events, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_events, lv_color_hex(COL_TEXT), 0);

    lv_obj_t* b_sync = make_btn(_root, 8, 182, 304, 42, "SYNC CALENDAR", COL_BLUE, COL_WHITE);
    lv_obj_add_event_cb(
        b_sync,
        [](lv_event_t* e) {
            auto* self      = static_cast<AppCalendar*>(lv_event_get_user_data(e));
            self->_status   = "Syncing...";
            self->_sync_req = true;
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);
}

void AppCalendar::refresh_ui()
{
    if (!_lbl_hdr) return;
    lv_label_set_text(_lbl_hdr, _status.c_str());
    if (_events.empty()) {
        lv_label_set_text(_lbl_events, "Secret iCal (.ics) URL in /sd/config/calendar.txt");
    } else {
        std::string txt;
        for (const auto& ev : _events) {
            txt += "- " + ev + "\n";
        }
        lv_label_set_text(_lbl_events, txt.c_str());
    }
}

void AppCalendar::onRunning()
{
    if (_sync_req.exchange(false)) {
        fetch_calendar();
        LvglLockGuard lock;
        refresh_ui();
    }
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppCalendar::onClose()
{
    LvglLockGuard lock;
    view::destroy_home_indicator();
    view::destroy_status_bar();
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}
