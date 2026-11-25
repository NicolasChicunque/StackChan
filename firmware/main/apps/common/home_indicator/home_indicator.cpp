/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "home_indicator.h"
#include "../status_bar/status_bar.h"

namespace view {

static bool s_home_indicator_active = false;

void create_home_indicator(std::function<void(void)> onGoHome, uint32_t colorButton, uint32_t colorBorder,
                           lv_obj_t* parent)
{
    (void)colorButton;
    (void)colorBorder;
    (void)parent;
    s_home_indicator_active = true;
    set_status_bar_home_callback(std::move(onGoHome));
}

void update_home_indicator()
{
    // Bottom swipe-up home bar has been replaced by the top status bar Home button.
}

bool is_home_indicator_created()
{
    return s_home_indicator_active;
}

void destroy_home_indicator()
{
    s_home_indicator_active = false;
    set_status_bar_home_callback(nullptr);
}

}  // namespace view
