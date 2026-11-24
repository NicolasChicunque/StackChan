/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "alarm_service.h"

#include <lvgl.h>
#include <mooncake.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief Google-Alarm-inspired Clock & Alarm app for StackChan.
 * Supports up to 6 NVS-persisted alarms with weekday schedules, nightstand dim
 * mode, 4 built-in synthesized ringtones + SD card MP3 playback, progressive
 * volume ramp, background triggering across all apps, head-pet Snooze, and
 * IMU-shake Dismiss.
 */
class AppAlarm : public mooncake::AppAbility {
public:
    AppAlarm();
    ~AppAlarm() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class Screen {
        Clock = 0,
        List,
        Edit,
        SdPicker,
        Timer,
        Settings,
        Ringing,
    };

    /* --------------------------- Audio & SD ------------------------------- */
    void start_audio(uint8_t tone, const std::string& mp3_path, bool preview_only);
    void stop_audio();
    static void audio_task_entry(void* arg);
    void audio_task_loop();
    void play_synth_loop(uint8_t tone, bool preview_only);
    bool play_sd_mp3_loop(const std::string& path, bool preview_only);

    bool mount_sd();
    void unmount_sd();
    void scan_sd_mp3s();

    /* --------------------------- NTP Sync --------------------------------- */
    void start_ntp_sync();
    static void ntp_task_entry(void* arg);
    void ntp_task_loop();

    /* --------------------------- Alarm Flow ------------------------------- */
    void begin_ringing(int alarm_idx);
    void action_snooze();
    void action_dismiss();
    void toggle_night_mode();
    void set_night_mode(bool on);
    void open_screen(Screen scr);
    void open_editor(int idx, bool is_new);
    void save_editor();
    void delete_editor();

    /* ------------------------------ UI ------------------------------------ */
    void build_ui();
    void destroy_ui();
    void rebuild_list_rows();
    void rebuild_sd_rows();
    void refresh_clock_ui(uint32_t now_ms);
    void refresh_editor_ui();
    void refresh_timer_ui(uint32_t now_ms);
    void refresh_settings_ui();
    void refresh_ringing_ui(uint32_t now_ms);
    void refresh_avatar_anim(uint32_t now_ms);
    void update_ringing_motion(uint32_t now_ms);
    void set_sync_text(const std::string& msg);

    /* ----------------------------- State ---------------------------------- */
    Screen _screen = Screen::Clock;
    std::vector<AlarmItem> _alarms;

    // Editor working copy
    int _edit_idx    = -1;
    bool _edit_is_new = false;
    AlarmItem _edit_item;

    // Ringing state
    int _ring_alarm_idx        = -1;
    AlarmItem _ring_item;
    uint32_t _ring_start_ms    = 0;
    uint32_t _ring_next_move   = 0;
    int _ring_move_step        = 0;
    int _last_ramp_vol         = -1;

    // Nightstand dim mode
    bool _night_mode           = false;
    uint8_t _saved_brightness  = 80;

    // Timer & Stopwatch state
    bool _chrono_tab           = false;  // false = Countdown Timer, true = Stopwatch
    bool _sw_running           = false;
    uint32_t _sw_start_ms      = 0;
    uint32_t _sw_accum_ms      = 0;
    uint32_t _sw_last_lap_ms   = 0;
    int _sw_lap_count          = 0;

    // Physical gesture signals (Head pet -> Snooze, Shake -> Dismiss)
    int _head_pet_conn         = -1;
    int _imu_conn              = -1;
    std::atomic<bool> _head_pet_flag{false};
    std::atomic<bool> _shake_flag{false};

    // Audio worker
    std::atomic<bool> _audio_running{false};
    std::atomic<bool> _audio_abort{false};
    std::atomic<bool> _audio_exited{true};
    std::atomic<int> _audio_rms{0};
    std::atomic<int> _target_vol{80};
    uint8_t _audio_tone        = 0;
    bool _audio_preview        = false;
    std::string _audio_mp3_path;

    // SD card
    void* _sd_card_handle      = nullptr;
    bool _sd_mounted           = false;
    bool _sd_scan_req          = false;
    std::vector<std::string> _sd_mp3_paths;

    // NTP background sync
    std::atomic<bool> _ntp_busy{false};
    mutable std::mutex _sync_mtx;
    std::string _sync_status;

    uint32_t _last_ui_tick     = 0;
    uint32_t _last_anim_tick   = 0;

    /* --------------------------- LVGL Widgets ----------------------------- */
    lv_obj_t* _root            = nullptr;

    // Clock screen
    lv_obj_t* _scr_clock       = nullptr;
    lv_obj_t* _lbl_hdr         = nullptr;
    lv_obj_t* _lbl_batt        = nullptr;
    lv_obj_t* _stage           = nullptr;
    lv_obj_t* _lbl_time        = nullptr;
    lv_obj_t* _lbl_sec         = nullptr;
    lv_obj_t* _lbl_date        = nullptr;
    lv_obj_t* _lbl_next        = nullptr;
    lv_obj_t* _face_card       = nullptr;
    lv_obj_t* _face_eye_l      = nullptr;
    lv_obj_t* _face_eye_r      = nullptr;
    lv_obj_t* _face_mouth      = nullptr;
    lv_obj_t* _avatar_img      = nullptr;
    lv_obj_t* _btn_night       = nullptr;
    lv_obj_t* _lbl_night       = nullptr;

    // Alarms list screen
    lv_obj_t* _scr_list        = nullptr;
    lv_obj_t* _lbl_list_title  = nullptr;
    lv_obj_t* _list_box        = nullptr;
    lv_obj_t* _btn_add_alarm   = nullptr;

    // Editor screen
    lv_obj_t* _scr_edit        = nullptr;
    lv_obj_t* _lbl_edit_hour   = nullptr;
    lv_obj_t* _lbl_edit_min    = nullptr;
    lv_obj_t* _btn_edit_day[7] = {};
    lv_obj_t* _lbl_edit_label  = nullptr;
    lv_obj_t* _lbl_edit_tone   = nullptr;
    lv_obj_t* _btn_edit_mp3    = nullptr;
    lv_obj_t* _btn_edit_del    = nullptr;

    // SD MP3 picker screen
    lv_obj_t* _scr_sd          = nullptr;
    lv_obj_t* _sd_list_box     = nullptr;

    // Timer & Stopwatch screen
    lv_obj_t* _scr_timer       = nullptr;
    lv_obj_t* _btn_tab_tmr     = nullptr;
    lv_obj_t* _btn_tab_sw      = nullptr;
    lv_obj_t* _box_timer       = nullptr;
    lv_obj_t* _lbl_tmr_time    = nullptr;
    lv_obj_t* _lbl_tmr_state   = nullptr;
    lv_obj_t* _btn_tmr_toggle  = nullptr;
    lv_obj_t* _lbl_tmr_toggle  = nullptr;
    lv_obj_t* _box_sw          = nullptr;
    lv_obj_t* _lbl_sw_time     = nullptr;
    lv_obj_t* _lbl_sw_lap      = nullptr;
    lv_obj_t* _btn_sw_toggle   = nullptr;
    lv_obj_t* _lbl_sw_toggle   = nullptr;

    // Settings / Sync screen
    lv_obj_t* _scr_settings    = nullptr;
    lv_obj_t* _lbl_set_snooze  = nullptr;
    lv_obj_t* _lbl_set_vol     = nullptr;
    lv_obj_t* _lbl_set_sync    = nullptr;

    // Ringing screen
    lv_obj_t* _scr_ring        = nullptr;
    lv_obj_t* _lbl_ring_time   = nullptr;
    lv_obj_t* _lbl_ring_label  = nullptr;
    lv_obj_t* _lbl_ring_snooze = nullptr;
    lv_obj_t* _ring_face_card  = nullptr;
    lv_obj_t* _ring_eye_l      = nullptr;
    lv_obj_t* _ring_eye_r      = nullptr;
    lv_obj_t* _ring_mouth      = nullptr;
    lv_obj_t* _ring_avatar_img = nullptr;

    // Naruto avatar sprites & animation state
    lv_image_dsc_t _img_naruto_calm   = {};
    lv_image_dsc_t _img_naruto_happy  = {};
    lv_image_dsc_t _img_naruto_sleepy = {};
    lv_image_dsc_t _img_naruto_pouty  = {};
    int _shown_naruto_idx             = -1;
    int _shown_ring_naruto_idx        = -1;
};
