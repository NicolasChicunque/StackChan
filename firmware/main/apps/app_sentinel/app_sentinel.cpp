#include "app_sentinel.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <cJSON.h>
#include <hal/hal.h>

using namespace community;

AppSentinel::AppSentinel()
{
    setAppInfo().name         = "SENTINEL";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_BLUE;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppSentinel::onCreate() {}

void AppSentinel::onOpen()
{
    std::vector<std::string> lines;
    if (read_sd_config_lines("sentinel.txt", lines, 1) && !lines.empty()) {
        _repo = lines[0];
    }

    LvglLockGuard lock;
    build_ui();
    refresh_ui();
    view::create_home_indicator([&]() { close(); }, COL_BLUE, COL_BG);
    view::create_status_bar(COL_BLUE, COL_BG);
    _check_req = true;
}

void AppSentinel::check_ci()
{
    GetHAL().startNetwork([&](std::string_view) {});
    std::string url = "https://api.github.com/repos/" + _repo + "/actions/runs?per_page=1";
    std::string body;
    if (!http_request("GET", url, body, {{"User-Agent", "StackChan-Sentinel"}})) {
        _state  = "OFFLINE";
        _detail = "Failed to reach GitHub API";
        _color  = COL_AMBER;
        return;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root) {
        cJSON* runs = cJSON_GetObjectItemCaseSensitive(root, "workflow_runs");
        if (cJSON_IsArray(runs) && cJSON_GetArraySize(runs) > 0) {
            cJSON* r      = cJSON_GetArrayItem(runs, 0);
            cJSON* name   = cJSON_GetObjectItemCaseSensitive(r, "name");
            cJSON* status = cJSON_GetObjectItemCaseSensitive(r, "status");
            cJSON* concl  = cJSON_GetObjectItemCaseSensitive(r, "conclusion");
            std::string st = (cJSON_IsString(status) && status->valuestring) ? status->valuestring : "unknown";
            std::string co = (cJSON_IsString(concl) && concl->valuestring) ? concl->valuestring : "";
            _detail        = (cJSON_IsString(name) && name->valuestring) ? name->valuestring : "Workflow";
            if (st != "completed") {
                _state = "RUNNING";
                _color = COL_AMBER;
                GetHAL().showRgbColor(255, 180, 0);
            } else if (co == "success") {
                _state = "PASSING";
                _color = COL_GREEN;
                GetHAL().showRgbColor(0, 220, 60);
            } else {
                _state = "FAILED";
                _color = COL_RED;
                GetHAL().showRgbColor(255, 20, 20);
            }
        }
        cJSON_Delete(root);
    }
}

void AppSentinel::build_ui()
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

    _lbl_repo = lv_label_create(card);
    lv_obj_set_pos(_lbl_repo, 12, 8);
    lv_obj_set_width(_lbl_repo, 280);
    lv_obj_set_style_text_font(_lbl_repo, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_repo, lv_color_hex(COL_DIM), 0);

    _lbl_state = lv_label_create(card);
    lv_obj_set_pos(_lbl_state, 12, 34);
    lv_obj_set_style_text_font(_lbl_state, &lv_font_montserrat_24, 0);

    _lbl_detail = lv_label_create(card);
    lv_obj_align(_lbl_detail, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    lv_obj_set_width(_lbl_detail, 280);
    lv_label_set_long_mode(_lbl_detail, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_detail, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_detail, lv_color_hex(COL_TEXT), 0);

    lv_obj_t* b_chk = make_btn(_root, 8, 182, 304, 42, "CHECK CI BUILD", COL_BLUE, COL_WHITE);
    lv_obj_add_event_cb(
        b_chk,
        [](lv_event_t* e) {
            auto* self       = static_cast<AppSentinel*>(lv_event_get_user_data(e));
            self->_state     = "CHECKING...";
            self->_check_req = true;
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);
}

void AppSentinel::refresh_ui()
{
    if (!_lbl_repo) return;
    lv_label_set_text(_lbl_repo, _repo.c_str());
    lv_label_set_text(_lbl_state, _state.c_str());
    lv_obj_set_style_text_color(_lbl_state, lv_color_hex(_color), 0);
    lv_label_set_text(_lbl_detail, _detail.c_str());
}

void AppSentinel::onRunning()
{
    if (_check_req.exchange(false)) {
        check_ci();
        LvglLockGuard lock;
        refresh_ui();
    }
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppSentinel::onClose()
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
