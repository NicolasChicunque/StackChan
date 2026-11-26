/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"
#include <apps/common/status_bar/status_bar.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <cstdint>
#include <vector>

using namespace view;

static const std::string_view _tag = "LauncherView";

LauncherView::~LauncherView()
{
    if (_grid) {
        lv_obj_delete(_grid);
        _grid = nullptr;
    }
    GetHAL().showRgbColor(0, 0, 0);
}

void LauncherView::init(std::vector<mooncake::AppProps_t> appPorps, int restoreAppID)
{
    _clicked_app_id = -1;

    if (_grid) {
        lv_obj_delete(_grid);
        _grid = nullptr;
    }

    const int top_y = view::get_status_bar_top_offset();
    const int grid_h = 240 - top_y;

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0F1A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 3-column x 3-row vertical scrollable app grid (320px wide, 96x60 tiles)
    _grid = lv_obj_create(scr);
    lv_obj_set_pos(_grid, 0, top_y);
    lv_obj_set_size(_grid, 320, grid_h);
    lv_obj_set_style_bg_color(_grid, lv_color_hex(0x0B0F1A), 0);
    lv_obj_set_style_bg_opa(_grid, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_grid, 0, 0);
    lv_obj_set_style_radius(_grid, 0, 0);
    lv_obj_set_style_pad_all(_grid, 0, 0);
    lv_obj_set_scroll_dir(_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_grid, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    constexpr int COLS   = 3;
    constexpr int TILE_W = 96;
    constexpr int TILE_H = 60;
    constexpr int GAP_X  = 8;
    constexpr int GAP_Y  = 6;
    constexpr int PAD_X  = 8;
    constexpr int PAD_Y  = 6;

    lv_obj_t* restore_tile = nullptr;

    for (size_t i = 0; i < appPorps.size(); ++i) {
        const auto& app = appPorps[i];
        const int col   = static_cast<int>(i % COLS);
        const int row   = static_cast<int>(i / COLS);
        const int x     = PAD_X + col * (TILE_W + GAP_X);
        const int y     = PAD_Y + row * (TILE_H + GAP_Y);

        uint32_t theme_col = 0x2DD4BF;
        if (app.info.userData != nullptr) {
            theme_col = *static_cast<uint32_t*>(app.info.userData);
        }

        lv_obj_t* tile = lv_button_create(_grid);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x151B2E), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(theme_col), LV_STATE_PRESSED);
        lv_obj_set_style_radius(tile, 12, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(theme_col), 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_60, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        // Colored accent bar at top of tile
        lv_obj_t* accent = lv_obj_create(tile);
        lv_obj_set_size(accent, 42, 4);
        lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 7);
        lv_obj_set_style_bg_color(accent, lv_color_hex(theme_col), 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(accent, 2, 0);
        lv_obj_set_style_border_width(accent, 0, 0);
        lv_obj_set_style_pad_all(accent, 0, 0);
        lv_obj_remove_flag(accent, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

        // Crisp app title centered in tile
        lv_obj_t* lbl = lv_label_create(tile);
        lv_obj_set_width(lbl, TILE_W - 8);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 5);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xF8FAFC), 0);
        lv_label_set_text(lbl, app.info.name.c_str());
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_user_data(tile, reinterpret_cast<void*>(static_cast<intptr_t>(app.appID)));
        lv_obj_add_event_cb(
            tile,
            [](lv_event_t* e) {
                auto* self      = static_cast<LauncherView*>(lv_event_get_user_data(e));
                auto* target    = static_cast<lv_obj_t*>(lv_event_get_target(e));
                self->_clicked_app_id = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
            },
            LV_EVENT_CLICKED, this);

        if (restoreAppID >= 0 && app.appID == restoreAppID) {
            restore_tile = tile;
        }
    }

    // Add slight bottom spacer so last row scrolls cleanly above bottom edge
    const int total_rows = static_cast<int>((appPorps.size() + COLS - 1) / COLS);
    lv_obj_t* spacer     = lv_obj_create(_grid);
    lv_obj_set_pos(spacer, 0, PAD_Y + total_rows * (TILE_H + GAP_Y));
    lv_obj_set_size(spacer, 10, 10);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    if (restore_tile) {
        lv_obj_scroll_to_view(restore_tile, LV_ANIM_OFF);
    }

    GetHAL().showRgbColor(0, 28, 36);
    mclog::tagInfo(_tag, "3x3 grid launcher initialized with {} apps", appPorps.size());
}

void LauncherView::update()
{
    if (_clicked_app_id >= 0) {
        int app_id      = _clicked_app_id;
        _clicked_app_id = -1;
        if (onAppClicked) {
            onAppClicked(app_id);
        }
    }
}
