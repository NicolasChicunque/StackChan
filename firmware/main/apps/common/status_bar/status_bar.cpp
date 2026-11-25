/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "status_bar.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <smooth_ui_toolkit.hpp>
#include <smooth_lvgl.hpp>
#include <assets/assets.h>
#include <fmt/chrono.h>
#include <hal/hal.h>
#include <memory>
#include <nvs.h>
#include <vector>
#include <lvgl.h>
#include <src/draw/lv_image_dsc.h>

using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

namespace {

constexpr const char* kStatusBarNvsNs  = "ui_prefs";
constexpr const char* kStatusBarNvsKey = "sb_mode";
constexpr int kStatusBarHeight         = 38;
constexpr int kVolumeStep              = 10;

std::atomic<int> s_cached_sb_mode{-1};
std::function<void(void)> s_pending_home_cb;

}  // namespace

/**
 * @brief Top-down swipe gesture detector (active only in AutoHide mode).
 *
 */
class StatuBarGesture {
public:
    std::function<void(void)> onGesture;

    StatuBarGesture() : _is_tracking(false), _last_state(LV_INDEV_STATE_REL)
    {
    }

    void init()
    {
        _is_tracking    = false;
        _last_state     = LV_INDEV_STATE_REL;
        _screen_height  = 240;
        _top_threshold  = 24;  // Within 24 px from top edge
        _swipe_min_dist = 45;  // Swipe down at least 45 px
    }

    void update()
    {
        lv_indev_t* indev = GetHAL().lvTouchpad;
        if (!indev) {
            return;
        }

        lv_indev_state_t state = lv_indev_get_state(indev);
        lv_point_t curr_point;
        lv_indev_get_point(indev, &curr_point);

        // 1. Transition: Released -> Pressed
        if (state == LV_INDEV_STATE_PR && _last_state == LV_INDEV_STATE_REL) {
            if (curr_point.y <= _top_threshold && curr_point.y >= 0) {
                _start_point = curr_point;
                _is_tracking = true;
            } else {
                _is_tracking = false;
            }
        }
        // 2. Transition: Pressed -> Released
        else if (state == LV_INDEV_STATE_REL && _last_state == LV_INDEV_STATE_PR) {
            if (_is_tracking) {
                int delta_y = curr_point.y - _start_point.y;
                int delta_x = abs(curr_point.x - _start_point.x);

                if (delta_y > _swipe_min_dist && delta_y > delta_x) {
                    if (onGesture) {
                        onGesture();
                    }
                }
                _is_tracking = false;
            }
        }

        _last_state = state;
    }

private:
    bool _is_tracking;
    lv_indev_state_t _last_state;
    lv_point_t _start_point;
    int _screen_height;
    int _top_threshold;
    int _swipe_min_dist;
};

namespace status_bar_view {

class Widget {
public:
    virtual ~Widget()     = default;
    virtual void update() = 0;
};

class TimeLabel : public Widget {
public:
    TimeLabel(lv_obj_t* parent, uint32_t colorText)
    {
        _label = std::make_unique<Label>(parent);
        _label->setText("");
        _label->setTextColor(lv_color_hex(colorText));
        _label->setTextFont(&lv_font_montserrat_12);
        _label->align(LV_ALIGN_LEFT_MID, 76, 0);
        lv_obj_remove_flag(_label->get(), LV_OBJ_FLAG_CLICKABLE);

        update();
    }

    void update() override
    {
        auto now   = std::chrono::system_clock::now();
        auto now_t = std::chrono::system_clock::to_time_t(now);

        struct tm local_tm;
        localtime_r(&now_t, &local_tm);

        int hour12 = local_tm.tm_hour % 12;
        if (hour12 == 0) {
            hour12 = 12;
        }

        _label->setText(fmt::format("{}:{:02d}{}", hour12, local_tm.tm_min, local_tm.tm_hour >= 12 ? "P" : "A"));
    }

private:
    std::unique_ptr<Label> _label;
};

class BatteryIcon {
public:
    BatteryIcon(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _color_primary = colorPrimary;

        _bat_top = std::make_unique<uitk::lvgl_cpp::Container>(parent);
        _bat_top->setBgColor(lv_color_hex(colorSecondary));
        _bat_top->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        _bat_top->setBorderWidth(0);
        _bat_top->setSize(3, 4);
        _bat_top->setRadius(2);
        _bat_top->setOutlineWidth(1);
        _bat_top->setOutlineColor(lv_color_hex(colorPrimary));
        lv_obj_remove_flag(_bat_top->get(), LV_OBJ_FLAG_CLICKABLE);

        _bar = std::make_unique<uitk::lvgl_cpp::Bar>(parent);
        _bar->setSize(22, 11);
        _bar->setRadius(3);
        _bar->setRadius(0, LV_PART_INDICATOR);
        _bar->setBgColor(lv_color_hex(colorSecondary));
        _bar->setBgColor(lv_color_hex(colorPrimary), LV_PART_INDICATOR);
        _bar->setBgOpa(LV_OPA_COVER);
        _bar->setRange(0, 100);
        _bar->setValue(0);
        _bar->setPadding(1, 1, 1, 1);
        _bar->setOutlineWidth(1);
        _bar->setOutlineColor(lv_color_hex(colorPrimary));
        _bar->setRadius(2, LV_PART_INDICATOR);
        lv_obj_remove_flag(_bar->get(), LV_OBJ_FLAG_CLICKABLE);

        _lightning_icon     = std::make_unique<uitk::lvgl_cpp::Image>(parent);
        _icon_bat_lightning = assets::get_image("icon_bat_lightning.bin");
        _lightning_icon->setSrc(&_icon_bat_lightning);
        _lightning_icon->setImageRecolor(lv_color_hex(colorPrimary));
        _lightning_icon->setImageRecolorOpa(LV_OPA_COVER);
        _lightning_icon->setHidden(true);
        lv_obj_remove_flag(_lightning_icon->get(), LV_OBJ_FLAG_CLICKABLE);
    }

    void align(lv_align_t align, int32_t x_ofs, int32_t y_ofs)
    {
        _bar->align(align, x_ofs, y_ofs);
        lv_obj_align_to(_bat_top->get(), _bar->get(), LV_ALIGN_CENTER, 12, 0);
        lv_obj_align_to(_lightning_icon->get(), _bar->get(), LV_ALIGN_CENTER, 0, 0);
    }

    void setLevel(uint8_t level)
    {
        _bar->setValue(level);
    }

    void setCharging(bool charging)
    {
        if (charging) {
            _bar->setBgColor(lv_color_hex(0x19C25F), LV_PART_INDICATOR);
            _lightning_icon->setHidden(false);
        } else {
            _bar->setBgColor(lv_color_hex(_color_primary), LV_PART_INDICATOR);
            _lightning_icon->setHidden(true);
        }
    }

private:
    std::unique_ptr<Bar> _bar;
    std::unique_ptr<Container> _bat_top;
    std::unique_ptr<Image> _lightning_icon;
    lv_image_dsc_t _icon_bat_lightning;

    uint32_t _color_primary = 0;
};

class Battery : public Widget {
public:
    Battery(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _label_level = std::make_unique<Label>(parent);
        _label_level->setText("");
        _label_level->setTextColor(lv_color_hex(colorPrimary));
        _label_level->setTextFont(&lv_font_montserrat_12);
        lv_obj_set_width(_label_level->get(), 34);
        lv_obj_set_style_text_align(_label_level->get(), LV_TEXT_ALIGN_RIGHT, 0);
        _label_level->align(LV_ALIGN_LEFT_MID, 125, 0);
        lv_obj_remove_flag(_label_level->get(), LV_OBJ_FLAG_CLICKABLE);

        _battery_icon = std::make_unique<BatteryIcon>(parent, colorSecondary, colorPrimary);
        _battery_icon->align(LV_ALIGN_LEFT_MID, 162, 0);

        update();
    }

    void update() override
    {
        auto level = GetHAL().getBatteryLevel();
        _label_level->setText(fmt::format("{}%", level));
        _battery_icon->setLevel(level);
        _battery_icon->setCharging(GetHAL().isBatteryCharging());
    }

private:
    std::unique_ptr<Label> _label_level;
    std::unique_ptr<BatteryIcon> _battery_icon;
};

class Wifi : public Widget {
public:
    Wifi(lv_obj_t* parent, uint32_t colorPrimary)
    {
        _wifi_icon = std::make_unique<Image>(parent);
        _wifi_icon->align(LV_ALIGN_LEFT_MID, 50, 0);
        _wifi_icon->setImageRecolor(lv_color_hex(colorPrimary));
        _wifi_icon->setImageRecolorOpa(LV_OPA_COVER);
        lv_obj_remove_flag(_wifi_icon->get(), LV_OBJ_FLAG_CLICKABLE);

        _icon_wifi_low    = assets::get_image("icon_wifi_low.bin");
        _icon_wifi_medium = assets::get_image("icon_wifi_medium.bin");
        _icon_wifi_high   = assets::get_image("icon_wifi_high.bin");
        _icon_wifi_slash  = assets::get_image("icon_wifi_slash.bin");

        update();
    }

    void update() override
    {
        auto status = GetHAL().getWifiStatus();
        switch (status) {
            case WifiStatus::None:
                _wifi_icon->setSrc(NULL);
                break;
            case WifiStatus::Low:
                _wifi_icon->setSrc(&_icon_wifi_low);
                break;
            case WifiStatus::Medium:
                _wifi_icon->setSrc(&_icon_wifi_medium);
                break;
            case WifiStatus::High:
                _wifi_icon->setSrc(&_icon_wifi_high);
                break;
            default:
                _wifi_icon->setSrc(&_icon_wifi_slash);
                break;
        }
    }

private:
    std::unique_ptr<Image> _wifi_icon;
    lv_image_dsc_t _icon_wifi_low;
    lv_image_dsc_t _icon_wifi_medium;
    lv_image_dsc_t _icon_wifi_high;
    lv_image_dsc_t _icon_wifi_slash;
};

class VolumeValueLabel : public Widget {
public:
    VolumeValueLabel(lv_obj_t* parent, uint32_t colorText)
    {
        _label = std::make_unique<Label>(parent);
        _label->setText("");
        _label->setTextColor(lv_color_hex(colorText));
        _label->setTextFont(&lv_font_montserrat_14);
        lv_obj_set_width(_label->get(), 32);
        lv_obj_set_style_text_align(_label->get(), LV_TEXT_ALIGN_CENTER, 0);
        _label->align(LV_ALIGN_LEFT_MID, 239, 0);
        lv_obj_remove_flag(_label->get(), LV_OBJ_FLAG_CLICKABLE);

        update();
    }

    void setImmediate(int vol)
    {
        _last_vol = vol;
        _label->setText(fmt::format("{}", vol));
    }

    void update() override
    {
        int cur = static_cast<int>(GetHAL().getSpeakerVolume());
        if (cur != _last_vol) {
            _last_vol = cur;
            _label->setText(fmt::format("{}", cur));
        }
    }

private:
    std::unique_ptr<Label> _label;
    int _last_vol = -1;
};

/**
 * @brief Tactile pill button with a clean 3px margin and 1px subtle border
 * so buttons clearly stand out from the status information labels.
 */
class BarActionButton {
public:
    BarActionButton(lv_obj_t* parent, int x_pos, int width, uint32_t colorPrimary, const char* symbolText,
                    bool useHomeIcon, std::function<void()> onClickCb)
        : _on_click(std::move(onClickCb))
    {
        _btn = lv_obj_create(parent);
        lv_obj_set_pos(_btn, x_pos, 3);
        lv_obj_set_size(_btn, width, kStatusBarHeight - 6);  // 32px tall with 3px top/bottom margin
        lv_obj_set_style_bg_color(_btn, lv_color_hex(colorPrimary), 0);
        lv_obj_set_style_bg_opa(_btn, 28, 0);
        lv_obj_set_style_bg_color(_btn, lv_color_hex(colorPrimary), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(_btn, 84, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(_btn, 1, 0);
        lv_obj_set_style_border_color(_btn, lv_color_hex(colorPrimary), 0);
        lv_obj_set_style_border_opa(_btn, 170, 0);
        lv_obj_set_style_outline_width(_btn, 0, 0);
        lv_obj_set_style_shadow_width(_btn, 0, 0);
        lv_obj_set_style_radius(_btn, 8, 0);
        lv_obj_set_style_pad_all(_btn, 0, 0);
        lv_obj_remove_flag(_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(_btn, 6);

        if (useHomeIcon) {
            _icon_home = assets::get_image("icon_home.bin");
        }

        if (useHomeIcon && _icon_home.data != nullptr && _icon_home.data_size > 0) {
            lv_obj_t* img = lv_image_create(_btn);
            lv_image_set_src(img, &_icon_home);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor(img, lv_color_hex(colorPrimary), 0);
            if (_icon_home.header.h > 22) {
                uint16_t scale = static_cast<uint16_t>((20 * 256) / _icon_home.header.h);
                lv_image_set_scale(img, scale);
            }
            lv_obj_center(img);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
        } else {
            lv_obj_t* lbl = lv_label_create(_btn);
            lv_label_set_text(lbl, symbolText);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colorPrimary), 0);
            lv_obj_center(lbl);
            lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        lv_obj_add_event_cb(
            _btn,
            [](lv_event_t* e) {
                auto* self = static_cast<BarActionButton*>(lv_event_get_user_data(e));
                if (self && self->_on_click) {
                    self->_on_click();
                }
            },
            LV_EVENT_CLICKED, this);
    }

private:
    lv_obj_t* _btn            = nullptr;
    lv_image_dsc_t _icon_home = {};
    std::function<void()> _on_click;
};

class StatusBarView {
public:
    std::function<void()> onHomeClicked;

    StatusBarView(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary, bool fixedMode)
        : _fixed_mode(fixedMode)
    {
        (void)parent;
        // Place the status bar on lv_layer_top() so no screen or modal container created
        // on lv_screen_active() can ever cover or block touch events on Home, VOL-, or VOL+!
        _panel = std::make_unique<uitk::lvgl_cpp::Container>(lv_layer_top());
        _panel->setBgColor(lv_color_hex(colorSecondary));
        _panel->setBgOpa(LV_OPA_COVER);
        _panel->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        _panel->align(LV_ALIGN_TOP_MID, 0, 0);
        _panel->setBorderWidth(0);
        _panel->setSize(320, kStatusBarHeight);
        _panel->setRadius(0);
        _panel->setPadding(0, 0, 0, 0);

        // Far Left: Home button [x = 4..44, w = 40]
        _btn_home = std::make_unique<BarActionButton>(
            _panel->get(), 4, 40, colorPrimary, LV_SYMBOL_HOME, true, [this]() {
                _home_clicked = true;
                if (onHomeClicked) {
                    onHomeClicked();
                }
            });

        // Center Status Information: [Wi-Fi (x=50)] [Time (x=76)] [Battery % + Icon (x=125..187)]
        _widgets.push_back(std::make_unique<Wifi>(_panel->get(), colorPrimary));
        _widgets.push_back(std::make_unique<TimeLabel>(_panel->get(), colorPrimary));
        _widgets.push_back(std::make_unique<Battery>(_panel->get(), colorSecondary, colorPrimary));

        // Far Right: [ - (x=194..238) ]  [ Vol 0..100 (x=239..271) ]  [ + (x=272..316) ]
        _btn_vol_dn = std::make_unique<BarActionButton>(
            _panel->get(), 194, 44, colorPrimary, LV_SYMBOL_MINUS, false,
            [this]() { adjust_global_volume(-kVolumeStep); });

        auto vol_widget = std::make_unique<VolumeValueLabel>(_panel->get(), colorPrimary);
        _vol_label_ptr  = vol_widget.get();
        _widgets.push_back(std::move(vol_widget));

        _btn_vol_up = std::make_unique<BarActionButton>(
            _panel->get(), 272, 44, colorPrimary, LV_SYMBOL_PLUS, false,
            [this]() { adjust_global_volume(+kVolumeStep); });

        int init_y = _fixed_mode ? _pos_y_show : _pos_y_hide;
        _panel->setPos(0, init_y);
        _pos_y_anim.springOptions().bounce         = 0.1;
        _pos_y_anim.springOptions().visualDuration = 0.3;
        _pos_y_anim.teleport(init_y);
    }

    void update()
    {
        if (!_fixed_mode) {
            _pos_y_anim.update();
            if (!_pos_y_anim.done()) {
                _panel->setPos(0, _pos_y_anim.directValue());
            } else {
                if (_hide_panel_flag) {
                    _hide_panel_flag = false;
                    _panel->setHidden(true);
                }
            }
        }

        if (GetHAL().millis() - _last_update_tick > 400) {
            _last_update_tick = GetHAL().millis();
            for (auto& widget : _widgets) {
                widget->update();
            }
        }
    }

    void show()
    {
        _panel->moveForeground();
        _panel->setHidden(false);
        if (_fixed_mode) {
            _panel->setPos(0, _pos_y_show);
            _pos_y_anim.teleport(_pos_y_show);
        } else {
            _pos_y_anim = _pos_y_show;
        }
        _is_hidden = false;
    }

    void hide()
    {
        if (_fixed_mode) {
            return;
        }
        _pos_y_anim      = _pos_y_hide;
        _is_hidden       = true;
        _hide_panel_flag = true;
    }

    bool isHidden() const
    {
        return _is_hidden;
    }

    bool consumeActionTouch()
    {
        if (_action_touched) {
            _action_touched = false;
            return true;
        }
        return false;
    }

    bool consumeHomeClick()
    {
        if (_home_clicked) {
            _home_clicked = false;
            return true;
        }
        return false;
    }

private:
    const int _pos_y_show = 0;
    const int _pos_y_hide = -kStatusBarHeight;

    bool _fixed_mode                = true;
    bool _home_clicked              = false;
    bool _action_touched            = false;
    VolumeValueLabel* _vol_label_ptr = nullptr;
    std::unique_ptr<Container> _panel;
    std::vector<std::unique_ptr<Widget>> _widgets;
    std::unique_ptr<BarActionButton> _btn_home;
    std::unique_ptr<BarActionButton> _btn_vol_dn;
    std::unique_ptr<BarActionButton> _btn_vol_up;
    AnimateValue _pos_y_anim;
    bool _is_hidden            = false;
    bool _hide_panel_flag      = false;
    uint32_t _last_update_tick = 0;

    void adjust_global_volume(int delta)
    {
        _action_touched = true;
        int cur  = static_cast<int>(GetHAL().getSpeakerVolume());
        int next = std::clamp(cur + delta, 0, 100);
        GetHAL().setSpeakerVolume(static_cast<uint8_t>(next), true);
        if (_vol_label_ptr) {
            _vol_label_ptr->setImmediate(next);
        }
    }
};

}  // namespace status_bar_view

/**
 * @brief StatusBar controller supporting Fixed and AutoHide modes + Home and [- Vol +] buttons.
 *
 */
class StatusBar {
public:
    std::function<void(void)> onGoHome;

    void init(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary, std::function<void(void)> homeCb)
    {
        onGoHome    = std::move(homeCb);
        _fixed_mode = (view::get_status_bar_mode() == view::StatusBarMode::Fixed);

        _status_bar_gesture            = std::make_unique<StatuBarGesture>();
        _status_bar_gesture->onGesture = [&]() { handle_gesture(); };
        _status_bar_gesture->init();

        _status_bar_view =
            std::make_unique<status_bar_view::StatusBarView>(parent, colorSecondary, colorPrimary, _fixed_mode);
        _status_bar_view->onHomeClicked = [this]() {
            if (_home_dispatched) {
                return;
            }
            _home_dispatched = true;
            if (onGoHome) {
                onGoHome();
            }
        };
        _status_bar_view->show();
        _status_bar_show_tick = GetHAL().millis();
        _is_first_show        = true;
    }

    void update()
    {
        if (!_fixed_mode) {
            _status_bar_gesture->update();
        }
        _status_bar_view->update();
        if (_status_bar_view->consumeActionTouch()) {
            _status_bar_show_tick = GetHAL().millis();
        }
        if (!_fixed_mode) {
            update_visibility();
        }
        if (_status_bar_view->consumeHomeClick() && !_home_dispatched) {
            _home_dispatched = true;
            if (onGoHome) {
                onGoHome();
            }
        }
    }

private:
    bool _fixed_mode      = true;
    bool _home_dispatched = false;
    std::unique_ptr<StatuBarGesture> _status_bar_gesture;
    std::unique_ptr<status_bar_view::StatusBarView> _status_bar_view;
    uint32_t _status_bar_show_tick = 0;
    bool _is_first_show            = true;

    void handle_gesture()
    {
        _status_bar_view->show();
        _status_bar_show_tick = GetHAL().millis();
    }

    void update_visibility()
    {
        if (!_status_bar_view->isHidden()) {
            if (GetHAL().millis() - _status_bar_show_tick > (_is_first_show ? 2200 : 6000)) {
                _is_first_show = false;
                _status_bar_view->hide();
            }
        }
    }
};

namespace view {

static std::unique_ptr<StatusBar> _status_bar;

StatusBarMode get_status_bar_mode()
{
    int cached = s_cached_sb_mode.load();
    if (cached >= 0) {
        return (cached == static_cast<int>(StatusBarMode::AutoHide)) ? StatusBarMode::AutoHide : StatusBarMode::Fixed;
    }

    uint8_t val = static_cast<uint8_t>(StatusBarMode::Fixed);
    nvs_handle_t h{};
    if (nvs_open(kStatusBarNvsNs, NVS_READONLY, &h) == ESP_OK) {
        uint8_t stored = val;
        if (nvs_get_u8(h, kStatusBarNvsKey, &stored) == ESP_OK && stored <= 1) {
            val = stored;
        }
        nvs_close(h);
    }

    s_cached_sb_mode.store(static_cast<int>(val));
    return static_cast<StatusBarMode>(val);
}

void set_status_bar_mode(StatusBarMode mode)
{
    uint8_t val = (mode == StatusBarMode::AutoHide) ? 1 : 0;
    s_cached_sb_mode.store(static_cast<int>(val));

    nvs_handle_t h{};
    if (nvs_open(kStatusBarNvsNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, kStatusBarNvsKey, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

int get_status_bar_top_offset()
{
    return (get_status_bar_mode() == StatusBarMode::Fixed) ? kStatusBarHeight : 0;
}

void create_status_bar(uint32_t colorSecondary, uint32_t colorPrimary, lv_obj_t* parent,
                       std::function<void(void)> onGoHome)
{
    if (!onGoHome && s_pending_home_cb) {
        onGoHome = s_pending_home_cb;
    }
    _status_bar = std::make_unique<StatusBar>();
    _status_bar->init(parent, colorSecondary, colorPrimary, std::move(onGoHome));
}

void set_status_bar_home_callback(std::function<void(void)> onGoHome)
{
    s_pending_home_cb = onGoHome;
    if (_status_bar) {
        _status_bar->onGoHome = std::move(onGoHome);
    }
}

void update_status_bar()
{
    if (_status_bar) {
        _status_bar->update();
    }
}

bool is_status_bar_created()
{
    return _status_bar != nullptr;
}

void destroy_status_bar()
{
    _status_bar.reset();
    s_pending_home_cb = nullptr;
}

}  // namespace view
