/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "workers.h"
#include <stackchan/stackchan.h>
#include <stackchan/avatar/skins/default/default.h>
#include <apps/common/status_bar/status_bar.h>
#include <apps/common/toast/toast.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <vector>
#include <string>

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace setup_workers;

static std::string _tag = "Setup-System";

struct TimezoneOption_t {
    std::string name;
    std::string tz_posix;
};

static const std::vector<TimezoneOption_t> _timezone_list = {
    {"Baker Is. (UTC-12)", "BIT12"},  {"Midway Island (UTC-11)", "SST11"}, {"Honolulu (UTC-10)", "HST10"},
    {"Alaska (UTC-9)", "AKST9"},      {"Los Angeles (UTC-8)", "PST8"},     {"Denver (UTC-7)", "MST7"},
    {"Chicago (UTC-6)", "CST6"},      {"New York (UTC-5)", "EST5"},        {"Halifax (UTC-4)", "AST4"},
    {"S.Paulo (UTC-3)", "BRT3"},      {"S.Georgia (UTC-2)", "GST2"},       {"Azores (UTC-1)", "AZOT1"},
    {"London (UTC+0)", "GMT0"},       {"Berlin (UTC+1)", "CET-1"},         {"Cairo (UTC+2)", "EET-2"},
    {"Moscow (UTC+3)", "MSK-3"},      {"Dubai (UTC+4)", "GST-4"},          {"Karachi (UTC+5)", "PKT-5"},
    {"Dhaka (UTC+6)", "BST-6"},       {"Bangkok (UTC+7)", "ICT-7"},        {"Beijing (UTC+8)", "CST-8"},
    {"Tokyo (UTC+9)", "JST-9"},       {"Sydney (UTC+10)", "AEST-10"},      {"Noumea (UTC+11)", "SBT-11"},
    {"Auckland (UTC+12)", "NZST-12"}, {"Fiji (UTC+13)", "FJT-13"},         {"Line Islands (UTC+14)", "LINT-14"}};

VolumeSetupWorker::VolumeSetupWorker()
{
    mclog::info("VolumeSetupWorker start");

    for (int volume = 0; volume <= 100; volume += 5) {
        _volume_levels.push_back(volume);
    }

    uint8_t current_volume = GetHAL().getSpeakerVolume();
    _original_volume       = current_volume;
    int current_index      = _volume_levels.size() - 1;
    for (size_t i = 0; i < _volume_levels.size(); i++) {
        if (_volume_levels[i] >= current_volume) {
            current_index = static_cast<int>(i);
            break;
        }
    }

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_volume = std::make_unique<Label>(*_panel);
    _label_volume->setText(fmt::format("{}%", _volume_levels[current_index]));
    _label_volume->setTextFont(&lv_font_montserrat_24);
    _label_volume->setTextColor(lv_color_hex(0x26206A));
    _label_volume->align(LV_ALIGN_CENTER, 0, -70);

    _slider = std::make_unique<Slider>(*_panel);
    _slider->align(LV_ALIGN_CENTER, 0, -12);
    _slider->setRange(0, _volume_levels.size() - 1);
    _slider->setSize(250, 18);
    _slider->setBgColor(lv_color_hex(0x615B9E), LV_PART_KNOB);
    _slider->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR);
    _slider->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _slider->setBgOpa(255);
    _slider->setValue(current_index);
    _slider->onValueChanged().connect([this](int32_t value) {
        _label_volume->setText(fmt::format("{}%", _volume_levels[value]));
        _target_volume = _volume_levels[value];
    });

    _btn_confirm = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_CENTER, 0, 60);
    _btn_confirm->setSize(150, 50);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() {
        _confirmed = true;
        _is_done   = true;
    });
}

VolumeSetupWorker::~VolumeSetupWorker()
{
    if (_confirmed) {
        auto volume = _volume_levels[_slider->getValue()];
        mclog::tagInfo(_tag, "final volume: {}", volume);
        GetHAL().setSpeakerVolume(volume, true);
        return;
    }

    mclog::tagInfo(_tag, "volume change cancelled, restore: {}", _original_volume);
    GetHAL().setSpeakerVolume(_original_volume, false);
}

void VolumeSetupWorker::update()
{
    if (_target_volume != -1) {
        GetHAL().setSpeakerVolume(_target_volume, false);
        _target_volume = -1;
    }
}

TimezoneWorker::TimezoneWorker()
{
    _panel = std::make_unique<uitk::lvgl_cpp::Container>(lv_screen_active());
    _panel->setPadding(0, 0, 0, 0);
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);

    _label = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
    _label->setText("Time Zone");
    _label->setTextFont(&lv_font_montserrat_16);
    _label->setTextColor(lv_color_hex(0x26206A));
    _label->align(LV_ALIGN_CENTER, 0, -100);

    // Timezone list
    std::string options;
    for (const auto& tz : _timezone_list) {
        options += tz.name + "\n";
    }
    // Remove last newline
    if (!options.empty()) {
        options.pop_back();
    }

    _roller = std::make_unique<uitk::lvgl_cpp::Roller>(_panel->get());
    _roller->setSize(210, 188);
    _roller->setOptions(options.c_str());
    _roller->align(LV_ALIGN_CENTER, -40, 16);
    _roller->setTextFont(&lv_font_montserrat_16);
    _roller->setTextColor(lv_color_hex(0x26206A));
    _roller->setBgColor(lv_color_hex(0xB8D3FD));
    _roller->setRadius(18);
    _roller->setShadowWidth(0);
    _roller->setBorderWidth(0);
    _roller->setBgColor(lv_color_hex(0x615B9E), LV_PART_SELECTED);

    // Set current selection
    std::string current_tz = GetHAL().getTimezone();
    int utc0_index         = -1;
    for (size_t i = 0; i < _timezone_list.size(); ++i) {
        if (current_tz == _timezone_list[i].tz_posix) {
            _roller->setSelected(i, LV_ANIM_OFF);
            utc0_index = -1;
            break;
        } else if (_timezone_list[i].tz_posix == "GMT0") {
            utc0_index = i;
        }
    }

    if (utc0_index >= 0) {
        // Default to UTC+0
        _roller->setSelected(utc0_index, LV_ANIM_OFF);
    }

    _btn_confirm = std::make_unique<uitk::lvgl_cpp::Button>(_panel->get());
    _btn_confirm->label().setText("ok");
    _btn_confirm->label().setTextFont(&lv_font_montserrat_24);
    _btn_confirm->setSize(60, 110);
    _btn_confirm->align(LV_ALIGN_CENTER, 115, 40);
    _btn_confirm->onClick().connect([&]() { _confirm_flag = true; });
    _btn_confirm->setRadius(18);
    _btn_confirm->setShadowWidth(0);
    _btn_confirm->setBgColor(lv_color_hex(0x615B9E));
}

TimezoneWorker::~TimezoneWorker()
{
}

void TimezoneWorker::update()
{
    if (_confirm_flag) {
        _confirm_flag = false;

        uint16_t selected_id = _roller->getSelected();
        if (selected_id < _timezone_list.size()) {
            const auto& selected_option = _timezone_list[selected_id];
            GetHAL().setTimezone(selected_option.tz_posix);

            view::pop_a_toast("Timezone Set", view::ToastType::Success);
            mclog::tagInfo(_tag, "timezone set to: {}", selected_option.name);
        }

        _is_done = true;
    }
}

FactoryResetWorker::FactoryResetWorker(std::function<bool()> beforeResetAction)
{
    _before_reset_action = std::move(beforeResetAction);

    _panel = std::make_unique<uitk::lvgl_cpp::Container>(lv_screen_active());
    _panel->setPadding(0, 0, 0, 0);
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);

    // Title
    _label_title = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
    _label_title->setText("Factory Reset");
    _label_title->setTextFont(&lv_font_montserrat_24);
    _label_title->setTextColor(lv_color_hex(0x26206A));
    _label_title->align(LV_ALIGN_CENTER, 0, -80);

    // Info
    _label_info = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
    _label_info->setTextFont(&lv_font_montserrat_16);
    _label_info->setTextColor(lv_color_hex(0x26206A));
    _label_info->align(LV_ALIGN_CENTER, 0, -20);
    _label_info->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_info->setWidth(280);

    // Cancel Button
    _btn_cancel = std::make_unique<uitk::lvgl_cpp::Button>(_panel->get());
    apply_button_common_style(*_btn_cancel);
    _btn_cancel->align(LV_ALIGN_CENTER, -72, 60);
    _btn_cancel->setSize(112, 48);
    _btn_cancel->label().setText("Cancel");
    _btn_cancel->label().setTextFont(&lv_font_montserrat_20);
    _btn_cancel->onClick().connect([this]() { _cancel_flag = true; });

    // Confirm Button
    _btn_confirm = std::make_unique<uitk::lvgl_cpp::Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_CENTER, 72, 60);
    _btn_confirm->setSize(112, 48);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->label().setTextFont(&lv_font_montserrat_20);
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });

    update_ui();
}

FactoryResetWorker::~FactoryResetWorker()
{
}

void FactoryResetWorker::update()
{
    if (_cancel_flag) {
        _is_done = true;
        return;
    }

    if (_confirm_flag) {
        _confirm_flag = false;
        _confirm_count++;

        if (_confirm_count >= 3) {
            // Keep the controls alive until the prerequisite succeeds. A
            // failed server unbind must never erase local configuration.
            _btn_cancel->addState(LV_STATE_DISABLED);
            _btn_confirm->addState(LV_STATE_DISABLED);
            if (_before_reset_action && !_before_reset_action()) {
                _confirm_flag  = false;
                _confirm_count = 2;
                _btn_cancel->removeState(LV_STATE_DISABLED);
                _btn_confirm->removeState(LV_STATE_DISABLED);
                _label_info->setText("Unbind failed.\nLocal settings kept.");
                _btn_confirm->label().setText("Retry");
                mclog::tagWarn(_tag, "Reset stopped: prerequisite failed; local settings kept");
                return;
            }

            mclog::tagInfo(_tag, "factory reset triggered");
            _btn_cancel.reset();
            _btn_confirm.reset();
            _label_title.reset();

            _label_info->setText("Factory Resetting...\nDo not turn off power.");
            _label_info->align(LV_ALIGN_CENTER, 0, 0);

            GetHAL().lvglUnlock();
            GetHAL().delay(200);
            GetHAL().factoryReset();

            // The device reboots above. Desktop HALs may return instead.
            GetHAL().lvglLock();
            _is_done = true;

        } else {
            update_ui();
        }
    }
}

void FactoryResetWorker::update_ui()
{
    if (_confirm_count == 0) {
        _label_info->setText("Reset all settings to factory default?\nThis cannot be undone.");
        _btn_confirm->label().setText("Reset");
        _btn_confirm->setBgColor(lv_color_hex(0xFFB8B8));
    } else if (_confirm_count == 1) {
        _label_info->setText("Are you absolutely sure?\nAll user data will be lost!");
        _btn_confirm->label().setText("Yes");
        _btn_confirm->setBgColor(lv_color_hex(0xFF8888));
    } else if (_confirm_count == 2) {
        _label_info->setText("Last Warning!\nPress Confirm to erase everything.");
        _btn_confirm->label().setText("Confirm");
        _btn_confirm->setBgColor(lv_color_hex(0xFF4444));
    }
}

AvatarStyleWorker::AvatarStyleWorker()
{
    _use_custom_avatar = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Custom);

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_title = std::make_unique<Label>(*_panel);
    _label_title->setText("Avatar Style");
    _label_title->setTextFont(&lv_font_montserrat_24);
    _label_title->setTextColor(lv_color_hex(0x26206A));
    _label_title->align(LV_ALIGN_CENTER, 0, -82);

    _label_hint = std::make_unique<Label>(*_panel);
    _label_hint->setText("Classic or Custom (/sd/avatars)");
    _label_hint->setTextFont(&lv_font_montserrat_14);
    _label_hint->setTextColor(lv_color_hex(0x5A558F));
    _label_hint->align(LV_ALIGN_CENTER, 0, -52);

    _btn_classic = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_classic);
    _btn_classic->align(LV_ALIGN_CENTER, -72, -2);
    _btn_classic->setSize(132, 54);
    _btn_classic->label().setTextFont(&lv_font_montserrat_20);
    _btn_classic->label().setText("Classic");
    _btn_classic->onClick().connect([this]() {
        _use_custom_avatar = false;
        refresh_buttons();
    });

    _btn_custom_avatar = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_custom_avatar);
    _btn_custom_avatar->align(LV_ALIGN_CENTER, 72, -2);
    _btn_custom_avatar->setSize(132, 54);
    _btn_custom_avatar->label().setTextFont(&lv_font_montserrat_20);
    _btn_custom_avatar->label().setText("Custom");
    _btn_custom_avatar->onClick().connect([this]() {
        _use_custom_avatar = true;
        refresh_buttons();
    });

    _btn_confirm = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_CENTER, 0, 68);
    _btn_confirm->setSize(150, 48);
    _btn_confirm->label().setTextFont(&lv_font_montserrat_20);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() {
        stackchan::avatar::setAvatarStyle(
            _use_custom_avatar ? stackchan::avatar::AvatarStyle::Custom : stackchan::avatar::AvatarStyle::Classic);
        _is_done = true;
    });

    refresh_buttons();
}

void AvatarStyleWorker::refresh_buttons()
{
    if (_btn_classic) {
        _btn_classic->setBgColor(lv_color_hex(_use_custom_avatar ? 0xB8D3FD : 0x615B9E));
        _btn_classic->label().setTextColor(lv_color_hex(_use_custom_avatar ? 0x26206A : 0xFFFFFF));
    }
    if (_btn_custom_avatar) {
        _btn_custom_avatar->setBgColor(lv_color_hex(_use_custom_avatar ? 0x615B9E : 0xB8D3FD));
        _btn_custom_avatar->label().setTextColor(lv_color_hex(_use_custom_avatar ? 0xFFFFFF : 0x26206A));
    }
}

void AvatarStyleWorker::update()
{
}

StatusBarModeWorker::StatusBarModeWorker()
{
    _use_fixed = (view::get_status_bar_mode() == view::StatusBarMode::Fixed);

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_title = std::make_unique<Label>(*_panel);
    _label_title->setText("Status Bar");
    _label_title->setTextFont(&lv_font_montserrat_24);
    _label_title->setTextColor(lv_color_hex(0x26206A));
    _label_title->align(LV_ALIGN_CENTER, 0, -76);

    _label_hint = std::make_unique<Label>(*_panel);
    _label_hint->setText("Top bar with Wi-Fi, Time, Battery & Home");
    _label_hint->setTextFont(&lv_font_montserrat_14);
    _label_hint->setTextColor(lv_color_hex(0x5A558F));
    _label_hint->align(LV_ALIGN_CENTER, 0, -48);

    _btn_fixed = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_fixed);
    _btn_fixed->align(LV_ALIGN_CENTER, -72, 2);
    _btn_fixed->setSize(132, 54);
    _btn_fixed->label().setTextFont(&lv_font_montserrat_20);
    _btn_fixed->label().setText("Fixed");
    _btn_fixed->onClick().connect([this]() {
        _use_fixed = true;
        refresh_buttons();
    });

    _btn_autohide = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_autohide);
    _btn_autohide->align(LV_ALIGN_CENTER, 72, 2);
    _btn_autohide->setSize(132, 54);
    _btn_autohide->label().setTextFont(&lv_font_montserrat_20);
    _btn_autohide->label().setText("Auto-hide");
    _btn_autohide->onClick().connect([this]() {
        _use_fixed = false;
        refresh_buttons();
    });

    _btn_confirm = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_CENTER, 0, 72);
    _btn_confirm->setSize(150, 48);
    _btn_confirm->label().setTextFont(&lv_font_montserrat_20);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() {
        view::set_status_bar_mode(_use_fixed ? view::StatusBarMode::Fixed : view::StatusBarMode::AutoHide);
        view::destroy_status_bar();
        view::create_status_bar();
        _is_done = true;
    });

    refresh_buttons();
}

void StatusBarModeWorker::refresh_buttons()
{
    if (_btn_fixed) {
        _btn_fixed->setBgColor(lv_color_hex(_use_fixed ? 0x615B9E : 0xB8D3FD));
        _btn_fixed->label().setTextColor(lv_color_hex(_use_fixed ? 0xFFFFFF : 0x26206A));
    }
    if (_btn_autohide) {
        _btn_autohide->setBgColor(lv_color_hex(_use_fixed ? 0xB8D3FD : 0x615B9E));
        _btn_autohide->label().setTextColor(lv_color_hex(_use_fixed ? 0x26206A : 0xFFFFFF));
    }
}

void StatusBarModeWorker::update()
{
}
