/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "music_wifi_service.h"
#include <mooncake.h>
#include <smooth_ui_toolkit.hpp>
#include <smooth_lvgl.hpp>
#include <lvgl.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <esp_heap_caps.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

template <typename T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() noexcept = default;
    template <class U>
    constexpr PsramAllocator(const PsramAllocator<U>&) noexcept
    {
    }
    T* allocate(std::size_t n)
    {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept
    {
        heap_caps_free(p);
    }
    template <class U>
    bool operator==(const PsramAllocator<U>&) const noexcept
    {
        return true;
    }
    template <class U>
    bool operator!=(const PsramAllocator<U>&) const noexcept
    {
        return false;
    }
};

using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

/**
 * @brief MicroSD (/sd) music player app for StackChan.
 * Clean UI with a 6-button control bar, animated Custom Avatar avatar,
 * dedicated settings screen, and automatic NVS persistence.
 */
class AppMusicPlayer : public mooncake::AppAbility {
public:
    AppMusicPlayer();
    ~AppMusicPlayer() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class ScreenMode {
        Main,
        Settings,
        List,
        Head,
        Radio,
        WebQr,
    };

    struct TrackEntry {
        PsramString path;
        PsramString title;
        bool is_mp3 = false;
    };

    struct AudioProbeInfo {
        uint32_t duration_ms = 0;
        uint32_t byte_rate   = 0;
        uint16_t format_tag  = 0;
        uint16_t channels    = 0;
        uint16_t bits        = 0;
        uint32_t sample_rate = 0;
        uint32_t data_offset = 0;
        uint32_t data_size   = 0;
        bool valid           = false;
    };

    /* --------------------------- SD & Scanning ---------------------------- */
    bool mount_sd();
    void unmount_sd();
    void scan_tracks();
    static bool is_audio_name(const std::string& name);
    static std::string ext_of(const std::string& name);
    static std::string base_title(const std::string& path);
    static std::string clip_text(const std::string& s, size_t max_chars);
    static std::string fmt_time(int32_t ms);

    /* ------------------------- Probing & Audio ---------------------------- */
    static AudioProbeInfo probe_wav(const std::string& path);
    static AudioProbeInfo probe_mp3(const std::string& path);
    static AudioProbeInfo probe_audio(const std::string& path, bool is_mp3);

    void start_audio_task();
    void stop_audio_task();
    static void audio_task_entry(void* arg);
    void audio_task_loop();

    void play_track(int idx, int32_t start_ms = 0);
    void play_radio(int station_idx);
    void toggle_play();
    void stop_playback();
    void seek_to(int32_t ms);
    void change_track(int delta);
    void change_volume(int delta);
    void set_volume(int vol);
    void cycle_loop();
    void toggle_move();
    void toggle_lights();
    void toggle_shuffle();
    int random_index() const;
    void track_finished();

    /* --------------------- Wi-Fi Radio & Web Remote ----------------------- */
    void init_wifi_service();
    void process_web_commands();
    std::string build_status_json() const;
    std::string build_tracks_json(const std::string& q, int page, int limit) const;
    bool handle_begin_sd_write();
    void handle_end_sd_write(bool success, const std::string& filename);
    void update_now_playing_title();

    /* ---------------------- Robot & NVS Persistence ----------------------- */
    void load_saved_prefs();
    void save_settings();
    void save_last_track();
    bool save_head_cfg();
    void move_home(int speed = 500);
    void rgb_off();
    void set_all_rgb(uint32_t hex_color);
    void beat_update(uint32_t now);

    /* ------------------------------- UI ----------------------------------- */
    void set_status(const std::string& msg, uint32_t duration_ms = 2500);
    std::string audio_state_summary() const;

    void build_ui();
    void destroy_ui();
    void switch_mode(ScreenMode new_mode);
    void open_list();
    void open_radio();
    void open_web_qr();

    void refresh_main_ui(uint32_t now);
    void refresh_button_styles();
    void refresh_settings_ui();
    void refresh_battery_ui();
    void apply_avatar_style();
    void refresh_avatar_anim(uint32_t now);
    void refresh_list_ui();
    void refresh_head_ui();
    void refresh_radio_ui();
    void refresh_web_qr_ui();

    /* ------------------------------ State --------------------------------- */
    bool _sd_mounted          = false;
    void* _sd_card_handle     = nullptr;
    bool _scan_error          = false;
    int _scan_entries         = 0;
    std::string _scan_other_ext;
    int _scan_other_n         = 0;
    std::string _scan_first_err;

    mutable std::mutex _tracks_mutex;
    std::vector<TrackEntry, PsramAllocator<TrackEntry>> _tracks;
    int _index                = 0;
    int _list_page            = 0;
    int _radio_index          = 0;
    int _radio_page           = 0;
    std::string _saved_track_path;
    int _saved_track_idx      = 0;
    bool _saved_radio_mode    = false;
    int _saved_radio_idx      = 0;
    std::string _saved_radio_url;

    // Wi-Fi Radio & Web Remote
    std::unique_ptr<MusicWifiService> _wifi_svc;
    std::atomic<bool> _radio_mode{false};
    std::atomic<bool> _sd_upload_active{false};
    mutable std::mutex _cmd_mutex;
    std::deque<MusicWifiService::WebCommand> _web_cmds;
    std::string _uploaded_notice;
    mutable std::mutex _title_mutex;
    std::string _now_playing_title;
    std::string _last_qr_url;

    // Persistent player preferences
    int _volume               = 60;
    int _loop_mode            = 0;      // 0: OFF, 1: 1, 2: ALL
    bool _robot_move          = true;
    bool _lights_on           = true;
    bool _shuffle_on          = false;
    int _y_home_deg           = 45;     // 0..90 degrees (45 = facing forward)
    int _sway_side            = 1;

    ScreenMode _mode          = ScreenMode::Main;
    std::string _status_msg;
    uint32_t _status_until    = 0;
    std::string _audio_err;

    uint32_t _last_beat       = 0xFFFFFFFF;
    uint32_t _last_anim_tick  = 0;
    uint32_t _last_time_tick  = 0;
    uint32_t _last_batt_tick  = 0;
    int _batt_pct             = -1;
    bool _batt_chg            = false;
    bool _avatar_showing_happy = false;

    // Hardware sensor controls: head tap = play/pause, swipe = next/prev, IMU shake = next
    int _head_pet_conn        = -1;
    int _imu_conn             = -1;
    std::atomic<int> _pending_head_action{0};  // 1=toggle, 2=next, 3=prev
    std::atomic<bool> _pending_shake{false};
    uint32_t _last_gesture_ms = 0;

    // Background audio task synchronization
    mutable std::mutex _audio_mutex;
    std::atomic<bool> _task_running{false};
    std::atomic<bool> _task_exited{true};
    std::atomic<bool> _playing{false};
    std::atomic<bool> _started{false};
    std::atomic<bool> _track_eof{false};
    std::atomic<bool> _cmd_reload{false};
    std::atomic<bool> _cmd_close_file{false};
    std::atomic<bool> _audio_file_open{false};
    std::atomic<int32_t> _elapsed_ms{0};
    std::atomic<int32_t> _duration_ms{0};
    std::atomic<uint32_t> _byte_rate{0};
    std::atomic<uint32_t> _decoded_bytes{0};

    std::string _active_path;
    bool _active_is_mp3       = false;
    bool _active_is_radio     = false;
    int32_t _target_start_ms  = 0;
    AudioProbeInfo _active_probe;

    // Custom Avatar avatar assets for the music player
    lv_image_dsc_t _img_avatar_calm  = {};
    lv_image_dsc_t _img_avatar_happy = {};

    /* --------------------------- LVGL Widgets ----------------------------- */
    lv_obj_t* _root_screen    = nullptr;

    // 1. Main view
    lv_obj_t* _main_cont      = nullptr;
    lv_obj_t* _lbl_title      = nullptr;
    lv_obj_t* _batt_cont      = nullptr;
    lv_obj_t* _lbl_batt       = nullptr;
    lv_obj_t* _batt_box       = nullptr;
    lv_obj_t* _batt_fill      = nullptr;
    lv_obj_t* _batt_tip       = nullptr;
    lv_obj_t* _bar_bg         = nullptr;
    lv_obj_t* _bar_fill       = nullptr;
    lv_obj_t* _lbl_elapsed    = nullptr;
    lv_obj_t* _lbl_track_num  = nullptr;
    lv_obj_t* _lbl_duration   = nullptr;
    lv_obj_t* _lbl_vol_ver    = nullptr;
    lv_obj_t* _lbl_status     = nullptr;

    // Center stage: animated Custom Avatar or classic StackChan avatar + floating musical notes
    lv_obj_t* _stage_cont     = nullptr;
    lv_obj_t* _avatar_img     = nullptr;
    lv_obj_t* _face_card      = nullptr;
    lv_obj_t* _face_eye_l     = nullptr;
    lv_obj_t* _face_eye_r     = nullptr;
    lv_obj_t* _face_mouth     = nullptr;
    lv_obj_t* _note_lbls[4]   = {nullptr};

    struct BtnWidget {
        const char* name = nullptr;
        lv_obj_t* btn    = nullptr;
        lv_obj_t* label  = nullptr;
    };
    BtnWidget _main_btns[4];

    // 2. Settings view
    lv_obj_t* _settings_cont  = nullptr;
    BtnWidget _cfg_btns[8];   // loop, shuf, lights, move, playlist, radio, web, head

    // 3. Playlist view
    lv_obj_t* _list_cont        = nullptr;
    lv_obj_t* _lbl_list_hdr     = nullptr;
    lv_obj_t* _lbl_list_count   = nullptr;
    lv_obj_t* _list_rows[5]     = {nullptr};
    lv_obj_t* _list_row_lbls[5] = {nullptr};
    lv_obj_t* _lbl_list_empty1  = nullptr;
    lv_obj_t* _lbl_list_empty2  = nullptr;
    lv_obj_t* _lbl_list_info    = nullptr;
    lv_obj_t* _btn_page_prev    = nullptr;
    lv_obj_t* _btn_page_next    = nullptr;
    lv_obj_t* _btn_open_head    = nullptr;
    lv_obj_t* _btn_exit_app     = nullptr;

    // 4. Head calibration view
    lv_obj_t* _head_cont        = nullptr;
    lv_obj_t* _lbl_head_hdr     = nullptr;
    lv_obj_t* _lbl_head_target  = nullptr;
    lv_obj_t* _lbl_head_real    = nullptr;
    lv_obj_t* _btn_head_minus   = nullptr;
    lv_obj_t* _btn_head_plus    = nullptr;
    lv_obj_t* _btn_head_save    = nullptr;

    // 5. Internet Radio view
    lv_obj_t* _radio_cont        = nullptr;
    lv_obj_t* _lbl_radio_hdr     = nullptr;
    lv_obj_t* _lbl_radio_count   = nullptr;
    lv_obj_t* _radio_rows[5]     = {nullptr};
    lv_obj_t* _radio_row_lbls[5] = {nullptr};
    lv_obj_t* _lbl_radio_info    = nullptr;

    // 6. Web Remote / QR view
    lv_obj_t* _web_cont          = nullptr;
    lv_obj_t* _web_qr_box        = nullptr;
    lv_obj_t* _web_qr            = nullptr;
    lv_obj_t* _lbl_web_status    = nullptr;
    lv_obj_t* _lbl_web_url       = nullptr;
    lv_obj_t* _lbl_web_hint      = nullptr;
};
