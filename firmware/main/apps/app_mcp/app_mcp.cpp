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
constexpr int W                      = 320;
constexpr int H                      = 240;
constexpr int Y_HOME_DEG             = 45;
constexpr uint32_t REC_SAMPLE_RATE   = 16000;
constexpr size_t REC_MAX_SECONDS     = 14;
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
    _rec_start_req   = false;
    _rec_stop_req    = false;
    _rec_active      = false;
    _rec_ready       = false;
    _rec_wav_bytes   = 0;
    _mic_level       = 0;
    _face            = static_cast<int>(FaceExpr::Calm);
    _gesture_type    = 0;
    _shown_naruto_idx = -1;

    // Load saved speaker volume
    {
        Settings s(NVS_NS, false);
        int v   = s.GetInt(KEY_VOL, 75);
        _volume = std::clamp(v, 10, 100);
    }
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
    GetStackChan().update();

    {
        LvglLockGuard lock;
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
        set_status_text("MCP Ready - Antigravity");
        GetHAL().showRgbColor(0, 48, 42);
    } else {
        set_status_text("HTTP server failed");
        GetHAL().showRgbColor(60, 0, 0);
    }

    while (_running.load()) {
        if (_play_pending.exchange(false)) {
            std::string url;
            {
                std::lock_guard<std::mutex> lk(_play_mtx);
                url = _next_play_url;
            }
            if (!url.empty()) {
                play_wav_url(url);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(25));
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

    while (_running.load()) {
        if (!_rec_start_req.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        if (!audio_codec || !_rec_wav_buf || _rec_wav_cap <= REC_HEADER_BYTES) {
            set_status_text("Mic unavailable");
            continue;
        }

        // Stop any active playback before opening mic (half-duplex safety)
        _play_abort = true;
        for (int i = 0; i < 40 && _playing.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        if (audio_codec->output_enabled()) {
            audio_codec->EnableOutput(false);
        }
        if (!audio_codec->input_enabled()) {
            audio_codec->EnableInput(true);
        }

        const size_t in_ch     = static_cast<size_t>(std::max(audio_codec->input_channels(), 1));
        const uint32_t in_rate = static_cast<uint32_t>(std::max(audio_codec->input_sample_rate(), 16000));
        // Read 40 ms chunks at codec input rate (e.g. 960 frames @ 24 kHz)
        const size_t in_frames  = (in_rate * 40) / 1000;
        const size_t out_frames = (REC_SAMPLE_RATE * 40) / 1000;
        raw_in.resize(in_frames * in_ch);
        mono_16k.resize(out_frames);

        _rec_stop_req = false;
        _rec_ready    = false;
        _rec_active   = true;
        _mic_level    = 0;
        set_status_text("Listening... speak now");
        GetHAL().showRgbColor(80, 12, 12);

        size_t pcm_pos       = 0;
        size_t max_pcm_bytes = _rec_wav_cap - REC_HEADER_BYTES;
        uint32_t start_ms    = GetHAL().millis();
        uint32_t last_voice  = start_ms;
        bool speech_heard    = false;
        float noise_floor    = 180.0f;
        int warmup_frames    = 0;

        {
            std::lock_guard<std::mutex> lk(_rec_mtx);
            while (_running.load() && !_rec_stop_req.load() && pcm_pos + out_frames * 2 <= max_pcm_bytes) {
                if (!audio_codec->InputData(raw_in)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                // Extract mic ch0 and resample in_rate (e.g. 24 kHz) -> 16 kHz mono + RMS
                uint64_t sum_sq = 0;
                for (size_t i = 0; i < out_frames; ++i) {
                    size_t src_i = std::min<size_t>((i * in_rate) / REC_SAMPLE_RATE, in_frames - 1);
                    int16_t s    = raw_in[src_i * in_ch];
                    mono_16k[i]  = s;
                    sum_sq += static_cast<uint64_t>(static_cast<int32_t>(s) * static_cast<int32_t>(s));
                }
                int rms = static_cast<int>(std::sqrt(static_cast<double>(sum_sq) / out_frames));
                _mic_level.store(std::clamp(rms / 35, 0, 100));

                std::memcpy(_rec_wav_buf + REC_HEADER_BYTES + pcm_pos, mono_16k.data(), out_frames * sizeof(int16_t));
                pcm_pos += out_frames * sizeof(int16_t);

                uint32_t now = GetHAL().millis();
                if (warmup_frames < 6) {
                    noise_floor = noise_floor * 0.8f + rms * 0.2f;
                    ++warmup_frames;
                    continue;
                }

                float thresh = std::max(420.0f, noise_floor * 2.6f);
                if (rms > thresh) {
                    speech_heard = true;
                    last_voice   = now;
                } else if (!speech_heard) {
                    noise_floor = noise_floor * 0.95f + rms * 0.05f;
                }

                // Auto-stop after 1.8s silence once user spoke (or 14s cap)
                if (speech_heard && (now - last_voice) >= 1800 && (now - start_ms) >= 1000) {
                    break;
                }
                if ((now - start_ms) >= REC_MAX_SECONDS * 1000) {
                    break;
                }
            }

            if (pcm_pos >= 3200) {
                write_wav_header(_rec_wav_buf, static_cast<uint32_t>(pcm_pos), REC_SAMPLE_RATE);
                _rec_wav_bytes = REC_HEADER_BYTES + pcm_pos;
                _rec_ready     = true;
            } else {
                _rec_wav_bytes = 0;
                _rec_ready     = false;
            }
        }

        if (audio_codec->input_enabled()) {
            audio_codec->EnableInput(false);
        }
        _mic_level  = 0;
        _rec_active = false;

        if (_rec_ready.load()) {
            set_status_text("Dictation ready for MCP!");
            GetHAL().showRgbColor(0, 72, 24);
        } else {
            set_status_text("MCP Ready - Antigravity");
            GetHAL().showRgbColor(0, 48, 42);
        }
    }

    _mic_exited = true;
}

void AppMcp::play_wav_url(const std::string& url)
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

    _play_abort      = false;
    _playing         = true;
    _play_started_ms = GetHAL().millis();
    _speak_rms       = 0;
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
    set_status_text(_rec_ready.load() ? "Dictation ready for MCP!" : "MCP Ready - Antigravity");
    GetHAL().showRgbColor(0, 48, 42);
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

void AppMcp::change_volume(int delta)
{
    int v   = std::clamp(_volume.load() + delta, 10, 100);
    _volume = v;
    GetHAL().setSpeakerVolume(static_cast<uint8_t>(v), false);
    Settings s(NVS_NS, true);
    s.SetInt(KEY_VOL, v);
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

    auto& motion = GetStackChan().motion();
    for (const auto& cmd : q) {
        if (cmd.kind == "move") {
            _gesture_type = 0;
            motion.moveWithSpeed(deg_to_yaw(cmd.x_deg), deg_to_pitch(cmd.y_deg), std::clamp(cmd.speed, 100, 1000));
        } else if (cmd.kind == "home") {
            _gesture_type = 0;
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
        size_t chunk = std::min<size_t>(4096, self->_rec_wav_bytes - sent);
        if (httpd_resp_send_chunk(req, reinterpret_cast<const char*>(self->_rec_wav_buf + sent),
                                  static_cast<ssize_t>(chunk)) != ESP_OK) {
            break;
        }
        sent += chunk;
    }
    httpd_resp_send_chunk(req, nullptr, 0);

    self->_rec_ready     = false;
    self->_rec_wav_bytes = 0;
    self->set_status_text("Sent dictation to MCP!");
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

    {
        std::lock_guard<std::mutex> lk(self->_play_mtx);
        self->_next_play_url = jurl->valuestring;
    }
    cJSON_Delete(root);

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

    // Top bar: MCP IP (left) + Battery (right)
    _lbl_ip = lv_label_create(_root);
    lv_obj_set_pos(_lbl_ip, 10, 5);
    lv_obj_set_size(_lbl_ip, 236, 18);
    lv_label_set_long_mode(_lbl_ip, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_ip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_ip, lv_color_hex(COL_TEAL), 0);
    lv_label_set_text(_lbl_ip, "MCP  Wi-Fi...");

    _lbl_batt = lv_label_create(_root);
    lv_obj_set_pos(_lbl_batt, 250, 6);
    lv_obj_set_width(_lbl_batt, 62);
    lv_obj_set_style_text_align(_lbl_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(_lbl_batt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_batt, lv_color_hex(COL_DIM), 0);

    // Center stage (tap to dictate / stop)
    _stage = lv_obj_create(_root);
    lv_obj_set_pos(_stage, 8, 26);
    lv_obj_set_size(_stage, 304, 152);
    style_box(_stage, COL_CARD, 14);
    lv_obj_set_style_border_width(_stage, 1, 0);
    lv_obj_set_style_border_color(_stage, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_add_flag(_stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        _stage,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->toggle_dictation();
        },
        LV_EVENT_CLICKED, this);

    // Naruto avatar image (scaled from 180x180 -> ~124x124)
    _avatar_img = lv_image_create(_stage);
    if (_naruto_imgs[0].data != nullptr) {
        lv_image_set_src(_avatar_img, &_naruto_imgs[0]);
        lv_image_set_scale(_avatar_img, 176);
    }
    lv_obj_align(_avatar_img, LV_ALIGN_CENTER, 0, -10);
    lv_obj_remove_flag(_avatar_img, LV_OBJ_FLAG_CLICKABLE);

    // Classic StackChan face fallback
    _face_card = lv_obj_create(_stage);
    lv_obj_set_size(_face_card, 120, 86);
    lv_obj_align(_face_card, LV_ALIGN_CENTER, 0, -10);
    style_box(_face_card, COL_BG, 14);
    lv_obj_set_style_border_width(_face_card, 2, 0);
    lv_obj_set_style_border_color(_face_card, lv_color_hex(COL_TEAL_DK), 0);
    lv_obj_remove_flag(_face_card, LV_OBJ_FLAG_CLICKABLE);

    _face_eye_l = lv_obj_create(_face_card);
    lv_obj_set_size(_face_eye_l, 18, 18);
    lv_obj_align(_face_eye_l, LV_ALIGN_CENTER, -26, -8);
    style_box(_face_eye_l, COL_WHITE, LV_RADIUS_CIRCLE);

    _face_eye_r = lv_obj_create(_face_card);
    lv_obj_set_size(_face_eye_r, 18, 18);
    lv_obj_align(_face_eye_r, LV_ALIGN_CENTER, 26, -8);
    style_box(_face_eye_r, COL_WHITE, LV_RADIUS_CIRCLE);

    _face_mouth = lv_obj_create(_face_card);
    lv_obj_set_size(_face_mouth, 38, 6);
    lv_obj_align(_face_mouth, LV_ALIGN_CENTER, 0, 18);
    style_box(_face_mouth, COL_WHITE, 4);

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

    // Bottom bar: DICTATE | VOL- | VOL+ | EXIT
    _btn_dictate = lv_button_create(_root);
    lv_obj_set_pos(_btn_dictate, 8, 186);
    lv_obj_set_size(_btn_dictate, 144, 44);
    style_box(_btn_dictate, COL_TEAL_DK, 10);
    _lbl_dictate = lv_label_create(_btn_dictate);
    lv_obj_center(_lbl_dictate);
    lv_obj_set_style_text_font(_lbl_dictate, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_dictate, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(_lbl_dictate, LV_SYMBOL_AUDIO " DICTATE");
    lv_obj_add_event_cb(
        _btn_dictate,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->toggle_dictation();
        },
        LV_EVENT_CLICKED, this);

    auto make_btn = [&](int x, int w, const char* txt, uint32_t bg, uint32_t fg) {
        lv_obj_t* b = lv_button_create(_root);
        lv_obj_set_pos(b, x, 186);
        lv_obj_set_size(b, w, 44);
        style_box(b, bg, 10);
        lv_obj_t* l = lv_label_create(b);
        lv_obj_center(l);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
        lv_label_set_text(l, txt);
        return b;
    };

    lv_obj_t* btn_vd = make_btn(156, 48, "VOL-", COL_CARD, COL_TEXT);
    lv_obj_add_event_cb(
        btn_vd,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->change_volume(-10);
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_vu = make_btn(208, 48, "VOL+", COL_CARD, COL_TEXT);
    lv_obj_add_event_cb(
        btn_vu,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->change_volume(10);
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_exit = make_btn(260, 52, "EXIT", COL_CARD_HI, COL_YELLOW);
    lv_obj_add_event_cb(
        btn_exit,
        [](lv_event_t* e) {
            static_cast<AppMcp*>(lv_event_get_user_data(e))->close();
        },
        LV_EVENT_CLICKED, this);
}

void AppMcp::destroy_ui()
{
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}

void AppMcp::refresh_ui(uint32_t now)
{
    (void)now;
    if (!_root) {
        return;
    }

    std::string ip;
    std::string st;
    {
        std::lock_guard<std::mutex> lk(_status_mtx);
        ip = _ip_str;
        st = _status_text;
    }

    if (_httpd_ok.load() && !ip.empty()) {
        std::string hdr = "MCP  " + ip + "  V" + std::to_string(_volume.load());
        lv_obj_set_style_text_color(_lbl_ip, lv_color_hex(COL_TEAL), 0);
        lv_label_set_text(_lbl_ip, hdr.c_str());
    } else {
        lv_obj_set_style_text_color(_lbl_ip, lv_color_hex(COL_YELLOW), 0);
        lv_label_set_text(_lbl_ip, "MCP  Connecting...");
    }

    char batt[16];
    std::snprintf(batt, sizeof(batt), "%d%%", GetHAL().getBatteryLevel());
    lv_label_set_text(_lbl_batt, batt);

    lv_label_set_text(_lbl_status, st.c_str());

    bool rec   = _rec_active.load();
    bool ready = _rec_ready.load();
    bool spk   = _playing.load();

    if (rec) {
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_RED), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_STOP " STOP");
        lv_obj_set_style_bg_color(_bar_level, lv_color_hex(COL_RED), 0);
        lv_obj_set_width(_bar_level, (_mic_level.load() * 260) / 100);
    } else if (spk) {
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_BLUE), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_VOLUME_MAX " SPEAKING");
        lv_obj_set_style_bg_color(_bar_level, lv_color_hex(COL_BLUE), 0);
        int lv = std::clamp(_speak_rms.load() / 35, 6, 100);
        lv_obj_set_width(_bar_level, (lv * 260) / 100);
    } else if (ready) {
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_GREEN), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_OK " READY");
        lv_obj_set_style_bg_color(_bar_level, lv_color_hex(COL_GREEN), 0);
        lv_obj_set_width(_bar_level, 260);
    } else {
        lv_obj_set_style_bg_color(_btn_dictate, lv_color_hex(COL_TEAL_DK), 0);
        lv_label_set_text(_lbl_dictate, LV_SYMBOL_AUDIO " DICTATE");
        lv_obj_set_width(_bar_level, 0);
    }
}

void AppMcp::refresh_avatar_anim(uint32_t now)
{
    if (!_stage) {
        return;
    }
    bool use_naruto = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto &&
                       _naruto_imgs[0].data != nullptr);
    bool speaking   = _playing.load();
    int face_idx    = std::clamp(_face.load(), 0, static_cast<int>(FaceExpr::Count) - 1);

    if (use_naruto) {
        lv_obj_add_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);

        int show_idx = face_idx;
        if (speaking && _speak_rms.load() > 600 && ((now / 180) % 2 == 0)) {
            show_idx = static_cast<int>(FaceExpr::Happy);
        }
        if (_naruto_imgs[show_idx].data == nullptr) {
            show_idx = 0;
        }
        if (show_idx != _shown_naruto_idx) {
            lv_image_set_src(_avatar_img, &_naruto_imgs[show_idx]);
            _shown_naruto_idx = show_idx;
        }
        int dy = speaking ? static_cast<int>(std::sin(now * 0.02f) * 3.0f) : 0;
        lv_obj_align(_avatar_img, LV_ALIGN_CENTER, 0, -10 + dy);
    } else {
        lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_face_card, LV_OBJ_FLAG_HIDDEN);

        bool blink = ((now % 4200) < 150);
        lv_obj_set_size(_face_eye_l, 18, blink ? 4 : 18);
        lv_obj_set_size(_face_eye_r, 18, blink ? 4 : 18);
        if (speaking) {
            int mh = 6 + std::clamp(_speak_rms.load() / 220, 0, 14);
            lv_obj_set_size(_face_mouth, 34, mh);
            lv_obj_set_style_radius(_face_mouth, (mh > 10) ? 8 : 4, 0);
        } else {
            lv_obj_set_size(_face_mouth, 38, 6);
            lv_obj_set_style_radius(_face_mouth, 4, 0);
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
