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
#include <memory>
#include <mutex>
#include <string>

class WebSocket;

/**
 * @brief Native Gemini Live API voice assistant app for StackChan.
 * Connects directly over Wi-Fi (WSS) to Gemini Live API using a free-tier
 * Google AI Studio API key loaded from /sd/gemini.txt (and cached in NVS).
 * Uses tap-to-talk (manual activityStart/activityEnd) to save Free Tier quota
 * and avoid acoustic echo on CoreS3.
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
        Ready,
        Listening,
        Thinking,
        Speaking,
        Error,
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
    bool read_key_from_sd(std::string& out_key, std::string& out_model);
    bool load_key_from_nvs(std::string& out_key, std::string& out_model);
    void save_key_to_nvs(const std::string& key, const std::string& model);

    /* --------------------- Background Tasks & Live API -------------------- */
    void start_tasks();
    void stop_tasks();
    static void net_task_entry(void* arg);
    static void spk_task_entry(void* arg);
    void net_task_loop();
    void spk_task_loop();

    bool connect_live_session();
    void disconnect_live_session();
    bool send_setup_message();
    void handle_server_json(const char* data, size_t len);
    void stream_mic_chunk();

    /* ------------------------- User Actions ------------------------------- */
    void on_primary_button();
    void on_retry_button();
    void change_volume(int delta);
    void set_state(SessionState st);
    void set_subtitle(const std::string& text, bool append = false);
    void set_error(const std::string& msg);
    void update_leds(SessionState st, int rms);

    /* ----------------------------- LVGL UI -------------------------------- */
    void build_ui();
    void destroy_ui();
    void refresh_ui(uint32_t now);
    void refresh_face_anim(uint32_t now, SessionState st);

    /* ------------------------------- State -------------------------------- */
    std::string _api_key;
    std::string _model_name = "gemini-3.8-live";
    int _volume             = 75;

    std::atomic<int> _state{static_cast<int>(SessionState::NeedKey)};
    std::atomic<bool> _closing{false};
    std::atomic<bool> _net_task_exited{true};
    std::atomic<bool> _spk_task_exited{true};
    std::atomic<bool> _want_connect{false};
    std::atomic<bool> _want_start_talk{false};
    std::atomic<bool> _want_stop_talk{false};
    std::atomic<bool> _setup_complete{false};
    std::atomic<bool> _server_turn_done{false};
    std::atomic<bool> _new_turn_subtitle{true};
    std::atomic<int> _audio_rms{0};

    std::unique_ptr<WebSocket> _ws;
    PcmRingBuffer _pcm_ring;
    uint8_t* _b64_decode_buf    = nullptr;
    size_t _b64_decode_cap      = 0;

    mutable std::mutex _text_mtx;
    std::string _subtitle;
    std::string _error_msg;

    uint32_t _last_ui_ms        = 0;
    uint32_t _last_led_ms       = 0;
    uint32_t _listen_start_ms   = 0;
    int _last_rendered_state    = -1;
    std::string _last_rendered_sub;

    /* ---------------------------- LVGL Widgets ---------------------------- */
    lv_obj_t* _root_cont        = nullptr;
    lv_obj_t* _lbl_badge        = nullptr;
    lv_obj_t* _lbl_vol_batt     = nullptr;

    lv_obj_t* _stage_cont       = nullptr;
    lv_obj_t* _face_card        = nullptr;
    lv_obj_t* _face_eye_l       = nullptr;
    lv_obj_t* _face_eye_r       = nullptr;
    lv_obj_t* _face_mouth       = nullptr;

    lv_obj_t* _setup_card       = nullptr;
    lv_obj_t* _lbl_setup_body   = nullptr;

    lv_obj_t* _sub_box          = nullptr;
    lv_obj_t* _lbl_subtitle     = nullptr;

    lv_obj_t* _btn_vol_dn       = nullptr;
    lv_obj_t* _btn_primary      = nullptr;
    lv_obj_t* _lbl_primary      = nullptr;
    lv_obj_t* _btn_vol_up       = nullptr;
    lv_obj_t* _btn_exit         = nullptr;
};
