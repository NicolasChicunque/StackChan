#include "app_files.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

using namespace community;

AppFiles::AppFiles()
{
    setAppInfo().name         = "FILES";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_AMBER;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppFiles::onCreate() {}

void AppFiles::scan_dir(const std::string& path)
{
    _cur_path = path;
    _entries.clear();
    _cursor = 0;

    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr && _entries.size() < 64) {
        std::string n = ent->d_name;
        if (n == "." || n == ".." || n == "System Volume Information") continue;
        std::string fp = path + "/" + n;
        struct stat st = {};
        bool is_d      = false;
        long kb        = 0;
        if (stat(fp.c_str(), &st) == 0) {
            is_d = S_ISDIR(st.st_mode);
            kb   = static_cast<long>(st.st_size / 1024);
        }
        _entries.push_back({n, is_d, kb});
    }
    closedir(dir);

    std::sort(_entries.begin(), _entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.name < b.name;
    });
}

void AppFiles::onOpen()
{
    mount_sd_temp(_card, _mounted_by_us);
    scan_dir("/sd");

    LvglLockGuard lock;
    build_ui();
    refresh_ui();
    view::create_home_indicator([&]() { close(); }, COL_AMBER, COL_BG);
    view::create_status_bar(COL_AMBER, COL_BG);
}

void AppFiles::build_ui()
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

    _lbl_path = lv_label_create(card);
    lv_obj_set_pos(_lbl_path, 10, 6);
    lv_obj_set_width(_lbl_path, 284);
    lv_obj_set_style_text_font(_lbl_path, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_path, lv_color_hex(COL_AMBER), 0);

    _lbl_list = lv_label_create(card);
    lv_obj_set_pos(_lbl_list, 10, 28);
    lv_obj_set_width(_lbl_list, 284);
    lv_label_set_long_mode(_lbl_list, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_list, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_list, lv_color_hex(COL_TEXT), 0);

    _lbl_info = lv_label_create(card);
    lv_obj_align(_lbl_info, LV_ALIGN_BOTTOM_LEFT, 10, -6);
    lv_obj_set_style_text_font(_lbl_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_info, lv_color_hex(COL_DIM), 0);

    lv_obj_t* b_up = make_btn(_root, 8, 182, 70, 42, "UP", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        b_up,
        [](lv_event_t* e) {
            auto* self = static_cast<AppFiles*>(lv_event_get_user_data(e));
            if (self->_cur_path != "/sd") {
                auto slash = self->_cur_path.rfind('/');
                self->scan_dir(slash != std::string::npos && slash > 0 ? self->_cur_path.substr(0, slash) : "/sd");
                self->refresh_ui();
            }
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* b_prev = make_btn(_root, 84, 182, 68, 42, "PREV", COL_CARD, COL_WHITE);
    lv_obj_add_event_cb(
        b_prev,
        [](lv_event_t* e) {
            auto* self = static_cast<AppFiles*>(lv_event_get_user_data(e));
            if (!self->_entries.empty()) {
                self->_cursor = (self->_cursor - 1 + static_cast<int>(self->_entries.size())) %
                                static_cast<int>(self->_entries.size());
                self->refresh_ui();
            }
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* b_next = make_btn(_root, 158, 182, 68, 42, "NEXT", COL_CARD, COL_WHITE);
    lv_obj_add_event_cb(
        b_next,
        [](lv_event_t* e) {
            auto* self = static_cast<AppFiles*>(lv_event_get_user_data(e));
            if (!self->_entries.empty()) {
                self->_cursor = (self->_cursor + 1) % static_cast<int>(self->_entries.size());
                self->refresh_ui();
            }
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* b_open = make_btn(_root, 232, 182, 80, 42, "OPEN", COL_AMBER, COL_BG);
    lv_obj_add_event_cb(
        b_open,
        [](lv_event_t* e) {
            auto* self = static_cast<AppFiles*>(lv_event_get_user_data(e));
            if (self->_cursor >= 0 && self->_cursor < static_cast<int>(self->_entries.size()) &&
                self->_entries[self->_cursor].is_dir) {
                self->scan_dir(self->_cur_path + "/" + self->_entries[self->_cursor].name);
                self->refresh_ui();
            }
        },
        LV_EVENT_CLICKED, this);
}

void AppFiles::refresh_ui()
{
    if (!_lbl_path) return;
    lv_label_set_text(_lbl_path, _cur_path.c_str());
    if (_entries.empty()) {
        lv_label_set_text(_lbl_list, "(empty folder or no MicroSD)");
        lv_label_set_text(_lbl_info, "0 items");
        return;
    }

    int start = (_cursor / 4) * 4;
    std::string txt;
    for (int i = start; i < std::min<int>(start + 4, static_cast<int>(_entries.size())); ++i) {
        const auto& e = _entries[i];
        char line[96];
        if (e.is_dir) {
            std::snprintf(line, sizeof(line), "%s [%s/]\n", (i == _cursor) ? ">" : " ", e.name.c_str());
        } else {
            std::snprintf(line, sizeof(line), "%s %s (%ld KB)\n", (i == _cursor) ? ">" : " ", e.name.c_str(),
                          e.size_kb);
        }
        txt += line;
    }
    lv_label_set_text(_lbl_list, txt.c_str());
    char info[48];
    std::snprintf(info, sizeof(info), "%d / %d items", _cursor + 1, static_cast<int>(_entries.size()));
    lv_label_set_text(_lbl_info, info);
}

void AppFiles::onRunning()
{
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppFiles::onClose()
{
    unmount_sd_temp(_card, _mounted_by_us);
    _card          = nullptr;
    _mounted_by_us = false;
    LvglLockGuard lock;
    view::destroy_home_indicator();
    view::destroy_status_bar();
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}
