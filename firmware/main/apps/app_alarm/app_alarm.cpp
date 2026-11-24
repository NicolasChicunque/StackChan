/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_alarm.h"

#include <apps/common/common.h>
#include <assets/assets.h>
#include <audio/audio_codec.h>
#include <board.h>
#include <hal/board/config.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <stackchan/avatar/skins/default/default.h>
#include <stackchan/stackchan.h>
#include <wifi_manager.h>

#include <driver/sdspi_host.h>
#include <esp_heap_caps.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "../app_music_player/minimp3.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

using namespace mooncake;
using namespace stackchan;

namespace {

constexpr const char* TAG            = "ALARM";
constexpr const char* SD_MOUNT_POINT = "/sd";
constexpr int W                      = 320;
constexpr int H                      = 240;
constexpr int Y_HOME_DEG             = 45;
constexpr uint32_t GESTURE_GUARD_MS  = 1200;
constexpr uint32_t RAMP_DURATION_MS  = 12000;
constexpr float TWO_PI_F             = 6.2831853f;

// Unified StackChan dark palette
constexpr uint32_t COL_BG            = 0x0B0F1A;
constexpr uint32_t COL_CARD          = 0x151B2E;
constexpr uint32_t COL_CARD_HI       = 0x232C4A;
constexpr uint32_t COL_TEAL          = 0x0FBAA6;
constexpr uint32_t COL_TEAL_DK       = 0x0A5E56;
constexpr uint32_t COL_AMBER         = 0xFF9F1C;
constexpr uint32_t COL_BLUE          = 0x3D7DFF;
constexpr uint32_t COL_RED           = 0xE74C3C;
constexpr uint32_t COL_GREEN         = 0x2ECC71;
constexpr uint32_t COL_YELLOW        = 0xFFC940;
constexpr uint32_t COL_TEXT          = 0xE8EEF9;
constexpr uint32_t COL_DIM           = 0x7C89A8;
constexpr uint32_t COL_WHITE         = 0xFFFFFF;

int deg_to_yaw(int x_deg)
{
    return std::clamp(x_deg, -128, 128) * 10;
}

int deg_to_pitch(int y_deg)
{
    return (45 - std::clamp(y_deg, 0, 90)) * 10;
}

void style_box(lv_obj_t* obj, uint32_t bg_hex, int radius = 0)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

bool is_mp3_filename(const std::string& name)
{
    if (name.size() <= 4 || name[0] == '.') {
        return false;
    }
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".mp3";
}

std::string basename_clean(const std::string& path)
{
    size_t pos = path.find_last_of('/');
    std::string s = (pos == std::string::npos) ? path : path.substr(pos + 1);
    if (s.size() > 4) {
        s.resize(s.size() - 4);
    }
    return s;
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                           Mooncake Lifecycle                               */
/* -------------------------------------------------------------------------- */

AppAlarm::AppAlarm()
{
    setAppInfo().name = "ALARM";

    static lv_image_dsc_t icon_from_assets = assets::get_image("icon_alarm.bin");
    if (icon_from_assets.data != nullptr && icon_from_assets.data_size > 0) {
        setAppInfo().icon = (void*)&icon_from_assets;
    }

    static uint32_t theme_color = COL_AMBER;
    setAppInfo().userData       = (void*)&theme_color;
}

AppAlarm::~AppAlarm()
{
    stop_audio();
    unmount_sd();
}

void AppAlarm::onCreate()
{
    mclog::tagInfo(TAG, "on create");
}

void AppAlarm::onOpen()
{
    mclog::tagInfo(TAG, "on open");

    AlarmService::instance().setAppOpen(true);

    _alarms           = AlarmService::instance().getAlarms();
    _target_vol       = AlarmService::instance().getVolume();
    _screen           = Screen::Clock;
    _night_mode       = false;
    _saved_brightness = std::max<uint8_t>(GetHAL().getBackLightBrightness(), 50);
    _head_pet_flag    = false;
    _shake_flag       = false;
    _audio_rms        = 0;
    _ring_alarm_idx   = -1;

    // Load Naruto expressions if available in assets partition
    _img_naruto_calm       = assets::get_image("naruto_calm_music.bin");
    if (_img_naruto_calm.data == nullptr) {
        _img_naruto_calm   = assets::get_image("naruto_calm.bin");
    }
    _img_naruto_happy      = assets::get_image("naruto_happy_music.bin");
    if (_img_naruto_happy.data == nullptr) {
        _img_naruto_happy  = assets::get_image("naruto_happy.bin");
    }
    _img_naruto_sleepy     = assets::get_image("naruto_sleepy.bin");
    _img_naruto_pouty      = assets::get_image("naruto_pouty.bin");
    _shown_naruto_idx      = -1;
    _shown_ring_naruto_idx = -1;

    // Ensure Naruto avatar style is active on custom/naruto branch
    if (_img_naruto_calm.data != nullptr) {
        stackchan::avatar::setAvatarStyle(stackchan::avatar::AvatarStyle::Naruto);
    }

    set_sync_text("RTC ready");

    // Subscribe physical gestures (Head pet -> Snooze, Shake -> Stop)
    _head_pet_conn = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture g) {
        if (g == HeadPetGesture::Press || g == HeadPetGesture::SwipeForward || g == HeadPetGesture::SwipeBackward) {
            _head_pet_flag = true;
        }
    });
    _imu_conn = GetHAL().onImuMotionEvent.connect([this](ImuMotionEvent e) {
        if (e == ImuMotionEvent::Shake) {
            _shake_flag = true;
        }
    });

    // Center servos gently
    GetHAL().setServoPowerEnabled(true);
    GetHAL().showRgbColor(0, 22, 26);
    auto& sc = GetStackChan();
    sc.clearModifiers();
    sc.motion().setAutoAngleSyncEnabled(false);
    sc.motion().setAutoTorqueReleaseEnabled(false);
    sc.motion().moveWithSpeed(deg_to_yaw(0), deg_to_pitch(Y_HOME_DEG), 500);

    {
        LvglLockGuard lock;
        build_ui();
        uint32_t now = GetHAL().millis();
        refresh_clock_ui(now);
        refresh_avatar_anim(now);
        view::create_home_indicator([&]() { close(); }, COL_AMBER, COL_BG);
        view::create_status_bar(COL_AMBER, COL_BG);
    }

    // If launched by a background alarm trigger, jump straight into ringing!
    int pending = AlarmService::instance().consumePendingRingIdx();
    if (pending >= 0) {
        LvglLockGuard lock;
        begin_ringing(pending);
    }
}

void AppAlarm::onRunning()
{
    uint32_t now = GetHAL().millis();

    // Check if background AlarmService fired while AppAlarm is open
    int pending = AlarmService::instance().consumePendingRingIdx();
    if (pending >= 0) {
        LvglLockGuard lock;
        begin_ringing(pending);
    }

    // Physical gesture handling while ringing (with short guard window)
    if (_screen == Screen::Ringing) {
        bool guard_passed = (now - _ring_start_ms) >= GESTURE_GUARD_MS;
        if (_shake_flag.exchange(false) && guard_passed) {
            LvglLockGuard lock;
            action_dismiss();
        } else if (_head_pet_flag.exchange(false) && guard_passed) {
            LvglLockGuard lock;
            action_snooze();
        } else {
            update_ringing_motion(now);
        }
    } else {
        // Wake from Night mode on head pet
        if (_head_pet_flag.exchange(false) && _night_mode) {
            LvglLockGuard lock;
            set_night_mode(false);
        }
        _shake_flag = false;
    }

    GetStackChan().update();

    {
        LvglLockGuard lock;
        if (now - _last_anim_tick >= 90) {
            _last_anim_tick = now;
            refresh_avatar_anim(now);
        }
        if (now - _last_ui_tick >= 250) {
            _last_ui_tick = now;
            if (_screen == Screen::Clock) {
                refresh_clock_ui(now);
            } else if (_screen == Screen::Ringing) {
                refresh_ringing_ui(now);
            } else if (_screen == Screen::Settings) {
                refresh_settings_ui();
            }
        }
        view::update_home_indicator();
        view::update_status_bar();
    }
}

void AppAlarm::onClose()
{
    mclog::tagInfo(TAG, "on close");

    AlarmService::instance().setAppOpen(false);

    if (_head_pet_conn >= 0) {
        GetHAL().onHeadPetGesture.disconnect(_head_pet_conn);
        _head_pet_conn = -1;
    }
    if (_imu_conn >= 0) {
        GetHAL().onImuMotionEvent.disconnect(_imu_conn);
        _imu_conn = -1;
    }

    stop_audio();
    unmount_sd();

    if (_night_mode) {
        GetHAL().setBackLightBrightness(_saved_brightness, false);
        _night_mode = false;
    }
    GetHAL().showRgbColor(0, 0, 0);
    GetStackChan().motion().moveWithSpeed(deg_to_yaw(0), deg_to_pitch(Y_HOME_DEG), 500);

    {
        LvglLockGuard lock;
        view::destroy_home_indicator();
        view::destroy_status_bar();
        destroy_ui();
    }

    GetHAL().requestWarmReboot(AlarmService::instance().launcherPageIndex());
}

/* -------------------------------------------------------------------------- */
/*                               Alarm Flow                                   */
/* -------------------------------------------------------------------------- */

void AppAlarm::begin_ringing(int alarm_idx)
{
    _alarms = AlarmService::instance().getAlarms();
    _ring_alarm_idx = alarm_idx;
    if (alarm_idx >= 0 && static_cast<size_t>(alarm_idx) < _alarms.size()) {
        _ring_item = _alarms[static_cast<size_t>(alarm_idx)];
    } else {
        _ring_item = AlarmItem{};
    }

    // Wake screen to full brightness immediately
    if (_night_mode) {
        _night_mode = false;
    }
    GetHAL().setBackLightBrightness(100, false);

    _ring_start_ms  = GetHAL().millis();
    _ring_next_move = 0;
    _ring_move_step = 0;
    _last_ramp_vol  = -1;
    _head_pet_flag  = false;
    _shake_flag     = false;

    open_screen(Screen::Ringing);
    refresh_ringing_ui(_ring_start_ms);

    _target_vol = AlarmService::instance().getVolume();
    start_audio(_ring_item.tone, _ring_item.mp3_path, false);
}

void AppAlarm::action_snooze()
{
    stop_audio();
    AlarmService::instance().scheduleSnooze(_ring_alarm_idx);
    GetHAL().setBackLightBrightness(_saved_brightness, false);
    GetHAL().showRgbColor(0, 28, 48);
    GetStackChan().motion().moveWithSpeed(deg_to_yaw(0), deg_to_pitch(Y_HOME_DEG), 500);
    open_screen(Screen::Clock);
    refresh_clock_ui(GetHAL().millis());
}

void AppAlarm::action_dismiss()
{
    stop_audio();
    AlarmService::instance().clearSnooze();
    _alarms = AlarmService::instance().getAlarms();
    GetHAL().setBackLightBrightness(_saved_brightness, false);
    GetHAL().showRgbColor(0, 22, 26);
    GetStackChan().motion().moveWithSpeed(deg_to_yaw(0), deg_to_pitch(Y_HOME_DEG), 500);
    open_screen(Screen::Clock);
    refresh_clock_ui(GetHAL().millis());
}

void AppAlarm::toggle_night_mode()
{
    set_night_mode(!_night_mode);
}

void AppAlarm::set_night_mode(bool on)
{
    _night_mode = on;
    if (on) {
        _saved_brightness = std::max<uint8_t>(GetHAL().getBackLightBrightness(), 50);
        GetHAL().setBackLightBrightness(10, false);
        GetHAL().showRgbColor(0, 0, 0);
    } else {
        GetHAL().setBackLightBrightness(_saved_brightness, false);
        GetHAL().showRgbColor(0, 22, 26);
    }
    if (_btn_night && _lbl_night) {
        lv_obj_set_style_bg_color(_btn_night, lv_color_hex(on ? COL_BLUE : COL_CARD), 0);
        lv_label_set_text(_lbl_night, on ? "WAKE" : "NIGHT");
    }
}

void AppAlarm::open_screen(Screen scr)
{
    if (_screen == Screen::Edit && scr != Screen::SdPicker && scr != Screen::Edit) {
        stop_audio();
    }
    _screen = scr;

    auto set_vis = [](lv_obj_t* obj, bool show) {
        if (!obj) return;
        if (show) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    };

    set_vis(_scr_clock, scr == Screen::Clock);
    set_vis(_scr_list, scr == Screen::List);
    set_vis(_scr_edit, scr == Screen::Edit);
    set_vis(_scr_sd, scr == Screen::SdPicker);
    set_vis(_scr_settings, scr == Screen::Settings);
    set_vis(_scr_ring, scr == Screen::Ringing);

    if (scr == Screen::List) {
        _alarms = AlarmService::instance().getAlarms();
        rebuild_list_rows();
    } else if (scr == Screen::Edit) {
        refresh_editor_ui();
    } else if (scr == Screen::Settings) {
        refresh_settings_ui();
    } else if (scr == Screen::Clock) {
        refresh_clock_ui(GetHAL().millis());
    }
}

void AppAlarm::open_editor(int idx, bool is_new)
{
    _edit_is_new = is_new;
    _edit_idx    = idx;
    if (is_new) {
        _edit_item           = AlarmItem{};
        _edit_item.enabled   = true;
        time_t now           = std::time(nullptr);
        struct tm lt         = {};
        localtime_r(&now, &lt);
        if (lt.tm_year + 1900 >= 2024) {
            int m            = (lt.tm_hour * 60 + lt.tm_min + 5) % 1440;
            _edit_item.hour  = static_cast<uint8_t>(m / 60);
            _edit_item.minute = static_cast<uint8_t>(m % 60);
        }
    } else if (idx >= 0 && static_cast<size_t>(idx) < _alarms.size()) {
        _edit_item = _alarms[static_cast<size_t>(idx)];
    }
    open_screen(Screen::Edit);
}

void AppAlarm::save_editor()
{
    stop_audio();
    _edit_item.enabled = true;
    if (_edit_is_new) {
        if (_alarms.size() < AlarmService::MAX_ALARMS) {
            _alarms.push_back(_edit_item);
        }
    } else if (_edit_idx >= 0 && static_cast<size_t>(_edit_idx) < _alarms.size()) {
        _alarms[static_cast<size_t>(_edit_idx)] = _edit_item;
    }
    AlarmService::instance().setAlarms(_alarms);
    open_screen(Screen::List);
}

void AppAlarm::delete_editor()
{
    stop_audio();
    if (!_edit_is_new && _edit_idx >= 0 && static_cast<size_t>(_edit_idx) < _alarms.size()) {
        _alarms.erase(_alarms.begin() + _edit_idx);
        AlarmService::instance().setAlarms(_alarms);
    }
    open_screen(Screen::List);
}

void AppAlarm::update_ringing_motion(uint32_t now_ms)
{
    // Progressive volume ramp: 35% -> target volume over 12 seconds
    uint32_t elapsed = now_ms - _ring_start_ms;
    int tgt          = std::clamp(_target_vol.load(), 35, 100);
    int ramp_vol     = 35 + static_cast<int>((std::min<uint32_t>(elapsed, RAMP_DURATION_MS) * (tgt - 35)) / RAMP_DURATION_MS);
    if (std::abs(ramp_vol - _last_ramp_vol) >= 3) {
        _last_ramp_vol = ramp_vol;
        GetHAL().setSpeakerVolume(static_cast<uint8_t>(ramp_vol), false);
    }

    // Playful wake-up head motion & pulsing amber/cyan RGB LEDs
    if (now_ms >= _ring_next_move) {
        _ring_next_move = now_ms + 420;
        static const int X_SEQ[4] = {-18, 18, -12, 12};
        static const int Y_SEQ[4] = {30, 52, 34, 48};
        int s                     = _ring_move_step % 4;
        GetStackChan().motion().moveWithSpeed(deg_to_yaw(X_SEQ[s]), deg_to_pitch(Y_SEQ[s]), 650);
        if (s % 2 == 0) {
            GetHAL().showRgbColor(96, 52, 0);
        } else {
            GetHAL().showRgbColor(0, 72, 64);
        }
        ++_ring_move_step;
    }
}

/* -------------------------------------------------------------------------- */
/*                           Audio Engine & SD                                */
/* -------------------------------------------------------------------------- */

void AppAlarm::start_audio(uint8_t tone, const std::string& mp3_path, bool preview_only)
{
    stop_audio();
    _audio_tone     = tone;
    _audio_mp3_path = mp3_path;
    _audio_preview  = preview_only;
    _audio_abort    = false;
    _audio_running  = true;
    _audio_exited   = false;

    // Internal SRAM stack (safe for SD FATFS + minimp3 + AudioCodec)
    if (xTaskCreatePinnedToCore(audio_task_entry, "alarm_aud", 8192, this, 4, nullptr, 1) != pdPASS) {
        _audio_running = false;
        _audio_exited  = true;
    }
}

void AppAlarm::stop_audio()
{
    if (!_audio_running.load() && _audio_exited.load()) {
        return;
    }
    _audio_abort   = true;
    _audio_running = false;
    for (int i = 0; i < 80 && !_audio_exited.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    _audio_rms = 0;
}

void AppAlarm::audio_task_entry(void* arg)
{
    static_cast<AppAlarm*>(arg)->audio_task_loop();
    vTaskDelete(nullptr);
}

void AppAlarm::audio_task_loop()
{
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        _audio_running = false;
        _audio_exited  = true;
        return;
    }

    if (codec->input_enabled()) {
        codec->EnableInput(false);
    }
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    int init_vol = _audio_preview ? std::clamp(_target_vol.load(), 35, 100) : 35;
    GetHAL().setSpeakerVolume(static_cast<uint8_t>(init_vol), false);

    if (_audio_tone == AlarmService::TONE_SD) {
        if (!play_sd_mp3_loop(_audio_mp3_path, _audio_preview) && !_audio_abort.load()) {
            // Guaranteed fallback so an unreadable SD card never silences a wake-up alarm
            play_synth_loop(0, _audio_preview);
        }
    } else {
        play_synth_loop(_audio_tone, _audio_preview);
    }

    if (codec->output_enabled()) {
        codec->EnableOutput(false);
    }
    _audio_rms     = 0;
    _audio_running = false;
    _audio_exited  = true;
}

void AppAlarm::play_synth_loop(uint8_t tone, bool preview_only)
{
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) return;

    struct Note {
        float freq;
        uint16_t dur_ms;
    };

    // 4 musical, distinct wake-up patterns
    static const Note TONE_CHIME[] = {
        {523.25f, 180}, {659.25f, 180}, {783.99f, 180}, {1046.50f, 340}, {0.0f, 320},
        {587.33f, 180}, {783.99f, 180}, {987.77f, 180}, {1174.66f, 360}, {0.0f, 520},
    };
    static const Note TONE_CLASSIC[] = {
        {880.0f, 120}, {0.0f, 80}, {880.0f, 120}, {0.0f, 80},
        {880.0f, 120}, {0.0f, 80}, {880.0f, 120}, {0.0f, 520},
    };
    static const Note TONE_RADAR[] = {
        {660.0f, 140}, {990.0f, 140}, {660.0f, 140}, {990.0f, 200}, {0.0f, 450},
    };
    static const Note TONE_ENERGETIC[] = {
        {523.25f, 140}, {659.25f, 140}, {783.99f, 140}, {1046.50f, 180},
        {880.00f, 140}, {987.77f, 140}, {1046.50f, 280}, {0.0f, 360},
    };

    const Note* seq = TONE_CHIME;
    size_t seq_len  = sizeof(TONE_CHIME) / sizeof(Note);
    if (tone == 1) {
        seq     = TONE_CLASSIC;
        seq_len = sizeof(TONE_CLASSIC) / sizeof(Note);
    } else if (tone == 2) {
        seq     = TONE_RADAR;
        seq_len = sizeof(TONE_RADAR) / sizeof(Note);
    } else if (tone == 3) {
        seq     = TONE_ENERGETIC;
        seq_len = sizeof(TONE_ENERGETIC) / sizeof(Note);
    }

    constexpr uint32_t SR         = AUDIO_OUTPUT_SAMPLE_RATE;  // 24000 Hz
    constexpr size_t CHUNK_FRAMES = 480;                       // 20 ms
    std::vector<int16_t> pcm(CHUNK_FRAMES);

    int loops_left = preview_only ? 1 : 999999;
    while (_audio_running.load() && !_audio_abort.load() && loops_left-- > 0) {
        for (size_t ni = 0; ni < seq_len && _audio_running.load() && !_audio_abort.load(); ++ni) {
            const float freq       = seq[ni].freq;
            const size_t total_smp = (static_cast<size_t>(seq[ni].dur_ms) * SR) / 1000;
            const size_t env_smp   = std::min<size_t>(total_smp / 5, 360);
            float phase            = 0.0f;
            float phase2           = 0.0f;
            const float dphi       = (freq > 0.0f) ? (TWO_PI_F * freq / static_cast<float>(SR)) : 0.0f;
            const float dphi2      = dphi * 2.0f;

            size_t done = 0;
            while (done < total_smp && _audio_running.load() && !_audio_abort.load()) {
                size_t n = std::min<size_t>(CHUNK_FRAMES, total_smp - done);
                pcm.resize(n);
                uint64_t sq = 0;
                for (size_t i = 0; i < n; ++i) {
                    size_t idx = done + i;
                    float env  = 1.0f;
                    if (idx < env_smp) {
                        env = static_cast<float>(idx) / static_cast<float>(env_smp);
                    } else if (idx + env_smp > total_smp) {
                        env = static_cast<float>(total_smp - idx) / static_cast<float>(env_smp);
                    }
                    int16_t sample = 0;
                    if (freq > 0.0f) {
                        // Gentle harmonic blend for pleasant speaker tone
                        float w = std::sin(phase) * 0.78f + std::sin(phase2) * 0.22f;
                        sample  = static_cast<int16_t>(w * env * 18500.0f);
                        phase += dphi;
                        phase2 += dphi2;
                        if (phase >= TWO_PI_F) phase -= TWO_PI_F;
                        if (phase2 >= TWO_PI_F) phase2 -= TWO_PI_F;
                    }
                    pcm[i] = sample;
                    sq += static_cast<uint64_t>(static_cast<int32_t>(sample) * static_cast<int32_t>(sample));
                }
                _audio_rms.store(static_cast<int>(std::sqrt(static_cast<double>(sq) / n)));
                codec->OutputData(pcm);
                done += n;
            }
        }
    }
}

bool AppAlarm::play_sd_mp3_loop(const std::string& path, bool preview_only)
{
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !mount_sd()) {
        return false;
    }

    std::string target = path;
    if (target.empty()) {
        scan_sd_mp3s();
        if (!_sd_mp3_paths.empty()) {
            target = _sd_mp3_paths[0];
        }
    }
    if (target.empty()) {
        return false;
    }

    auto* mp3d = new (std::nothrow) mp3dec_t();
    if (!mp3d) {
        return false;
    }
    mp3dec_init(mp3d);

    constexpr size_t MP3_BUF_CAP = 4096;
    auto* mp3_buf = static_cast<uint8_t*>(heap_caps_malloc(MP3_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* pcm_raw = static_cast<int16_t*>(heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (!mp3_buf || !pcm_raw) {
        if (mp3_buf) heap_caps_free(mp3_buf);
        if (pcm_raw) heap_caps_free(pcm_raw);
        delete mp3d;
        return false;
    }

    bool played_any   = false;
    uint32_t start_ms = GetHAL().millis();
    std::vector<int16_t> pcm_out;
    pcm_out.reserve(2048);

    while (_audio_running.load() && !_audio_abort.load()) {
        FILE* fp = std::fopen(target.c_str(), "rb");
        if (!fp) {
            break;
        }
        mp3dec_init(mp3d);
        size_t buffered = 0;

        while (_audio_running.load() && !_audio_abort.load()) {
            if (preview_only && (GetHAL().millis() - start_ms) >= 4000) {
                break;
            }
            if (buffered < 2048 && !std::feof(fp)) {
                size_t rd = std::fread(mp3_buf + buffered, 1, MP3_BUF_CAP - buffered, fp);
                buffered += rd;
            }
            if (buffered == 0) {
                break;
            }

            mp3dec_frame_info_t frame_info = {};
            int samples = mp3dec_decode_frame(mp3d, mp3_buf, static_cast<int>(buffered), pcm_raw, &frame_info);

            if (frame_info.frame_bytes > 0) {
                size_t consumed = static_cast<size_t>(frame_info.frame_bytes);
                if (consumed < buffered) {
                    std::memmove(mp3_buf, mp3_buf + consumed, buffered - consumed);
                    buffered -= consumed;
                } else {
                    buffered = 0;
                }
            } else {
                if (std::feof(fp)) {
                    break;
                }
                if (buffered > 0) {
                    std::memmove(mp3_buf, mp3_buf + 1, --buffered);
                }
                continue;
            }

            if (samples > 0 && frame_info.hz > 0 && frame_info.channels > 0) {
                played_any = true;
                int ch     = frame_info.channels;
                int out_n  = static_cast<int>((static_cast<uint64_t>(samples) * AUDIO_OUTPUT_SAMPLE_RATE) / frame_info.hz);
                if (out_n > 0) {
                    pcm_out.resize(static_cast<size_t>(out_n));
                    uint64_t sq = 0;
                    for (int i = 0; i < out_n; ++i) {
                        size_t si = std::min<size_t>((static_cast<uint64_t>(i) * frame_info.hz) / AUDIO_OUTPUT_SAMPLE_RATE, static_cast<size_t>(samples - 1));
                        int32_t s = (ch >= 2) ? ((int32_t)pcm_raw[si * ch] + pcm_raw[si * ch + 1]) / 2 : pcm_raw[si];
                        pcm_out[static_cast<size_t>(i)] = static_cast<int16_t>(s);
                        sq += static_cast<uint64_t>(s * s);
                    }
                    _audio_rms.store(static_cast<int>(std::sqrt(static_cast<double>(sq) / out_n)));
                    codec->OutputData(pcm_out);
                }
            }
        }
        std::fclose(fp);
        if (preview_only) {
            break;
        }
    }

    heap_caps_free(mp3_buf);
    heap_caps_free(pcm_raw);
    delete mp3d;
    return played_any;
}

bool AppAlarm::mount_sd()
{
    struct stat st = {};
    if (stat(SD_MOUNT_POINT, &st) == 0) {
        _sd_mounted = true;
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed           = false;
    mount_cfg.max_files                        = 4;
    mount_cfg.allocation_unit_size             = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SPI3_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs               = GPIO_NUM_4;
    slot_cfg.host_id               = SPI3_HOST;

    sdmmc_card_t* card = nullptr;
    if (esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &card) != ESP_OK) {
        _sd_mounted     = false;
        _sd_card_handle = nullptr;
        return false;
    }
    _sd_card_handle = card;
    _sd_mounted     = true;
    return true;
}

void AppAlarm::unmount_sd()
{
    if (_sd_card_handle) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, static_cast<sdmmc_card_t*>(_sd_card_handle));
        _sd_card_handle = nullptr;
    }
    _sd_mounted = false;
}

void AppAlarm::scan_sd_mp3s()
{
    _sd_mp3_paths.clear();
    if (!mount_sd()) {
        return;
    }

    std::vector<std::pair<std::string, int>> stack;
    stack.push_back({SD_MOUNT_POINT, 0});

    while (!stack.empty() && _sd_mp3_paths.size() < 60) {
        auto [dir_path, depth] = stack.back();
        stack.pop_back();

        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr && _sd_mp3_paths.size() < 60) {
            const char* dname = ent->d_name;
            if (!dname || dname[0] == '\0' || dname[0] == '.') continue;

            std::string name(dname);
            std::string full_path = dir_path + "/" + name;

            if (is_mp3_filename(name)) {
                _sd_mp3_paths.push_back(full_path);
                continue;
            }

            bool is_dir = (ent->d_type == DT_DIR);
            if (ent->d_type == DT_UNKNOWN) {
                struct stat st = {};
                if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    is_dir = true;
                }
            }

            if (is_dir && depth < 2) {
                stack.push_back({full_path, depth + 1});
            }
        }
        closedir(dir);
    }
    std::sort(_sd_mp3_paths.begin(), _sd_mp3_paths.end());
}

/* -------------------------------------------------------------------------- */
/*                               NTP Sync                                     */
/* -------------------------------------------------------------------------- */

void AppAlarm::start_ntp_sync()
{
    if (_ntp_busy.exchange(true)) {
        return;
    }
    set_sync_text("Connecting Wi-Fi...");
    xTaskCreatePinnedToCore(ntp_task_entry, "alarm_ntp", 4096, this, 3, nullptr, 0);
}

void AppAlarm::ntp_task_entry(void* arg)
{
    static_cast<AppAlarm*>(arg)->ntp_task_loop();
    vTaskDelete(nullptr);
}

void AppAlarm::ntp_task_loop()
{
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsConnected()) {
        GetHAL().startNetwork([](std::string_view) {});
        for (int i = 0; i < 40 && !wifi.IsConnected(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    if (!wifi.IsConnected()) {
        set_sync_text("No Wi-Fi (check SETUP)");
        _ntp_busy = false;
        return;
    }
    set_sync_text("Syncing NTP time...");
    GetHAL().startSntp();
    vTaskDelay(pdMS_TO_TICKS(2500));
    GetHAL().syncSystemTimeToRtc();
    set_sync_text("Time synced via NTP");
    _ntp_busy = false;
}

void AppAlarm::set_sync_text(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(_sync_mtx);
    _sync_status = msg;
}

/* -------------------------------------------------------------------------- */
/*                                LVGL UI                                     */
/* -------------------------------------------------------------------------- */

void AppAlarm::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    _root = lv_obj_create(scr);
    lv_obj_set_size(_root, W, H);
    lv_obj_set_pos(_root, 0, 0);
    style_box(_root, COL_BG, 0);

    auto make_btn = [&](lv_obj_t* parent, int x, int y, int w, int h, const char* txt, uint32_t bg, uint32_t fg) {
        lv_obj_t* b = lv_button_create(parent);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_size(b, w, h);
        style_box(b, bg, 10);
        lv_obj_t* l = lv_label_create(b);
        lv_obj_center(l);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
        lv_label_set_text(l, txt);
        return b;
    };

    /* ======================== 1. CLOCK SCREEN ======================== */
    _scr_clock = lv_obj_create(_root);
    lv_obj_set_size(_scr_clock, W, H);
    lv_obj_set_pos(_scr_clock, 0, 0);
    style_box(_scr_clock, COL_BG, 0);

    _lbl_hdr = lv_label_create(_scr_clock);
    lv_obj_set_pos(_lbl_hdr, 10, 5);
    lv_obj_set_size(_lbl_hdr, 236, 18);
    lv_obj_set_style_text_font(_lbl_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_hdr, lv_color_hex(COL_AMBER), 0);
    lv_label_set_text(_lbl_hdr, LV_SYMBOL_BELL " ALARM CLOCK");

    _lbl_batt = lv_label_create(_scr_clock);
    lv_obj_set_pos(_lbl_batt, 250, 6);
    lv_obj_set_width(_lbl_batt, 62);
    lv_obj_set_style_text_align(_lbl_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(_lbl_batt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_batt, lv_color_hex(COL_DIM), 0);

    // Unified center stage with clock + centered avatar + next alarm badge
    _stage = lv_obj_create(_scr_clock);
    lv_obj_set_pos(_stage, 8, 26);
    lv_obj_set_size(_stage, 304, 152);
    style_box(_stage, COL_CARD, 14);
    lv_obj_set_style_border_width(_stage, 1, 0);
    lv_obj_set_style_border_color(_stage, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_add_flag(_stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        _stage,
        [](lv_event_t* e) {
            auto* self = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            if (self->_night_mode) {
                self->set_night_mode(false);
            }
        },
        LV_EVENT_CLICKED, this);

    _lbl_time = lv_label_create(_stage);
    lv_obj_set_pos(_lbl_time, 14, 28);
    lv_obj_set_style_text_font(_lbl_time, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lbl_time, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(_lbl_time, "--:--");

    _lbl_sec = lv_label_create(_stage);
    lv_obj_set_pos(_lbl_sec, 90, 36);
    lv_obj_set_style_text_font(_lbl_sec, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_sec, lv_color_hex(COL_TEAL), 0);
    lv_label_set_text(_lbl_sec, ":00");

    _lbl_date = lv_label_create(_stage);
    lv_obj_set_pos(_lbl_date, 14, 62);
    lv_obj_set_style_text_font(_lbl_date, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_date, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(_lbl_date, "---");

    // Center-right avatar: supports both Classic StackChan vector face and Naruto sprite
    auto on_avatar_click = [](lv_event_t* e) {
        auto* self = static_cast<AppAlarm*>(lv_event_get_user_data(e));
        bool currently_naruto = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto);
        auto next_style = currently_naruto ? stackchan::avatar::AvatarStyle::Classic : stackchan::avatar::AvatarStyle::Naruto;
        stackchan::avatar::setAvatarStyle(next_style);
        self->refresh_avatar_anim(GetHAL().millis());
    };

    // Classic expressive StackChan avatar card
    _face_card = lv_obj_create(_stage);
    lv_obj_set_size(_face_card, 118, 86);
    lv_obj_align(_face_card, LV_ALIGN_RIGHT_MID, -14, -10);
    style_box(_face_card, COL_BG, 14);
    lv_obj_set_style_border_width(_face_card, 2, 0);
    lv_obj_set_style_border_color(_face_card, lv_color_hex(COL_TEAL_DK), 0);
    lv_obj_add_flag(_face_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_face_card, on_avatar_click, LV_EVENT_CLICKED, this);

    _face_eye_l = lv_obj_create(_face_card);
    lv_obj_set_size(_face_eye_l, 16, 16);
    lv_obj_align(_face_eye_l, LV_ALIGN_CENTER, -24, -8);
    style_box(_face_eye_l, COL_WHITE, LV_RADIUS_CIRCLE);

    _face_eye_r = lv_obj_create(_face_card);
    lv_obj_set_size(_face_eye_r, 16, 16);
    lv_obj_align(_face_eye_r, LV_ALIGN_CENTER, 24, -8);
    style_box(_face_eye_r, COL_WHITE, LV_RADIUS_CIRCLE);

    _face_mouth = lv_obj_create(_face_card);
    lv_obj_set_size(_face_mouth, 34, 6);
    lv_obj_align(_face_mouth, LV_ALIGN_CENTER, 0, 16);
    style_box(_face_mouth, COL_WHITE, 4);

    // Naruto avatar image
    _avatar_img = lv_image_create(_stage);
    if (_img_naruto_calm.data != nullptr) {
        lv_image_set_src(_avatar_img, &_img_naruto_calm);
        uint16_t scale = (_img_naruto_calm.header.w > 130) ? 150 : 256;
        lv_image_set_scale(_avatar_img, scale);
    }
    lv_obj_align(_avatar_img, LV_ALIGN_RIGHT_MID, -14, -10);
    lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_avatar_img, on_avatar_click, LV_EVENT_CLICKED, this);

    _lbl_next = lv_label_create(_stage);
    lv_obj_align(_lbl_next, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_width(_lbl_next, 286);
    lv_obj_set_style_text_align(_lbl_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_lbl_next, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_next, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_next, lv_color_hex(COL_AMBER), 0);
    lv_label_set_text(_lbl_next, "No active alarms");

    // Bottom 4-button bar: ALARMS | NIGHT | SET | EXIT
    lv_obj_t* btn_alarms = make_btn(_scr_clock, 8, 186, 118, 44, LV_SYMBOL_BELL " ALARMS", COL_TEAL_DK, COL_WHITE);
    lv_obj_add_event_cb(
        btn_alarms,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->open_screen(Screen::List);
        },
        LV_EVENT_CLICKED, this);

    _btn_night = make_btn(_scr_clock, 130, 186, 66, 44, "NIGHT", COL_CARD, COL_TEXT);
    _lbl_night = lv_obj_get_child(_btn_night, 0);
    lv_obj_add_event_cb(
        _btn_night,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->toggle_night_mode();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_set = make_btn(_scr_clock, 200, 186, 60, 44, LV_SYMBOL_SETTINGS " SET", COL_CARD, COL_TEXT);
    lv_obj_add_event_cb(
        btn_set,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->open_screen(Screen::Settings);
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_exit = make_btn(_scr_clock, 264, 186, 48, 44, "EXIT", COL_CARD_HI, COL_YELLOW);
    lv_obj_add_event_cb(
        btn_exit,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->close();
        },
        LV_EVENT_CLICKED, this);

    /* ======================== 2. LIST SCREEN ========================= */
    _scr_list = lv_obj_create(_root);
    lv_obj_set_size(_scr_list, W, H);
    lv_obj_set_pos(_scr_list, 0, 0);
    style_box(_scr_list, COL_BG, 0);
    lv_obj_add_flag(_scr_list, LV_OBJ_FLAG_HIDDEN);

    _lbl_list_title = lv_label_create(_scr_list);
    lv_obj_set_pos(_lbl_list_title, 10, 10);
    lv_obj_set_style_text_font(_lbl_list_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_list_title, lv_color_hex(COL_AMBER), 0);
    lv_label_set_text(_lbl_list_title, "ALARMS");

    _btn_add_alarm = make_btn(_scr_list, 168, 5, 68, 30, LV_SYMBOL_PLUS " NEW", COL_TEAL_DK, COL_WHITE);
    lv_obj_add_event_cb(
        _btn_add_alarm,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->open_editor(-1, true);
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_list_back = make_btn(_scr_list, 242, 5, 70, 30, LV_SYMBOL_LEFT " BACK", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_list_back,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->open_screen(Screen::Clock);
        },
        LV_EVENT_CLICKED, this);

    _list_box = lv_obj_create(_scr_list);
    lv_obj_set_pos(_list_box, 8, 40);
    lv_obj_set_size(_list_box, 304, 194);
    lv_obj_set_style_bg_opa(_list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_list_box, 0, 0);
    lv_obj_set_style_pad_all(_list_box, 0, 0);
    lv_obj_set_style_pad_row(_list_box, 6, 0);
    lv_obj_set_flex_flow(_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_list_box, LV_DIR_VER);

    /* ======================== 3. EDIT SCREEN ========================= */
    _scr_edit = lv_obj_create(_root);
    lv_obj_set_size(_scr_edit, W, H);
    lv_obj_set_pos(_scr_edit, 0, 0);
    style_box(_scr_edit, COL_BG, 0);
    lv_obj_add_flag(_scr_edit, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* lbl_ed = lv_label_create(_scr_edit);
    lv_obj_set_pos(lbl_ed, 10, 10);
    lv_obj_set_style_text_font(lbl_ed, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ed, lv_color_hex(COL_TEAL), 0);
    lv_label_set_text(lbl_ed, "EDIT ALARM");

    _btn_edit_del = make_btn(_scr_edit, 174, 5, 62, 30, "DEL", COL_RED, COL_WHITE);
    lv_obj_add_event_cb(
        _btn_edit_del,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->delete_editor();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_ed_save = make_btn(_scr_edit, 242, 5, 70, 30, LV_SYMBOL_OK " SAVE", COL_GREEN, COL_BG);
    lv_obj_add_event_cb(
        btn_ed_save,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->save_editor();
        },
        LV_EVENT_CLICKED, this);

    // Row 1: Hour & Minute tactile steppers
    lv_obj_t* time_card = lv_obj_create(_scr_edit);
    lv_obj_set_pos(time_card, 8, 40);
    lv_obj_set_size(time_card, 304, 52);
    style_box(time_card, COL_CARD, 10);

    auto wire_time_btn = [&](lv_obj_t* btn, int dh, int dm) {
        int code = (dh + 2) * 10 + (dm + 2);
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(code)));
        auto cb = [](lv_event_t* e) {
            auto* self         = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            lv_obj_t* target   = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
            int c              = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
            int d_h            = (c / 10) - 2;
            int d_m            = (c % 10) - 2;
            self->_edit_item.hour   = static_cast<uint8_t>((self->_edit_item.hour + 24 + d_h) % 24);
            self->_edit_item.minute = static_cast<uint8_t>((self->_edit_item.minute + 60 + d_m) % 60);
            self->refresh_editor_ui();
        };
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_LONG_PRESSED_REPEAT, this);
    };

    wire_time_btn(make_btn(time_card, 8, 7, 40, 38, "-", COL_CARD_HI, COL_WHITE), -1, 0);
    _lbl_edit_hour = lv_label_create(time_card);
    lv_obj_set_pos(_lbl_edit_hour, 50, 12);
    lv_obj_set_width(_lbl_edit_hour, 44);
    lv_obj_set_style_text_align(_lbl_edit_hour, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_edit_hour, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lbl_edit_hour, lv_color_hex(COL_WHITE), 0);
    wire_time_btn(make_btn(time_card, 96, 7, 40, 38, "+", COL_CARD_HI, COL_WHITE), +1, 0);

    lv_obj_t* lbl_colon = lv_label_create(time_card);
    lv_obj_set_pos(lbl_colon, 144, 11);
    lv_obj_set_style_text_font(lbl_colon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_colon, lv_color_hex(COL_AMBER), 0);
    lv_label_set_text(lbl_colon, ":");

    wire_time_btn(make_btn(time_card, 166, 7, 40, 38, "-", COL_CARD_HI, COL_WHITE), 0, -1);
    _lbl_edit_min = lv_label_create(time_card);
    lv_obj_set_pos(_lbl_edit_min, 208, 12);
    lv_obj_set_width(_lbl_edit_min, 44);
    lv_obj_set_style_text_align(_lbl_edit_min, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_edit_min, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lbl_edit_min, lv_color_hex(COL_WHITE), 0);
    wire_time_btn(make_btn(time_card, 254, 7, 40, 38, "+", COL_CARD_HI, COL_WHITE), 0, +1);

    // Row 2: 7 Weekday toggle pills (S M T W T F S)
    static const char* const DAY_CH[7] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int i = 0; i < 7; ++i) {
        _btn_edit_day[i] = make_btn(_scr_edit, 8 + i * 44, 98, 40, 36, DAY_CH[i], COL_CARD, COL_WHITE);
        lv_obj_set_user_data(_btn_edit_day[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            _btn_edit_day[i],
            [](lv_event_t* e) {
                auto* self       = static_cast<AppAlarm*>(lv_event_get_user_data(e));
                lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                int d            = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                self->_edit_item.days_mask ^= static_cast<uint8_t>(1u << d);
                self->refresh_editor_ui();
            },
            LV_EVENT_CLICKED, this);
    }

    // Row 3: Label & Tone cycle pills
    lv_obj_t* btn_lbl = make_btn(_scr_edit, 8, 140, 148, 38, "Wake Up", COL_CARD, COL_TEXT);
    _lbl_edit_label   = lv_obj_get_child(btn_lbl, 0);
    lv_obj_add_event_cb(
        btn_lbl,
        [](lv_event_t* e) {
            auto* self                = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            self->_edit_item.label_idx = static_cast<uint8_t>((self->_edit_item.label_idx + 1) % 6);
            self->refresh_editor_ui();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_tone = make_btn(_scr_edit, 164, 140, 148, 38, "Tone: Chime", COL_CARD, COL_AMBER);
    _lbl_edit_tone     = lv_obj_get_child(btn_tone, 0);
    lv_obj_add_event_cb(
        btn_tone,
        [](lv_event_t* e) {
            auto* self           = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            self->_edit_item.tone = static_cast<uint8_t>((self->_edit_item.tone + 1) % 5);
            self->refresh_editor_ui();
        },
        LV_EVENT_CLICKED, this);

    // Row 4: TEST | SD MP3 | CANCEL
    lv_obj_t* btn_test = make_btn(_scr_edit, 8, 186, 98, 44, LV_SYMBOL_AUDIO " TEST", COL_BLUE, COL_WHITE);
    lv_obj_add_event_cb(
        btn_test,
        [](lv_event_t* e) {
            auto* self = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            if (self->_audio_running.load()) {
                self->stop_audio();
            } else {
                self->start_audio(self->_edit_item.tone, self->_edit_item.mp3_path, true);
            }
        },
        LV_EVENT_CLICKED, this);

    _btn_edit_mp3 = make_btn(_scr_edit, 112, 186, 102, 44, LV_SYMBOL_SD_CARD " SD MP3", COL_TEAL_DK, COL_WHITE);
    lv_obj_add_event_cb(
        _btn_edit_mp3,
        [](lv_event_t* e) {
            auto* self = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            self->stop_audio();
            self->scan_sd_mp3s();
            self->rebuild_sd_rows();
            self->open_screen(Screen::SdPicker);
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_cancel = make_btn(_scr_edit, 220, 186, 92, 44, "CANCEL", COL_CARD_HI, COL_TEXT);
    lv_obj_add_event_cb(
        btn_cancel,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->open_screen(Screen::List);
        },
        LV_EVENT_CLICKED, this);

    /* ======================== 4. SD PICKER =========================== */
    _scr_sd = lv_obj_create(_root);
    lv_obj_set_size(_scr_sd, W, H);
    lv_obj_set_pos(_scr_sd, 0, 0);
    style_box(_scr_sd, COL_BG, 0);
    lv_obj_add_flag(_scr_sd, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* lbl_sd = lv_label_create(_scr_sd);
    lv_obj_set_pos(lbl_sd, 10, 10);
    lv_obj_set_style_text_font(lbl_sd, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_sd, lv_color_hex(COL_TEAL), 0);
    lv_label_set_text(lbl_sd, LV_SYMBOL_SD_CARD " SELECT MP3");

    lv_obj_t* btn_sd_back = make_btn(_scr_sd, 242, 5, 70, 30, LV_SYMBOL_LEFT " BACK", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_sd_back,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->open_screen(Screen::Edit);
        },
        LV_EVENT_CLICKED, this);

    _sd_list_box = lv_obj_create(_scr_sd);
    lv_obj_set_pos(_sd_list_box, 8, 40);
    lv_obj_set_size(_sd_list_box, 304, 194);
    lv_obj_set_style_bg_opa(_sd_list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_sd_list_box, 0, 0);
    lv_obj_set_style_pad_all(_sd_list_box, 0, 0);
    lv_obj_set_style_pad_row(_sd_list_box, 5, 0);
    lv_obj_set_flex_flow(_sd_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_sd_list_box, LV_DIR_VER);

    /* ======================== 5. SETTINGS SCREEN ===================== */
    _scr_settings = lv_obj_create(_root);
    lv_obj_set_size(_scr_settings, W, H);
    lv_obj_set_pos(_scr_settings, 0, 0);
    style_box(_scr_settings, COL_BG, 0);
    lv_obj_add_flag(_scr_settings, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* lbl_st = lv_label_create(_scr_settings);
    lv_obj_set_pos(lbl_st, 10, 10);
    lv_obj_set_style_text_font(lbl_st, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_st, lv_color_hex(COL_TEAL), 0);
    lv_label_set_text(lbl_st, LV_SYMBOL_SETTINGS " ALARM SETTINGS");

    lv_obj_t* btn_st_back = make_btn(_scr_settings, 242, 5, 70, 30, LV_SYMBOL_LEFT " BACK", COL_TEAL_DK, COL_WHITE);
    lv_obj_add_event_cb(
        btn_st_back,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->open_screen(Screen::Clock);
        },
        LV_EVENT_CLICKED, this);

    auto make_set_row = [&](int y, const char* title) {
        lv_obj_t* c = lv_obj_create(_scr_settings);
        lv_obj_set_pos(c, 8, y);
        lv_obj_set_size(c, 304, 52);
        style_box(c, COL_CARD, 10);
        lv_obj_t* l = lv_label_create(c);
        lv_obj_set_pos(l, 12, 18);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
        lv_label_set_text(l, title);
        return c;
    };

    // Snooze duration
    lv_obj_t* row_sn = make_set_row(42, "SNOOZE");
    lv_obj_t* btn_sm = make_btn(row_sn, 118, 7, 46, 38, "-", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_sm,
        [](lv_event_t* e) {
            auto* self = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            AlarmService::instance().setSnoozeMin(AlarmService::instance().getSnoozeMin() - 1);
            self->refresh_settings_ui();
        },
        LV_EVENT_CLICKED, this);
    _lbl_set_snooze = lv_label_create(row_sn);
    lv_obj_set_pos(_lbl_set_snooze, 166, 17);
    lv_obj_set_width(_lbl_set_snooze, 76);
    lv_obj_set_style_text_align(_lbl_set_snooze, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_set_snooze, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_set_snooze, lv_color_hex(COL_WHITE), 0);
    lv_obj_t* btn_sp = make_btn(row_sn, 246, 7, 46, 38, "+", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_sp,
        [](lv_event_t* e) {
            auto* self = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            AlarmService::instance().setSnoozeMin(AlarmService::instance().getSnoozeMin() + 1);
            self->refresh_settings_ui();
        },
        LV_EVENT_CLICKED, this);

    // Alarm max volume
    lv_obj_t* row_vl = make_set_row(100, "VOLUME");
    lv_obj_t* btn_vm = make_btn(row_vl, 118, 7, 46, 38, "-", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_vm,
        [](lv_event_t* e) {
            auto* self = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            int v      = AlarmService::instance().getVolume() - 10;
            AlarmService::instance().setVolume(v);
            self->_target_vol = AlarmService::instance().getVolume();
            self->refresh_settings_ui();
        },
        LV_EVENT_CLICKED, this);
    _lbl_set_vol = lv_label_create(row_vl);
    lv_obj_set_pos(_lbl_set_vol, 166, 17);
    lv_obj_set_width(_lbl_set_vol, 76);
    lv_obj_set_style_text_align(_lbl_set_vol, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_set_vol, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_set_vol, lv_color_hex(COL_WHITE), 0);
    lv_obj_t* btn_vp = make_btn(row_vl, 246, 7, 46, 38, "+", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_vp,
        [](lv_event_t* e) {
            auto* self = static_cast<AppAlarm*>(lv_event_get_user_data(e));
            int v      = AlarmService::instance().getVolume() + 10;
            AlarmService::instance().setVolume(v);
            self->_target_vol = AlarmService::instance().getVolume();
            self->refresh_settings_ui();
        },
        LV_EVENT_CLICKED, this);

    // NTP Sync + Demo Ring
    lv_obj_t* btn_sync = make_btn(_scr_settings, 8, 160, 148, 42, LV_SYMBOL_REFRESH " SYNC NTP", COL_BLUE, COL_WHITE);
    lv_obj_add_event_cb(
        btn_sync,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->start_ntp_sync();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_demo = make_btn(_scr_settings, 164, 160, 148, 42, LV_SYMBOL_BELL " TEST RING", COL_AMBER, COL_BG);
    lv_obj_add_event_cb(
        btn_demo,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->begin_ringing(0);
        },
        LV_EVENT_CLICKED, this);

    _lbl_set_sync = lv_label_create(_scr_settings);
    lv_obj_set_pos(_lbl_set_sync, 10, 210);
    lv_obj_set_width(_lbl_set_sync, 300);
    lv_obj_set_style_text_align(_lbl_set_sync, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_set_sync, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_set_sync, lv_color_hex(COL_DIM), 0);

    /* ======================== 6. RINGING SCREEN ====================== */
    _scr_ring = lv_obj_create(_root);
    lv_obj_set_size(_scr_ring, W, H);
    lv_obj_set_pos(_scr_ring, 0, 0);
    style_box(_scr_ring, COL_BG, 0);
    lv_obj_add_flag(_scr_ring, LV_OBJ_FLAG_HIDDEN);

    _lbl_ring_time = lv_label_create(_scr_ring);
    lv_obj_align(_lbl_ring_time, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_font(_lbl_ring_time, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lbl_ring_time, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(_lbl_ring_time, "07:00");

    _lbl_ring_label = lv_label_create(_scr_ring);
    lv_obj_align(_lbl_ring_label, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_text_font(_lbl_ring_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_ring_label, lv_color_hex(COL_AMBER), 0);
    lv_label_set_text(_lbl_ring_label, "Wake Up");

    _ring_face_card = lv_obj_create(_scr_ring);
    lv_obj_set_size(_ring_face_card, 132, 84);
    lv_obj_align(_ring_face_card, LV_ALIGN_CENTER, 0, -4);
    style_box(_ring_face_card, COL_CARD, 16);
    lv_obj_set_style_border_width(_ring_face_card, 2, 0);
    lv_obj_set_style_border_color(_ring_face_card, lv_color_hex(COL_AMBER), 0);

    _ring_eye_l = lv_obj_create(_ring_face_card);
    lv_obj_set_size(_ring_eye_l, 18, 18);
    lv_obj_align(_ring_eye_l, LV_ALIGN_CENTER, -26, -8);
    style_box(_ring_eye_l, COL_WHITE, LV_RADIUS_CIRCLE);

    _ring_eye_r = lv_obj_create(_ring_face_card);
    lv_obj_set_size(_ring_eye_r, 18, 18);
    lv_obj_align(_ring_eye_r, LV_ALIGN_CENTER, 26, -8);
    style_box(_ring_eye_r, COL_WHITE, LV_RADIUS_CIRCLE);

    _ring_mouth = lv_obj_create(_ring_face_card);
    lv_obj_set_size(_ring_mouth, 38, 10);
    lv_obj_align(_ring_mouth, LV_ALIGN_CENTER, 0, 16);
    style_box(_ring_mouth, COL_WHITE, 6);

    // Naruto avatar image on ringing screen
    _ring_avatar_img = lv_image_create(_scr_ring);
    if (_img_naruto_happy.data != nullptr) {
        lv_image_set_src(_ring_avatar_img, &_img_naruto_happy);
        uint16_t rscale = (_img_naruto_happy.header.w > 130) ? 160 : 256;
        lv_image_set_scale(_ring_avatar_img, rscale);
    }
    lv_obj_align(_ring_avatar_img, LV_ALIGN_CENTER, 0, -4);
    lv_obj_remove_flag(_ring_avatar_img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl_hint = lv_label_create(_scr_ring);
    lv_obj_align(lbl_hint, LV_ALIGN_CENTER, 0, 48);
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_hint, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(lbl_hint, "Pet head: Snooze  |  Shake: Stop");

    lv_obj_t* btn_snooze = make_btn(_scr_ring, 8, 182, 148, 50, "SNOOZE 5m", COL_BLUE, COL_WHITE);
    _lbl_ring_snooze     = lv_obj_get_child(btn_snooze, 0);
    lv_obj_set_style_text_font(_lbl_ring_snooze, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(
        btn_snooze,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->action_snooze();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_stop = make_btn(_scr_ring, 164, 182, 148, 50, LV_SYMBOL_STOP " STOP", COL_RED, COL_WHITE);
    lv_obj_set_style_text_font(lv_obj_get_child(btn_stop, 0), &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(
        btn_stop,
        [](lv_event_t* e) {
            static_cast<AppAlarm*>(lv_event_get_user_data(e))->action_dismiss();
        },
        LV_EVENT_CLICKED, this);
}

void AppAlarm::destroy_ui()
{
    if (_root) {
        lv_obj_delete(_root);
        _root           = nullptr;
        _scr_clock      = nullptr;
        _scr_list       = nullptr;
        _scr_edit       = nullptr;
        _scr_sd         = nullptr;
        _scr_settings   = nullptr;
        _scr_ring       = nullptr;
        _face_card      = nullptr;
        _ring_face_card = nullptr;
        _avatar_img      = nullptr;
        _ring_avatar_img = nullptr;
    }
}

void AppAlarm::rebuild_list_rows()
{
    if (!_list_box) return;
    lv_obj_clean(_list_box);

    char title[32];
    std::snprintf(title, sizeof(title), "ALARMS (%u/%u)",
                  static_cast<unsigned>(_alarms.size()), static_cast<unsigned>(AlarmService::MAX_ALARMS));
    if (_lbl_list_title) {
        lv_label_set_text(_lbl_list_title, title);
    }
    if (_btn_add_alarm) {
        if (_alarms.size() >= AlarmService::MAX_ALARMS) {
            lv_obj_add_flag(_btn_add_alarm, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(_btn_add_alarm, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (_alarms.empty()) {
        lv_obj_t* empty = lv_label_create(_list_box);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(empty, "No alarms yet. Tap + NEW");
        return;
    }

    for (size_t i = 0; i < _alarms.size(); ++i) {
        const auto& a = _alarms[i];
        lv_obj_t* row = lv_obj_create(_list_box);
        lv_obj_set_size(row, 298, 56);
        style_box(row, COL_CARD, 10);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(a.enabled ? COL_TEAL_DK : COL_CARD_HI), 0);

        // Left area clickable -> Edit alarm
        lv_obj_t* hit = lv_obj_create(row);
        lv_obj_set_pos(hit, 0, 0);
        lv_obj_set_size(hit, 224, 56);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hit, 0, 0);
        lv_obj_set_style_pad_all(hit, 0, 0);
        lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(hit, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            hit,
            [](lv_event_t* e) {
                auto* self       = static_cast<AppAlarm*>(lv_event_get_user_data(e));
                lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                int idx          = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                self->open_editor(idx, false);
            },
            LV_EVENT_CLICKED, this);

        char tbuf[16];
        std::snprintf(tbuf, sizeof(tbuf), "%02u:%02u", a.hour, a.minute);
        lv_obj_t* lt = lv_label_create(hit);
        lv_obj_set_pos(lt, 12, 6);
        lv_obj_set_style_text_font(lt, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lt, lv_color_hex(a.enabled ? COL_WHITE : COL_DIM), 0);
        lv_label_set_text(lt, tbuf);
        lv_obj_remove_flag(lt, LV_OBJ_FLAG_CLICKABLE);

        std::string sub = std::string(AlarmService::labelName(a.label_idx)) + " - " + AlarmService::formatDays(a.days_mask);
        lv_obj_t* ls    = lv_label_create(hit);
        lv_obj_set_pos(ls, 12, 32);
        lv_obj_set_width(ls, 206);
        lv_label_set_long_mode(ls, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(ls, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(ls, lv_color_hex(a.enabled ? COL_AMBER : COL_DIM), 0);
        lv_label_set_text(ls, sub.c_str());
        lv_obj_remove_flag(ls, LV_OBJ_FLAG_CLICKABLE);

        // Right ON/OFF toggle pill
        lv_obj_t* tog = lv_button_create(row);
        lv_obj_set_pos(tog, 228, 10);
        lv_obj_set_size(tog, 62, 36);
        style_box(tog, a.enabled ? COL_TEAL_DK : COL_CARD_HI, 10);
        lv_obj_set_user_data(tog, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_t* tlbl = lv_label_create(tog);
        lv_obj_center(tlbl);
        lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(tlbl, lv_color_hex(a.enabled ? COL_WHITE : COL_DIM), 0);
        lv_label_set_text(tlbl, a.enabled ? "ON" : "OFF");
        lv_obj_remove_flag(tlbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            tog,
            [](lv_event_t* e) {
                auto* self       = static_cast<AppAlarm*>(lv_event_get_user_data(e));
                lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                int idx          = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                if (idx >= 0 && static_cast<size_t>(idx) < self->_alarms.size()) {
                    self->_alarms[static_cast<size_t>(idx)].enabled = !self->_alarms[static_cast<size_t>(idx)].enabled;
                    AlarmService::instance().setAlarms(self->_alarms);
                    self->rebuild_list_rows();
                }
            },
            LV_EVENT_CLICKED, this);
    }
}

void AppAlarm::rebuild_sd_rows()
{
    if (!_sd_list_box) return;
    lv_obj_clean(_sd_list_box);

    if (_sd_mp3_paths.empty()) {
        lv_obj_t* l = lv_label_create(_sd_list_box);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(l, "No .mp3 files found on SD card");
        return;
    }

    for (size_t i = 0; i < _sd_mp3_paths.size(); ++i) {
        std::string title = basename_clean(_sd_mp3_paths[i]);
        lv_obj_t* b       = lv_button_create(_sd_list_box);
        lv_obj_set_size(b, 298, 38);
        bool selected = (_edit_item.mp3_path == _sd_mp3_paths[i]);
        style_box(b, selected ? COL_TEAL_DK : COL_CARD, 8);
        lv_obj_set_user_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        lv_obj_t* l = lv_label_create(b);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_width(l, 278);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_WHITE), 0);
        lv_label_set_text(l, title.c_str());
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(
            b,
            [](lv_event_t* e) {
                auto* self     = static_cast<AppAlarm*>(lv_event_get_user_data(e));
                lv_obj_t* btn  = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                int idx        = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn)));
                if (idx >= 0 && static_cast<size_t>(idx) < self->_sd_mp3_paths.size()) {
                    self->_edit_item.tone     = AlarmService::TONE_SD;
                    self->_edit_item.mp3_path = self->_sd_mp3_paths[static_cast<size_t>(idx)];
                    self->open_screen(Screen::Edit);
                }
            },
            LV_EVENT_CLICKED, this);
    }
}

void AppAlarm::refresh_clock_ui(uint32_t now_ms)
{
    (void)now_ms;
    if (!_scr_clock) return;

    time_t now   = std::time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);

    char tbuf[16];
    char sbuf[16];
    char dbuf[32];
    std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    std::snprintf(sbuf, sizeof(sbuf), ":%02d", lt.tm_sec);

    static const char* const WDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::snprintf(dbuf, sizeof(dbuf), "%s, %s %d", WDAY[lt.tm_wday % 7], MON[lt.tm_mon % 12], lt.tm_mday);

    lv_label_set_text(_lbl_time, tbuf);
    lv_label_set_text(_lbl_sec, sbuf);
    lv_label_set_text(_lbl_date, dbuf);

    char batt[16];
    std::snprintf(batt, sizeof(batt), "%d%%", GetHAL().getBatteryLevel());
    lv_label_set_text(_lbl_batt, batt);

    std::string next_str;
    bool has_next = AlarmService::instance().findNextAlarmText(next_str);
    lv_obj_set_style_text_color(_lbl_next, lv_color_hex(has_next ? COL_AMBER : COL_DIM), 0);
    lv_label_set_text(_lbl_next, next_str.c_str());
}

void AppAlarm::refresh_editor_ui()
{
    if (!_scr_edit) return;

    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02u", _edit_item.hour);
    lv_label_set_text(_lbl_edit_hour, buf);
    std::snprintf(buf, sizeof(buf), "%02u", _edit_item.minute);
    lv_label_set_text(_lbl_edit_min, buf);

    for (int i = 0; i < 7; ++i) {
        bool on = (_edit_item.days_mask & (1u << i)) != 0;
        lv_obj_set_style_bg_color(_btn_edit_day[i], lv_color_hex(on ? COL_TEAL_DK : COL_CARD), 0);
    }

    std::string lbl = std::string("Tag: ") + AlarmService::labelName(_edit_item.label_idx);
    lv_label_set_text(_lbl_edit_label, lbl.c_str());

    std::string tone = std::string("Tone: ") + AlarmService::toneName(_edit_item.tone);
    if (_edit_item.tone == AlarmService::TONE_SD && !_edit_item.mp3_path.empty()) {
        tone = "MP3: " + basename_clean(_edit_item.mp3_path);
        if (tone.size() > 18) tone.resize(18);
    }
    lv_label_set_text(_lbl_edit_tone, tone.c_str());

    if (_btn_edit_del) {
        if (_edit_is_new) {
            lv_obj_add_flag(_btn_edit_del, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(_btn_edit_del, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AppAlarm::refresh_settings_ui()
{
    if (!_scr_settings) return;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d min", AlarmService::instance().getSnoozeMin());
    lv_label_set_text(_lbl_set_snooze, buf);

    std::snprintf(buf, sizeof(buf), "%d%%", AlarmService::instance().getVolume());
    lv_label_set_text(_lbl_set_vol, buf);

    std::string st;
    {
        std::lock_guard<std::mutex> lk(_sync_mtx);
        st = _sync_status;
    }
    lv_label_set_text(_lbl_set_sync, st.c_str());
}

void AppAlarm::refresh_ringing_ui(uint32_t now_ms)
{
    if (!_scr_ring) return;
    time_t now   = std::time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);

    char tbuf[16];
    std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    lv_label_set_text(_lbl_ring_time, tbuf);
    lv_label_set_text(_lbl_ring_label, AlarmService::labelName(_ring_item.label_idx));

    char sbuf[24];
    std::snprintf(sbuf, sizeof(sbuf), "SNOOZE %dm", AlarmService::instance().getSnoozeMin());
    lv_label_set_text(_lbl_ring_snooze, sbuf);

    bool flash = ((now_ms / 450) % 2) == 0;
    lv_obj_set_style_border_color(_ring_face_card, lv_color_hex(flash ? COL_AMBER : COL_TEAL), 0);
}

void AppAlarm::refresh_avatar_anim(uint32_t now_ms)
{
    bool use_naruto = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto &&
                       _img_naruto_calm.data != nullptr);

    if (_screen == Screen::Clock) {
        if (use_naruto && _avatar_img) {
            if (_face_card) lv_obj_add_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);

            const lv_image_dsc_t* target_img = _night_mode ? &_img_naruto_sleepy : &_img_naruto_calm;
            int target_idx                   = _night_mode ? 1 : 0;
            if (target_img->data != nullptr && _shown_naruto_idx != target_idx) {
                lv_image_set_src(_avatar_img, target_img);
                uint16_t scale = (target_img->header.w > 130) ? 150 : 256;
                lv_image_set_scale(_avatar_img, scale);
                _shown_naruto_idx = target_idx;
            }
            int dy = _night_mode ? 0 : static_cast<int>(std::sin(now_ms * 0.003f) * 2.0f);
            lv_obj_align(_avatar_img, LV_ALIGN_RIGHT_MID, -14, -10 + dy);
        } else if (_face_card) {
            if (_avatar_img) lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(_face_card, LV_OBJ_FLAG_HIDDEN);

            bool blink = ((now_ms % 4200) < 150);
            int eye_h  = _night_mode ? 4 : (blink ? 4 : 16);
            lv_obj_set_size(_face_eye_l, 16, eye_h);
            lv_obj_set_size(_face_eye_r, 16, eye_h);
            lv_obj_set_size(_face_mouth, _night_mode ? 20 : 34, 6);
            lv_obj_set_style_border_color(_face_card, lv_color_hex(_night_mode ? COL_BLUE : COL_TEAL_DK), 0);
        }
    } else if (_screen == Screen::Ringing) {
        if (use_naruto && _ring_avatar_img) {
            if (_ring_face_card) lv_obj_add_flag(_ring_face_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(_ring_avatar_img, LV_OBJ_FLAG_HIDDEN);

            int dy = static_cast<int>(std::sin(now_ms * 0.025f) * 5.0f);
            lv_obj_align(_ring_avatar_img, LV_ALIGN_CENTER, 0, -4 + dy);

            // Responsive expression based on audio volume
            bool loud = _audio_rms.load() > 800;
            const lv_image_dsc_t* ring_img = loud ? &_img_naruto_happy : &_img_naruto_pouty;
            if (ring_img->data == nullptr) ring_img = &_img_naruto_happy;
            if (ring_img->data == nullptr) ring_img = &_img_naruto_calm;
            int ring_idx = loud ? 1 : 0;
            if (ring_img->data != nullptr && _shown_ring_naruto_idx != ring_idx) {
                lv_image_set_src(_ring_avatar_img, ring_img);
                uint16_t rscale = (ring_img->header.w > 130) ? 160 : 256;
                lv_image_set_scale(_ring_avatar_img, rscale);
                _shown_ring_naruto_idx = ring_idx;
            }
        } else if (_ring_face_card) {
            if (_ring_avatar_img) lv_obj_add_flag(_ring_avatar_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(_ring_face_card, LV_OBJ_FLAG_HIDDEN);

            bool blink = ((now_ms % 4200) < 150);
            int dy     = static_cast<int>(std::sin(now_ms * 0.025f) * 4.0f);
            lv_obj_align(_ring_face_card, LV_ALIGN_CENTER, 0, -4 + dy);
            lv_obj_set_size(_ring_eye_l, 18, blink ? 4 : 14);
            lv_obj_set_size(_ring_eye_r, 18, blink ? 4 : 14);
            int mh = 8 + std::clamp<int>(_audio_rms.load() / 320, 0, 14);
            lv_obj_set_size(_ring_mouth, 40, mh);
            lv_obj_set_style_radius(_ring_mouth, (mh > 12) ? 8 : 5, 0);
        }
    }
}
