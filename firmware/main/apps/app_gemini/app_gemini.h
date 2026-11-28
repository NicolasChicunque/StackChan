/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <mooncake.h>
#include <lvgl.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

class WebSocket;
struct httpd_req;

/**
 * @brief Unified Native Gemini Live API + Auto-Attach MCP Bridge app for StackChan.
 *
 * Standalone (default): connects directly over Wi-Fi (WSS) to Gemini 3.8 Live
 * using /sd/config/gemini.txt (cached in NVS) with 1 big avatar.
 *
 * MCP Attached (when /stackchanchat sends POST /mcp/attach heartbeat):
 * automatically switches to a 2-avatar layout (Left: LIVE <Voice> Cyan,
 * Right: AGENT $ Amber), enables the `escalate_to_engineer` tool on the
 * ESP32-native Gemini Live session, and serves HTTP :80 (/play, /move, /nod,
 * /shake, /home, /face, /mcp/escalation, /mcp/reply) until detached or lease expires.
 */
class AppGemini : public mooncake::AppAbility {
public:
    AppGemini();
    ~AppGemini() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class SessionState : int {
        NeedKey = 0,
        ConnectingWifi,
        ConnectingApi,
        Ready,      // Paused / muted by user
        Listening,  // Hands-free armed: waiting for voice locally (0 quota)
        Hearing,    // Voice detected: streaming audio to Gemini
        Thinking,   // Pause detected: waiting for Gemini reply
        Speaking,   // Playing Gemini voice reply
        AgentWait,  // Escalated to PC coding agent (mic OFF, right AGENT $ avatar lit)
        Error,
    };

    struct MotionCmd {
        std::string kind;  // move, nod, shake, home
        int x_deg  = 0;
        int y_deg  = 45;
        int speed  = 500;
    };

    /* ----------------------- PSRAM PCM Ring Buffer ------------------------ */
    struct PcmRingBuffer {
        int16_t* buf      = nullptr;
        size_t capacity   = 0;
        size_t head       = 0;
        size_t tail       = 0;
        size_t count      = 0;
        mutable std::mutex mtx;

        bool allocate(size_t samples);
        void release();
        void clear();
        size_t push(const int16_t* data, size_t samples);
        size_t pop(int16_t* out, size_t max_samples);
        size_t available() const;
    };

    /* ----------------------- Config (SD + NVS) ---------------------------- */
    bool load_api_config();
    bool read_key_from_sd(std::string& out_key, std::string& out_model, std::string& out_voice);
    bool load_key_from_nvs(std::string& out_key, std::string& out_model, std::string& out_voice, int& out_pitch_pct);
    void save_key_to_nvs(const std::string& key, const std::string& model, const std::string& voice, int pitch_pct);
    void save_voice_pitch_to_nvs();

    /* --------------------- Background Tasks & Live API -------------------- */
    void start_tasks();
    void stop_tasks();
    static void net_task_entry(void* arg);
    static void spk_task_entry(void* arg);
    void net_task_loop();
    void spk_task_loop();

    bool connect_live_session();
    void disconnect_live_session();
    bool send_setup_message(bool include_search, bool include_mcp_tools);
    bool send_pcm16k_chunk(const int16_t* pcm, size_t samples);
    void handle_server_json(const char* data, size_t len);
    void process_mic_vad();

    /* ------------------------- MCP HTTP Bridge ---------------------------- */
    bool start_httpd();
    void stop_httpd();
    void play_http_wav(const std::string& url, bool wait_reply_after);
    void process_motion_cmds();
    void run_gesture_step(uint32_t now);

    static int handle_audio_status(httpd_req* req);
    static int handle_playback_status(httpd_req* req);
    static int handle_play(httpd_req* req);
    static int handle_mcp_attach(httpd_req* req);
    static int handle_mcp_detach(httpd_req* req);
    static int handle_mcp_escalation(httpd_req* req);
    static int handle_mcp_wait_reply(httpd_req* req);
    static int handle_mcp_reply(httpd_req* req);
    static int handle_agent_mode(httpd_req* req);
    static int handle_listen_resume(httpd_req* req);
    static int handle_move(httpd_req* req);
    static int handle_nod(httpd_req* req);
    static int handle_shake(httpd_req* req);
    static int handle_home(httpd_req* req);
    static int handle_face(httpd_req* req);

    /* ------------------------- User Actions ------------------------------- */
    void on_primary_button();
    void on_retry_button();
    void change_volume(int delta);
    void open_settings();
    void close_settings();
    void cycle_voice(int delta);
    void change_pitch(int delta_pct);
    void set_mic_sens(int sens);
    void set_speak_move(bool enabled);
    void update_speak_motion(uint32_t now);
    void set_state(SessionState st);
    void set_subtitle(const std::string& text, bool append = false);
    void set_error(const std::string& msg);
    void update_leds(SessionState st, int rms);

    /* ----------------------------- LVGL UI -------------------------------- */
    void build_ui();
    void destroy_ui();
    void refresh_ui(uint32_t now);
    void refresh_settings_ui();
    void refresh_face_anim(uint32_t now, SessionState st);

    /* ------------------------------- State -------------------------------- */
    std::string _api_key;
    std::string _model_name = "gemini-3.8-live";
    std::string _voice_name = "Puck";
    int _voice_index        = 0;
    std::atomic<int> _pitch_pct{100};
    std::atomic<int> _mic_sens{1};  // 0=LOW, 1=MED, 2=HIGH
    std::atomic<bool> _speak_move{true};
    bool _settings_open       = false;
    bool _settings_paused_mic = false;
    bool _voice_dirty         = false;
    int _volume             = 75;

    std::atomic<int> _state{static_cast<int>(SessionState::NeedKey)};
    std::atomic<bool> _closing{false};
    std::atomic<bool> _net_task_exited{true};
    std::atomic<bool> _spk_task_exited{true};
    std::atomic<bool> _want_connect{false};
    std::atomic<bool> _want_arm_listen{false};
    std::atomic<bool> _want_stop_talk{false};
    std::atomic<bool> _want_mute{false};
    std::atomic<bool> _muted{false};
    std::atomic<bool> _setup_complete{false};
    std::atomic<bool> _use_google_search{true};
    std::atomic<bool> _server_turn_done{false};
    std::atomic<bool> _new_turn_subtitle{true};
    std::atomic<int> _audio_rms{0};

    // MCP Bridge State (auto-attaches on POST /mcp/attach, auto-detaches on lease expiry)
    void* _httpd = nullptr;
    std::atomic<bool> _httpd_ok{false};
    std::atomic<bool> _mcp_attached{false};
    std::atomic<bool> _mcp_agent_active{false};
    std::atomic<uint32_t> _mcp_lease_until_ms{0};
    std::atomic<bool> _mcp_handoff_pending{false};
    std::atomic<bool> _mcp_handoff_speaking{false};
    std::atomic<uint32_t> _mcp_handoff_started_ms{0};
    std::atomic<bool> _mcp_end_pending{false};
    std::atomic<bool> _pause_pending{false};
    std::atomic<bool> _mute_after_speak{false};
    std::atomic<bool> _voice_preview_pending{false};
    std::atomic<bool> _mcp_wait_reply_mode{false};
    std::atomic<bool> _mcp_escalation_ready{false};
    std::atomic<bool> _mcp_reply_ready{false};
    std::atomic<bool> _mcp_playing_wav{false};
    std::atomic<bool> _mcp_play_pending{false};
    std::atomic<bool> _mcp_play_abort{false};
    std::atomic<uint32_t> _mcp_play_started_ms{0};
    std::atomic<bool> _mcp_play_wait_reply{false};
    mutable std::mutex _mcp_mtx;
    std::string _ip_str;
    std::string _mcp_context;
    std::string _mcp_next_play_url;
    std::string _mcp_next_subtitle;
    std::string _mcp_last_user_text;
    std::string _mcp_last_bot_text;
    std::string _mcp_prev_user_text;
    std::string _mcp_prev_bot_text;
    std::string _mcp_escalation_task;
    std::string _mcp_escalation_user;
    std::string _mcp_reply_text;
    std::string _mcp_handoff_call_id;
    std::string _mcp_handoff_call_name;

    // Motion queue (for MCP /move, /nod, /shake, /home)
    mutable std::mutex _motion_mtx;
    std::deque<MotionCmd> _motion_cmds;
    int _gesture_type      = 0;  // 0=none, 1=nod, 2=shake
    int _gesture_step      = 0;
    uint32_t _gesture_next = 0;
    bool _speak_moving     = false;
    int _speak_move_step   = 0;
    uint32_t _speak_move_next = 0;
    int _base_yaw_deg      = 0;
    int _base_pitch_deg    = 45;

    std::unique_ptr<WebSocket> _ws;
    PcmRingBuffer _pcm_ring;
    uint8_t* _b64_decode_buf    = nullptr;
    size_t _b64_decode_cap      = 0;
    int16_t* _mic_pcm16k_buf    = nullptr;
    unsigned char* _mic_b64_buf = nullptr;
    int16_t* _preroll_buf       = nullptr;
    int _preroll_write_idx      = 0;
    int _preroll_count          = 0;

    // Adaptive client-side VAD state (net_task only)
    float _noise_floor          = 300.0f;
    int _speech_hits            = 0;
    uint32_t _speech_start_ms   = 0;
    uint32_t _last_voice_ms     = 0;

    mutable std::mutex _text_mtx;
    std::string _subtitle;
    std::string _error_msg;

    uint32_t _last_ui_ms        = 0;
    uint32_t _last_led_ms       = 0;
    uint32_t _listen_start_ms   = 0;
    int _last_rendered_state    = -1;
    bool _last_rendered_mcp     = false;
    bool _last_rendered_agent   = false;
    std::string _last_rendered_sub;

    /* ---------------------------- LVGL Widgets ---------------------------- */
    lv_obj_t* _root_cont        = nullptr;
    lv_obj_t* _lbl_badge        = nullptr;
    lv_obj_t* _lbl_vol_batt     = nullptr;

    lv_obj_t* _stage_cont       = nullptr;
    lv_image_dsc_t _img_avatar_calm  = {};
    lv_image_dsc_t _img_avatar_happy = {};
    bool _avatar_showing_happy  = false;

    // Always-on Dual-Avatar stage (Left: LIVE <Voice>, Right: AGENT $)
    lv_obj_t* _dual_stage       = nullptr;
    lv_obj_t* _slot_card[2]     = {nullptr, nullptr};
    lv_obj_t* _slot_dot[2]      = {nullptr, nullptr};
    lv_obj_t* _slot_lbl[2]      = {nullptr, nullptr};
    lv_obj_t* _dual_img[2]      = {nullptr, nullptr};
    lv_obj_t* _dual_face[2]     = {nullptr, nullptr};
    lv_obj_t* _dual_eye_l[2]    = {nullptr, nullptr};
    lv_obj_t* _dual_eye_r[2]    = {nullptr, nullptr};
    lv_obj_t* _dual_mouth[2]    = {nullptr, nullptr};
    lv_obj_t* _lbl_agent_hint   = nullptr;
    lv_obj_t* _mcp_help_card    = nullptr;
    lv_obj_t* _lbl_mcp_help     = nullptr;

    lv_obj_t* _setup_card       = nullptr;
    lv_obj_t* _lbl_setup_body   = nullptr;

    lv_obj_t* _sub_box          = nullptr;
    lv_obj_t* _lbl_subtitle     = nullptr;

    lv_obj_t* _btn_vol_dn       = nullptr;
    lv_obj_t* _btn_primary      = nullptr;
    lv_obj_t* _lbl_primary      = nullptr;
    lv_obj_t* _btn_vol_up       = nullptr;
    lv_obj_t* _btn_settings     = nullptr;
    lv_obj_t* _btn_exit         = nullptr;

    // Settings overlay (Voice, Pitch, Mic, Move)
    lv_obj_t* _settings_cont    = nullptr;
    lv_obj_t* _lbl_voice_val    = nullptr;
    lv_obj_t* _lbl_pitch_val    = nullptr;
    lv_obj_t* _btn_mic_sens[3]  = {nullptr, nullptr, nullptr};
    lv_obj_t* _btn_move_toggle  = nullptr;
    lv_obj_t* _lbl_move_toggle  = nullptr;
};
