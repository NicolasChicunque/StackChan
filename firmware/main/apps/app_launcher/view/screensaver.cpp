/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"
#include <assets/assets.h>
#include <stackchan/avatar/skins/default/default.h>
#include <cstdint>
#include <memory>

using namespace view;
using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace uitk::games;
using namespace uitk::games::dvd_screensaver;

static const Vector2 _screen_size               = {320, 240};
static const Vector2 _classic_logo_size         = {64, 48};
static const Vector2 _naruto_logo_size          = {74, 74};
static const int _logo_id                       = 666;
static const std::vector<uint32_t> _logo_colors = {
    0xffffff, 0xfffa01, 0xff8300, 0x00feff, 0xff2600, 0xbe00ff, 0x0026ff, 0xff008b,
};
static const uint32_t _bg_color = 0x000000;

Screensaver::~Screensaver()
{
    _prev_screen->load();
}

void Screensaver::onInit()
{
    _prev_screen = std::make_unique<ScreenActive>();

    _screen = std::make_unique<Screen>();
    _screen->setBgColor(lv_color_hex(_bg_color));
    _screen->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _screen->setPadding(0, 0, 0, 0);
    _screen->load();

    _naruto_imgs[0] = assets::get_image("avatar_calm_music.bin");
    _naruto_imgs[1] = assets::get_image("avatar_happy_music.bin");
    _naruto_imgs[2] = assets::get_image("avatar_smug.bin");
    _naruto_imgs[3] = assets::get_image("avatar_sleepy.bin");

    _use_naruto = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto &&
                   _naruto_imgs[0].data != nullptr);

    const Vector2 active_size = _use_naruto ? _naruto_logo_size : _classic_logo_size;

    _logo = std::make_unique<Container>(_screen->get());
    _logo->setSize(active_size.width, active_size.height);
    _logo->align(LV_ALIGN_TOP_LEFT, 2333, 2333);
    _logo->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _logo->setPadding(0, 0, 0, 0);
    _logo->setBorderWidth(0);
    _logo->setRadius(0);

    if (_use_naruto) {
        _logo->setBgOpa(LV_OPA_TRANSP);
        _naruto_img = lv_image_create(_logo->get());
        lv_image_set_src(_naruto_img, &_naruto_imgs[0]);
        if (_naruto_imgs[0].header.w > 0) {
            uint16_t scale = static_cast<uint16_t>((72 * 256) / _naruto_imgs[0].header.w);
            lv_image_set_scale(_naruto_img, scale);
        }
        lv_obj_center(_naruto_img);
        lv_obj_remove_flag(_naruto_img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        _logo->setBgColor(lv_color_hex(_logo_colors[0]));

        _left_eye = std::make_unique<Container>(_logo->get());
        _left_eye->align(LV_ALIGN_CENTER, -15, -3);
        _left_eye->setBgColor(lv_color_hex(_bg_color));
        _left_eye->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        _left_eye->setRadius(LV_RADIUS_CIRCLE);
        _left_eye->setBorderWidth(0);
        _left_eye->setSize(6, 6);

        _right_eye = std::make_unique<Container>(_logo->get());
        _right_eye->align(LV_ALIGN_CENTER, 15, -3);
        _right_eye->setBgColor(lv_color_hex(_bg_color));
        _right_eye->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        _right_eye->setRadius(LV_RADIUS_CIRCLE);
        _right_eye->setBorderWidth(0);
        _right_eye->setSize(6, 6);

        _mouth = std::make_unique<Container>(_logo->get());
        _mouth->align(LV_ALIGN_CENTER, 0, 5);
        _mouth->setBgColor(lv_color_hex(_bg_color));
        _mouth->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        _mouth->setBorderWidth(0);
        _mouth->setSize(18, 2);
        _mouth->setRadius(0);
    }
}

void Screensaver::onBuildLevel()
{
    addScreenFrameAsWall(_screen_size);

    auto& random      = Random::getInstance();
    Vector2 direction = {random.getFloat(0.3, 0.7), random.getFloat(0.3, 0.7)};
    direction         = direction.normalized();

    const Vector2 active_size = _use_naruto ? _naruto_logo_size : _classic_logo_size;
    addLogo(_logo_id, {_screen_size.width / 2, _screen_size.height / 2}, active_size, direction, 110);
}

void Screensaver::onRender(float dt)
{
    const Vector2 active_size = _use_naruto ? _naruto_logo_size : _classic_logo_size;
    getWorld().forEachObject([&](GameObject* obj) {
        if (obj->groupId == _logo_id) {
            auto p = obj->get<Transform>()->position;
            _logo->setPos((int)p.x - active_size.width / 2, (int)p.y - active_size.height / 2);
        }
    });
}

void Screensaver::onLogoCollide(int logoGroupId)
{
    _color_index++;
    if (_use_naruto && _naruto_img) {
        int idx = _color_index % 4;
        if (_naruto_imgs[idx].data == nullptr) {
            idx = 0;
        }
        if (_naruto_imgs[idx].data != nullptr) {
            lv_image_set_src(_naruto_img, &_naruto_imgs[idx]);
            uint16_t scale = static_cast<uint16_t>((72 * 256) / _naruto_imgs[idx].header.w);
            lv_image_set_scale(_naruto_img, scale);
        }
        return;
    }

    if (_color_index >= _logo_colors.size()) {
        _color_index = 0;
    }

    _logo->setBgColor(lv_color_hex(_logo_colors[_color_index]));
}
