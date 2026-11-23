/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "default.h"
#include <assets/assets.h>
#include <hal/hal.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstring>

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

namespace {

constexpr const char* kNarutoAssetNames[7] = {
    "naruto_calm.bin",     // 0: Neutral / Calm
    "naruto_thinking.bin", // 1: Doubt / Thinking / Listening
    "naruto_happy.bin",    // 2: Happy / Laughing
    "naruto_sleepy.bin",   // 3: Sleepy
    "naruto_shy.bin",      // 4: Sad / Shy
    "naruto_smug.bin",     // 5: Smug / Speaking
    "naruto_pouty.bin",    // 6: Angry / Pouty
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

DefaultAvatar::~DefaultAvatar()
{
    _naruto_img = nullptr;
    _pannel.reset();
    for (int i = 0; i < 7; ++i) {
        if (_naruto_psram_bufs[i]) {
            heap_caps_free(_naruto_psram_bufs[i]);
            _naruto_psram_bufs[i] = nullptr;
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

    // Load all 7 Naruto sprites and copy them to PSRAM to avoid Flash SPI contention during audio/Wi-Fi
    _naruto_enabled = false;
    for (int i = 0; i < 7; ++i) {
        _naruto_sprites[i] = assets::get_image(kNarutoAssetNames[i]);
        if (_naruto_sprites[i].data != nullptr && _naruto_sprites[i].data_size > 0) {
            uint8_t* psram_copy = static_cast<uint8_t*>(
                heap_caps_malloc(_naruto_sprites[i].data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (psram_copy) {
                std::memcpy(psram_copy, _naruto_sprites[i].data, _naruto_sprites[i].data_size);
                _naruto_psram_bufs[i]   = psram_copy;
                _naruto_sprites[i].data = psram_copy;
            }
            _naruto_enabled = true;
        }
    }

    // Keep core elements for compatibility with avatar modifiers (Blink, Breath, Speaking, etc.)
    _key_elements.leftEye  = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, true);
    _key_elements.rightEye = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, false);
    _key_elements.mouth    = std::make_unique<DefaultMouth>(_pannel->get(), primaryColor, secondaryColor);

    if (_naruto_enabled) {
        _key_elements.leftEye->setVisible(false);
        _key_elements.rightEye->setVisible(false);
        _key_elements.mouth->setVisible(false);

        _naruto_img = lv_image_create(_pannel->get());
        lv_obj_align(_naruto_img, LV_ALIGN_CENTER, 0, -10);
        lv_obj_remove_flag(_naruto_img, LV_OBJ_FLAG_CLICKABLE);
        _last_ox = 0;
        _last_oy = -10;
        apply_naruto_sprite(0);
    }

    // Create SpeechBubble last so it always renders above the avatar
    _key_elements.speechBubble =
        std::make_unique<DefaultSpeechBubble>(_pannel->get(), primaryColor, secondaryColor, font);
}

void DefaultAvatar::apply_naruto_sprite(int idx)
{
    if (!_naruto_img || idx < 0 || idx >= 7) {
        return;
    }
    if (_naruto_sprites[idx].data == nullptr) {
        idx = 0;
    }
    if (_current_sprite != idx && _naruto_sprites[idx].data != nullptr) {
        _current_sprite = idx;
        lv_image_set_src(_naruto_img, &_naruto_sprites[idx]);
    }
}

void DefaultAvatar::setEmotion(const Emotion& emotion)
{
    Avatar::setEmotion(emotion);
    if (_naruto_enabled) {
        _forced_variant = -1;
        apply_naruto_sprite(emotion_to_sprite_idx(emotion));
    }
}

void DefaultAvatar::setSpriteVariant(int variant)
{
    _forced_variant = variant;
    if (_naruto_enabled) {
        int idx = (variant >= 0 && variant < 7) ? variant : emotion_to_sprite_idx(_emotion);
        apply_naruto_sprite(idx);
    }
}

void DefaultAvatar::update()
{
    Avatar::update();

    if (!_naruto_enabled || !_naruto_img) {
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
            apply_naruto_sprite((_current_sprite == 2) ? 0 : 2);
        }
        return;
    }

    if (now - _last_anim_ms < 500) {
        return;
    }
    _last_anim_ms = now;

    if (_forced_variant < 0) {
        apply_naruto_sprite(emotion_to_sprite_idx(_emotion));
    }

    int oy = -10 + (((now / 1500) % 2 == 0) ? -2 : 2);
    if (_last_ox != 0 || _last_oy != oy) {
        _last_ox = 0;
        _last_oy = oy;
        lv_obj_align(_naruto_img, LV_ALIGN_CENTER, 0, oy);
    }
}

Container* DefaultAvatar::getPanel() const
{
    if (_pannel) {
        return _pannel.get();
    }
    return NULL;
}
