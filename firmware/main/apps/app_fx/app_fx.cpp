#include "app_fx.h"
#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <cJSON.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <settings.h>
#include <cstdio>

using namespace community;

namespace {
constexpr const char* kPairLabels[5] = {"USD -> COP", "COP -> USD", "EUR -> COP", "USD -> MXN", "USD -> EUR"};
constexpr const char* kNvsNs         = "app_fx";
}  // namespace

AppFx::AppFx()
{
    setAppInfo().name         = "FX";
    static auto icon          = assets::get_image("icon_app_center.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_GREEN;
    setAppInfo().userData     = (void*)&theme_col;
}

void AppFx::onCreate() {}

void AppFx::onOpen()
{
    {
        Settings s(kNvsNs, false);
        _pair_idx = std::clamp<int>(s.GetInt("pair", 0), 0, 4);
        int cop   = s.GetInt("usd_cop", 4050);
        if (cop > 100) {
            _rates[0] = static_cast<double>(cop);
            _rates[1] = 1.0 / _rates[0];
        }
    }

    LvglLockGuard lock;
    build_ui();
    refresh_ui();
    view::create_home_indicator([&]() { close(); }, COL_GREEN, COL_BG);
    view::create_status_bar(COL_GREEN, COL_BG);
    _sync_req = true;
}

void AppFx::fetch_rates()
{
    GetHAL().startNetwork([&](std::string_view) {});
    std::string body;
    if (http_request("GET", "https://open.er-api.com/v6/latest/USD", body)) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root) {
            cJSON* rates = cJSON_GetObjectItemCaseSensitive(root, "rates");
            if (cJSON_IsObject(rates)) {
                auto get_r = [&](const char* k, double fb) {
                    cJSON* v = cJSON_GetObjectItemCaseSensitive(rates, k);
                    return cJSON_IsNumber(v) ? v->valuedouble : fb;
                };
                double cop = get_r("COP", _rates[0]);
                double eur = get_r("EUR", _rates[4]);
                double mxn = get_r("MXN", _rates[3]);
                _rates[0]  = cop;
                _rates[1]  = (cop > 0.0) ? (1.0 / cop) : _rates[1];
                _rates[2]  = (eur > 0.0) ? (cop / eur) : _rates[2];
                _rates[3]  = mxn;
                _rates[4]  = eur;
                Settings s(kNvsNs, true);
                s.SetInt("usd_cop", static_cast<int>(cop));
                _status = "Live FX updated";
            }
            cJSON_Delete(root);
        }
    } else {
        _status = "Offline (cached rates)";
    }
}

void AppFx::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    _root         = lv_obj_create(scr);
    lv_obj_set_size(_root, 320, 240);
    style_box(_root, COL_BG, 0);

    const int top_y = view::get_status_bar_top_offset();

    lv_obj_t* card = lv_obj_create(_root);
    lv_obj_set_pos(card, 8, top_y + 4);
    lv_obj_set_size(card, 304, 118 - top_y + 20);
    style_box(card, COL_CARD, 12);

    _lbl_pair = lv_label_create(card);
    lv_obj_set_pos(_lbl_pair, 12, 8);
    lv_obj_set_style_text_font(_lbl_pair, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_pair, lv_color_hex(COL_GREEN), 0);

    _lbl_input = lv_label_create(card);
    lv_obj_set_pos(_lbl_input, 12, 34);
    lv_obj_set_style_text_font(_lbl_input, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_input, lv_color_hex(COL_DIM), 0);

    _lbl_result = lv_label_create(card);
    lv_obj_set_pos(_lbl_result, 12, 60);
    lv_obj_set_width(_lbl_result, 280);
    lv_obj_set_style_text_font(_lbl_result, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lbl_result, lv_color_hex(COL_WHITE), 0);

    _lbl_status = lv_label_create(card);
    lv_obj_align(_lbl_status, LV_ALIGN_BOTTOM_LEFT, 12, -6);
    lv_obj_set_style_text_font(_lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(COL_DIM), 0);

    const int y_row1 = 148;
    const int y_row2 = 190;
    static constexpr double kSteps[4] = {1, 10, 50, 100};
    const char* kStepLbls[4]          = {"+1", "+10", "+50", "+100"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* b = make_btn(_root, 8 + i * 77, y_row1, 71, 36, kStepLbls[i], COL_CARD_HI, COL_WHITE);
        lv_obj_set_user_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            b,
            [](lv_event_t* e) {
                auto* self = static_cast<AppFx*>(lv_event_get_user_data(e));
                int idx    = static_cast<int>(
                    reinterpret_cast<intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
                self->_amount += kSteps[idx];
                self->refresh_ui();
            },
            LV_EVENT_CLICKED, this);
    }

    lv_obj_t* btn_pair = make_btn(_root, 8, y_row2, 104, 38, "PAIR", COL_TEAL_DK, COL_WHITE);
    lv_obj_add_event_cb(
        btn_pair,
        [](lv_event_t* e) {
            auto* self      = static_cast<AppFx*>(lv_event_get_user_data(e));
            self->_pair_idx = (self->_pair_idx + 1) % 5;
            Settings s(kNvsNs, true);
            s.SetInt("pair", self->_pair_idx);
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_clr = make_btn(_root, 118, y_row2, 92, 38, "RESET", COL_CARD, COL_AMBER);
    lv_obj_add_event_cb(
        btn_clr,
        [](lv_event_t* e) {
            auto* self    = static_cast<AppFx*>(lv_event_get_user_data(e));
            self->_amount = 1.0;
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_sync = make_btn(_root, 216, y_row2, 96, 38, "SYNC", COL_BLUE, COL_WHITE);
    lv_obj_add_event_cb(
        btn_sync,
        [](lv_event_t* e) {
            auto* self      = static_cast<AppFx*>(lv_event_get_user_data(e));
            self->_status   = "Syncing...";
            self->_sync_req = true;
            self->refresh_ui();
        },
        LV_EVENT_CLICKED, this);
}

void AppFx::refresh_ui()
{
    if (!_lbl_pair) return;
    lv_label_set_text(_lbl_pair, kPairLabels[_pair_idx]);
    char b1[48], b2[48];
    std::snprintf(b1, sizeof(b1), "Amount: %.0f", _amount);
    double out = _amount * _rates[_pair_idx];
    if (out >= 100.0) {
        std::snprintf(b2, sizeof(b2), "= %.1f", out);
    } else {
        std::snprintf(b2, sizeof(b2), "= %.4f", out);
    }
    lv_label_set_text(_lbl_input, b1);
    lv_label_set_text(_lbl_result, b2);
    lv_label_set_text(_lbl_status, _status.c_str());
}

void AppFx::onRunning()
{
    if (_sync_req.exchange(false)) {
        fetch_rates();
        LvglLockGuard lock;
        refresh_ui();
    }
    LvglLockGuard lock;
    view::update_home_indicator();
    view::update_status_bar();
}

void AppFx::onClose()
{
    LvglLockGuard lock;
    view::destroy_home_indicator();
    view::destroy_status_bar();
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}
