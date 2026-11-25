/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <cstdint>
#include <functional>

namespace view {

enum class StatusBarMode : uint8_t {
    Fixed    = 0,
    AutoHide = 1,
};

StatusBarMode get_status_bar_mode();
void set_status_bar_mode(StatusBarMode mode);
int get_status_bar_top_offset();

void create_status_bar(uint32_t colorSecondary = 0xB8D3FD, uint32_t colorPrimary = 0x26206A,
                       lv_obj_t* parent = lv_screen_active(), std::function<void(void)> onGoHome = nullptr);
void set_status_bar_home_callback(std::function<void(void)> onGoHome);
void update_status_bar();
bool is_status_bar_created();
void destroy_status_bar();

}  // namespace view
