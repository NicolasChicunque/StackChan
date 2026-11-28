/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "speaker_wifi_service.h"
#include <lvgl.h>
#include <mooncake.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

/**
 * @brief Unified External Speaker App for StackChan (ESP32-S3):
 *  - WI-FI Mode: Local Web App (QR) with WebSocket PCM streaming (PC/Tab audio,
 *    Live Mic, Local Audio File) + low-latency UDP PCM receiver (port 8765).
 *  - USB-C Mode: Native USB Audio Class (UAC2) speaker ("StackChan Speaker")
 *    over ESP32-S3 USB-OTG (48 kHz -> 24 kHz downsampled on the fly).
 *  - Shared Engine: Real-time 128-pt FFT lip-sync, 8-band equalizer, Classic
 *    StackChan Face & Custom Avatar Avatar support, reactive servo dance, RGB lights,
 *    head touch gestures, and NVS persistence.
 */
class AppSpeaker : public mooncake::AppAbility {
public:
    AppSpeaker();
    ~AppSpeaker() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    // Called by USB-C UAC trampoline callback
    void on_uac_pcm_48k(const int16_t* samples, size_t count);
    void on_uac_volume(uint32_t volume);
    void on_uac_mute(uint32_t mute);

private:
    enum class InputMode : uint8_t {
        Wifi = 0,
        Usb  = 1,
    };

    enum class ViewMode : uint8_t {
        Main = 0,
        Info = 1,
    };

    struct PcmRingBuffer {
        int16_t* buf    = nullptr;
        size_t capacity = 0;  // in samples
        size_t head     = 0;
        size_t tail     = 0;
        size_t count    = 0;
        mutable std::mutex mtx;

        bool allocate(size_t samples);
        void release();
        void clear();
        void push(const int16_t* data, size_t samples, size_t max_latency_samples);
        size_t pop(int16_t* out, size_t samples);
        size_t available() const;
    };

    /* ------------------------- Audio & Modes ------------------------------ */
    void switch_input_mode(InputMode mode);
    void start_wifi_mode();
    void stop_wifi_mode();
    void start_usb_mode();
    void stop_usb_mode();

    void push_pcm_24k(const int16_t* samples, size_t count);
    void start_audio_task();
    void stop_audio_task();
    static void audio_task_entry(void* arg);
    void audio_task_loop();

    /* --------------------- Controls & Persistence ------------------------- */
    void load_prefs();
    void save_prefs();
    void set_volume(int vol);
    void change_volume(int delta);
    void toggle_mute();
    void toggle_move();
    void toggle_lights();
    void move_home(int speed = 500);
    void update_robot_dance(uint32_t now, bool active, float level);

    std::string build_status_json() const;
    void process_remote_cmds();

    /* ------------------------------- UI ----------------------------------- */
    void build_ui();
    void destroy_ui();
    void switch_view(ViewMode view);
    void refresh_ui(uint32_t now);
    void refresh_mode_bar();
    void refresh_bottom_bar();
    void refresh_avatar_and_eq(uint32_t now);
    void refresh_info_view();

    /* ------------------------------ State --------------------------------- */
    InputMode _input_mode = InputMode::Wifi;
    ViewMode _view_mode   = ViewMode::Main;

    int _volume           = 60;
    bool _muted           = false;
    bool _robot_move      = true;
    bool _lights_on       = true;
    int _y_home_deg       = 45;
    int _sway_side        = 1;

    std::atomic<bool> _audio_running{false};
    std::atomic<bool> _audio_exited{true};
    std::atomic<bool> _usb_attached{false};
    std::atomic<uint32_t> _last_rx_ms{0};
    std::atomic<uint32_t> _last_sound_ms{0};
    std::atomic<uint32_t> _rx_packets{0};
    std::atomic<float> _mouth_ratio{0.0f};
    std::atomic<float> _energy_level{0.0f};
    std::array<std::atomic<uint8_t>, 8> _eq_bars{};

    // Pending cross-thread actions (from Web / USB / Head gestures)
    mutable std::mutex _cmd_mtx;
    std::atomic<int> _pending_vol{-1};
    std::atomic<int> _pending_mute{-1};
    std::atomic<bool> _pending_toggle_mute{false};
    std::atomic<bool> _pending_toggle_move{false};
    std::atomic<bool> _pending_toggle_lights{false};
    std::atomic<int> _pending_vol_delta{0};

    PcmRingBuffer _ring;
    std::unique_ptr<SpeakerWifiService> _wifi_svc;

    int _head_pet_conn    = -1;
    uint32_t _last_ui_ms  = 0;
    uint32_t _last_dance_ms = 0;
    uint32_t _last_blink_ms = 0;
    bool _eyes_closed     = false;
    bool _was_active      = false;
    bool _avatar_happy    = false;
    std::string _last_qr_url;
    std::string _usb_status_msg = "Tap USB-C to start";

    lv_image_dsc_t _img_avatar_calm  = {};
    lv_image_dsc_t _img_avatar_happy = {};

    /* --------------------------- LVGL Widgets ----------------------------- */
    lv_obj_t* _root           = nullptr;
    lv_obj_t* _main_cont      = nullptr;
    lv_obj_t* _info_cont      = nullptr;

    // Mode bar
    lv_obj_t* _btn_mode_wifi  = nullptr;
    lv_obj_t* _lbl_mode_wifi  = nullptr;
    lv_obj_t* _btn_mode_usb   = nullptr;
    lv_obj_t* _lbl_mode_usb   = nullptr;
    lv_obj_t* _badge_state    = nullptr;
    lv_obj_t* _lbl_badge      = nullptr;

    // Center stage
    lv_obj_t* _stage_card     = nullptr;
    lv_obj_t* _avatar_img     = nullptr;
    lv_obj_t* _face_box       = nullptr;
    lv_obj_t* _eye_l          = nullptr;
    lv_obj_t* _eye_r          = nullptr;
    lv_obj_t* _mouth          = nullptr;
    lv_obj_t* _eq_objs[8]     = {nullptr};
    lv_obj_t* _lbl_conn_hint  = nullptr;

    // Bottom 4-button bar
    lv_obj_t* _btn_mute       = nullptr;
    lv_obj_t* _lbl_mute       = nullptr;
    lv_obj_t* _btn_move       = nullptr;
    lv_obj_t* _lbl_move       = nullptr;
    lv_obj_t* _btn_lights     = nullptr;
    lv_obj_t* _lbl_lights     = nullptr;
    lv_obj_t* _btn_info       = nullptr;
    lv_obj_t* _lbl_info       = nullptr;

    // Info / QR view
    lv_obj_t* _lbl_info_hdr   = nullptr;
    lv_obj_t* _qr_box         = nullptr;
    lv_obj_t* _qr_obj         = nullptr;
    lv_obj_t* _lbl_info_title = nullptr;
    lv_obj_t* _lbl_info_body  = nullptr;
    lv_obj_t* _lbl_info_foot  = nullptr;
};
