/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <lvgl_image.h>
#include <string>

struct httpd_req;

/**
 * @brief Native MCP Voice & Robot Bridge app for StackChan.
 * Exposes the stackchan-mcp HTTP protocol on port 80 so Antigravity (or any MCP
 * client) can speak aloud through StackChan (/play), listen to voice dictation
 * (/audio/status + /audio), change expressions (/face), and move servos
 * (/move, /nod, /shake, /home).
 */
class AppMcp : public mooncake::AppAbility {
public:
    AppMcp();
    ~AppMcp() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class FaceExpr : int {
        Calm = 0,
        Thinking,
        Happy,
        Sleepy,
        Shy,
        Smug,
        Pouty,
        Count,
    };

    enum class AgentMode : int {
        Idle = 0,
        Free,
        Engineer,
        Live,
    };

    struct MotionCmd {
        std::string kind;  // move, nod, shake, home
        int x_deg  = 0;
        int y_deg  = 45;
        int speed  = 500;
    };

    /* --------------------------- Tasks & HTTP ----------------------------- */
    void start_tasks();
    void stop_tasks();
    static void net_task_entry(void* arg);
    void net_task_loop();
    static void mic_task_entry(void* arg);
    void mic_task_loop();

    bool start_httpd();
    void stop_httpd();
    void play_wav_url(const std::string& url, bool wait_reply = false);
    void stop_current_playback();
    void toggle_dictation();
    void toggle_auto_mode();
    void change_volume(int delta);
    void change_pause_ms(int delta_ms);
    void set_mic_sens(int sens);
    void set_speak_move(bool enabled);
    void cycle_voice(int delta);
    void set_settings_open(bool open);
    void refresh_settings_ui();
    void process_motion_cmds();
    void run_gesture_step(uint32_t now);
    void update_speak_motion(uint32_t now);

    // HTTP handlers (stackchan-mcp compatible)
    static int handle_audio_status(httpd_req* req);
    static int handle_audio_get(httpd_req* req);
    static int handle_playback_status(httpd_req* req);
    static int handle_play(httpd_req* req);
    static int handle_move(httpd_req* req);
    static int handle_nod(httpd_req* req);
    static int handle_shake(httpd_req* req);
    static int handle_home(httpd_req* req);
    static int handle_face(httpd_req* req);
    static int handle_servo_status(httpd_req* req);
    static int handle_booth_get(httpd_req* req);
    static int handle_booth_post(httpd_req* req);
    static int handle_listen_resume(httpd_req* req);
    static int handle_agent_mode(httpd_req* req);
    static int handle_snapshot(httpd_req* req);

    /* ------------------------------- UI ----------------------------------- */
    void build_ui();
    void destroy_ui();
    void refresh_ui(uint32_t now);
    void refresh_avatar_anim(uint32_t now);
    void set_status_text(const std::string& msg);
    static FaceExpr parse_face(const std::string& s);
    static const char* face_name(FaceExpr f);

    /* ------------------------------ State --------------------------------- */
    void* _httpd = nullptr;

    std::atomic<bool> _running{false};
    std::atomic<bool> _net_exited{true};
    std::atomic<bool> _mic_exited{true};
    std::atomic<bool> _wifi_ok{false};
    std::atomic<bool> _httpd_ok{false};

    // Speech playback (stackchan_say -> POST /play)
    std::atomic<bool> _playing{false};
    std::atomic<bool> _play_abort{false};
    std::atomic<bool> _play_pending{false};
    std::atomic<uint32_t> _play_started_ms{0};
    std::atomic<int> _speak_rms{0};
    mutable std::mutex _play_mtx;
    std::string _next_play_url;
    bool _next_play_wait_reply = false;

    // Dictation recording (AUTO hands-free or DICTATE -> GET /audio/status + GET /audio)
    std::atomic<bool> _auto_mode{true};
    std::atomic<bool> _rec_start_req{false};
    std::atomic<bool> _rec_stop_req{false};
    std::atomic<bool> _rec_active{false};
    std::atomic<bool> _rec_speaking{false};
    std::atomic<bool> _rec_ready{false};
    std::atomic<bool> _waiting_reply{false};
    std::atomic<uint32_t> _auto_hold_until_ms{0};
    std::atomic<int> _mic_level{0};
    mutable std::mutex _rec_mtx;
    uint8_t* _rec_wav_buf  = nullptr;
    size_t _rec_wav_cap    = 0;
    size_t _rec_wav_bytes  = 0;

    // Robot state & motion queue
    std::atomic<int> _agent_mode{static_cast<int>(AgentMode::Idle)};
    std::atomic<int> _face{static_cast<int>(FaceExpr::Calm)};
    std::atomic<int> _volume{75};
    std::atomic<uint32_t> _silence_stop_ms{2000};
    std::atomic<int> _mic_sens{1};  // 0=LOW, 1=MED, 2=HIGH
    std::atomic<bool> _speak_move{true};
    std::atomic<int> _voice_idx{0};  // 0=Puck, 1=Kore, 2=Aoede, 3=Fenrir, 4=Zephyr
    bool _settings_open = false;
    mutable std::mutex _motion_mtx;
    std::deque<MotionCmd> _motion_cmds;
    int _gesture_type               = 0;  // 0=none, 1=nod, 2=shake
    int _gesture_step               = 0;
    uint32_t _gesture_next          = 0;
    bool _speak_moving              = false;
    int _speak_move_step            = 0;
    uint32_t _speak_move_next       = 0;
    uint32_t _explicit_motion_until = 0;
    int _base_yaw_deg               = 0;
    int _base_pitch_deg             = 45;

    mutable std::mutex _status_mtx;
    std::string _ip_str;
    std::string _status_text    = "Starting Wi-Fi...";
    std::string _free_slot_lbl  = "FREE";

    uint32_t _last_ui_tick   = 0;
    uint32_t _last_anim_tick = 0;

    // Cached 7-state Naruto expressions
    lv_image_dsc_t _naruto_imgs[static_cast<int>(FaceExpr::Count)] = {};
    int _shown_naruto_idx[2] = {-1, -1};

    /* --------------------------- LVGL Widgets ----------------------------- */
    lv_obj_t* _root         = nullptr;
    lv_obj_t* _lbl_ip       = nullptr;
    lv_obj_t* _lbl_batt     = nullptr;
    lv_obj_t* _stage        = nullptr;
    lv_obj_t* _slot_card[2] = {nullptr, nullptr};
    lv_obj_t* _slot_dot[2]  = {nullptr, nullptr};
    lv_obj_t* _slot_lbl[2]  = {nullptr, nullptr};
    lv_obj_t* _avatar_img[2] = {nullptr, nullptr};
    lv_obj_t* _face_card[2]  = {nullptr, nullptr};
    lv_obj_t* _face_eye_l[2] = {nullptr, nullptr};
    lv_obj_t* _face_eye_r[2] = {nullptr, nullptr};
    lv_obj_t* _face_mouth[2] = {nullptr, nullptr};
    lv_obj_t* _lbl_status   = nullptr;
    lv_obj_t* _bar_level_bg = nullptr;
    lv_obj_t* _bar_level    = nullptr;
    lv_obj_t* _btn_dictate  = nullptr;
    lv_obj_t* _lbl_dictate  = nullptr;
    lv_obj_t* _btn_auto     = nullptr;
    lv_obj_t* _lbl_auto     = nullptr;
    lv_obj_t* _lbl_vol      = nullptr;

    // Settings overlay widgets
    lv_obj_t* _settings_panel    = nullptr;
    lv_obj_t* _snap_panel        = nullptr;
    lv_obj_t* _snap_view         = nullptr;
    std::shared_ptr<LvglImage> _snap_img;
    std::shared_ptr<LvglImage> _snap_shown_img;
    std::mutex _snap_mtx;
    std::atomic<bool> _snap_new{false};
    uint32_t _snap_hide_ms       = 0;
    void show_snapshot();
    void hide_snapshot();
    lv_obj_t* _lbl_set_pause     = nullptr;
    lv_obj_t* _lbl_set_vol       = nullptr;
    lv_obj_t* _lbl_set_voice     = nullptr;
    lv_obj_t* _btn_sens[3]       = {nullptr, nullptr, nullptr};
    lv_obj_t* _btn_move_off      = nullptr;
    lv_obj_t* _btn_move_on       = nullptr;
};
