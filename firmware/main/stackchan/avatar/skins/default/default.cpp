/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "default.h"
#include <assets/assets.h>
#include <hal/hal.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#include <atomic>
#include <cmath>
#include <cstring>

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

namespace {

constexpr const char* kAvatarNvsNs  = "avatar_prefs";
constexpr const char* kAvatarNvsKey = "style";
std::atomic<int> s_cached_avatar_style{-1};

constexpr const char* kAvatarAssetNames[7] = {
    "avatar_calm.bin",     // 0: Neutral / Calm
    "avatar_thinking.bin", // 1: Doubt / Thinking / Listening
    "avatar_happy.bin",    // 2: Happy / Laughing
    "avatar_sleepy.bin",   // 3: Sleepy
    "avatar_shy.bin",      // 4: Sad / Shy
    "avatar_smug.bin",     // 5: Smug / Speaking
    "avatar_pouty.bin",    // 6: Angry / Pouty
};

int emotion_to_sprite_idx(const Emotion& emotion)
{
    switch (emotion) {
        case Emotion::Neutral: return 0; // Calm
        case Emotion::Doubt:   return 1; // Thinking
        case Emotion::Happy:   return 2; // Happy
        case Emotion::Sleepy:  return 3; // Sleepy
        case Emotion::Sad:     return 4; // Shy
        case Emotion::Angry:   return 6; // Pouty
        default:               return 0;
    }
}

}  // namespace

AvatarStyle stackchan::avatar::getAvatarStyle()
{
    int cached = s_cached_avatar_style.load();
    if (cached >= 0) {
        return (cached == static_cast<int>(AvatarStyle::Custom)) ? AvatarStyle::Custom : AvatarStyle::Classic;
    }

    uint8_t val = static_cast<uint8_t>(AvatarStyle::Custom);
    nvs_handle_t h{};
    if (nvs_open(kAvatarNvsNs, NVS_READONLY, &h) == ESP_OK) {
        uint8_t stored = val;
        if (nvs_get_u8(h, kAvatarNvsKey, &stored) == ESP_OK && stored <= 1) {
            val = stored;
        }
        nvs_close(h);
    }

    s_cached_avatar_style.store(static_cast<int>(val));
    return static_cast<AvatarStyle>(val);
}

void stackchan::avatar::setAvatarStyle(AvatarStyle style)
{
    uint8_t val = (style == AvatarStyle::Custom) ? 1 : 0;
    s_cached_avatar_style.store(static_cast<int>(val));

    nvs_handle_t h{};
    if (nvs_open(kAvatarNvsNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, kAvatarNvsKey, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

DefaultAvatar::~DefaultAvatar()
{
    _avatar_img = nullptr;
    _pannel.reset();
    for (int i = 0; i < 7; ++i) {
        if (_avatar_psram_bufs[i]) {
            heap_caps_free(_avatar_psram_bufs[i]);
            _avatar_psram_bufs[i] = nullptr;
        }
    }
}

void DefaultAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _pannel = std::make_unique<Container>(parent);
    _pannel->align(LV_ALIGN_CENTER, 0, 0);
    _pannel->setSize(320, 240);
    _pannel->setRadius(0);
    _pannel->setBorderWidth(0);
    _pannel->setBgColor(lv_color_hex(0x0B0F1A));
    _pannel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Load all 7 Custom Avatar sprites and copy them to PSRAM to avoid Flash SPI contention during audio/Wi-Fi
    _avatar_enabled = false;
    for (int i = 0; i < 7; ++i) {
        _avatar_sprites[i] = assets::get_image(kAvatarAssetNames[i]);
        if (_avatar_sprites[i].data != nullptr && _avatar_sprites[i].data_size > 0) {
            uint8_t* psram_copy = static_cast<uint8_t*>(
                heap_caps_malloc(_avatar_sprites[i].data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (psram_copy) {
                std::memcpy(psram_copy, _avatar_sprites[i].data, _avatar_sprites[i].data_size);
                _avatar_psram_bufs[i]   = psram_copy;
                _avatar_sprites[i].data = psram_copy;
            }
            _avatar_enabled = true;
        }
    }

    // Keep core elements for compatibility with avatar modifiers (Blink, Breath, Speaking, etc.)
    _key_elements.leftEye  = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, true);
    _key_elements.rightEye = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, false);
    _key_elements.mouth    = std::make_unique<DefaultMouth>(_pannel->get(), primaryColor, secondaryColor);

    if (_avatar_enabled) {
        _avatar_img = lv_image_create(_pannel->get());
        lv_obj_align(_avatar_img, LV_ALIGN_CENTER, 0, -10);
        lv_obj_remove_flag(_avatar_img, LV_OBJ_FLAG_CLICKABLE);
        _last_ox = 0;
        _last_oy = -10;
    }

    sync_avatar_style(true);

    // Create SpeechBubble last so it always renders above the avatar
    _key_elements.speechBubble =
        std::make_unique<DefaultSpeechBubble>(_pannel->get(), primaryColor, secondaryColor, font);
}

void DefaultAvatar::sync_avatar_style(bool force)
{
    AvatarStyle desired =
        (_avatar_enabled && _avatar_img && getAvatarStyle() == AvatarStyle::Custom)
            ? AvatarStyle::Custom
            : AvatarStyle::Classic;

    if (!force && desired == _active_style) {
        return;
    }

    _active_style = desired;
    if (desired == AvatarStyle::Custom) {
        _pannel->setBgColor(lv_color_hex(0x0B0F1A));
        _key_elements.leftEye->setVisible(false);
        _key_elements.rightEye->setVisible(false);
        _key_elements.mouth->setVisible(false);
        lv_obj_remove_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
        _current_sprite = -1;
        int idx         = (_forced_variant >= 0 && _forced_variant < 7)
                              ? _forced_variant
                              : emotion_to_sprite_idx(_emotion);
        apply_avatar_sprite(idx);
    } else {
        _pannel->setBgColor(secondaryColor);
        if (_avatar_img) {
            lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
        }
        _key_elements.leftEye->setVisible(true);
        _key_elements.rightEye->setVisible(true);
        _key_elements.mouth->setVisible(true);
    }
}

void DefaultAvatar::apply_avatar_sprite(int idx)
{
    if (!_avatar_img || idx < 0 || idx >= 7) {
        return;
    }
    if (_avatar_sprites[idx].data == nullptr) {
        idx = 0;
    }
    if (_current_sprite != idx && _avatar_sprites[idx].data != nullptr) {
        _current_sprite = idx;
        lv_image_set_src(_avatar_img, &_avatar_sprites[idx]);
    }
}

void DefaultAvatar::setEmotion(const Emotion& emotion)
{
    Avatar::setEmotion(emotion);
    _forced_variant = -1;
    if (_active_style == AvatarStyle::Custom) {
        apply_avatar_sprite(emotion_to_sprite_idx(emotion));
    }
}

void DefaultAvatar::setSpriteVariant(int variant)
{
    _forced_variant = variant;
    if (_active_style == AvatarStyle::Custom) {
        int idx = (variant >= 0 && variant < 7) ? variant : emotion_to_sprite_idx(_emotion);
        apply_avatar_sprite(idx);
    }
}

void DefaultAvatar::update()
{
    Avatar::update();
    sync_avatar_style(false);

    if (_active_style != AvatarStyle::Custom || !_avatar_img) {
        return;
    }

    if (_key_elements.leftEye->getVisible()) {
        _key_elements.leftEye->setVisible(false);
        _key_elements.rightEye->setVisible(false);
        _key_elements.mouth->setVisible(false);
    }

    // Keep CPU and SPI bus free for Xiaozhi audio:
    // - While speaking (mouth_weight > 15): hold sprite position fixed (avoid full 180x180 redraws on pixel offsets)
    //   and toggle expression at most every 450 ms when Neutral.
    // - While idle: gentle breathing shift every 500 ms.
    uint32_t now     = GetHAL().millis();
    int mouth_weight = _key_elements.mouth ? _key_elements.mouth->getWeight() : 0;

    if (mouth_weight > 15) {
        if (now - _last_anim_ms < 450) {
            return;
        }
        _last_anim_ms = now;
        if (_forced_variant < 0 && _emotion == Emotion::Neutral) {
            apply_avatar_sprite((_current_sprite == 2) ? 0 : 2);
        }
        return;
    }

    if (now - _last_anim_ms < 500) {
        return;
    }
    _last_anim_ms = now;

    if (_forced_variant < 0) {
        apply_avatar_sprite(emotion_to_sprite_idx(_emotion));
    }

    int oy = -10 + (((now / 1500) % 2 == 0) ? -2 : 2);
    if (_last_ox != 0 || _last_oy != oy) {
        _last_ox = 0;
        _last_oy = oy;
        lv_obj_align(_avatar_img, LV_ALIGN_CENTER, 0, oy);
    }
}

Container* DefaultAvatar::getPanel() const
{
    if (_pannel) {
        return _pannel.get();
    }
    return NULL;
}
