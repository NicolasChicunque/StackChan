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
#include <mutex>
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
    void play_wav_url(const std::string& url);
    void stop_current_playback();
    void toggle_dictation();
    void toggle_auto_mode();
    void change_volume(int delta);
    void process_motion_cmds();
    void run_gesture_step(uint32_t now);

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

    // Dictation recording (AUTO hands-free or DICTATE -> GET /audio/status + GET /audio)
    std::atomic<bool> _auto_mode{true};
    std::atomic<bool> _rec_start_req{false};
    std::atomic<bool> _rec_stop_req{false};
    std::atomic<bool> _rec_active{false};
    std::atomic<bool> _rec_speaking{false};
    std::atomic<bool> _rec_ready{false};
    std::atomic<uint32_t> _auto_hold_until_ms{0};
    std::atomic<int> _mic_level{0};
    mutable std::mutex _rec_mtx;
    uint8_t* _rec_wav_buf  = nullptr;
    size_t _rec_wav_cap    = 0;
    size_t _rec_wav_bytes  = 0;

    // Robot state & motion queue
    std::atomic<int> _face{static_cast<int>(FaceExpr::Calm)};
    std::atomic<int> _volume{75};
    mutable std::mutex _motion_mtx;
    std::deque<MotionCmd> _motion_cmds;
    int _gesture_type      = 0;  // 0=none, 1=nod, 2=shake
    int _gesture_step      = 0;
    uint32_t _gesture_next = 0;

    mutable std::mutex _status_mtx;
    std::string _ip_str;
    std::string _status_text = "Starting Wi-Fi...";

    uint32_t _last_ui_tick   = 0;
    uint32_t _last_anim_tick = 0;

    // Cached 7-state Naruto expressions
    lv_image_dsc_t _naruto_imgs[static_cast<int>(FaceExpr::Count)] = {};
    int _shown_naruto_idx = -1;

    /* --------------------------- LVGL Widgets ----------------------------- */
    lv_obj_t* _root         = nullptr;
    lv_obj_t* _lbl_ip       = nullptr;
    lv_obj_t* _lbl_batt     = nullptr;
    lv_obj_t* _stage        = nullptr;
    lv_obj_t* _avatar_img   = nullptr;
    lv_obj_t* _face_card    = nullptr;
    lv_obj_t* _face_eye_l   = nullptr;
    lv_obj_t* _face_eye_r   = nullptr;
    lv_obj_t* _face_mouth   = nullptr;
    lv_obj_t* _lbl_status   = nullptr;
    lv_obj_t* _bar_level_bg = nullptr;
    lv_obj_t* _bar_level    = nullptr;
    lv_obj_t* _btn_dictate  = nullptr;
    lv_obj_t* _lbl_dictate  = nullptr;
    lv_obj_t* _btn_auto     = nullptr;
    lv_obj_t* _lbl_auto     = nullptr;
    lv_obj_t* _lbl_vol      = nullptr;
};
