/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <games/dvd_screensaver/dvd_screensaver.hpp>
#include <smooth_lvgl.hpp>
#include <functional>
#include <vector>
#include <memory>

namespace view {

/**
 * @brief
 *
 */
class LauncherView {
public:
    ~LauncherView();

    enum State_t {
        STATE_STARTUP,
        STATE_NORMAL,
    };

    std::function<void(int appID)> onAppClicked;

    void init(std::vector<mooncake::AppProps_t> appPorps, int restoreAppID = -1);
    void update();

private:
    lv_obj_t* _grid = nullptr;
    int _clicked_app_id = -1;
};

/**
 * @brief
 *
 */
class Screensaver : public uitk::games::dvd_screensaver::DvdScreensaver {
public:
    ~Screensaver();

    void onInit() override;
    void onBuildLevel() override;
    void onRender(float dt) override;
    void onLogoCollide(int logoGroupId) override;

private:
    std::unique_ptr<uitk::lvgl_cpp::ScreenActive> _prev_screen;
    std::unique_ptr<uitk::lvgl_cpp::Screen> _screen;
    std::unique_ptr<uitk::lvgl_cpp::Container> _logo;
    std::unique_ptr<uitk::lvgl_cpp::Container> _left_eye;
    std::unique_ptr<uitk::lvgl_cpp::Container> _right_eye;
    std::unique_ptr<uitk::lvgl_cpp::Container> _mouth;
    lv_obj_t* _naruto_img          = nullptr;
    lv_image_dsc_t _naruto_imgs[4] = {};
    bool _use_naruto               = false;
    int _color_index               = 0;
};

}  // namespace view
