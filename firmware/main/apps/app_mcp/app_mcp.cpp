/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_mcp.h"

#include <apps/common/common.h>
#include <assets/assets.h>
#include <audio/audio_codec.h>
#include <board.h>
#include <hal/board/config.h>
#include <hal/board/hal_bridge.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <settings.h>
#include <stackchan/avatar/skins/default/default.h>
#include <stackchan/stackchan.h>
#include <wifi_manager.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <jpg/image_to_jpeg.h>
#include <hal/utils/jpeg_to_image/jpeg_decoder.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace mooncake;
using namespace stackchan;

namespace {

constexpr const char* TAG            = "MCP";
constexpr const char* NVS_NS         = "mcp_bridge";
constexpr const char* KEY_VOL        = "volume";
constexpr const char* KEY_AUTO       = "auto_mode";
constexpr const char* KEY_PAUSE_MS   = "pause_ms";
constexpr const char* KEY_MIC_SENS   = "mic_sens";
constexpr const char* KEY_SPEAK_MOVE = "speak_move";
constexpr int W                      = 320;
constexpr int H                      = 240;
constexpr int Y_HOME_DEG             = 45;
constexpr uint32_t REC_SAMPLE_RATE   = 16000;
constexpr size_t REC_MAX_SECONDS     = 30;
constexpr uint32_t DEFAULT_PAUSE_MS  = 1500;
constexpr uint32_t REPLY_WAIT_MS     = 90000;  // mic stays closed until a reply plays (or this timeout)
constexpr size_t PREROLL_CHUNKS      = 8;  // 8 * 40 ms = 320 ms pre-roll
constexpr size_t REC_HEADER_BYTES    = 44;
constexpr size_t REC_BUF_BYTES       = REC_HEADER_BYTES + REC_SAMPLE_RATE * 2 * REC_MAX_SECONDS;

constexpr uint32_t COL_BG            = 0x0B0F1A;
constexpr uint32_t COL_CARD          = 0x151B2E;
constexpr uint32_t COL_CARD_HI       = 0x232C4A;
constexpr uint32_t COL_TEAL          = 0x0FBAA6;
constexpr uint32_t COL_TEAL_DK       = 0x0A5E56;
constexpr uint32_t COL_BLUE          = 0x3D7DFF;
constexpr uint32_t COL_RED           = 0xE74C3C;
constexpr uint32_t COL_GREEN         = 0x2ECC71;
constexpr uint32_t COL_YELLOW        = 0xFFC940;
constexpr uint32_t COL_TEXT          = 0xE8EEF9;
constexpr uint32_t COL_DIM           = 0x7C89A8;
constexpr uint32_t COL_WHITE         = 0xFFFFFF;

const char* const NARUTO_ASSET_NAMES[7] = {
    "naruto_calm.bin",
    "naruto_thinking.bin",
    "naruto_happy.bin",
    "naruto_sleepy.bin",
    "naruto_shy.bin",
    "naruto_smug.bin",
    "naruto_pouty.bin",
};

int deg_to_yaw(int x_deg)
{
    return std::clamp(x_deg, -128, 128) * 10;
}

int deg_to_pitch(int y_deg)
{
    return (45 - std::clamp(y_deg, 0, 90)) * 10;
}

uint16_t rd_u16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

void wr_u16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void wr_u32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void write_wav_header(uint8_t* hdr, uint32_t pcm_bytes, uint32_t sample_rate)
{
    std::memcpy(hdr + 0, "RIFF", 4);
    wr_u32(hdr + 4, 36 + pcm_bytes);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    wr_u32(hdr + 16, 16);               // PCM chunk size
    wr_u16(hdr + 20, 1);                // PCM format
    wr_u16(hdr + 22, 1);                // Mono
    wr_u32(hdr + 24, sample_rate);      // Sample rate
    wr_u32(hdr + 28, sample_rate * 2);  // Byte rate
    wr_u16(hdr + 32, 2);                // Block align
    wr_u16(hdr + 34, 16);               // Bits per sample
    std::memcpy(hdr + 36, "data", 4);
    wr_u32(hdr + 40, pcm_bytes);
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

bool read_body(httpd_req_t* req, std::string& out, size_t max_len = 1024)
{
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > max_len) {
        return false;
    }
    out.resize(static_cast<size_t>(req->content_len));
    size_t got = 0;
    while (got < out.size()) {
        int r = httpd_req_recv(req, out.data() + got, out.size() - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

void send_json(httpd_req_t* req, const std::string& json, const char* status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.size()));
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                           Mooncake Lifecycle                               */
/* -------------------------------------------------------------------------- */

AppMcp::AppMcp()
{
    setAppInfo().name = "MCP";

    static lv_image_dsc_t icon_from_assets = assets::get_image("icon_mcp.bin");
    if (icon_from_assets.data != nullptr && icon_from_assets.data_size > 0) {
        setAppInfo().icon = (void*)&icon_from_assets;
    }

    static uint32_t theme_color = COL_TEAL;
    setAppInfo().userData       = (void*)&theme_color;
}

AppMcp::~AppMcp()
{
    stop_tasks();
}

void AppMcp::onCreate()
{
    mclog::tagInfo(TAG, "on create");
}

void AppMcp::onOpen()
{
    mclog::tagInfo(TAG, "on open");

    _wifi_ok         = false;
    _httpd_ok        = false;
    _playing         = false;
    _play_abort      = false;
    _play_pending    = false;
    _play_started_ms = 0;
    _speak_rms       = 0;
    _rec_start_req       = false;
    _rec_stop_req        = false;
    _rec_active          = false;
    _rec_speaking        = false;
    _rec_ready           = false;
    _waiting_reply       = false;
    _rec_wav_bytes       = 0;
    _auto_hold_until_ms  = 0;
    _mic_level           = 0;
    _agent_mode          = static_cast<int>(AgentMode::Idle);
    _face                = static_cast<int>(FaceExpr::Calm);
    _gesture_type        = 0;
    _speak_moving        = false;
    _speak_move_step     = 0;
    _speak_move_next     = 0;
    _explicit_motion_until = 0;
    _base_yaw_deg        = 0;
    _base_pitch_deg      = Y_HOME_DEG;
    _shown_naruto_idx[0] = -1;
    _shown_naruto_idx[1] = -1;

    // Load saved speaker volume, auto mode, silence pause, mic sensitivity & speak motion
    {
        Settings s(NVS_NS, true);
        int v    = s.GetInt(KEY_VOL, 75);
        int p    = s.GetInt(KEY_PAUSE_MS, static_cast<int>(DEFAULT_PAUSE_MS));
        int sens = s.GetInt(KEY_MIC_SENS, 1);
        if (p == 1600) {
            p = 1500;
            s.SetInt(KEY_PAUSE_MS, p);
        }
        _volume          = std::clamp(v, 10, 100);
        _auto_mode       = s.GetBool(KEY_AUTO, true);
        _silence_stop_ms = static_cast<uint32_t>(std::clamp(p, 500, 4000));
        _mic_sens        = std::clamp(sens, 0, 2);
        _speak_move      = s.GetBool(KEY_SPEAK_MOVE, true);
    }
    _settings_open = false;
    GetHAL().setSpeakerVolume(static_cast<uint8_t>(_volume.load()), false);

    // Load Naruto 7-state expressions (if present in assets partition)
    for (int i = 0; i < static_cast<int>(FaceExpr::Count); ++i) {
        _naruto_imgs[i] = assets::get_image(NARUTO_ASSET_NAMES[i]);
    }

    // Allocate dictation WAV buffer in PSRAM
    _rec_wav_buf = static_cast<uint8_t*>(heap_caps_malloc(REC_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _rec_wav_cap = _rec_wav_buf ? REC_BUF_BYTES : 0;

    // Initialize servos & LEDs
    GetHAL().setServoPowerEnabled(true);
    GetHAL().showRgbColor(0, 24, 28);
    auto& sc = GetStackChan();
    sc.clearModifiers();
    sc.motion().setAutoAngleSyncEnabled(false);
    sc.motion().setAutoTorqueReleaseEnabled(false);
    sc.motion().moveWithSpeed(deg_to_yaw(0), deg_to_pitch(Y_HOME_DEG), 500);

    set_status_text("Connecting Wi-Fi...");

    {
        LvglLockGuard lock;
        build_ui();
        uint32_t now = GetHAL().millis();
        refresh_ui(now);
        refresh_avatar_anim(now);
        view::create_home_indicator([&]() { close(); }, COL_TEAL, COL_BG);
        view::create_status_bar(COL_TEAL, COL_BG);
    }

    start_tasks();
}

void AppMcp::onRunning()
{
    uint32_t now = GetHAL().millis();

    process_motion_cmds();
    run_gesture_step(now);
    update_speak_motion(now);
    GetStackChan().update();

    {
        LvglLockGuard lock;
        int hal_vol = static_cast<int>(GetHAL().getSpeakerVolume());
        if (hal_vol != _volume.load()) {
            _volume = hal_vol;
            Settings s(NVS_NS, true);
            s.SetInt(KEY_VOL, hal_vol);
            if (_settings_open) {
                refresh_settings_ui();
            }
        }
        if (now - _last_anim_tick >= 90) {
            _last_anim_tick = now;
            refresh_avatar_anim(now);
        }
        if (now - _last_ui_tick >= 200) {
            _last_ui_tick = now;
            refresh_ui(now);
        }
        view::update_home_indicator();
        view::update_status_bar();
        if (_snap_new.exchange(false)) {
            show_snapshot();
        }
        if (_snap_panel && _snap_hide_ms != 0 && static_cast<int32_t>(now - _snap_hide_ms) >= 0) {
            hide_snapshot();
        }
    }
}

void AppMcp::onClose()
{
    mclog::tagInfo(TAG, "on close");

    stop_tasks();

    if (_rec_wav_buf) {
        heap_caps_free(_rec_wav_buf);
        _rec_wav_buf = nullptr;
        _rec_wav_cap = 0;
    }

    GetHAL().showRgbColor(0, 0, 0);
    GetStackChan().motion().moveWithSpeed(deg_to_yaw(0), deg_to_pitch(Y_HOME_DEG), 500);

    {
        LvglLockGuard lock;
        view::destroy_home_indicator();
        view::destroy_status_bar();
        destroy_ui();
    }

    GetHAL().requestWarmReboot(6);
}

/* -------------------------------------------------------------------------- */
/*                               Tasks & HTTP                                 */
/* -------------------------------------------------------------------------- */

void AppMcp::start_tasks()
{
    if (_running.load()) {
        return;
    }
    _running    = true;
    _net_exited = false;
    _mic_exited = false;

    // Both tasks use internal SRAM stacks (safe for Wi-Fi/NVS/HTTP/Flash)
    if (xTaskCreatePinnedToCore(net_task_entry, "mcp_net", 8192, this, 4, nullptr, 0) != pdPASS) {
        _net_exited = true;
    }
    if (xTaskCreatePinnedToCore(mic_task_entry, "mcp_mic", 4096, this, 5, nullptr, 1) != pdPASS) {
        _mic_exited = true;
    }
}

void AppMcp::stop_tasks()
{
    if (!_running.load()) {
        stop_httpd();
        return;
    }
    _play_abort   = true;
    _rec_stop_req = true;
    _running      = false;

    stop_httpd();

    for (int i = 0; i < 150 && (!_net_exited.load() || !_mic_exited.load()); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    auto audio_codec = Board::GetInstance().GetAudioCodec();
    if (audio_codec) {
        if (audio_codec->input_enabled()) {
            audio_codec->EnableInput(false);
        }
        if (audio_codec->output_enabled()) {
            audio_codec->EnableOutput(false);
        }
    }
}

void AppMcp::net_task_entry(void* arg)
{
    static_cast<AppMcp*>(arg)->net_task_loop();
    vTaskDelete(nullptr);
}

void AppMcp::net_task_loop()
{
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsConnected()) {
        GetHAL().startNetwork([this](std::string_view log) {
            if (!log.empty()) {
                set_status_text(std::string(log.data(), std::min<size_t>(log.size(), 32)));
            }
        });
        for (int i = 0; i < 75 && _running.load() && !wifi.IsConnected(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    if (!_running.load()) {
        _net_exited = true;
        return;
    }

    if (!wifi.IsConnected()) {
        set_status_text("No Wi-Fi (check SETUP)");
        GetHAL().showRgbColor(60, 0, 0);
        while (_running.load()) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        _net_exited = true;
        return;
    }

    _wifi_ok = true;
    {
        std::lock_guard<std::mutex> lk(_status_mtx);
        _ip_str = wifi.GetIpAddress();
    }

    if (start_httpd()) {
        _httpd_ok = true;
        set_status_text("Waiting for /stackchanchat...");
        GetHAL().showRgbColor(0, 18, 22);
    } else {
        set_status_text("HTTP server failed");
        GetHAL().showRgbColor(60, 0, 0);
    }

    while (_running.load()) {
        if (_play_pending.exchange(false)) {
            std::string url;
            bool wait_reply = false;
            {
                std::lock_guard<std::mutex> lk(_play_mtx);
                url        = _next_play_url;
                wait_reply = _next_play_wait_reply;
            }
            if (!url.empty()) {
                play_wav_url(url, wait_reply);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    _net_exited = true;
}

void AppMcp::mic_task_entry(void* arg)
{
    static_cast<AppMcp*>(arg)->mic_task_loop();
    vTaskDelete(nullptr);
}

void AppMcp::mic_task_loop()
{
    auto audio_codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> raw_in;
    std::vector<int16_t> mono_16k;
    float noise_floor = 130.0f;

    while (_running.load()) {
        bool manual_req     = _rec_start_req.exchange(false);
        uint32_t now_t      = GetHAL().millis();
        bool session_active = (_agent_mode.load() != static_cast<int>(AgentMode::Idle));
        bool auto_ready     = _wifi_ok.load() && _httpd_ok.load() && _auto_mode.load() && session_active &&
                              !_settings_open && !_playing.load() && !_play_pending.load() && !_rec_ready.load() &&
                              static_cast<int32_t>(now_t - _auto_hold_until_ms.load()) >= 0;

        if (!manual_req && !auto_ready) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        if (!audio_codec || !_rec_wav_buf || _rec_wav_cap <= REC_HEADER_BYTES) {
            set_status_text("Mic unavailable");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Stop any active playback before opening mic (half-duplex safety)
        if (manual_req) {
            _play_abort = true;
            for (int i = 0; i < 40 && _playing.load(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(15));
            }
        }
        if (_playing.load() || _play_pending.load()) {
            continue;
        }
        if (audio_codec->output_enabled()) {
            audio_codec->EnableOutput(false);
        }
        if (!audio_codec->input_enabled()) {
            audio_codec->EnableInput(true);
        }

        const size_t in_ch      = static_cast<size_t>(std::max(audio_codec->input_channels(), 1));
        const uint32_t in_rate  = static_cast<uint32_t>(std::max(audio_codec->input_sample_rate(), 16000));
        const size_t in_frames  = (in_rate * 40) / 1000;
        const size_t out_frames = (REC_SAMPLE_RATE * 40) / 1000;  // 640 samples (40 ms @ 16 kHz)
        raw_in.resize(in_frames * in_ch);
        mono_16k.resize(out_frames);

        // Circular 320 ms pre-roll ring so the first syllable is never clipped
        std::vector<int16_t> preroll(PREROLL_CHUNKS * out_frames, 0);
        size_t preroll_head  = 0;
        size_t preroll_count = 0;

        _rec_stop_req  = false;
        _waiting_reply = false;
        if (manual_req) {
            _rec_ready          = false;
            _auto_hold_until_ms = 0;
        }
        _rec_active   = true;
        _rec_speaking = false;
        _mic_level    = 0;
        set_status_text(_auto_mode.load() ? "Speak now (hands-free)" : "Listening... speak now");
        GetHAL().showRgbColor(0, 78, 26);

        size_t pcm_pos            = 0;
        size_t last_voice_pcm_pos = 0;
        size_t max_pcm_bytes      = _rec_wav_cap - REC_HEADER_BYTES;
        uint32_t speech_start_ms  = 0;
        uint32_t last_voice_ms    = 0;
        bool speech_heard         = false;
        int speech_streak         = 0;
        int active_voice_streak   = 0;
        int voiced_frames         = 0;
        int warmup_frames         = 0;
        float speech_peak_rms     = 400.0f;

        while (_running.load() && !_rec_stop_req.load() && !_play_pending.load() && !_playing.load()) {
            // If user turned AUTO off, returned to IDLE, or opened settings while silently waiting, exit standby cleanly
            if ((!manual_req && (!_auto_mode.load() || _agent_mode.load() == static_cast<int>(AgentMode::Idle)) &&
                 !speech_heard) ||
                (_settings_open && !speech_heard)) {
                break;
            }
            if (!audio_codec->InputData(raw_in)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Extract mic ch0 and resample in_rate (24 kHz) -> 16 kHz mono + RMS
            uint64_t sum_sq = 0;
            for (size_t i = 0; i < out_frames; ++i) {
                size_t src_i = std::min<size_t>((i * in_rate) / REC_SAMPLE_RATE, in_frames - 1);
                int16_t s    = raw_in[src_i * in_ch];
                mono_16k[i]  = s;
                sum_sq += static_cast<uint64_t>(static_cast<int32_t>(s) * static_cast<int32_t>(s));
            }
            int rms = static_cast<int>(std::sqrt(static_cast<double>(sum_sq) / out_frames));
            _mic_level.store(std::clamp(rms / 35, 0, 100));

            uint32_t now = GetHAL().millis();
            if (warmup_frames < 1) {
                noise_floor = std::clamp(static_cast<float>(rms), 100.0f, 580.0f);
                ++warmup_frames;
                continue;
            }

            int sens      = _mic_sens.load();
            float min_thr = (sens == 0) ? 540.0f : (sens == 2 ? 230.0f : 340.0f);
            float mult    = (sens == 0) ? 2.4f : (sens == 2 ? 1.65f : 1.95f);
            float thresh  = std::max(min_thr, noise_floor * mult);
            if (!speech_heard) {
                // Store chunk in pre-roll ring while waiting for voice
                std::memcpy(preroll.data() + preroll_head * out_frames, mono_16k.data(),
                            out_frames * sizeof(int16_t));
                preroll_head = (preroll_head + 1) % PREROLL_CHUNKS;
                if (preroll_count < PREROLL_CHUNKS) {
                    ++preroll_count;
                }

                if (rms > thresh) {
                    ++speech_streak;
                    if (speech_streak >= 2 || rms > thresh * 1.22f) {
                        speech_heard        = true;
                        speech_start_ms     = now;
                        last_voice_ms       = now;
                        voiced_frames       = 2;
                        active_voice_streak = 2;
                        speech_peak_rms     = std::clamp(static_cast<float>(rms), thresh, 2600.0f);
                        _rec_speaking       = true;
                        set_status_text("Hearing you... (pause to send)");
                        GetHAL().showRgbColor(88, 12, 12);

                        // Flush pre-roll into PSRAM WAV buffer under lock
                        std::lock_guard<std::mutex> lk(_rec_mtx);
                        size_t start_slot = (preroll_head + PREROLL_CHUNKS - preroll_count) % PREROLL_CHUNKS;
                        for (size_t k = 0; k < preroll_count && pcm_pos + out_frames * 2 <= max_pcm_bytes; ++k) {
                            size_t slot = (start_slot + k) % PREROLL_CHUNKS;
                            std::memcpy(_rec_wav_buf + REC_HEADER_BYTES + pcm_pos,
                                        preroll.data() + slot * out_frames, out_frames * sizeof(int16_t));
                            pcm_pos += out_frames * sizeof(int16_t);
                        }
                        last_voice_pcm_pos = pcm_pos;
                    }
                } else {
                    speech_streak = 0;
                    if (rms < noise_floor) {
                        noise_floor = noise_floor * 0.85f + rms * 0.15f;
                    } else {
                        noise_floor = noise_floor * 0.93f + rms * 0.07f;
                    }
                    noise_floor = std::clamp(noise_floor, 100.0f, 600.0f);
                }
                continue;
            }

            // Speech active: append PCM chunk to PSRAM buffer
            if (pcm_pos + out_frames * 2 <= max_pcm_bytes) {
                std::lock_guard<std::mutex> lk(_rec_mtx);
                std::memcpy(_rec_wav_buf + REC_HEADER_BYTES + pcm_pos, mono_16k.data(),
                            out_frames * sizeof(int16_t));
                pcm_pos += out_frames * sizeof(int16_t);
            } else {
                break;  // Reached 30s max cap
            }

            if (rms > thresh) {
                float capped_rms = std::min(static_cast<float>(rms), 2600.0f);
                speech_peak_rms  = std::max(speech_peak_rms * 0.994f, speech_peak_rms * 0.80f + capped_rms * 0.20f);
            } else {
                speech_peak_rms = std::max(thresh, speech_peak_rms * 0.996f);
            }

            float cont_thresh = std::max(thresh, noise_floor + (speech_peak_rms - noise_floor) * 0.28f);
            if (rms > cont_thresh) {
                ++voiced_frames;
                if (++active_voice_streak >= 2 || rms > cont_thresh * 1.22f) {
                    last_voice_ms      = now;
                    last_voice_pcm_pos = pcm_pos;
                }
            } else {
                active_voice_streak = 0;
            }

            // Auto-stop after configured silence pause following speech
            uint32_t pause_ms = _silence_stop_ms.load();
            if ((now - last_voice_ms) >= pause_ms && (now - speech_start_ms) >= 450) {
                break;
            }
            if ((now - speech_start_ms) >= REC_MAX_SECONDS * 1000) {
                break;
            }
        }

        if (audio_codec->input_enabled()) {
            audio_codec->EnableInput(false);
        }
        _mic_level    = 0;
        _rec_speaking = false;
        _rec_active   = false;

        if (speech_heard && last_voice_pcm_pos > 0) {
            // Trim trailing silence beyond 240 ms after the last voiced frame for faster Wi-Fi + ASR
            size_t tail_bytes = 6 * out_frames * sizeof(int16_t);
            pcm_pos           = std::min(pcm_pos, last_voice_pcm_pos + tail_bytes);
        }

        if (speech_heard && voiced_frames >= 5 && pcm_pos >= 6400 && !_play_pending.load() && !_playing.load()) {
            std::lock_guard<std::mutex> lk(_rec_mtx);
            write_wav_header(_rec_wav_buf, static_cast<uint32_t>(pcm_pos), REC_SAMPLE_RATE);
            _rec_wav_bytes = REC_HEADER_BYTES + pcm_pos;
            _rec_ready     = true;
            set_status_text("Sending voice to Antigravity...");
            GetHAL().showRgbColor(0, 72, 24);
        } else if (!_playing.load() && !_play_pending.load()) {
            const bool active = (_agent_mode.load() != static_cast<int>(AgentMode::Idle));
            set_status_text(!active ? "Waiting for /stackchanchat..."
                                    : (_auto_mode.load() ? "Hands-free: speak anytime" : "MCP Ready - Antigravity"));
            GetHAL().showRgbColor(0, active ? 48 : 18, active ? 42 : 22);
        }
    }

    _mic_exited = true;
}

void AppMcp::play_wav_url(const std::string& url, bool wait_reply)
{
    auto audio_codec = Board::GetInstance().GetAudioCodec();
    if (!audio_codec) {
        return;
    }

    // Stop any recording cleanly before playing speech
    if (_rec_active.load()) {
        _rec_stop_req = true;
        for (int i = 0; i < 40 && _rec_active.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

    // Clear any stale unconsumed dictation so mic always re-arms right after playback
    {
        std::lock_guard<std::mutex> lk(_rec_mtx);
        _rec_ready     = false;
        _rec_wav_bytes = 0;
    }

    _play_abort      = false;
    _waiting_reply   = false;
    _playing         = true;
    _play_started_ms = GetHAL().millis();
    _speak_rms       = 0;
    if (_face.load() == static_cast<int>(FaceExpr::Thinking)) {
        _face = static_cast<int>(FaceExpr::Calm);
    }
    set_status_text("Speaking...");
    GetHAL().showRgbColor(18, 56, 110);

    esp_http_client_config_t cfg = {};
    cfg.url                      = url.c_str();
    cfg.method                   = HTTP_METHOD_GET;
    cfg.timeout_ms               = 8000;
    cfg.buffer_size              = 4096;
    cfg.buffer_size_tx           = 1024;
    cfg.crt_bundle_attach        = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        _playing = false;
        set_status_text("HTTP client error");
        return;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        mclog::tagWarn(TAG, "Failed to open voice_url: {}", url);
        esp_http_client_cleanup(client);
        _playing = false;
        set_status_text("Cannot reach PC audio");
        GetHAL().showRgbColor(60, 20, 0);
        return;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        mclog::tagWarn(TAG, "voice_url HTTP {}", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        _playing = false;
        set_status_text("Audio HTTP " + std::to_string(status));
        return;
    }

    // Parse WAV header cleanly
    uint8_t riff[12];
    int n_riff = esp_http_client_read(client, reinterpret_cast<char*>(riff), 12);
    uint32_t src_rate = AUDIO_OUTPUT_SAMPLE_RATE;
    uint16_t channels = 1;
    bool found_data   = false;

    if (n_riff == 12 && std::memcmp(riff, "RIFF", 4) == 0 && std::memcmp(riff + 8, "WAVE", 4) == 0) {
        for (int c = 0; c < 12 && _running.load() && !_play_abort.load(); ++c) {
            uint8_t ch[8];
            if (esp_http_client_read(client, reinterpret_cast<char*>(ch), 8) != 8) {
                break;
            }
            uint32_t sz = rd_u32(ch + 4);
            if (std::memcmp(ch, "fmt ", 4) == 0) {
                uint8_t fmt[32] = {};
                size_t to_rd    = std::min<size_t>(sz, sizeof(fmt));
                if (esp_http_client_read(client, reinterpret_cast<char*>(fmt), static_cast<int>(to_rd)) >= 16) {
                    channels = rd_u16(fmt + 2);
                    src_rate = rd_u32(fmt + 4);
                }
                size_t skip = (sz > to_rd) ? (sz - to_rd) : 0;
                uint8_t dump[64];
                while (skip > 0) {
                    int r = esp_http_client_read(client, reinterpret_cast<char*>(dump),
                                                 static_cast<int>(std::min<size_t>(skip, sizeof(dump))));
                    if (r <= 0) break;
                    skip -= static_cast<size_t>(r);
                }
            } else if (std::memcmp(ch, "data", 4) == 0) {
                found_data = true;
                break;
            } else {
                size_t skip = sz + (sz & 1);
                uint8_t dump[64];
                while (skip > 0) {
                    int r = esp_http_client_read(client, reinterpret_cast<char*>(dump),
                                                 static_cast<int>(std::min<size_t>(skip, sizeof(dump))));
                    if (r <= 0) break;
                    skip -= static_cast<size_t>(r);
                }
            }
        }
    }

    if (found_data && src_rate > 0 && channels >= 1) {
        if (audio_codec->input_enabled()) {
            audio_codec->EnableInput(false);
        }
        if (!audio_codec->output_enabled()) {
            audio_codec->EnableOutput(true);
        }
        GetHAL().setSpeakerVolume(static_cast<uint8_t>(_volume.load()), false);

        constexpr size_t RAW_BYTES = 2048;
        uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(RAW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!raw) {
            raw = static_cast<uint8_t*>(std::malloc(RAW_BYTES));
        }

        std::vector<int16_t> pcm_out;
        pcm_out.reserve(1024);
        size_t leftover = 0;

        while (raw && _running.load() && !_play_abort.load()) {
            int r = esp_http_client_read(client, reinterpret_cast<char*>(raw + leftover),
                                         static_cast<int>(RAW_BYTES - leftover));
            if (r <= 0) {
                break;
            }
            size_t total       = leftover + static_cast<size_t>(r);
            size_t frame_bytes = static_cast<size_t>(channels) * 2;
            size_t frames      = total / frame_bytes;
            size_t used        = frames * frame_bytes;
            leftover           = total - used;

            if (frames > 0) {
                const int16_t* src = reinterpret_cast<const int16_t*>(raw);
                if (src_rate == AUDIO_OUTPUT_SAMPLE_RATE && channels == 1) {
                    pcm_out.assign(src, src + frames);
                } else {
                    int out_n = static_cast<int>((static_cast<uint64_t>(frames) * AUDIO_OUTPUT_SAMPLE_RATE) / src_rate);
                    pcm_out.resize(std::max(1, out_n));
                    for (int i = 0; i < out_n; ++i) {
                        uint64_t fp = (static_cast<uint64_t>(i) * src_rate << 12) / AUDIO_OUTPUT_SAMPLE_RATE;
                        int idx0    = std::min<int>(static_cast<int>(fp >> 12), static_cast<int>(frames - 1));
                        int idx1    = std::min<int>(idx0 + 1, static_cast<int>(frames - 1));
                        int frac    = static_cast<int>(fp & 0xFFF);
                        int32_t s0  = (channels >= 2)
                                          ? ((int32_t)src[idx0 * channels] + src[idx0 * channels + 1]) / 2
                                          : src[idx0];
                        int32_t s1  = (channels >= 2)
                                          ? ((int32_t)src[idx1 * channels] + src[idx1 * channels + 1]) / 2
                                          : src[idx1];
                        pcm_out[i]  = static_cast<int16_t>(std::clamp<int32_t>(s0 + (((s1 - s0) * frac) >> 12), -32768, 32767));
                    }
                }

                // Compute speaking amplitude for mouth animation
                uint64_t sq = 0;
                for (int16_t s : pcm_out) {
                    sq += static_cast<uint64_t>(static_cast<int32_t>(s) * static_cast<int32_t>(s));
                }
                _speak_rms.store(static_cast<int>(std::sqrt(static_cast<double>(sq) / pcm_out.size())));
                audio_codec->OutputData(pcm_out);
            }

            if (leftover > 0) {
                std::memmove(raw, raw + used, leftover);
            }
        }

        if (raw) {
            heap_caps_free(raw);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    _speak_rms = 0;
    _playing   = false;
    if (wait_reply && !_play_abort.load()) {
        _agent_mode         = static_cast<int>(AgentMode::Engineer);
        _face               = static_cast<int>(FaceExpr::Thinking);
        _waiting_reply      = true;
        _auto_hold_until_ms = GetHAL().millis() + REPLY_WAIT_MS;
        set_status_text("Waiting for Antigravity reply...");
        GetHAL().showRgbColor(64, 42, 0);
    } else {
        _auto_hold_until_ms = GetHAL().millis() + 120;
        set_status_text(_rec_ready.load() ? "Sending voice to Antigravity..."
                                          : (_auto_mode.load() ? "Hands-free: speak anytime" : "MCP Ready - Antigravity"));
        GetHAL().showRgbColor(0, 48, 42);
    }
}

void AppMcp::stop_current_playback()
{
    _play_abort = true;
}

void AppMcp::toggle_dictation()
{
    if (_rec_active.load()) {
        _rec_stop_req = true;
    } else {
        _rec_start_req = true;
    }
}

void AppMcp::toggle_auto_mode()
{
    bool next  = !_auto_mode.load();
    _auto_mode = next;
    Settings s(NVS_NS, true);
    s.SetBool(KEY_AUTO, next);
    if (!next && _rec_active.load() && !_rec_speaking.load()) {
        _rec_stop_req = true;
    }
    set_status_text(next ? "Hands-free AUTO ON" : "Manual DICTATE mode");
}

void AppMcp::change_volume(int delta)
{
    int v   = std::clamp(_volume.load() + delta, 0, 100);
    _volume = v;
    GetHAL().setSpeakerVolume(static_cast<uint8_t>(v), true);
    Settings s(NVS_NS, true);
    s.SetInt(KEY_VOL, v);
    refresh_settings_ui();
}

void AppMcp::change_pause_ms(int delta_ms)
{
    int cur          = static_cast<int>(_silence_stop_ms.load());
    int rounded      = ((cur + delta_ms + 50) / 100) * 100;
    int p            = std::clamp(rounded, 500, 4000);
    _silence_stop_ms = static_cast<uint32_t>(p);
    Settings s(NVS_NS, true);
    s.SetInt(KEY_PAUSE_MS, p);
    refresh_settings_ui();
}

void AppMcp::set_mic_sens(int sens)
{
    int clamped = std::clamp(sens, 0, 2);
    _mic_sens   = clamped;
    Settings s(NVS_NS, true);
    s.SetInt(KEY_MIC_SENS, clamped);
    refresh_settings_ui();
}

void AppMcp::set_speak_move(bool enabled)
{
    _speak_move = enabled;
    Settings s(NVS_NS, true);
    s.SetBool(KEY_SPEAK_MOVE, enabled);
    refresh_settings_ui();
}

void AppMcp::set_settings_open(bool open)
{
    _settings_open = open;
    if (open && _rec_active.load() && !_rec_speaking.load()) {
        _rec_stop_req = true;
    }
    if (_settings_panel) {
        if (open) {
            refresh_settings_ui();
            lv_obj_remove_flag(_settings_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(_settings_panel);
        } else {
            lv_obj_add_flag(_settings_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AppMcp::refresh_settings_ui()
{
    if (_lbl_set_pause) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1fs", _silence_stop_ms.load() / 1000.0f);
        lv_label_set_text(_lbl_set_pause, buf);
    }
    if (_lbl_set_vol) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", _volume.load());
        lv_label_set_text(_lbl_set_vol, buf);
    }
    int sens = _mic_sens.load();
    for (int i = 0; i < 3; ++i) {
        if (_btn_sens[i]) {
            lv_obj_set_style_bg_color(_btn_sens[i], lv_color_hex(i == sens ? COL_TEAL_DK : COL_CARD_HI), 0);
        }
    }
    bool mv = _speak_move.load();
    if (_btn_move_off) {
        lv_obj_set_style_bg_color(_btn_move_off, lv_color_hex(!mv ? COL_TEAL_DK : COL_CARD_HI), 0);
    }
    if (_btn_move_on) {
        lv_obj_set_style_bg_color(_btn_move_on, lv_color_hex(mv ? COL_TEAL_DK : COL_CARD_HI), 0);
    }
}

void AppMcp::process_motion_cmds()
{
    std::deque<MotionCmd> q;
    {
        std::lock_guard<std::mutex> lk(_motion_mtx);
        if (_motion_cmds.empty()) {
            return;
        }
        q.swap(_motion_cmds);
    }

    uint32_t now = GetHAL().millis();
    auto& motion = GetStackChan().motion();
    for (const auto& cmd : q) {
        _explicit_motion_until = now + 3500;
        _speak_moving          = false;
        if (cmd.kind == "move") {
            _gesture_type   = 0;
            _base_yaw_deg   = std::clamp(cmd.x_deg, -128, 128);
            _base_pitch_deg = std::clamp(cmd.y_deg, 0, 90);
            motion.moveWithSpeed(deg_to_yaw(cmd.x_deg), deg_to_pitch(cmd.y_deg), std::clamp(cmd.speed, 100, 1000));
        } else if (cmd.kind == "home") {
            _gesture_type   = 0;
            _base_yaw_deg   = 0;
            _base_pitch_deg = Y_HOME_DEG;
            motion.moveWithSpeed(deg_to_yaw(0), deg_to_pitch(Y_HOME_DEG), 500);
        } else if (cmd.kind == "nod") {
            _gesture_type = 1;
            _gesture_step = 0;
            _gesture_next = 0;
        } else if (cmd.kind == "shake") {
            _gesture_type = 2;
            _gesture_step = 0;
            _gesture_next = 0;
        }
    }
}

void AppMcp::run_gesture_step(uint32_t now)
{
    if (_gesture_type == 0 || now < _gesture_next) {
        return;
    }
    auto& motion = GetStackChan().motion();
    if (_gesture_type == 1) {  // Nod
        static const int nod_y[4] = {28, 56, 32, Y_HOME_DEG};
        motion.moveWithSpeed(deg_to_yaw(0), deg_to_pitch(nod_y[_gesture_step]), 700);
        _gesture_next = now + 260;
        if (++_gesture_step >= 4) {
            _gesture_type = 0;
        }
    } else if (_gesture_type == 2) {  // Shake
        static const int shake_x[4] = {-22, 22, -16, 0};
        motion.moveWithSpeed(deg_to_yaw(shake_x[_gesture_step]), deg_to_pitch(Y_HOME_DEG), 700);
        _gesture_next = now + 260;
        if (++_gesture_step >= 4) {
            _gesture_type = 0;
        }
    }
}

void AppMcp::update_speak_motion(uint32_t now)
{
    if (_gesture_type != 0) {
        return;
    }
    if (_explicit_motion_until != 0 && static_cast<int32_t>(now - _explicit_motion_until) < 0) {
        return;
    }

    auto& motion = GetStackChan().motion();
    if (_playing.load() && _speak_move.load()) {
        if (!_speak_moving) {
            _base_yaw_deg    = std::clamp(motion.getCurrentYawAngle() / 10, -120, 120);
            _base_pitch_deg  = std::clamp(45 - (motion.getCurrentPitchAngle() / 10), 15, 75);
            _speak_moving    = true;
            _speak_move_step = 0;
            _speak_move_next = now;
        }
        if (static_cast<int32_t>(now - _speak_move_next) >= 0) {
            if (_speak_rms.load() > 450) {
                static const int dx_pat[4] = {-5, 4, 6, -4};
                static const int dy_pat[4] = {-6, 5, -4, 6};
                int tx = std::clamp(_base_yaw_deg + dx_pat[_speak_move_step & 3], -120, 120);
                int ty = std::clamp(_base_pitch_deg + dy_pat[_speak_move_step & 3], 15, 75);
                motion.moveWithSpeed(deg_to_yaw(tx), deg_to_pitch(ty), 360);
                ++_speak_move_step;
                _speak_move_next = now + 420;
            } else {
                _speak_move_next = now + 120;
            }
        }
    } else if (_speak_moving) {
        motion.moveWithSpeed(deg_to_yaw(_base_yaw_deg), deg_to_pitch(_base_pitch_deg), 380);
        _speak_moving = false;
        uint32_t settle_until = now + 450;
        uint32_t cur_hold     = _auto_hold_until_ms.load();
        if (static_cast<int32_t>(settle_until - cur_hold) > 0) {
            _auto_hold_until_ms.store(settle_until);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                              HTTP Server                                   */
/* -------------------------------------------------------------------------- */

bool AppMcp::start_httpd()
{
    if (_httpd) {
        return true;
    }
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.stack_size        = 8192;
    cfg.max_uri_handlers  = 16;
    cfg.max_open_sockets  = 7;
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.task_priority     = 4;

    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        return false;
    }

    const httpd_uri_t uris[] = {
        {"/audio/status",    HTTP_GET,  handle_audio_status,    this},
        {"/audio",           HTTP_GET,  handle_audio_get,       this},
        {"/playback/status", HTTP_GET,  handle_playback_status, this},
        {"/play",            HTTP_POST, handle_play,            this},
        {"/move",            HTTP_POST, handle_move,            this},
        {"/nod",             HTTP_POST, handle_nod,             this},
        {"/shake",           HTTP_POST, handle_shake,           this},
        {"/home",            HTTP_POST, handle_home,            this},
        {"/face",            HTTP_POST, handle_face,            this},
        {"/servo/status",    HTTP_GET,  handle_servo_status,    this},
        {"/booth",           HTTP_GET,  handle_booth_get,       this},
        {"/booth",           HTTP_POST, handle_booth_post,      this},
        {"/listen/resume",   HTTP_POST, handle_listen_resume,   this},
        {"/agent_mode",      HTTP_POST, handle_agent_mode,      this},
        {"/snapshot",        HTTP_GET,  handle_snapshot,        this},
    };
    for (const auto& u : uris) {
        httpd_register_uri_handler(srv, &u);
    }

    _httpd = srv;
    mclog::tagInfo(TAG, "MCP HTTP server ready on port 80");
    return true;
}

void AppMcp::stop_httpd()
{
    if (_httpd) {
        httpd_stop(static_cast<httpd_handle_t>(_httpd));
        _httpd = nullptr;
    }
    _httpd_ok = false;
}

int AppMcp::handle_audio_status(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    std::string ip;
    {
        std::lock_guard<std::mutex> lk(self->_status_mtx);
        ip = self->_ip_str;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ready", self->_rec_ready.load());
    cJSON_AddBoolToObject(root, "recording", self->_rec_active.load());
    cJSON_AddNumberToObject(root, "bytes", static_cast<double>(self->_rec_wav_bytes));
    cJSON_AddStringToObject(root, "mode", "mcp");
    const int amode       = self->_agent_mode.load();
    const char* amode_str = (amode == static_cast<int>(AgentMode::Free))       ? "free"
                            : (amode == static_cast<int>(AgentMode::Engineer)) ? "engineer"
                                                                               : "idle";
    cJSON_AddStringToObject(root, "agent_mode", amode_str);
    cJSON_AddStringToObject(root, "ip", ip.c_str());
    cJSON_AddStringToObject(root, "face", face_name(static_cast<FaceExpr>(self->_face.load())));
    char* s = cJSON_PrintUnformatted(root);
    send_json(req, s ? s : "{}");
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

int AppMcp::handle_audio_get(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    if (!self->_rec_ready.load()) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "No recording ready", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    std::lock_guard<std::mutex> lk(self->_rec_mtx);
    if (!self->_rec_wav_buf || self->_rec_wav_bytes <= REC_HEADER_BYTES) {
        self->_rec_ready = false;
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "Empty recording", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    size_t sent = 0;
    while (sent < self->_rec_wav_bytes) {
        size_t chunk = std::min<size_t>(16384, self->_rec_wav_bytes - sent);
        if (httpd_resp_send_chunk(req, reinterpret_cast<const char*>(self->_rec_wav_buf + sent),
                                  static_cast<ssize_t>(chunk)) != ESP_OK) {
            break;
        }
        sent += chunk;
    }
    httpd_resp_send_chunk(req, nullptr, 0);

    self->_rec_ready          = false;
    self->_rec_wav_bytes      = 0;
    self->_face               = static_cast<int>(FaceExpr::Thinking);
    self->_waiting_reply      = true;
    self->_auto_hold_until_ms = GetHAL().millis() + REPLY_WAIT_MS;
    self->set_status_text("Waiting for Antigravity reply...");
    GetHAL().showRgbColor(64, 42, 0);
    return ESP_OK;
}

int AppMcp::handle_playback_status(httpd_req* req)
{
    auto* self  = static_cast<AppMcp*>(req->user_ctx);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "playing", self->_playing.load());
    cJSON_AddNumberToObject(root, "started_ms", static_cast<double>(self->_play_started_ms.load()));
    cJSON_AddStringToObject(root, "kind", "wav");
    char* s = cJSON_PrintUnformatted(root);
    send_json(req, s ? s : "{}");
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

int AppMcp::handle_play(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    std::string body;
    if (!read_body(req, body, 1024)) {
        send_json(req, "{\"success\":false,\"error\":\"Invalid body\"}", "400 Bad Request");
        return ESP_OK;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        send_json(req, "{\"success\":false,\"error\":\"Invalid JSON\"}", "400 Bad Request");
        return ESP_OK;
    }
    cJSON* jurl = cJSON_GetObjectItem(root, "voice_url");
    if (!cJSON_IsString(jurl) || !jurl->valuestring || jurl->valuestring[0] == '\0') {
        cJSON_Delete(root);
        send_json(req, "{\"success\":false,\"error\":\"Missing voice_url\"}", "400 Bad Request");
        return ESP_OK;
    }

    cJSON* jwait    = cJSON_GetObjectItem(root, "wait_reply");
    bool wait_reply = cJSON_IsTrue(jwait);

    {
        std::lock_guard<std::mutex> lk(self->_play_mtx);
        self->_next_play_url        = jurl->valuestring;
        self->_next_play_wait_reply = wait_reply;
    }
    cJSON_Delete(root);

    {
        std::lock_guard<std::mutex> lk(self->_rec_mtx);
        self->_rec_ready     = false;
        self->_rec_wav_bytes = 0;
    }
    self->_play_abort   = true;
    self->_play_pending = true;
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppMcp::handle_move(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    std::string body;
    if (!read_body(req, body, 512)) {
        send_json(req, "{\"success\":false}", "400 Bad Request");
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        send_json(req, "{\"success\":false}", "400 Bad Request");
        return ESP_OK;
    }
    MotionCmd cmd;
    cmd.kind = "move";
    if (auto* jx = cJSON_GetObjectItem(root, "x"); cJSON_IsNumber(jx)) cmd.x_deg = static_cast<int>(jx->valuedouble);
    if (auto* jy = cJSON_GetObjectItem(root, "y"); cJSON_IsNumber(jy)) cmd.y_deg = static_cast<int>(jy->valuedouble);
    if (auto* js = cJSON_GetObjectItem(root, "speed"); cJSON_IsNumber(js)) cmd.speed = static_cast<int>(js->valuedouble) * 10;
    cJSON_Delete(root);

    {
        std::lock_guard<std::mutex> lk(self->_motion_mtx);
        self->_motion_cmds.push_back(cmd);
    }
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppMcp::handle_nod(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    std::lock_guard<std::mutex> lk(self->_motion_mtx);
    self->_motion_cmds.push_back({"nod", 0, Y_HOME_DEG, 700});
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppMcp::handle_shake(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    std::lock_guard<std::mutex> lk(self->_motion_mtx);
    self->_motion_cmds.push_back({"shake", 0, Y_HOME_DEG, 700});
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppMcp::handle_home(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    std::lock_guard<std::mutex> lk(self->_motion_mtx);
    self->_motion_cmds.push_back({"home", 0, Y_HOME_DEG, 500});
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppMcp::handle_face(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    std::string body;
    if (!read_body(req, body, 256)) {
        send_json(req, "{\"success\":false}", "400 Bad Request");
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        send_json(req, "{\"success\":false}", "400 Bad Request");
        return ESP_OK;
    }
    cJSON* jf = cJSON_GetObjectItem(root, "face");
    if (cJSON_IsString(jf) && jf->valuestring) {
        self->_face.store(static_cast<int>(parse_face(jf->valuestring)));
    }
    cJSON_Delete(root);
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppMcp::handle_servo_status(httpd_req* req)
{
    auto& motion = GetStackChan().motion();
    int x_deg    = motion.getCurrentYawAngle() / 10;
    int y_deg    = 45 - (motion.getCurrentPitchAngle() / 10);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{\"x\":%d,\"y\":%d}", x_deg, y_deg);
    send_json(req, buf);
    return ESP_OK;
}

int AppMcp::handle_booth_get(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "{\"success\":true,\"booth_mode\":false,\"persisted\":true,\"capture_allowed\":true,\"mic_running\":%s}",
                  self->_rec_active.load() ? "true" : "false");
    send_json(req, buf);
    return ESP_OK;
}

int AppMcp::handle_booth_post(httpd_req* req)
{
    return handle_booth_get(req);
}

int AppMcp::handle_snapshot(httpd_req* req)
{
    auto* camera = hal_bridge::board_get_camera();
    if (!camera) {
        send_json(req, "{\"success\":false,\"error\":\"Camera unavailable\"}", "503 Service Unavailable");
        return ESP_OK;
    }

    auto frame_lock = camera->TryLockFrame();
    if (!frame_lock.owns_lock()) {
        send_json(req, "{\"success\":false,\"error\":\"Camera busy\"}", "503 Service Unavailable");
        return ESP_OK;
    }

    bool captured = false;
    for (int attempt = 0; attempt < 3 && !captured; ++attempt) {
        captured = camera->StreamCaptures();
    }
    if (!captured) {
        send_json(req, "{\"success\":false,\"error\":\"Capture failed\"}", "500 Internal Server Error");
        return ESP_OK;
    }

    uint8_t* jpeg_data = nullptr;
    size_t jpeg_len    = 0;
    bool encoded       = image_to_jpeg(const_cast<uint8_t*>(camera->GetFrameData()), camera->GetFrameSize(),
                                       camera->GetFrameWidth(), camera->GetFrameHeight(),
                                       static_cast<v4l2_pix_fmt_t>(camera->GetFrameFormat()), 80, &jpeg_data,
                                       &jpeg_len);
    if (!encoded || !jpeg_data || jpeg_len == 0) {
        if (jpeg_data) {
            free(jpeg_data);
        }
        send_json(req, "{\"success\":false,\"error\":\"JPEG encode failed\"}", "500 Internal Server Error");
        return ESP_OK;
    }

    bool is_warmup = false;
    char query[48] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        std::strstr(query, "preview=0") != nullptr) {
        is_warmup = true;
    }

    if (!is_warmup) {
        auto* self   = static_cast<AppMcp*>(req->user_ctx);
        auto decoded = jpeg_dec::decode_to_lvgl(jpeg_data, jpeg_len);
        if (decoded) {
            std::lock_guard<std::mutex> lk(self->_snap_mtx);
            self->_snap_img = decoded;
            self->_snap_new = true;
        }
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t sent = 0;
    while (sent < jpeg_len) {
        size_t chunk = std::min<size_t>(8192, jpeg_len - sent);
        if (httpd_resp_send_chunk(req, reinterpret_cast<const char*>(jpeg_data + sent), static_cast<ssize_t>(chunk)) !=
            ESP_OK) {
            break;
        }
        sent += chunk;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    free(jpeg_data);
    return ESP_OK;
}

int AppMcp::handle_listen_resume(httpd_req* req)
{
    auto* self                = static_cast<AppMcp*>(req->user_ctx);
    self->_waiting_reply      = false;
    self->_auto_hold_until_ms = 0;
    if (self->_face.load() == static_cast<int>(FaceExpr::Thinking)) {
        self->_face = static_cast<int>(FaceExpr::Calm);
    }
    if (!self->_playing.load() && !self->_play_pending.load()) {
        self->set_status_text(self->_auto_mode.load() ? "Hands-free: speak anytime" : "MCP Ready - Antigravity");
        GetHAL().showRgbColor(0, 48, 42);
    }
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppMcp::handle_agent_mode(httpd_req* req)
{
    auto* self = static_cast<AppMcp*>(req->user_ctx);
    std::string body;
    if (!read_body(req, body, 256)) {
        send_json(req, "{\"success\":false,\"error\":\"Invalid body\"}", "400 Bad Request");
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        send_json(req, "{\"success\":false,\"error\":\"Invalid JSON\"}", "400 Bad Request");
        return ESP_OK;
    }
    cJSON* jmode = cJSON_GetObjectItem(root, "mode");
    std::string m = (cJSON_IsString(jmode) && jmode->valuestring) ? jmode->valuestring : "";
    cJSON_Delete(root);

    if (m == "free" || m == "flash" || m == "casual") {
        self->_agent_mode = static_cast<int>(AgentMode::Free);
    } else if (m == "engineer" || m == "agent" || m == "ide" || m == "antigravity") {
        self->_agent_mode = static_cast<int>(AgentMode::Engineer);
    } else {
        self->_agent_mode = static_cast<int>(AgentMode::Idle);
        if (!self->_playing.load() && !self->_play_pending.load() && !self->_rec_speaking.load()) {
            self->set_status_text("Waiting for /stackchanchat...");
            GetHAL().showRgbColor(0, 18, 22);
        }
    }
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                                LVGL UI                                     */
/* -------------------------------------------------------------------------- */

void AppMcp::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    _root = lv_obj_create(scr);
    lv_obj_set_size(_root, W, H);
    lv_obj_set_pos(_root, 0, 0);
    style_box(_root, COL_BG, 0);

    const int top_y   = view::get_status_bar_top_offset();
    const bool is_fix = (top_y > 0);
    const int hdr_y   = top_y + (is_fix ? 2 : 5);
    const int stage_y = top_y + (is_fix ? 22 : 26);
    const int stage_h = 180 - stage_y;

    // App header below system status bar: MCP IP across full width (battery & volume are in top bar)
    _lbl_ip = lv_label_create(_root);
    lv_obj_set_pos(_lbl_ip, 10, hdr_y);
    lv_obj_set_size(_lbl_ip, 300, 18);
    lv_label_set_long_mode(_lbl_ip, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_ip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_ip, lv_color_hex(COL_TEAL), 0);
    lv_label_set_text(_lbl_ip, "MCP  Wi-Fi...");
    _lbl_batt = nullptr;

    // Center stage (non-clickable so accidental taps on avatars never interrupt playback/flow)
    _stage = lv_obj_create(_root);
    lv_obj_set_pos(_stage, 8, stage_y);
    lv_obj_set_size(_stage, 304, stage_h);
    style_box(_stage, COL_CARD, 14);
    lv_obj_set_style_border_width(_stage, 1, 0);
    lv_obj_set_style_border_color(_stage, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_remove_flag(_stage, LV_OBJ_FLAG_CLICKABLE);

    // Dual side-by-side avatar slots: Left = FREE (Flash-Lite), Right = AGENT $ (Coding Agent)
    // Supports both Naruto sprites (custom/naruto) and classic StackChan face fallback (public)
    const int slot_w       = 138;
    const int slot_h       = stage_h - 32;
    const int slot_x[2]    = {8, 158};
    const char* slot_txt[2] = {"FREE", "AGENT $"};
    const int naruto_scale = is_fix ? 104 : 114;

    for (int i = 0; i < 2; ++i) {
        _slot_card[i] = lv_obj_create(_stage);
        lv_obj_set_pos(_slot_card[i], slot_x[i], 6);
        lv_obj_set_size(_slot_card[i], slot_w, slot_h);
        style_box(_slot_card[i], 0x0F172A, 12);
        lv_obj_set_style_border_width(_slot_card[i], 1, 0);
        lv_obj_set_style_border_color(_slot_card[i], lv_color_hex(0x24314F), 0);
        lv_obj_remove_flag(_slot_card[i], LV_OBJ_FLAG_CLICKABLE);

        _slot_dot[i] = lv_obj_create(_slot_card[i]);
        lv_obj_set_size(_slot_dot[i], 7, 7);
        lv_obj_set_pos(_slot_dot[i], 8, 8);
        style_box(_slot_dot[i], 0x64748B, LV_RADIUS_CIRCLE);
        lv_obj_remove_flag(_slot_dot[i], LV_OBJ_FLAG_CLICKABLE);

        _slot_lbl[i] = lv_label_create(_slot_card[i]);
        lv_obj_set_pos(_slot_lbl[i], 19, 5);
        lv_obj_set_style_text_font(_slot_lbl[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(_slot_lbl[i], lv_color_hex(0x94A3B8), 0);
        lv_label_set_text(_slot_lbl[i], slot_txt[i]);

        // Naruto avatar image (scaled cleanly inside slot)
        _avatar_img[i] = lv_image_create(_slot_card[i]);
        if (_naruto_imgs[0].data != nullptr) {
            lv_image_set_src(_avatar_img[i], &_naruto_imgs[0]);
            lv_image_set_scale(_avatar_img[i], naruto_scale);
        }
        lv_obj_align(_avatar_img[i], LV_ALIGN_CENTER, 0, 8);
        lv_obj_remove_flag(_avatar_img[i], LV_OBJ_FLAG_CLICKABLE);

        // Classic StackChan face fallback (used when Naruto sprites are not active/present)
        _face_card[i] = lv_obj_create(_slot_card[i]);
        lv_obj_set_size(_face_card[i], 96, is_fix ? 56 : 62);
        lv_obj_align(_face_card[i], LV_ALIGN_CENTER, 0, 9);
        style_box(_face_card[i], COL_BG, 12);
        lv_obj_set_style_border_width(_face_card[i], 2, 0);
        lv_obj_set_style_border_color(_face_card[i], lv_color_hex(0x24314F), 0);
        lv_obj_remove_flag(_face_card[i], LV_OBJ_FLAG_CLICKABLE);

        _face_eye_l[i] = lv_obj_create(_face_card[i]);
        lv_obj_set_size(_face_eye_l[i], 14, 14);
        lv_obj_align(_face_eye_l[i], LV_ALIGN_CENTER, -20, -6);
        style_box(_face_eye_l[i], COL_WHITE, LV_RADIUS_CIRCLE);

        _face_eye_r[i] = lv_obj_create(_face_card[i]);
        lv_obj_set_size(_face_eye_r[i], 14, 14);
        lv_obj_align(_face_eye_r[i], LV_ALIGN_CENTER, 20, -6);
        style_box(_face_eye_r[i], COL_WHITE, LV_RADIUS_CIRCLE);

        _face_mouth[i] = lv_obj_create(_face_card[i]);
        lv_obj_set_size(_face_mouth[i], 28, 5);
        lv_obj_align(_face_mouth[i], LV_ALIGN_CENTER, 0, 14);
        style_box(_face_mouth[i], COL_WHITE, 3);
    }

    // Status line + live mic/speak meter at bottom of stage
    _lbl_status = lv_label_create(_stage);
    lv_obj_align(_lbl_status, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_width(_lbl_status, 286);
    lv_obj_set_style_text_align(_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_lbl_status, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(COL_TEXT), 0);

    _bar_level_bg = lv_obj_create(_stage);
    lv_obj_align(_bar_level_bg, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_size(_bar_level_bg, 260, 4);
    style_box(_bar_level_bg, COL_CARD_HI, 2);

    _bar_level = lv_obj_create(_bar_level_bg);
    lv_obj_set_pos(_bar_level, 0, 0);
    lv_obj_set_size(_bar_level, 0, 4);
    style_box(_bar_level, COL_TEAL, 2);

    // Bottom 3-button bar: TALK | AUTO | SET (Exit is in the top status bar)
    _btn_dictate = lv_button_create(_root);
    lv_obj_set_pos(_btn_dictate, 8, 186);
    lv_obj_set_size(_btn_dictate, 148, 44);
    style_box(_btn_dictate, COL_TEAL_DK, 10);
    _lbl_dictate = lv_label_create(_btn_dictate);
    lv_obj_center(_lbl_dictate);
    lv_obj_set_style_text_font(_lbl_dictate, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_dictate, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(_lbl_dictate, LV_SYMBOL_AUDIO " TALK");
    lv_obj_add_event_cb(
        _btn_dictate,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->toggle_dictation();
        },
        LV_EVENT_CLICKED, this);

    _btn_auto = lv_button_create(_root);
    lv_obj_set_pos(_btn_auto, 162, 186);
    lv_obj_set_size(_btn_auto, 74, 44);
    style_box(_btn_auto, COL_GREEN, 10);
    _lbl_auto = lv_label_create(_btn_auto);
    lv_obj_center(_lbl_auto);
    lv_obj_set_style_text_align(_lbl_auto, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_auto, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_auto, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(_lbl_auto, "AUTO\nON");
    lv_obj_add_event_cb(
        _btn_auto,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->toggle_auto_mode();
        },
        LV_EVENT_CLICKED, this);

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

    lv_obj_t* btn_set = make_btn(_root, 242, 186, 70, 44, LV_SYMBOL_SETTINGS " SET", COL_CARD, COL_TEXT);
    lv_obj_add_event_cb(
        btn_set,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->set_settings_open(true);
        },
        LV_EVENT_CLICKED, this);

    // Settings overlay (hidden by default): Silence Pause, Mic Sensitivity, Volume, Speak Move
    _settings_panel = lv_obj_create(_root);
    lv_obj_set_pos(_settings_panel, 0, 0);
    lv_obj_set_size(_settings_panel, W, H);
    style_box(_settings_panel, COL_BG, 0);
    lv_obj_add_flag(_settings_panel, LV_OBJ_FLAG_HIDDEN);

    const int set_hdr_y = top_y + (is_fix ? 2 : 8);
    const int row_h     = is_fix ? 39 : 43;
    const int row_y0    = top_y + (is_fix ? 33 : 42);
    const int row_step  = is_fix ? 44 : 48;
    const int btn_ry    = is_fix ? 4 : 5;
    const int btn_rh    = is_fix ? 31 : 33;
    const int lbl_ry    = is_fix ? 12 : 14;

    lv_obj_t* lbl_set_title = lv_label_create(_settings_panel);
    lv_obj_set_pos(lbl_set_title, 12, set_hdr_y + 4);
    lv_obj_set_style_text_font(lbl_set_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_set_title, lv_color_hex(COL_TEAL), 0);
    lv_label_set_text(lbl_set_title, LV_SYMBOL_SETTINGS " MCP SETTINGS");

    lv_obj_t* btn_back = make_btn(_settings_panel, 240, set_hdr_y, 70, 28, LV_SYMBOL_LEFT " BACK", COL_TEAL_DK, COL_WHITE);
    lv_obj_add_event_cb(
        btn_back,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->set_settings_open(false);
        },
        LV_EVENT_CLICKED, this);

    auto make_row = [&](int y, const char* title) {
        lv_obj_t* card = lv_obj_create(_settings_panel);
        lv_obj_set_pos(card, 10, y);
        lv_obj_set_size(card, 300, row_h);
        style_box(card, COL_CARD, 10);
        lv_obj_t* l = lv_label_create(card);
        lv_obj_set_pos(l, 12, lbl_ry + 1);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
        lv_label_set_text(l, title);
        return card;
    };

    // Row 1: Silence Pause (0.5s .. 4.0s in 0.1s steps)
    lv_obj_t* row_pause = make_row(row_y0, "PAUSE");
    lv_obj_t* btn_pm    = make_btn(row_pause, 114, btn_ry, 48, btn_rh, "-", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_pm,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->change_pause_ms(-100);
        },
        LV_EVENT_CLICKED, this);

    _lbl_set_pause = lv_label_create(row_pause);
    lv_obj_set_pos(_lbl_set_pause, 164, lbl_ry);
    lv_obj_set_width(_lbl_set_pause, 72);
    lv_obj_set_style_text_align(_lbl_set_pause, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_set_pause, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_set_pause, lv_color_hex(COL_WHITE), 0);

    lv_obj_t* btn_pp = make_btn(row_pause, 238, btn_ry, 48, btn_rh, "+", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_pp,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->change_pause_ms(100);
        },
        LV_EVENT_CLICKED, this);

    // Row 2: Mic Sensitivity (LOW / MED / HIGH)
    lv_obj_t* row_mic          = make_row(row_y0 + row_step, "MIC");
    const char* sens_labels[3] = {"LOW", "MED", "HIGH"};
    for (int i = 0; i < 3; ++i) {
        _btn_sens[i] = make_btn(row_mic, 114 + i * 59, btn_ry, 54, btn_rh, sens_labels[i], COL_CARD_HI, COL_WHITE);
        lv_obj_set_user_data(_btn_sens[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            _btn_sens[i],
            [](lv_event_t* e) {
                auto* self = static_cast<AppMcp*>(lv_event_get_user_data(e));
                int idx    = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
                self->set_mic_sens(idx);
            },
            LV_EVENT_CLICKED, this);
    }

    // Row 3: Speaker Volume (10% .. 100%)
    lv_obj_t* row_vol = make_row(row_y0 + row_step * 2, "VOLUME");
    lv_obj_t* btn_vm  = make_btn(row_vol, 114, btn_ry, 48, btn_rh, "-", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_vm,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->change_volume(-10);
        },
        LV_EVENT_CLICKED, this);

    _lbl_set_vol = lv_label_create(row_vol);
    lv_obj_set_pos(_lbl_set_vol, 164, lbl_ry);
    lv_obj_set_width(_lbl_set_vol, 72);
    lv_obj_set_style_text_align(_lbl_set_vol, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_set_vol, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_set_vol, lv_color_hex(COL_WHITE), 0);

    lv_obj_t* btn_vp = make_btn(row_vol, 238, btn_ry, 48, btn_rh, "+", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        btn_vp,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->change_volume(10);
        },
        LV_EVENT_CLICKED, this);

    // Row 4: Speak Motion (OFF / ON)
    lv_obj_t* row_move = make_row(row_y0 + row_step * 3, "MOVE");
    _btn_move_off      = make_btn(row_move, 114, btn_ry, 84, btn_rh, "OFF", COL_CARD_HI, COL_WHITE);
    lv_obj_add_event_cb(
        _btn_move_off,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->set_speak_move(false);
        },
        LV_EVENT_CLICKED, this);

    _btn_move_on = make_btn(row_move, 202, btn_ry, 84, btn_rh, "ON", COL_TEAL_DK, COL_WHITE);
    lv_obj_add_event_cb(
        _btn_move_on,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->set_speak_move(true);
        },
        LV_EVENT_CLICKED, this);

    refresh_settings_ui();

    // Camera preview overlay (shown when a snapshot is taken via MCP; tap to close)
    _snap_panel = lv_obj_create(_root);
    lv_obj_set_pos(_snap_panel, 0, top_y);
    lv_obj_set_size(_snap_panel, W, H - top_y);
    style_box(_snap_panel, COL_BG, 0);
    lv_obj_add_flag(_snap_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_snap_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        _snap_panel,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->hide_snapshot();
        },
        LV_EVENT_CLICKED, this);

    _snap_view = lv_image_create(_snap_panel);
    lv_obj_align(_snap_view, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(_snap_view, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl_snap = lv_label_create(_snap_panel);
    lv_obj_align(lbl_snap, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_font(lbl_snap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_snap, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_bg_color(lbl_snap, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(lbl_snap, LV_OPA_70, 0);
    lv_label_set_text(lbl_snap, LV_SYMBOL_IMAGE " CAMERA - tap to close");
}

void AppMcp::show_snapshot()
{
    if (!_snap_panel || !_snap_view) {
        return;
    }
    std::shared_ptr<LvglImage> img;
    {
        std::lock_guard<std::mutex> lk(_snap_mtx);
        img.swap(_snap_img);
    }
    if (!img) {
        return;
    }
    const lv_img_dsc_t* dsc = img->image_dsc();
    const int avail_h       = H - view::get_status_bar_top_offset();
    const int scale_w       = (256 * W) / std::max<int>(dsc->header.w, 1);
    const int scale_h       = (256 * avail_h) / std::max<int>(dsc->header.h, 1);
    lv_image_set_src(_snap_view, dsc);
    _snap_shown_img = std::move(img);
    lv_image_set_scale(_snap_view, static_cast<uint32_t>(std::min(scale_w, scale_h)));
    lv_obj_align(_snap_view, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(_snap_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_snap_panel);
    _snap_hide_ms = GetHAL().millis() + 8000;
}

void AppMcp::hide_snapshot()
{
    _snap_hide_ms = 0;
    if (_snap_panel) {
        lv_obj_add_flag(_snap_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (_snap_view) {
        lv_image_set_src(_snap_view, nullptr);
    }
    _snap_shown_img.reset();
    std::lock_guard<std::mutex> lk(_snap_mtx);
    _snap_img.reset();
}

void AppMcp::destroy_ui()
{
    if (_root) {
        if (_snap_view) {
            lv_image_set_src(_snap_view, nullptr);
        }
        lv_obj_delete(_root);
        _root           = nullptr;
        for (int i = 0; i < 2; ++i) {
            _slot_card[i]  = nullptr;
            _slot_dot[i]   = nullptr;
            _slot_lbl[i]   = nullptr;
            _avatar_img[i] = nullptr;
            _face_card[i]  = nullptr;
            _face_eye_l[i] = nullptr;
            _face_eye_r[i] = nullptr;
            _face_mouth[i] = nullptr;
        }
        _snap_panel     = nullptr;
        _snap_view      = nullptr;
        _snap_hide_ms   = 0;
        _snap_shown_img.reset();
        {
            std::lock_guard<std::mutex> lk(_snap_mtx);
            _snap_img.reset();
        }
        _snap_new       = false;
        _settings_panel = nullptr;
        _lbl_set_pause  = nullptr;
        _lbl_set_vol    = nullptr;
        for (int i = 0; i < 3; ++i) _btn_sens[i] = nullptr;
        _btn_move_off   = nullptr;
        _btn_move_on    = nullptr;
    }
}

void AppMcp::refresh_ui(uint32_t now)
{
    if (!_root) {
        return;
    }

    bool rec     = _rec_active.load();
    bool hearing = _rec_speaking.load();
    bool ready   = _rec_ready.load();
    bool spk     = _playing.load();
    bool auto_on = _auto_mode.load();
    bool waiting = _waiting_reply.load();

    if (waiting && static_cast<int32_t>(now - _auto_hold_until_ms.load()) >= 0) {
        if (_waiting_reply.exchange(false)) {
            if (_face.load() == static_cast<int>(FaceExpr::Thinking)) {
                _face = static_cast<int>(FaceExpr::Calm);
            }
            if (!rec && !spk && !ready && !_play_pending.load()) {
                set_status_text(auto_on ? "Hands-free: speak anytime" : "MCP Ready - Antigravity");
                GetHAL().showRgbColor(0, 48, 42);
            }
        }
        waiting = false;
    }

    std::string ip;
    std::string st;
    {
        std::lock_guard<std::mutex> lk(_status_mtx);
        ip = _ip_str;
        st = _status_text;
    }

    if (_httpd_ok.load() && !ip.empty()) {
        char hdr[48];
        std::snprintf(hdr, sizeof(hdr), "MCP %s  %.1fs", ip.c_str(), _silence_stop_ms.load() / 1000.0f);
        lv_obj_set_style_text_color(_lbl_ip, lv_color_hex(COL_TEAL), 0);
        lv_label_set_text(_lbl_ip, hdr);
    } else {
        lv_obj_set_style_text_color(_lbl_ip, lv_color_hex(COL_YELLOW), 0);
        lv_label_set_text(_lbl_ip, "MCP  Connecting...");
    }

    lv_label_set_text(_lbl_status, st.c_str());

    if (_slot_card[0] && _slot_card[1] && _stage) {
        const auto amode      = static_cast<AgentMode>(_agent_mode.load());
        const bool free_on    = (amode == AgentMode::Free);
        const bool agent_on   = (amode == AgentMode::Engineer);
        const uint32_t bg[2]  = {free_on ? 0x112624U : 0x0F172AU, agent_on ? 0x2A1E10U : 0x0F172AU};
        const uint32_t brd[2] = {free_on ? 0x34D399U : 0x24314FU, agent_on ? 0xFBBF24U : 0x24314FU};
        const uint32_t txt[2] = {free_on ? 0x34D399U : 0x64748BU, agent_on ? 0xFBBF24U : 0x64748BU};
        const int bw[2]       = {free_on ? 2 : 1, agent_on ? 2 : 1};
        const lv_opa_t opa[2] = {
            free_on ? LV_OPA_COVER : (agent_on ? LV_OPA_40 : LV_OPA_50),
            agent_on ? LV_OPA_COVER : (free_on ? LV_OPA_40 : LV_OPA_50),
        };
        for (int i = 0; i < 2; ++i) {
            lv_obj_set_style_bg_color(_slot_card[i], lv_color_hex(bg[i]), 0);
            lv_obj_set_style_border_color(_slot_card[i], lv_color_hex(brd[i]), 0);
            lv_obj_set_style_border_width(_slot_card[i], bw[i], 0);
            lv_obj_set_style_opa(_slot_card[i], opa[i], 0);
            lv_obj_set_style_bg_color(_slot_dot[i], lv_color_hex(txt[i]), 0);
            lv_obj_set_style_text_color(_slot_lbl[i], lv_color_hex(txt[i]), 0);
            if (_face_card[i]) {
                lv_obj_set_style_border_color(_face_card[i], lv_color_hex(brd[i]), 0);
            }
        }
        lv_obj_set_style_border_color(
            _stage, lv_color_hex(free_on ? 0x1F6E58U : (agent_on ? 0xB45309U : COL_CARD_HI)), 0);
    }

    if (_btn_auto && _lbl_auto) {
        lv_obj_set_style_bg_color(_btn_auto, lv_color_hex(auto_on ? COL_TEAL_DK : COL_CARD), 0);
        lv_obj_set_style_text_color(_lbl_auto, lv_color_hex(auto_on ? COL_WHITE : COL_DIM), 0);
        lv_label_set_text(_lbl_auto, auto_on ? "AUTO\nON" : "AUTO\nOFF");
    }

    if (rec && hearing) {
        lv_obj_set_style_text_color(_lbl_dictate, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_RED), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_STOP " SEND");
        lv_obj_set_style_bg_color(_bar_level, lv_color_hex(COL_RED), 0);
        lv_obj_set_width(_bar_level, (_mic_level.load() * 260) / 100);
    } else if (rec) {
        lv_obj_set_style_text_color(_lbl_dictate, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_TEAL_DK), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_AUDIO " LISTENING");
        lv_obj_set_style_bg_color(_bar_level, lv_color_hex(COL_TEAL), 0);
        lv_obj_set_width(_bar_level, (_mic_level.load() * 260) / 100);
    } else if (spk) {
        lv_obj_set_style_text_color(_lbl_dictate, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_BLUE), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_VOLUME_MAX " SPEAKING");
        lv_obj_set_style_bg_color(_bar_level, lv_color_hex(COL_BLUE), 0);
        int lv = std::clamp(_speak_rms.load() / 35, 6, 100);
        lv_obj_set_width(_bar_level, (lv * 260) / 100);
    } else if (ready) {
        lv_obj_set_style_text_color(_lbl_dictate, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_GREEN), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_OK " SENDING");
        lv_obj_set_style_bg_color(_bar_level, lv_color_hex(COL_GREEN), 0);
        lv_obj_set_width(_bar_level, 260);
    } else if (waiting) {
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_YELLOW), 0);
        lv_obj_set_style_text_color(_lbl_dictate, lv_color_hex(COL_BG), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_REFRESH " WAITING");
        lv_obj_set_style_bg_color(_bar_level, lv_color_hex(COL_YELLOW), 0);
        lv_obj_set_width(_bar_level, 260);
    } else {
        lv_obj_set_style_text_color(_lbl_dictate, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_TEAL_DK), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_AUDIO " TALK");
        lv_obj_set_width(_bar_level, 0);
    }
}

void AppMcp::refresh_avatar_anim(uint32_t now)
{
    if (!_stage || !_slot_card[0] || !_slot_card[1]) {
        return;
    }
    bool use_naruto  = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto &&
                        _naruto_imgs[0].data != nullptr);
    bool speaking    = _playing.load();
    int face_idx     = std::clamp(_face.load(), 0, static_cast<int>(FaceExpr::Count) - 1);
    const auto amode = static_cast<AgentMode>(_agent_mode.load());
    const int active = (amode == AgentMode::Free) ? 0 : (amode == AgentMode::Engineer ? 1 : -1);

    for (int i = 0; i < 2; ++i) {
        const bool is_active = (i == active);
        if (use_naruto) {
            lv_obj_add_flag(_face_card[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(_avatar_img[i], LV_OBJ_FLAG_HIDDEN);

            int show_idx = is_active ? face_idx : static_cast<int>(FaceExpr::Calm);
            if (is_active && speaking && _speak_rms.load() > 600 && ((now / 180) % 2 == 0)) {
                show_idx = static_cast<int>(FaceExpr::Happy);
            }
            if (_naruto_imgs[show_idx].data == nullptr) {
                show_idx = 0;
            }
            if (show_idx != _shown_naruto_idx[i]) {
                lv_image_set_src(_avatar_img[i], &_naruto_imgs[show_idx]);
                _shown_naruto_idx[i] = show_idx;
            }
            int dy = (is_active && speaking) ? static_cast<int>(std::sin(now * 0.02f) * 2.5f) : 0;
            lv_obj_align(_avatar_img[i], LV_ALIGN_CENTER, 0, 8 + dy);
        } else {
            lv_obj_add_flag(_avatar_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(_face_card[i], LV_OBJ_FLAG_HIDDEN);

            bool blink = is_active && ((now % 4200) < 150);
            lv_obj_set_size(_face_eye_l[i], 14, blink ? 3 : 14);
            lv_obj_set_size(_face_eye_r[i], 14, blink ? 3 : 14);
            if (is_active && speaking) {
                int mh = 5 + std::clamp(_speak_rms.load() / 240, 0, 12);
                lv_obj_set_size(_face_mouth[i], 26, mh);
                lv_obj_set_style_radius(_face_mouth[i], (mh > 9) ? 6 : 3, 0);
            } else {
                lv_obj_set_size(_face_mouth[i], 28, 5);
                lv_obj_set_style_radius(_face_mouth[i], 3, 0);
            }
        }
    }
}

void AppMcp::set_status_text(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(_status_mtx);
    _status_text = msg;
}

AppMcp::FaceExpr AppMcp::parse_face(const std::string& s)
{
    std::string l = s;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "thinking") return FaceExpr::Thinking;
    if (l == "happy") return FaceExpr::Happy;
    if (l == "sleepy") return FaceExpr::Sleepy;
    if (l == "shy") return FaceExpr::Shy;
    if (l == "smug") return FaceExpr::Smug;
    if (l == "pouty") return FaceExpr::Pouty;
    return FaceExpr::Calm;
}

const char* AppMcp::face_name(FaceExpr f)
{
    switch (f) {
        case FaceExpr::Thinking: return "thinking";
        case FaceExpr::Happy: return "happy";
        case FaceExpr::Sleepy: return "sleepy";
        case FaceExpr::Shy: return "shy";
        case FaceExpr::Smug: return "smug";
        case FaceExpr::Pouty: return "pouty";
        default: return "calm";
    }
}
