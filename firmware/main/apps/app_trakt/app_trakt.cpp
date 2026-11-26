#include "app_trakt.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <cJSON.h>
#include <hal/hal.h>

using namespace community;

AppTrakt::AppTrakt()
{
    setAppInfo().name         = "TRAKT";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_RED;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppTrakt::onCreate() {}

void AppTrakt::onOpen()
{
    std::vector<std::string> lines;
    _client_id.clear();
    _username.clear();
    if (read_sd_config_lines("trakt.txt", lines, 2) && lines.size() >= 2) {
        _client_id = lines[0];
        _username  = lines[1];
        _status    = "Syncing Trakt.tv...";
        _sync_req  = true;
    } else {
        _status = "Line 1: Client ID, Line 2: Username in /sd/config/trakt.txt";
    }

    LvglLockGuard lock;
    build_ui();
    refresh_ui();
    view::create_home_indicator([&]() { close(); }, COL_RED, COL_BG);
    view::create_status_bar(COL_RED, COL_BG);
}

void AppTrakt::fetch_trakt()
{
    if (_client_id.empty() || _username.empty()) return;
    GetHAL().startNetwork([&](std::string_view) {});
    std::string url = "https://api.trakt.tv/users/" + _username + "/watchlist/shows?limit=5";
    std::string body;
    if (!http_request("GET", url, body,
                      {{"Content-Type", "application/json"},
                       {"trakt-api-version", "2"},
                       {"trakt-api-key", _client_id}})) {
        _status = "Trakt sync failed";
        return;
    }

    _shows.clear();
    cJSON* root = cJSON_Parse(body.c_str());
    if (cJSON_IsArray(root)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root)
        {
            cJSON* show  = cJSON_GetObjectItemCaseSensitive(item, "show");
            cJSON* title = show ? cJSON_GetObjectItemCaseSensitive(show, "title") : nullptr;
            if (cJSON_IsString(title) && title->valuestring) {
                _shows.push_back(title->valuestring);
            }
        }
    }
    if (root) cJSON_Delete(root);
    _status = _shows.empty() ? "Watchlist empty" : ("Trakt: @" + _username);
}

void AppTrakt::build_ui()
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
    lv_obj_set_style_text_color(_lbl_hdr, lv_color_hex(COL_RED), 0);

    _lbl_shows = lv_label_create(card);
    lv_obj_set_pos(_lbl_shows, 12, 34);
    lv_obj_set_width(_lbl_shows, 280);
    lv_label_set_long_mode(_lbl_shows, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_shows, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_shows, lv_color_hex(COL_TEXT), 0);

    lv_obj_t* b_sync = make_btn(_root, 8, 182, 304, 42, "SYNC TRAKT", COL_RED, COL_WHITE);
    lv_obj_add_event_cb(
        b_sync,
        [](lv_event_t* e) {
            auto* self      = static_cast<AppTrakt*>(lv_event_get_user_data(e));
            self->_status   = "Syncing...";
            self->_sync_req = true;
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);
}

void AppTrakt::refresh_ui()
{
    if (!_lbl_hdr) return;
    lv_label_set_text(_lbl_hdr, _status.c_str());
    if (_shows.empty()) {
        lv_label_set_text(_lbl_shows, "Save Trakt Client ID & username in /sd/config/trakt.txt");
    } else {
        std::string txt;
        for (const auto& s : _shows) {
            txt += "- " + s + "\n";
        }
        lv_label_set_text(_lbl_shows, txt.c_str());
    }
}

void AppTrakt::onRunning()
{
    if (_sync_req.exchange(false)) {
        fetch_trakt();
        LvglLockGuard lock;
        refresh_ui();
    }
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppTrakt::onClose()
{
    LvglLockGuard lock;
    view::destroy_home_indicator();
    view::destroy_status_bar();
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}
