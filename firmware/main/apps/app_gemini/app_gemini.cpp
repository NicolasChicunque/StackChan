/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_gemini.h"

#include <hal/hal.h>
#include <hal/board/config.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <stackchan/avatar/skins/default/default.h>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <board.h>
#include <audio/audio_codec.h>
#include <web_socket.h>
#include <network_interface.h>
#include <wifi_manager.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <mbedtls/base64.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

using namespace mooncake;

LV_IMAGE_DECLARE(icon_gemini);

namespace {

constexpr const char* TAG           = "GEMINI";
constexpr const char* NVS_NS        = "gemini_live";
constexpr const char* KEY_API_KEY   = "api_key";
constexpr const char* KEY_MODEL     = "model";
constexpr const char* KEY_VOICE     = "voice";
constexpr const char* KEY_PITCH     = "pitch_pct";
constexpr const char* KEY_MIC_SENS  = "mic_sens";
constexpr const char* KEY_SPEAK_MV  = "speak_move";
constexpr const char* DEFAULT_MODEL = "gemini-3.8-live";
constexpr const char* DEFAULT_VOICE = "Puck";
constexpr int DEFAULT_PITCH_PCT     = 100;
constexpr int MIN_PITCH_PCT         = 85;
constexpr int MAX_PITCH_PCT         = 130;
constexpr int PITCH_STEP_PCT        = 5;
constexpr const char* SD_KEY_PATH   = "/sd/gemini.txt";
constexpr const char* SD_MOUNT_PT   = "/sd";

struct VoicePreset {
    const char* name;
    const char* desc;
};

constexpr VoicePreset kVoicePresets[] = {
    {"Puck",   "Youthful M"},
    {"Fenrir", "Excitable M"},
    {"Orus",   "Firm M"},
    {"Charon", "Deep M"},
    {"Zephyr", "Bright F"},
    {"Kore",   "Firm F"},
    {"Aoede",  "Breezy F"},
    {"Leda",   "Youthful F"},
};
constexpr int kVoicePresetCount = static_cast<int>(sizeof(kVoicePresets) / sizeof(kVoicePresets[0]));

int find_voice_index(const std::string& name)
{
    for (int i = 0; i < kVoicePresetCount; ++i) {
        if (name == kVoicePresets[i].name) {
            return i;
        }
    }
    return 0;
}

constexpr int W                     = 320;
constexpr int H                     = 240;
constexpr int VOLUME_STEP           = 10;
constexpr uint32_t MAX_LISTEN_MS    = 30000;
constexpr uint32_t SILENCE_END_MS   = 1700;         // Auto-send after ~1.7 s pause
constexpr uint32_t ECHO_GUARD_MS    = 600;          // Wait 600 ms after speaker stops so tail echo never re-triggers mic
constexpr size_t MIC_OUT_FRAMES     = 1600;         // 100 ms @ 16 kHz
constexpr int PREROLL_CHUNKS        = 3;            // 300 ms pre-roll so first syllable is never clipped
constexpr size_t PCM_RING_SAMPLES   = 24000 * 24;   // 24 s of 24 kHz 16-bit mono (~1.15 MB in PSRAM)
constexpr size_t B64_DECODE_CAP     = 192 * 1024;   // 192 KB PSRAM scratch for decoded PCM chunks

// Dark Gemini palette
constexpr uint32_t COL_BG           = 0x0B0F1A;
constexpr uint32_t COL_CARD         = 0x151B2E;
constexpr uint32_t COL_CARD_HI      = 0x232C4A;
constexpr uint32_t COL_GEMINI_BLUE  = 0x9B5DE5;
constexpr uint32_t COL_GEMINI_PURP  = 0xC77DFF;
constexpr uint32_t COL_CYAN         = 0x38BDF8;
constexpr uint32_t COL_GREEN        = 0x22C55E;
constexpr uint32_t COL_GREEN_DK     = 0x14532D;
constexpr uint32_t COL_AMBER        = 0xF59E0B;
constexpr uint32_t COL_RED          = 0xEF4444;
constexpr uint32_t COL_RED_DK       = 0x7F1D1D;
constexpr uint32_t COL_TEXT         = 0xE2E8F0;
constexpr uint32_t COL_DIM          = 0x94A3B8;
constexpr uint32_t COL_WHITE        = 0xFFFFFF;
constexpr int Y_HOME_DEG            = 45;

inline int deg_to_yaw(int deg)
{
    return std::clamp(deg, -128, 128) * 10;
}

inline int deg_to_pitch(int deg)
{
    return (45 - std::clamp(deg, 0, 90)) * 10;
}

inline uint16_t rd_u16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t rd_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool read_http_body(httpd_req_t* req, std::string& out, size_t max_len = 2048)
{
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > max_len) {
        return false;
    }
    out.resize(static_cast<size_t>(req->content_len));
    size_t got = 0;
    while (got < out.size()) {
        int r = httpd_req_recv(req, out.data() + got, out.size() - got);
        if (r <= 0) {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

void send_http_json(httpd_req_t* req, const char* json, const char* status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

void style_flat_box(lv_obj_t* obj, uint32_t bg_hex, int radius = 0)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

std::string trim_line(const std::string& in)
{
    size_t start = 0;
    // Strip UTF-8 BOM if present
    if (in.size() >= 3 && static_cast<uint8_t>(in[0]) == 0xEF && static_cast<uint8_t>(in[1]) == 0xBB &&
        static_cast<uint8_t>(in[2]) == 0xBF) {
        start = 3;
    }
    while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start]))) {
        ++start;
    }
    size_t end = in.size();
    while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1]))) {
        --end;
    }
    return in.substr(start, end - start);
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                           PSRAM PCM Ring Buffer                            */
/* -------------------------------------------------------------------------- */

bool AppGemini::PcmRingBuffer::allocate(size_t samples)
{
    std::lock_guard<std::mutex> lock(mtx);
    release();
    buf = static_cast<int16_t*>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        return false;
    }
    capacity = samples;
    head     = 0;
    tail     = 0;
    count    = 0;
    return true;
}

void AppGemini::PcmRingBuffer::release()
{
    if (buf) {
        heap_caps_free(buf);
        buf = nullptr;
    }
    capacity = 0;
    head     = 0;
    tail     = 0;
    count    = 0;
}

void AppGemini::PcmRingBuffer::clear()
{
    std::lock_guard<std::mutex> lock(mtx);
    head  = 0;
    tail  = 0;
    count = 0;
}

size_t AppGemini::PcmRingBuffer::push(const int16_t* data, size_t samples)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (!buf || capacity == 0 || samples == 0) {
        return 0;
    }
    size_t free_space = capacity - count;
    size_t to_write   = std::min(samples, free_space);
    if (to_write == 0) {
        return 0;
    }
    size_t first = std::min(to_write, capacity - head);
    std::memcpy(buf + head, data, first * sizeof(int16_t));
    if (to_write > first) {
        std::memcpy(buf, data + first, (to_write - first) * sizeof(int16_t));
    }
    head = (head + to_write) % capacity;
    count += to_write;
    return to_write;
}

size_t AppGemini::PcmRingBuffer::pop(int16_t* out, size_t max_samples)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (!buf || count == 0 || max_samples == 0) {
        return 0;
    }
    size_t to_read = std::min(max_samples, count);
    size_t first   = std::min(to_read, capacity - tail);
    std::memcpy(out, buf + tail, first * sizeof(int16_t));
    if (to_read > first) {
        std::memcpy(out + first, buf, (to_read - first) * sizeof(int16_t));
    }
    tail = (tail + to_read) % capacity;
    count -= to_read;
    return to_read;
}

size_t AppGemini::PcmRingBuffer::available() const
{
    std::lock_guard<std::mutex> lock(mtx);
    return count;
}

/* -------------------------------------------------------------------------- */
/*                            Mooncake Lifecycle                              */
/* -------------------------------------------------------------------------- */

AppGemini::AppGemini()
{
    setAppInfo().name = "GEMINI";

    static lv_image_dsc_t icon_from_assets = assets::get_image("icon_gemini.bin");
    if (icon_from_assets.data != nullptr) {
        setAppInfo().icon = (void*)&icon_from_assets;
    } else {
        setAppInfo().icon = (void*)&icon_gemini;
    }

    static uint32_t theme_color = COL_GEMINI_BLUE;
    setAppInfo().userData       = (void*)&theme_color;
}

AppGemini::~AppGemini() = default;

void AppGemini::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppGemini::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _closing              = false;
    _want_connect         = false;
    _want_arm_listen      = false;
    _want_stop_talk       = false;
    _want_mute            = false;
    _muted                = false;
    _setup_complete       = false;
    _use_google_search    = true;
    _server_turn_done     = false;
    _new_turn_subtitle    = true;
    _audio_rms            = 0;
    _last_ui_ms           = 0;
    _last_led_ms          = 0;
    _listen_start_ms      = 0;
    _preroll_write_idx    = 0;
    _preroll_count        = 0;
    _noise_floor          = 300.0f;
    _speech_hits          = 0;
    _speech_start_ms      = 0;
    _last_voice_ms        = 0;
    _last_rendered_state  = -1;
    _last_rendered_mcp    = false;
    _last_rendered_agent  = false;
    _last_rendered_sub.clear();
    _settings_open        = false;
    _voice_dirty          = false;

    _httpd_ok             = false;
    _mcp_attached         = false;
    _mcp_agent_active     = false;
    _mcp_lease_until_ms     = 0;
    _mcp_handoff_pending    = false;
    _mcp_handoff_speaking   = false;
    _mcp_handoff_started_ms = 0;
    _mcp_end_pending        = false;
    _mcp_wait_reply_mode    = false;
    _mcp_escalation_ready   = false;
    _mcp_reply_ready        = false;
    _mcp_playing_wav        = false;
    _mcp_play_pending       = false;
    _mcp_play_abort         = false;
    _mcp_play_started_ms    = 0;
    _mcp_play_wait_reply    = false;
    _gesture_type           = 0;
    _gesture_step           = 0;
    _gesture_next           = 0;
    _speak_moving           = false;
    _speak_move_step        = 0;
    _speak_move_next        = 0;
    _base_yaw_deg           = 0;
    _base_pitch_deg         = Y_HOME_DEG;
    {
        nvs_handle_t h{};
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            int32_t ms = 1;
            int32_t mv = 1;
            if (nvs_get_i32(h, KEY_MIC_SENS, &ms) == ESP_OK) {
                _mic_sens = std::clamp<int>(static_cast<int>(ms), 0, 2);
            }
            if (nvs_get_i32(h, KEY_SPEAK_MV, &mv) == ESP_OK) {
                _speak_move = (mv != 0);
            }
            nvs_close(h);
        }
    }
    GetHAL().setServoPowerEnabled(true);
    {
        auto& sc = GetStackChan();
        sc.clearModifiers();
        sc.motion().setAutoAngleSyncEnabled(false);
        sc.motion().setAutoTorqueReleaseEnabled(false);
        sc.motion().moveWithSpeed(deg_to_yaw(0), deg_to_pitch(Y_HOME_DEG), 500);
    }
    {
        std::lock_guard<std::mutex> lk(_mcp_mtx);
        _ip_str.clear();
        _mcp_context.clear();
        _mcp_next_play_url.clear();
        _mcp_last_user_text.clear();
        _mcp_escalation_task.clear();
        _mcp_escalation_user.clear();
        _mcp_reply_text.clear();
        _mcp_handoff_call_id.clear();
        _mcp_handoff_call_name.clear();
    }

    _volume = std::clamp(static_cast<int>(GetHAL().getSpeakerVolume()), 10, 100);

    // Load Naruto sprites for custom/naruto branch (respects global Classic/Naruto toggle)
    _img_naruto_calm      = assets::get_image("avatar_calm_music.bin");
    _img_naruto_happy     = assets::get_image("avatar_happy_music.bin");
    _avatar_showing_happy = false;

    // Allocate PSRAM playback ring buffer & Base64/VAD scratch buffers
    if (!_pcm_ring.allocate(PCM_RING_SAMPLES)) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM PCM ring buffer");
    }
    _b64_decode_buf = static_cast<uint8_t*>(heap_caps_malloc(B64_DECODE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _b64_decode_cap = _b64_decode_buf ? B64_DECODE_CAP : 0;
    _mic_pcm16k_buf =
        static_cast<int16_t*>(heap_caps_malloc(MIC_OUT_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _mic_b64_buf = static_cast<unsigned char*>(heap_caps_malloc(4400, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _preroll_buf = static_cast<int16_t*>(
        heap_caps_malloc(PREROLL_CHUNKS * MIC_OUT_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    // Load API key from /sd/gemini.txt (updating NVS) or cached NVS
    bool has_key = load_api_config();

    {
        LvglLockGuard lock;
        build_ui();
        view::create_home_indicator([&]() { close(); }, COL_GEMINI_BLUE, COL_BG);
        view::create_status_bar(COL_GEMINI_BLUE, COL_BG);
    }

    if (has_key) {
        set_state(SessionState::ConnectingWifi);
        set_subtitle("Connecting to Wi-Fi...");
        _want_connect = true;
    } else {
        set_state(SessionState::NeedKey);
        set_subtitle("No API key found");
    }

    start_tasks();

    {
        LvglLockGuard lock;
        refresh_ui(GetHAL().millis());
    }
}

void AppGemini::onRunning()
{
    uint32_t now = GetHAL().millis();

    process_motion_cmds();
    run_gesture_step(now);
    update_speak_motion(now);
    GetStackChan().update();

    // Safety timeout: if Charon handoff produced no audio within 5s, still release escalation to AGENT $
    if (_mcp_handoff_speaking.load() && _mcp_handoff_started_ms.load() != 0 &&
        (now - _mcp_handoff_started_ms.load()) >= 5000 &&
        static_cast<SessionState>(_state.load()) != SessionState::Speaking) {
        if (_mcp_handoff_speaking.exchange(false)) {
            _mcp_agent_active     = true;
            _mcp_escalation_ready = true;
            set_state(SessionState::AgentWait);
            set_subtitle("Waiting for lead engineer...");
        }
    }

    // Auto-detach MCP mode cleanly when /stackchanchat heartbeat lease expires
    if (_mcp_attached.load()) {
        uint32_t lease = _mcp_lease_until_ms.load();
        if (lease != 0 && static_cast<int32_t>(now - lease) >= 0) {
            ESP_LOGI(TAG, "MCP heartbeat lease expired; returning to standalone GEMINI mode");
            _mcp_attached         = false;
            _mcp_agent_active     = false;
            _mcp_wait_reply_mode  = false;
            _mcp_escalation_ready = false;
            _mcp_reply_ready      = false;
            _mcp_lease_until_ms   = 0;
            _want_connect         = true;
        }
    }

    // Auto-stop hearing safety cap (30 s)
    SessionState st = static_cast<SessionState>(_state.load());
    if (st == SessionState::Hearing && _speech_start_ms > 0 && (now - _speech_start_ms) >= MAX_LISTEN_MS) {
        _want_stop_talk = true;
    }

    if (now - _last_ui_ms >= 60) {
        _last_ui_ms = now;
        LvglLockGuard lock;
        refresh_ui(now);
        view::update_home_indicator();
        view::update_status_bar();
    }

    if (now - _last_led_ms >= 80) {
        _last_led_ms = now;
        update_leds(st, _audio_rms.load());
    }

    GetHAL().delay(15);
}

void AppGemini::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _closing        = true;
    _mcp_play_abort = true;
    stop_httpd();
    stop_tasks();

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        if (codec->input_enabled()) {
            codec->EnableInput(false);
        }
        if (codec->output_enabled()) {
            codec->EnableOutput(false);
        }
    }

    // Turn off RGB LEDs
    for (int i = 0; i < 12; ++i) {
        GetHAL().setRgbColor(i, 0, 0, 0);
    }
    GetHAL().refreshRgb();

    {
        LvglLockGuard lock;
        view::destroy_home_indicator();
        view::destroy_status_bar();
        destroy_ui();
    }

    _pcm_ring.release();
    if (_b64_decode_buf) {
        heap_caps_free(_b64_decode_buf);
        _b64_decode_buf = nullptr;
        _b64_decode_cap = 0;
    }
    if (_mic_pcm16k_buf) {
        heap_caps_free(_mic_pcm16k_buf);
        _mic_pcm16k_buf = nullptr;
    }
    if (_mic_b64_buf) {
        heap_caps_free(_mic_b64_buf);
        _mic_b64_buf = nullptr;
    }
    if (_preroll_buf) {
        heap_caps_free(_preroll_buf);
        _preroll_buf = nullptr;
    }

    GetHAL().requestWarmReboot(6);
}

/* -------------------------------------------------------------------------- */
/*                          Config Loading (SD + NVS)                         */
/* -------------------------------------------------------------------------- */

bool AppGemini::read_key_from_sd(std::string& out_key, std::string& out_model, std::string& out_voice)
{
    bool already_mounted = false;
    sdmmc_card_t* card   = nullptr;
    struct stat st       = {};
    if (stat(SD_MOUNT_PT, &st) == 0) {
        already_mounted = true;
    } else {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot         = SPI3_HOST;
        host.max_freq_khz = 12000;

        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs               = GPIO_NUM_4;
        slot_config.host_id               = static_cast<spi_host_device_t>(host.slot);

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
        mount_config.format_if_mount_failed           = false;
        mount_config.max_files                        = 3;
        mount_config.allocation_unit_size             = 16 * 1024;

        esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_PT, &host, &slot_config, &mount_config, &card);
        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "microSD not mounted (%s); will check NVS", esp_err_to_name(ret));
            return false;
        }
    }

    bool found            = false;
    const char* used_path = "/sd/config/gemini.txt";
    FILE* f               = std::fopen(used_path, "r");
    if (!f) {
        used_path = SD_KEY_PATH;
        f         = std::fopen(used_path, "r");
    }
    if (f) {
        char line[256];
        std::vector<std::string> lines;
        while (std::fgets(line, sizeof(line), f)) {
            std::string trimmed = trim_line(line);
            if (!trimmed.empty() && trimmed[0] != '#') {
                lines.push_back(trimmed);
                if (lines.size() >= 3) {
                    break;
                }
            }
        }
        std::fclose(f);

        if (!lines.empty() && lines[0].size() >= 10) {
            out_key   = lines[0];
            out_model = (lines.size() >= 2 && !lines[1].empty()) ? lines[1] : "";
            out_voice = (lines.size() >= 3 && !lines[2].empty()) ? lines[2] : "";
            found     = true;
            ESP_LOGI(TAG, "Loaded Gemini API key from %s", used_path);
        }
    }

    if (!already_mounted && card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_PT, card);
    }
    return found;
}

bool AppGemini::load_key_from_nvs(std::string& out_key, std::string& out_model, std::string& out_voice,
                                  int& out_pitch_pct)
{
    out_model     = DEFAULT_MODEL;
    out_voice     = DEFAULT_VOICE;
    out_pitch_pct = DEFAULT_PITCH_PCT;

    nvs_handle_t h{};
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    bool ok  = false;
    size_t l = 0;
    if (nvs_get_str(h, KEY_API_KEY, nullptr, &l) == ESP_OK && l > 10 && l < 256) {
        std::string buf(l, '\0');
        if (nvs_get_str(h, KEY_API_KEY, buf.data(), &l) == ESP_OK) {
            if (!buf.empty() && buf.back() == '\0') {
                buf.pop_back();
            }
            out_key = buf;
            ok      = true;
        }
    }

    size_t ml = 0;
    if (nvs_get_str(h, KEY_MODEL, nullptr, &ml) == ESP_OK && ml > 2 && ml < 128) {
        std::string mbuf(ml, '\0');
        if (nvs_get_str(h, KEY_MODEL, mbuf.data(), &ml) == ESP_OK) {
            if (!mbuf.empty() && mbuf.back() == '\0') {
                mbuf.pop_back();
            }
            if (!mbuf.empty()) {
                out_model = mbuf;
            }
        }
    }

    size_t vl = 0;
    if (nvs_get_str(h, KEY_VOICE, nullptr, &vl) == ESP_OK && vl > 1 && vl < 64) {
        std::string vbuf(vl, '\0');
        if (nvs_get_str(h, KEY_VOICE, vbuf.data(), &vl) == ESP_OK) {
            if (!vbuf.empty() && vbuf.back() == '\0') {
                vbuf.pop_back();
            }
            if (!vbuf.empty()) {
                out_voice = vbuf;
            }
        }
    }

    int32_t stored_pitch = DEFAULT_PITCH_PCT;
    if (nvs_get_i32(h, KEY_PITCH, &stored_pitch) == ESP_OK) {
        out_pitch_pct = std::clamp(static_cast<int>(stored_pitch), MIN_PITCH_PCT, MAX_PITCH_PCT);
    }

    nvs_close(h);
    return ok;
}

void AppGemini::save_key_to_nvs(const std::string& key, const std::string& model, const std::string& voice,
                                int pitch_pct)
{
    nvs_handle_t h{};
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_API_KEY, key.c_str());
        nvs_set_str(h, KEY_MODEL, model.c_str());
        nvs_set_str(h, KEY_VOICE, voice.c_str());
        nvs_set_i32(h, KEY_PITCH, static_cast<int32_t>(pitch_pct));
        nvs_commit(h);
        nvs_close(h);
    }
}

void AppGemini::save_voice_pitch_to_nvs()
{
    nvs_handle_t h{};
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_VOICE, _voice_name.c_str());
        nvs_set_i32(h, KEY_PITCH, static_cast<int32_t>(_pitch_pct.load()));
        nvs_commit(h);
        nvs_close(h);
    }
}

bool AppGemini::load_api_config()
{
    std::string nvs_key;
    std::string nvs_model = DEFAULT_MODEL;
    std::string nvs_voice = DEFAULT_VOICE;
    int nvs_pitch         = DEFAULT_PITCH_PCT;
    bool has_nvs          = load_key_from_nvs(nvs_key, nvs_model, nvs_voice, nvs_pitch);

    std::string sd_key;
    std::string sd_model;
    std::string sd_voice;
    if (read_key_from_sd(sd_key, sd_model, sd_voice)) {
        _api_key     = sd_key;
        _model_name  = !sd_model.empty() ? sd_model : (has_nvs ? nvs_model : DEFAULT_MODEL);
        // Preserve in-app NVS voice unless line 3 of /sd/gemini.txt explicitly overrides it
        _voice_name  = !sd_voice.empty() ? sd_voice : (has_nvs ? nvs_voice : DEFAULT_VOICE);
        _voice_index = find_voice_index(_voice_name);
        _pitch_pct.store(nvs_pitch);
        save_key_to_nvs(_api_key, _model_name, _voice_name, nvs_pitch);
        ESP_LOGI(TAG, "Config ready (model=%s, voice=%s, pitch=%d%%)", _model_name.c_str(), _voice_name.c_str(),
                 nvs_pitch);
        return true;
    }

    if (has_nvs) {
        _api_key     = nvs_key;
        _model_name  = nvs_model;
        _voice_name  = nvs_voice;
        _voice_index = find_voice_index(_voice_name);
        _pitch_pct.store(nvs_pitch);
        ESP_LOGI(TAG, "Loaded cached Gemini config from NVS (model=%s, voice=%s, pitch=%d%%)", _model_name.c_str(),
                 _voice_name.c_str(), nvs_pitch);
        return true;
    }

    _api_key.clear();
    _model_name  = DEFAULT_MODEL;
    _voice_name  = DEFAULT_VOICE;
    _voice_index = 0;
    _pitch_pct.store(DEFAULT_PITCH_PCT);
    return false;
}

/* -------------------------------------------------------------------------- */
/*                       Background Network & Audio Tasks                     */
/* -------------------------------------------------------------------------- */

void AppGemini::start_tasks()
{
    _net_task_exited = false;
    _spk_task_exited = false;

    // Must use internal SRAM stacks because startNetwork()/Wi-Fi/NVS/TLS access SPI Flash
    BaseType_t ok_net = xTaskCreatePinnedToCore(net_task_entry, "gemini_net", 8192, this, 4, nullptr, 0);
    if (ok_net != pdPASS) {
        _net_task_exited = true;
        ESP_LOGE(TAG, "Failed to create gemini_net task");
    }

    BaseType_t ok_spk = xTaskCreatePinnedToCore(spk_task_entry, "gemini_spk", 4096, this, 5, nullptr, 1);
    if (ok_spk != pdPASS) {
        _spk_task_exited = true;
        ESP_LOGE(TAG, "Failed to create gemini_spk task");
    }
}

void AppGemini::stop_tasks()
{
    uint32_t t0 = GetHAL().millis();
    while ((!_net_task_exited.load() || !_spk_task_exited.load()) && (GetHAL().millis() - t0 < 4000)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void AppGemini::net_task_entry(void* arg)
{
    auto* self = static_cast<AppGemini*>(arg);
    self->net_task_loop();
    self->_net_task_exited = true;
    vTaskDelete(nullptr);
}

void AppGemini::spk_task_entry(void* arg)
{
    auto* self = static_cast<AppGemini*>(arg);
    self->spk_task_loop();
    self->_spk_task_exited = true;
    vTaskDelete(nullptr);
}

bool AppGemini::send_setup_message(bool include_search, bool include_mcp_tools)
{
    if (!_ws || !_ws->IsConnected()) {
        return false;
    }

    cJSON* root  = cJSON_CreateObject();
    cJSON* setup = cJSON_AddObjectToObject(root, "setup");

    std::string model_path = _model_name;
    if (model_path.rfind("models/", 0) != 0) {
        model_path = "models/" + model_path;
    }
    cJSON_AddStringToObject(setup, "model", model_path.c_str());

    cJSON* gen_cfg    = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON* modalities = cJSON_AddArrayToObject(gen_cfg, "responseModalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));

    // Select voice preset
    cJSON* speech_cfg = cJSON_AddObjectToObject(gen_cfg, "speechConfig");
    cJSON* voice_cfg  = cJSON_AddObjectToObject(speech_cfg, "voiceConfig");
    cJSON* prebuilt   = cJSON_AddObjectToObject(voice_cfg, "prebuiltVoiceConfig");
    cJSON_AddStringToObject(prebuilt, "voiceName", _voice_name.empty() ? DEFAULT_VOICE : _voice_name.c_str());

    // Tools: optional Google Search + optional MCP escalation tools when /stackchanchat is attached
    if (include_search || include_mcp_tools) {
        cJSON* tools = cJSON_AddArrayToObject(setup, "tools");
        if (include_search) {
            cJSON* search_tool = cJSON_CreateObject();
            cJSON_AddObjectToObject(search_tool, "googleSearch");
            cJSON_AddItemToArray(tools, search_tool);
        }
        if (include_mcp_tools) {
            cJSON* fn_wrap = cJSON_CreateObject();
            cJSON* fns     = cJSON_AddArrayToObject(fn_wrap, "functionDeclarations");

            // escalate_to_engineer(task)
            cJSON* fn_esc = cJSON_CreateObject();
            cJSON_AddStringToObject(fn_esc, "name", "escalate_to_engineer");
            cJSON_AddStringToObject(
                fn_esc, "description",
                "Hand off a coding, repository, file, git, build, flash, project architecture, or "
                "relay-to-engineer request to the PC lead engineer agent.");
            cJSON* params = cJSON_AddObjectToObject(fn_esc, "parameters");
            cJSON_AddStringToObject(params, "type", "OBJECT");
            cJSON* props  = cJSON_AddObjectToObject(params, "properties");
            cJSON* p_task = cJSON_AddObjectToObject(props, "task");
            cJSON_AddStringToObject(p_task, "type", "STRING");
            cJSON_AddStringToObject(p_task, "description", "Concise English summary of the task for the coding agent.");
            cJSON* req = cJSON_AddArrayToObject(params, "required");
            cJSON_AddItemToArray(req, cJSON_CreateString("task"));
            cJSON_AddItemToArray(fns, fn_esc);

            // end_chat()
            cJSON* fn_end = cJSON_CreateObject();
            cJSON_AddStringToObject(fn_end, "name", "end_chat");
            cJSON_AddStringToObject(fn_end, "description",
                                    "Detach MCP mode when the user says goodbye, stop, terminamos, or end chat.");
            cJSON* end_params = cJSON_AddObjectToObject(fn_end, "parameters");
            cJSON_AddStringToObject(end_params, "type", "OBJECT");
            cJSON_AddObjectToObject(end_params, "properties");
            cJSON_AddItemToArray(fns, fn_end);

            cJSON_AddItemToArray(tools, fn_wrap);
        }
    }

    cJSON* sys_inst = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON* parts    = cJSON_AddArrayToObject(sys_inst, "parts");
    cJSON* part0    = cJSON_CreateObject();
    std::string sys_prompt =
        "You are StackChan, a calm, friendly, and conversational desktop robot companion. "
        "Speak in a relaxed, natural, balanced tone — never overly intense or hyper. "
        "Keep replies concise (1-2 short natural sentences) in the user's spoken language.";
    if (include_search) {
        sys_prompt += " You have live Google Search enabled; use it for current news, weather, dates, and live facts.";
    }
    if (include_mcp_tools) {
        sys_prompt +=
            " You are linked to the user's PC coding agent (lead engineer / Antigravity). "
            "If the user asks for code edits, repository/file/log inspection, git/build/flash commands, "
            "project architecture changes, OR asks you to pass/relay/tell anything to the engineer or agent, "
            "IMMEDIATELY call `escalate_to_engineer` without replying conversationally first. "
            "If the user says goodbye, terminamos, or end chat, call `end_chat`.";
        std::string ctx;
        {
            std::lock_guard<std::mutex> lk(_mcp_mtx);
            ctx = _mcp_context;
        }
        if (!ctx.empty()) {
            sys_prompt += " Recent context from lead engineer: " + ctx;
        }
    }
    cJSON_AddStringToObject(part0, "text", sys_prompt.c_str());
    cJSON_AddItemToArray(parts, part0);

    // Disable automatic VAD so the robot uses clean client-side adaptive VAD (zero echo, minimal Free Tier quota)
    cJSON* rt_cfg = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON* aad    = cJSON_AddObjectToObject(rt_cfg, "automaticActivityDetection");
    cJSON_AddBoolToObject(aad, "disabled", true);

    // Enable both input and output audio transcription
    cJSON_AddObjectToObject(setup, "inputAudioTranscription");
    cJSON_AddObjectToObject(setup, "outputAudioTranscription");

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return false;
    }

    std::string payload(printed);
    cJSON_free(printed);
    ESP_LOGI(TAG, "Sending Gemini Live setup (%s, search=%d, mcp=%d, voice=%s)", model_path.c_str(),
             include_search ? 1 : 0, include_mcp_tools ? 1 : 0, _voice_name.c_str());
    return _ws->Send(payload);
}

bool AppGemini::connect_live_session()
{
    disconnect_live_session();

    set_state(SessionState::ConnectingWifi);
    set_subtitle("Connecting to Wi-Fi...");

    GetHAL().startNetwork([this](std::string_view log_msg) {
        set_subtitle(std::string(log_msg));
    });

    if (_closing.load()) {
        return false;
    }

    // Start lightweight MCP HTTP listener on port 80 once Wi-Fi is ready
    {
        auto ip = WifiManager::GetInstance().GetIpAddress();
        std::lock_guard<std::mutex> lk(_mcp_mtx);
        _ip_str = ip;
    }
    if (!_httpd) {
        _httpd_ok = start_httpd();
    }

    auto* network = Board::GetInstance().GetNetwork();
    if (!network) {
        set_error("Network interface unavailable");
        return false;
    }

    set_state(SessionState::ConnectingApi);
    set_subtitle("Connecting to Gemini Live...");

    _setup_complete   = false;
    _server_turn_done = false;

    _ws = network->CreateWebSocket(0);
    if (!_ws) {
        set_error("Failed to create WebSocket");
        return false;
    }
    _ws->SetReceiveBufferSize(16384);

    _ws->OnData([this](const char* data, size_t len, bool /*binary*/) {
        if (!_closing.load() && data && len > 0) {
            handle_server_json(data, len);
        }
    });

    _ws->OnError([this](const NetworkError& err) {
        ESP_LOGE(TAG, "WebSocket error: %s", err.ToString().c_str());
        if (!_closing.load()) {
            set_error("Connection error: " + err.ToString());
        }
    });

    _ws->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Gemini WebSocket disconnected");
        if (!_closing.load()) {
            SessionState cur = static_cast<SessionState>(_state.load());
            if (cur != SessionState::Error && cur != SessionState::NeedKey &&
                cur != SessionState::ConnectingApi) {
                set_error("Session closed. Tap RETRY.");
            }
        }
    });

    std::string url =
        "wss://generativelanguage.googleapis.com:443/ws/"
        "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" +
        _api_key;

    bool try_search = _use_google_search.load();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {
            // Setup closed immediately (often Free Tier quota=0 for googleSearch); retry cleanly without search
            ESP_LOGW(TAG, "Retrying Gemini Live setup without googleSearch...");
            set_subtitle("Retrying without web search...");
            _use_google_search = false;
            try_search         = false;
            disconnect_live_session();
            _ws = network->CreateWebSocket(0);
            if (!_ws) {
                set_error("Failed to create WebSocket");
                return false;
            }
            _ws->SetReceiveBufferSize(16384);
            _ws->OnData([this](const char* data, size_t len, bool /*binary*/) {
                if (!_closing.load() && data && len > 0) {
                    handle_server_json(data, len);
                }
            });
            _ws->OnError([this](const NetworkError& err) {
                ESP_LOGE(TAG, "WebSocket error: %s", err.ToString().c_str());
                if (!_closing.load()) {
                    set_error("Connection error: " + err.ToString());
                }
            });
            _ws->OnDisconnected([this]() {
                ESP_LOGW(TAG, "Gemini WebSocket disconnected");
                if (!_closing.load()) {
                    SessionState cur = static_cast<SessionState>(_state.load());
                    if (cur != SessionState::Error && cur != SessionState::NeedKey &&
                        cur != SessionState::ConnectingApi) {
                        set_error("Session closed. Tap RETRY.");
                    }
                }
            });
        }

        _setup_complete   = false;
        _server_turn_done = false;
        set_state(SessionState::ConnectingApi);

        auto res = _ws->Connect(url.c_str());
        if (!res) {
            ESP_LOGE(TAG, "Connect failed: %s", res.error().ToString().c_str());
            set_error("Connect failed (check Wi-Fi / API key)");
            disconnect_live_session();
            return false;
        }

        if (!send_setup_message(try_search, _mcp_attached.load())) {
            set_error("Failed to send setup message");
            disconnect_live_session();
            return false;
        }

        // Wait up to 8 seconds for setupComplete
        uint32_t t0 = GetHAL().millis();
        while (!_closing.load() && !_setup_complete.load() && _ws && _ws->IsConnected()) {
            if (GetHAL().millis() - t0 > 8000) {
                set_error("Setup timeout (check API key / model)");
                disconnect_live_session();
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
        }

        if (_closing.load()) {
            return false;
        }
        if (_setup_complete.load()) {
            break;
        }
        // If we already tried without search, report quota/rejection clearly
        if (!try_search) {
            set_error("API quota exceeded. Wait & tap RETRY.");
            disconnect_live_session();
            return false;
        }
    }

    if (!_setup_complete.load()) {
        return false;
    }

    if (_mcp_agent_active.load() && !_mcp_wait_reply_mode.load()) {
        set_state(SessionState::AgentWait);
        set_subtitle("Waiting for lead engineer...");
    } else if (_muted.load()) {
        set_state(SessionState::Ready);
        set_subtitle("Paused. Tap RESUME to talk");
    } else {
        _want_arm_listen = true;
    }
    return true;
}

void AppGemini::disconnect_live_session()
{
    if (_ws) {
        _ws->Close();
        _ws.reset();
    }
    _setup_complete = false;
}

void AppGemini::handle_server_json(const char* data, size_t len)
{
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "Non-JSON server frame (%u bytes)", static_cast<unsigned>(len));
        return;
    }

    // Handle server-side errors cleanly
    cJSON* err_obj = cJSON_GetObjectItem(root, "error");
    if (err_obj) {
        cJSON* msg = cJSON_GetObjectItem(err_obj, "message");
        std::string err_str = (msg && cJSON_IsString(msg)) ? msg->valuestring : "API error";
        ESP_LOGE(TAG, "Gemini error: %s", err_str.c_str());
        set_error(err_str);
        cJSON_Delete(root);
        return;
    }

    if (cJSON_GetObjectItem(root, "setupComplete") != nullptr) {
        ESP_LOGI(TAG, "Gemini Live setupComplete");
        _setup_complete = true;
    }

    cJSON* sc = cJSON_GetObjectItem(root, "serverContent");
    if (sc && cJSON_IsObject(sc)) {
        cJSON* interrupted = cJSON_GetObjectItem(sc, "interrupted");
        if (interrupted && cJSON_IsTrue(interrupted)) {
            _pcm_ring.clear();
            _audio_rms = 0;
        }

        // Process ALL parts in modelTurn (audio chunks, etc.)
        cJSON* mt = cJSON_GetObjectItem(sc, "modelTurn");
        if (mt && cJSON_IsObject(mt)) {
            cJSON* parts = cJSON_GetObjectItem(mt, "parts");
            if (parts && cJSON_IsArray(parts)) {
                cJSON* part = nullptr;
                cJSON_ArrayForEach(part, parts)
                {
                    cJSON* inline_data = cJSON_GetObjectItem(part, "inlineData");
                    if (!inline_data || !cJSON_IsObject(inline_data)) {
                        continue;
                    }
                    cJSON* b64 = cJSON_GetObjectItem(inline_data, "data");
                    if (!b64 || !cJSON_IsString(b64) || !b64->valuestring) {
                        continue;
                    }
                    const size_t b64_len = std::strlen(b64->valuestring);
                    if (b64_len == 0 || !_b64_decode_buf) {
                        continue;
                    }
                    size_t out_len = 0;
                    int dec_ret    = mbedtls_base64_decode(
                        _b64_decode_buf, _b64_decode_cap, &out_len,
                        reinterpret_cast<const unsigned char*>(b64->valuestring), b64_len);
                    if (dec_ret == 0 && out_len >= 2) {
                        if (!_mcp_wait_reply_mode.load()) {
                            size_t samples = out_len / sizeof(int16_t);
                            _pcm_ring.push(reinterpret_cast<const int16_t*>(_b64_decode_buf), samples);
                            SessionState cur = static_cast<SessionState>(_state.load());
                            if (cur == SessionState::Thinking || cur == SessionState::Ready) {
                                set_state(SessionState::Speaking);
                            }
                        }
                    } else if (dec_ret != 0) {
                        ESP_LOGW(TAG, "Base64 decode failed (ret=%d, in=%u)", dec_ret, static_cast<unsigned>(b64_len));
                    }
                }
            }
        }

        // Input audio transcription (user speech text for escalation & --wait-reply)
        cJSON* in_tr = cJSON_GetObjectItem(sc, "inputTranscription");
        if (in_tr && cJSON_IsObject(in_tr)) {
            cJSON* txt = cJSON_GetObjectItem(in_tr, "text");
            if (txt && cJSON_IsString(txt) && txt->valuestring && txt->valuestring[0] != '\0') {
                std::lock_guard<std::mutex> lk(_mcp_mtx);
                _mcp_last_user_text += txt->valuestring;
                if (_mcp_wait_reply_mode.load()) {
                    _mcp_reply_text += txt->valuestring;
                }
            }
        }

        // Output audio transcription (live captions)
        cJSON* out_tr = cJSON_GetObjectItem(sc, "outputTranscription");
        if (out_tr && cJSON_IsObject(out_tr)) {
            cJSON* txt = cJSON_GetObjectItem(out_tr, "text");
            if (txt && cJSON_IsString(txt) && txt->valuestring && txt->valuestring[0] != '\0' &&
                !_mcp_wait_reply_mode.load()) {
                bool reset = _new_turn_subtitle.exchange(false);
                set_subtitle(txt->valuestring, !reset);
            }
        }

        cJSON* tc = cJSON_GetObjectItem(sc, "turnComplete");
        if (tc && cJSON_IsTrue(tc)) {
            if (_mcp_wait_reply_mode.exchange(false)) {
                _pcm_ring.clear();
                _mcp_reply_ready = true;
                set_state(SessionState::AgentWait);
                set_subtitle("Reply sent to engineer...");
            } else {
                _server_turn_done  = true;
                _new_turn_subtitle = true;
            }
        }
    }

    // Handle MCP toolCall (escalate_to_engineer / end_chat)
    cJSON* tool_call = cJSON_GetObjectItem(root, "toolCall");
    if (tool_call && cJSON_IsObject(tool_call)) {
        // If we were waiting for a direct reply to AGENT $, capture the user's text immediately instead of escalating again
        if (_mcp_wait_reply_mode.exchange(false)) {
            _pcm_ring.clear();
            {
                std::lock_guard<std::mutex> lk(_mcp_mtx);
                if (_mcp_reply_text.empty()) {
                    _mcp_reply_text = _mcp_last_user_text;
                }
            }
            _mcp_reply_ready = true;
            set_state(SessionState::AgentWait);
            set_subtitle("Reply sent to engineer...");
            cJSON_Delete(root);
            return;
        }
        cJSON* fcs = cJSON_GetObjectItem(tool_call, "functionCalls");
        if (fcs && cJSON_IsArray(fcs)) {
            cJSON* fc = nullptr;
            cJSON_ArrayForEach(fc, fcs)
            {
                cJSON* jname = cJSON_GetObjectItem(fc, "name");
                cJSON* jid   = cJSON_GetObjectItem(fc, "id");
                if (!jname || !cJSON_IsString(jname) || !jname->valuestring) {
                    continue;
                }
                std::string fn_name = jname->valuestring;
                std::string fn_id   = (jid && cJSON_IsString(jid) && jid->valuestring) ? jid->valuestring : "";
                if (fn_name == "escalate_to_engineer") {
                    std::string task = "User requested technical assistance";
                    cJSON* jargs     = cJSON_GetObjectItem(fc, "args");
                    if (jargs && cJSON_IsObject(jargs)) {
                        cJSON* jtask = cJSON_GetObjectItem(jargs, "task");
                        if (jtask && cJSON_IsString(jtask) && jtask->valuestring && jtask->valuestring[0] != '\0') {
                            task = jtask->valuestring;
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lk(_mcp_mtx);
                        _mcp_escalation_task   = task;
                        _mcp_escalation_user   = _mcp_last_user_text;
                        _mcp_handoff_call_id   = fn_id;
                        _mcp_handoff_call_name = fn_name;
                    }
                    _mcp_handoff_pending = true;
                    break;
                } else if (fn_name == "end_chat") {
                    {
                        std::lock_guard<std::mutex> lk(_mcp_mtx);
                        _mcp_handoff_call_id   = fn_id;
                        _mcp_handoff_call_name = fn_name;
                    }
                    _mcp_end_pending = true;
                    break;
                }
            }
        }
    }

    cJSON_Delete(root);
}

bool AppGemini::send_pcm16k_chunk(const int16_t* pcm, size_t samples)
{
    if (!_ws || !_ws->IsConnected() || !_mic_b64_buf || !pcm || samples == 0) {
        return false;
    }

    const size_t raw_bytes = samples * sizeof(int16_t);
    size_t b64_len         = 0;
    if (mbedtls_base64_encode(_mic_b64_buf, 4399, &b64_len, reinterpret_cast<const unsigned char*>(pcm), raw_bytes) !=
        0) {
        return false;
    }
    _mic_b64_buf[b64_len] = '\0';

    std::string msg;
    msg.reserve(b64_len + 96);
    msg.append("{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"");
    msg.append(reinterpret_cast<const char*>(_mic_b64_buf), b64_len);
    msg.append("\"}}}");

    if (!_ws->Send(msg)) {
        ESP_LOGW(TAG, "Failed to send audio chunk");
        return false;
    }
    return true;
}

void AppGemini::process_mic_vad()
{
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !_ws || !_ws->IsConnected() || !_mic_pcm16k_buf || !_mic_b64_buf) {
        return;
    }

    // Read 100 ms @ 24 kHz = 2400 frames; CoreS3 has 2 input channels (ch0=mic, ch1=ref)
    constexpr size_t kInFrames = 2400;
    const size_t in_ch         = std::max(codec->input_channels(), 1);

    std::vector<int16_t> raw_in(kInFrames * in_ch);
    if (!codec->InputData(raw_in)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    // Extract mic channel (ch0) & 3:2 downsample 24 kHz -> 16 kHz + compute RMS
    int64_t sum_sq = 0;
    for (size_t g = 0; g < kInFrames / 3; ++g) {
        int32_t s0                 = raw_in[(g * 3 + 0) * in_ch];
        int32_t s1                 = raw_in[(g * 3 + 1) * in_ch];
        int32_t s2                 = raw_in[(g * 3 + 2) * in_ch];
        int16_t o0                 = static_cast<int16_t>(s0);
        int16_t o1                 = static_cast<int16_t>((s1 + s2) / 2);
        _mic_pcm16k_buf[g * 2 + 0] = o0;
        _mic_pcm16k_buf[g * 2 + 1] = o1;
        sum_sq += static_cast<int64_t>(o0) * o0 + static_cast<int64_t>(o1) * o1;
    }
    int rms = static_cast<int>(std::sqrt(static_cast<double>(sum_sq) / static_cast<double>(MIC_OUT_FRAMES)));
    _audio_rms.store(rms);

    uint32_t now     = GetHAL().millis();
    SessionState cur = static_cast<SessionState>(_state.load());
    const int sens   = std::clamp<int>(_mic_sens.load(), 0, 2);
    // 0=LOW (quiet/strict), 1=MED (balanced default), 2=HIGH (sensitive)
    static constexpr int kMinTrig[3]   = {1400, 950, 620};
    static constexpr float kTrigMul[3] = {3.6f, 3.0f, 2.4f};
    static constexpr int kMinHold[3]   = {800, 560, 400};
    static constexpr float kHoldMul[3] = {2.2f, 1.9f, 1.65f};
    static constexpr int kHitsReq[3]   = {3, 3, 2};
    const int trigger_rms              = std::max<int>(kMinTrig[sens], static_cast<int>(_noise_floor * kTrigMul[sens]));
    const int hold_rms                 = std::max<int>(kMinHold[sens], static_cast<int>(_noise_floor * kHoldMul[sens]));
    const int hits_needed              = kHitsReq[sens];

    if (cur == SessionState::Listening) {
        // Save chunk in circular 300 ms pre-roll buffer
        if (_preroll_buf) {
            std::memcpy(_preroll_buf + (_preroll_write_idx * MIC_OUT_FRAMES), _mic_pcm16k_buf,
                        MIC_OUT_FRAMES * sizeof(int16_t));
            _preroll_write_idx = (_preroll_write_idx + 1) % PREROLL_CHUNKS;
            if (_preroll_count < PREROLL_CHUNKS) {
                ++_preroll_count;
            }
        }

        if (rms >= trigger_rms) {
            ++_speech_hits;
            if (_speech_hits >= hits_needed) {
                ESP_LOGI(TAG, "Voice detected (rms=%d, floor=%.0f, trig=%d)", rms, _noise_floor, trigger_rms);
                _speech_start_ms = now;
                _last_voice_ms   = now;
                {
                    std::lock_guard<std::mutex> lk(_mcp_mtx);
                    _mcp_last_user_text.clear();
                }
                _ws->Send("{\"realtimeInput\":{\"activityStart\":{}}}");

                // Flush pre-roll buffer in chronological order so first syllable is preserved
                if (_preroll_buf && _preroll_count > 0) {
                    int start_idx = (_preroll_write_idx - _preroll_count + PREROLL_CHUNKS) % PREROLL_CHUNKS;
                    for (int k = 0; k < _preroll_count; ++k) {
                        int slot = (start_idx + k) % PREROLL_CHUNKS;
                        send_pcm16k_chunk(_preroll_buf + (slot * MIC_OUT_FRAMES), MIC_OUT_FRAMES);
                    }
                } else {
                    send_pcm16k_chunk(_mic_pcm16k_buf, MIC_OUT_FRAMES);
                }

                _preroll_count = 0;
                set_state(SessionState::Hearing);
                set_subtitle("Hearing you...");
            }
        } else {
            _speech_hits = 0;
            // Gently track ambient room noise floor while silent
            _noise_floor = std::clamp(_noise_floor * 0.92f + static_cast<float>(rms) * 0.08f, 120.0f, 1200.0f);
        }
    } else if (cur == SessionState::Hearing) {
        send_pcm16k_chunk(_mic_pcm16k_buf, MIC_OUT_FRAMES);
        if (rms >= hold_rms) {
            _last_voice_ms = now;
        }
        // Auto-send after ~1.8 seconds of silence pause
        if (now - _last_voice_ms >= SILENCE_END_MS) {
            ESP_LOGI(TAG, "Pause detected (%lu ms), ending turn",
                     static_cast<unsigned long>(now - _last_voice_ms));
            _want_stop_talk = true;
        }
    }
}

void AppGemini::net_task_loop()
{
    auto* codec = Board::GetInstance().GetAudioCodec();

    while (!_closing.load()) {
        if (_want_connect.exchange(false)) {
            connect_live_session();
            continue;
        }

        if (_mcp_play_pending.exchange(false)) {
            std::string url;
            bool wait_after = false;
            {
                std::lock_guard<std::mutex> lk(_mcp_mtx);
                url        = _mcp_next_play_url;
                wait_after = _mcp_play_wait_reply.load();
                _mcp_next_play_url.clear();
            }
            if (!url.empty()) {
                if (codec && codec->input_enabled()) {
                    codec->EnableInput(false);
                }
                play_http_wav(url, wait_after);
            }
            continue;
        }

        if (_mcp_handoff_pending.exchange(false)) {
            std::string cid;
            {
                std::lock_guard<std::mutex> lk(_mcp_mtx);
                cid = _mcp_handoff_call_id;
            }
            if (_ws && _ws->IsConnected()) {
                _mcp_handoff_speaking   = true;
                _mcp_handoff_started_ms = GetHAL().millis();
                _server_turn_done       = false;
                if (!cid.empty()) {
                    std::string tr = "{\"toolResponse\":{\"functionResponses\":[{\"id\":\"" + cid +
                                     "\",\"name\":\"escalate_to_engineer\",\"response\":{\"status\":\"ok\"}}]}}";
                    _ws->Send(tr);
                }
                _ws->Send(
                    "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":"
                    "\"Di exactamente esta frase corta en el idioma del usuario y nada mas: "
                    "Paso la orden al ingeniero principal.\"}]}],\"turnComplete\":true}}");
            } else {
                // Offline/disconnected fallback: release escalation immediately
                _mcp_agent_active     = true;
                _mcp_escalation_ready = true;
                set_state(SessionState::AgentWait);
                set_subtitle("Waiting for lead engineer...");
            }
            continue;
        }

        if (_mcp_end_pending.exchange(false)) {
            std::string cid;
            {
                std::lock_guard<std::mutex> lk(_mcp_mtx);
                cid = _mcp_handoff_call_id;
            }
            if (_ws && _ws->IsConnected()) {
                if (!cid.empty()) {
                    std::string tr = "{\"toolResponse\":{\"functionResponses\":[{\"id\":\"" + cid +
                                     "\",\"name\":\"end_chat\",\"response\":{\"status\":\"ok\"}}]}}";
                    _ws->Send(tr);
                }
                _ws->Send(
                    "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":"
                    "\"Di exactamente esta frase corta en el idioma del usuario y nada mas: "
                    "Listo, cerramos el modo agente. Sigo disponible aqui.\"}]}],\"turnComplete\":true}}");
            }
            _mcp_attached       = false;
            _mcp_agent_active   = false;
            _mcp_lease_until_ms = 0;
            continue;
        }

        if (_want_mute.exchange(false)) {
            _muted = true;
            if (codec && codec->input_enabled()) {
                codec->EnableInput(false);
            }
            if (static_cast<SessionState>(_state.load()) == SessionState::Hearing && _ws && _ws->IsConnected()) {
                _ws->Send("{\"realtimeInput\":{\"activityEnd\":{}}}");
            }
            _audio_rms       = 0;
            _speech_hits     = 0;
            _preroll_count   = 0;
            _speech_start_ms = 0;
            set_state(SessionState::Ready);
            set_subtitle("Paused. Tap RESUME to talk");
            continue;
        }

        if (_want_arm_listen.exchange(false)) {
            if (_muted.load()) {
                set_state(SessionState::Ready);
                set_subtitle("Paused. Tap RESUME to talk");
                continue;
            }
            if (!_ws || !_ws->IsConnected() || !_setup_complete.load()) {
                if (!connect_live_session()) {
                    continue;
                }
            }

            // Flush any remaining speaker audio (safe for barge-in or post-reply re-arm)
            _pcm_ring.clear();
            _server_turn_done  = false;
            _new_turn_subtitle = true;
            _audio_rms         = 0;
            _speech_hits       = 0;
            _preroll_count     = 0;
            _preroll_write_idx = 0;
            _speech_start_ms   = 0;
            _last_voice_ms     = 0;

            if (codec && codec->output_enabled()) {
                codec->EnableOutput(false);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            if (codec && !codec->input_enabled()) {
                codec->EnableInput(true);
            }
            set_state(SessionState::Listening);
            set_subtitle(_mcp_wait_reply_mode.load() ? "Reply to engineer..." : "Speak naturally...");
        }

        if (_want_stop_talk.exchange(false)) {
            if (codec && codec->input_enabled()) {
                codec->EnableInput(false);
            }
            _audio_rms       = 0;
            _speech_hits     = 0;
            _preroll_count   = 0;
            _speech_start_ms = 0;

            if (_ws && _ws->IsConnected()) {
                _ws->Send("{\"realtimeInput\":{\"activityEnd\":{}}}");
                set_state(SessionState::Thinking);
                set_subtitle("Thinking...");
            }
        }

        SessionState st = static_cast<SessionState>(_state.load());
        if (st == SessionState::Listening || st == SessionState::Hearing) {
            process_mic_vad();
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    if (codec && codec->input_enabled()) {
        codec->EnableInput(false);
    }
    disconnect_live_session();
}

void AppGemini::spk_task_loop()
{
    auto* codec                  = Board::GetInstance().GetAudioCodec();
    constexpr size_t kOutSamples = 480;   // 20 ms @ 24 kHz
    std::vector<int16_t> resample_buf;
    resample_buf.reserve(1024);
    std::vector<int16_t> pull_buf(768);
    std::vector<int16_t> chunk(kOutSamples);
    double frac_pos    = 0.0;
    bool output_active = false;

    while (!_closing.load()) {
        const bool is_mcp_wav = _mcp_playing_wav.load();
        const int pitch_pct   = is_mcp_wav ? 100 : std::clamp(_pitch_pct.load(), MIN_PITCH_PCT, MAX_PITCH_PCT);
        const double ratio    = static_cast<double>(pitch_pct) / 100.0;
        const size_t need   = (pitch_pct == 100) ? kOutSamples
                                                 : static_cast<size_t>(std::ceil(kOutSamples * ratio)) + 2;

        // Top up local resample buffer from PSRAM ring buffer
        if (resample_buf.size() < need) {
            size_t want = std::min(pull_buf.size(), need - resample_buf.size());
            size_t got  = _pcm_ring.pop(pull_buf.data(), want);
            if (got > 0) {
                resample_buf.insert(resample_buf.end(), pull_buf.begin(), pull_buf.begin() + got);
            }
        }

        const bool has_enough = (resample_buf.size() >= need);
        const bool draining   = (!resample_buf.empty() && _pcm_ring.available() == 0 && _server_turn_done.load());

        if (codec && (has_enough || draining)) {
            if (!output_active) {
                codec->SetOutputVolume(_volume);
                codec->EnableOutput(true);
                output_active = true;
            }

            if (pitch_pct == 100) {
                size_t copy_n = std::min(kOutSamples, resample_buf.size());
                std::memcpy(chunk.data(), resample_buf.data(), copy_n * sizeof(int16_t));
                if (copy_n < kOutSamples) {
                    std::fill(chunk.begin() + copy_n, chunk.end(), 0);
                }
                resample_buf.erase(resample_buf.begin(), resample_buf.begin() + copy_n);
                frac_pos = 0.0;
            } else {
                // Ensure enough zero-padded tail when draining at end of turn
                const bool was_padded_tail = (resample_buf.size() < need);
                if (was_padded_tail) {
                    resample_buf.resize(need, 0);
                }
                for (size_t i = 0; i < kOutSamples; ++i) {
                    size_t idx = static_cast<size_t>(frac_pos);
                    double f   = frac_pos - static_cast<double>(idx);
                    double s0  = static_cast<double>(resample_buf[idx]);
                    double s1  = static_cast<double>(resample_buf[idx + 1]);
                    chunk[i]   = static_cast<int16_t>(s0 + (s1 - s0) * f);
                    frac_pos += ratio;
                }
                size_t consumed = static_cast<size_t>(frac_pos);
                frac_pos -= static_cast<double>(consumed);
                if (was_padded_tail || consumed >= resample_buf.size()) {
                    resample_buf.clear();
                    frac_pos = 0.0;
                } else {
                    resample_buf.erase(resample_buf.begin(), resample_buf.begin() + consumed);
                }
            }

            // Compute voice RMS for live mouth animation & LED pulse
            int64_t sum_sq = 0;
            for (size_t i = 0; i < kOutSamples; ++i) {
                int32_t s = chunk[i];
                sum_sq += static_cast<int64_t>(s) * s;
            }
            int rms = static_cast<int>(std::sqrt(static_cast<double>(sum_sq) / static_cast<double>(kOutSamples)));
            _audio_rms.store(rms);

            codec->OutputData(chunk);
        } else {
            if (output_active && _pcm_ring.available() == 0 && resample_buf.empty()) {
                // Keep output open briefly between network packets unless turn is complete or interrupted
                if (_server_turn_done.load() ||
                    static_cast<SessionState>(_state.load()) != SessionState::Speaking) {
                    if (codec) {
                        // Flush 120 ms of silence through I2S DMA so the final syllable never clips
                        std::fill(chunk.begin(), chunk.end(), 0);
                        for (int z = 0; z < 6; ++z) {
                            codec->OutputData(chunk);
                        }
                        codec->EnableOutput(false);
                    }
                    output_active = false;
                    frac_pos      = 0.0;
                    _audio_rms.store(0);
                    if (_server_turn_done.exchange(false) &&
                        static_cast<SessionState>(_state.load()) == SessionState::Speaking) {
                        // Brief echo guard before automatically re-arming hands-free mic
                        vTaskDelay(pdMS_TO_TICKS(ECHO_GUARD_MS));
                        if (_mcp_handoff_speaking.exchange(false)) {
                            // Charon finished speaking handoff: NOW switch to AGENT $ and release escalation to PC
                            _mcp_agent_active     = true;
                            _mcp_escalation_ready = true;
                            set_state(SessionState::AgentWait);
                            set_subtitle("Waiting for lead engineer...");
                        } else if (_mcp_wait_reply_mode.load() || _want_arm_listen.load()) {
                            // Wait-reply mode: play_http_wav arms listening cleanly; do NOT force AgentWait
                            _want_arm_listen = true;
                        } else if (_mcp_agent_active.load()) {
                            set_state(SessionState::AgentWait);
                            set_subtitle("Waiting for lead engineer...");
                        } else if (!_closing.load() && !_muted.load()) {
                            _want_arm_listen = true;
                        } else if (!_closing.load()) {
                            set_state(SessionState::Ready);
                        }
                    }
                }
            } else if (static_cast<SessionState>(_state.load()) != SessionState::Speaking && !resample_buf.empty()) {
                // Flush stale resample tail on barge-in
                resample_buf.clear();
                frac_pos = 0.0;
            } else if (!output_active && _server_turn_done.load() &&
                       static_cast<SessionState>(_state.load()) == SessionState::Thinking) {
                // Model completed a turn with no audio output; re-arm listening cleanly
                _server_turn_done = false;
                if (!_closing.load() && !_muted.load()) {
                    _want_arm_listen = true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (output_active && codec) {
        codec->EnableOutput(false);
    }
}

/* -------------------------------------------------------------------------- */
/*                        MCP HTTP Bridge & Motion                            */
/* -------------------------------------------------------------------------- */

bool AppGemini::start_httpd()
{
    if (_httpd) {
        return true;
    }
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.stack_size        = 6144;
    cfg.max_uri_handlers  = 16;
    cfg.max_open_sockets  = 6;
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 8;
    cfg.send_wait_timeout = 8;
    cfg.task_priority     = 4;

    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start MCP HTTP server on :80");
        return false;
    }

    const httpd_uri_t uris[] = {
        {"/audio/status",    HTTP_GET,  handle_audio_status,    this},
        {"/playback/status", HTTP_GET,  handle_playback_status, this},
        {"/play",            HTTP_POST, handle_play,            this},
        {"/mcp/attach",      HTTP_POST, handle_mcp_attach,      this},
        {"/mcp/detach",      HTTP_POST, handle_mcp_detach,      this},
        {"/mcp/escalation",  HTTP_GET,  handle_mcp_escalation,  this},
        {"/mcp/wait_reply",  HTTP_POST, handle_mcp_wait_reply,  this},
        {"/mcp/reply",       HTTP_GET,  handle_mcp_reply,       this},
        {"/agent_mode",      HTTP_POST, handle_agent_mode,      this},
        {"/listen/resume",   HTTP_POST, handle_listen_resume,   this},
        {"/move",            HTTP_POST, handle_move,            this},
        {"/nod",             HTTP_POST, handle_nod,             this},
        {"/shake",           HTTP_POST, handle_shake,           this},
        {"/home",            HTTP_POST, handle_home,            this},
        {"/face",            HTTP_POST, handle_face,            this},
    };
    for (const auto& u : uris) {
        httpd_register_uri_handler(srv, &u);
    }

    _httpd = srv;
    ESP_LOGI(TAG, "Unified Gemini+MCP HTTP server ready on :80");
    return true;
}

void AppGemini::stop_httpd()
{
    if (_httpd) {
        httpd_stop(static_cast<httpd_handle_t>(_httpd));
        _httpd = nullptr;
    }
    _httpd_ok = false;
}

void AppGemini::play_http_wav(const std::string& url, bool wait_reply_after)
{
    _mcp_play_abort      = false;
    _mcp_playing_wav     = true;
    _mcp_play_started_ms = GetHAL().millis();
    _pcm_ring.clear();
    _server_turn_done = false;
    set_state(SessionState::Speaking);
    std::string sub_txt;
    {
        std::lock_guard<std::mutex> lk(_mcp_mtx);
        sub_txt = _mcp_next_subtitle;
        _mcp_next_subtitle.clear();
    }
    set_subtitle(sub_txt.empty() ? "Lead engineer speaking..." : sub_txt);

    esp_http_client_config_t cfg = {};
    cfg.url                      = url.c_str();
    cfg.method                   = HTTP_METHOD_GET;
    cfg.timeout_ms               = 4000;
    cfg.buffer_size              = 4096;
    cfg.buffer_size_tx           = 1024;
    if (url.rfind("https://", 0) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        _mcp_playing_wav = false;
        return;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        _mcp_playing_wav = false;
        return;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        _mcp_playing_wav = false;
        return;
    }

    uint8_t riff[12];
    uint32_t src_rate = 24000;
    uint16_t channels = 1;
    bool found_data   = false;
    if (esp_http_client_read(client, reinterpret_cast<char*>(riff), 12) == 12 &&
        std::memcmp(riff, "RIFF", 4) == 0 && std::memcmp(riff + 8, "WAVE", 4) == 0) {
        for (int c = 0; c < 12 && !_closing.load() && !_mcp_play_abort.load(); ++c) {
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
        constexpr size_t RAW_BYTES = 2048;
        uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(RAW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        std::vector<int16_t> out24k;
        out24k.reserve(1024);
        size_t leftover = 0;

        while (raw && !_closing.load() && !_mcp_play_abort.load()) {
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
                if (src_rate == 24000 && channels == 1) {
                    size_t pushed = 0;
                    while (pushed < frames && !_closing.load() && !_mcp_play_abort.load()) {
                        pushed += _pcm_ring.push(src + pushed, frames - pushed);
                        if (pushed < frames) vTaskDelay(pdMS_TO_TICKS(10));
                    }
                } else {
                    int out_n = static_cast<int>((static_cast<uint64_t>(frames) * 24000ULL) / src_rate);
                    out24k.resize(std::max<int>(1, out_n));
                    for (int i = 0; i < out_n; ++i) {
                        uint64_t fp = (static_cast<uint64_t>(i) * src_rate << 12) / 24000ULL;
                        int idx0    = std::min<int>(static_cast<int>(fp >> 12), static_cast<int>(frames - 1));
                        int idx1    = std::min<int>(idx0 + 1, static_cast<int>(frames - 1));
                        int frac    = static_cast<int>(fp & 0xFFF);
                        int32_t s0  = (channels >= 2) ? ((int32_t)src[idx0 * channels] + src[idx0 * channels + 1]) / 2
                                                      : src[idx0];
                        int32_t s1  = (channels >= 2) ? ((int32_t)src[idx1 * channels] + src[idx1 * channels + 1]) / 2
                                                      : src[idx1];
                        out24k[i]   = static_cast<int16_t>(s0 + (((s1 - s0) * frac) >> 12));
                    }
                    size_t pushed = 0;
                    while (pushed < out24k.size() && !_closing.load() && !_mcp_play_abort.load()) {
                        pushed += _pcm_ring.push(out24k.data() + pushed, out24k.size() - pushed);
                        if (pushed < out24k.size()) vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
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

    // Prepare wait_reply flag BEFORE signaling _server_turn_done so spk_task_loop never forces AgentWait
    if (wait_reply_after && !_closing.load()) {
        {
            std::lock_guard<std::mutex> lk(_mcp_mtx);
            _mcp_reply_text.clear();
        }
        _mcp_reply_ready     = false;
        _mcp_wait_reply_mode = true;
    }

    // Wait for speaker ring buffer to drain cleanly
    _server_turn_done = true;
    uint32_t t0       = GetHAL().millis();
    while (!_closing.load() && !_mcp_play_abort.load() && _pcm_ring.available() > 0 &&
           (GetHAL().millis() - t0 < 20000)) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    vTaskDelay(pdMS_TO_TICKS(150));
    _mcp_playing_wav = false;

    if (wait_reply_after && !_closing.load()) {
        _want_arm_listen = true;
    }
}

void AppGemini::process_motion_cmds()
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
        if (cmd.kind == "move") {
            _gesture_type = 0;
            motion.moveWithSpeed(deg_to_yaw(cmd.x_deg), deg_to_pitch(cmd.y_deg), std::clamp(cmd.speed, 100, 1000));
        } else if (cmd.kind == "home") {
            _gesture_type = 0;
            motion.moveWithSpeed(deg_to_yaw(0), deg_to_pitch(Y_HOME_DEG), 500);
        } else if (cmd.kind == "nod") {
            _gesture_type = 1;
            _gesture_step = 0;
            _gesture_next = now;
        } else if (cmd.kind == "shake") {
            _gesture_type = 2;
            _gesture_step = 0;
            _gesture_next = now;
        }
    }
}

void AppGemini::run_gesture_step(uint32_t now)
{
    if (_gesture_type == 0 || static_cast<int32_t>(now - _gesture_next) < 0) {
        return;
    }
    auto& motion = GetStackChan().motion();
    if (_gesture_type == 1) {
        static const int nod_y[4] = {30, 58, 34, Y_HOME_DEG};
        motion.moveWithSpeed(deg_to_yaw(0), deg_to_pitch(nod_y[_gesture_step]), 700);
        _gesture_next = now + 260;
        if (++_gesture_step >= 4) _gesture_type = 0;
    } else if (_gesture_type == 2) {
        static const int shake_x[4] = {-22, 22, -16, 0};
        motion.moveWithSpeed(deg_to_yaw(shake_x[_gesture_step]), deg_to_pitch(Y_HOME_DEG), 700);
        _gesture_next = now + 260;
        if (++_gesture_step >= 4) _gesture_type = 0;
    }
}

int AppGemini::handle_audio_status(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    std::string ip;
    {
        std::lock_guard<std::mutex> lk(self->_mcp_mtx);
        ip = self->_ip_str;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ready", false);
    cJSON_AddBoolToObject(root, "recording", static_cast<SessionState>(self->_state.load()) == SessionState::Hearing);
    cJSON_AddStringToObject(root, "mode", "gemini_mcp");
    cJSON_AddBoolToObject(root, "mcp_attached", self->_mcp_attached.load());
    const char* am = !self->_mcp_attached.load()     ? "standalone"
                     : self->_mcp_agent_active.load() ? "engineer"
                                                      : "live";
    cJSON_AddStringToObject(root, "agent_mode", am);
    cJSON_AddStringToObject(root, "voice", self->_voice_name.c_str());
    cJSON_AddStringToObject(root, "ip", ip.c_str());
    cJSON_AddBoolToObject(root, "escalation_ready", self->_mcp_escalation_ready.load());
    cJSON_AddBoolToObject(root, "reply_ready", self->_mcp_reply_ready.load());
    char* s = cJSON_PrintUnformatted(root);
    send_http_json(req, s ? s : "{}");
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

int AppGemini::handle_playback_status(httpd_req* req)
{
    auto* self  = static_cast<AppGemini*>(req->user_ctx);
    bool play   = self->_mcp_playing_wav.load() ||
                  (static_cast<SessionState>(self->_state.load()) == SessionState::Speaking);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "playing", play);
    cJSON_AddNumberToObject(root, "started_ms", static_cast<double>(self->_mcp_play_started_ms.load()));
    cJSON_AddStringToObject(root, "kind", "wav");
    char* s = cJSON_PrintUnformatted(root);
    send_http_json(req, s ? s : "{}");
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

int AppGemini::handle_play(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    std::string body;
    if (!read_http_body(req, body, 1024)) {
        send_http_json(req, "{\"success\":false,\"error\":\"Invalid body\"}", "400 Bad Request");
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        send_http_json(req, "{\"success\":false,\"error\":\"Invalid JSON\"}", "400 Bad Request");
        return ESP_OK;
    }
    cJSON* jurl = cJSON_GetObjectItem(root, "voice_url");
    if (!cJSON_IsString(jurl) || !jurl->valuestring || jurl->valuestring[0] == '\0') {
        cJSON_Delete(root);
        send_http_json(req, "{\"success\":false,\"error\":\"Missing voice_url\"}", "400 Bad Request");
        return ESP_OK;
    }
    bool wait_reply = cJSON_IsTrue(cJSON_GetObjectItem(root, "wait_reply"));
    cJSON* jsub     = cJSON_GetObjectItem(root, "subtitle");
    if (!cJSON_IsString(jsub)) {
        jsub = cJSON_GetObjectItem(root, "text");
    }
    std::string sub = (cJSON_IsString(jsub) && jsub->valuestring) ? jsub->valuestring : "";
    if (sub.size() > 140) {
        sub = sub.substr(0, 137) + "...";
    }
    {
        std::lock_guard<std::mutex> lk(self->_mcp_mtx);
        self->_mcp_next_play_url = jurl->valuestring;
        self->_mcp_next_subtitle = sub;
    }
    self->_mcp_play_wait_reply = wait_reply;
    cJSON_Delete(root);
    self->_mcp_play_abort   = true;
    self->_mcp_play_pending = true;
    send_http_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppGemini::handle_mcp_attach(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    uint32_t ttl_ms   = 25000;
    std::string mode;
    std::string ctx;
    bool has_ctx = false;

    std::string body;
    if (read_http_body(req, body, 1024)) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root) {
            if (auto* jt = cJSON_GetObjectItem(root, "ttl_ms"); cJSON_IsNumber(jt)) {
                ttl_ms = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(jt->valuedouble), 5000, 120000));
            }
            if (auto* jm = cJSON_GetObjectItem(root, "mode"); cJSON_IsString(jm) && jm->valuestring) {
                mode = jm->valuestring;
            }
            if (auto* jc = cJSON_GetObjectItem(root, "context"); cJSON_IsString(jc) && jc->valuestring) {
                ctx     = jc->valuestring;
                has_ctx = true;
            }
            cJSON_Delete(root);
        }
    }

    bool was_attached = self->_mcp_attached.exchange(true);
    self->_mcp_lease_until_ms = GetHAL().millis() + ttl_ms;
    bool need_reconnect       = !was_attached;

    if (has_ctx) {
        std::lock_guard<std::mutex> lk(self->_mcp_mtx);
        if (self->_mcp_context != ctx) {
            self->_mcp_context = ctx;
            need_reconnect     = true;
        }
    }
    if (mode == "engineer") {
        self->_mcp_agent_active = true;
        self->set_state(SessionState::AgentWait);
    } else if (mode == "live") {
        bool was_agent = self->_mcp_agent_active.exchange(false);
        self->_mcp_escalation_ready = false;
        self->_mcp_wait_reply_mode  = false;
        if (was_agent && !need_reconnect) {
            self->_want_arm_listen = true;
        }
    }
    if (need_reconnect) {
        self->_want_connect = true;
    }

    send_http_json(req, "{\"success\":true,\"attached\":true}");
    return ESP_OK;
}

int AppGemini::handle_mcp_detach(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    bool was   = self->_mcp_attached.exchange(false);
    self->_mcp_agent_active     = false;
    self->_mcp_wait_reply_mode  = false;
    self->_mcp_escalation_ready = false;
    self->_mcp_reply_ready      = false;
    self->_mcp_lease_until_ms   = 0;
    if (was) {
        self->_want_connect = true;
    }
    send_http_json(req, "{\"success\":true,\"attached\":false}");
    return ESP_OK;
}

int AppGemini::handle_mcp_escalation(httpd_req* req)
{
    auto* self  = static_cast<AppGemini*>(req->user_ctx);
    cJSON* root = cJSON_CreateObject();
    if (self->_mcp_escalation_ready.exchange(false)) {
        std::string task, user;
        {
            std::lock_guard<std::mutex> lk(self->_mcp_mtx);
            task = self->_mcp_escalation_task;
            user = self->_mcp_escalation_user;
            self->_mcp_escalation_task.clear();
            self->_mcp_escalation_user.clear();
        }
        cJSON_AddBoolToObject(root, "ready", true);
        cJSON_AddStringToObject(root, "task", task.c_str());
        cJSON_AddStringToObject(root, "user_text", user.c_str());
    } else {
        cJSON_AddBoolToObject(root, "ready", false);
    }
    char* s = cJSON_PrintUnformatted(root);
    send_http_json(req, s ? s : "{}");
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

int AppGemini::handle_mcp_wait_reply(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    {
        std::lock_guard<std::mutex> lk(self->_mcp_mtx);
        self->_mcp_reply_text.clear();
    }
    self->_mcp_reply_ready     = false;
    self->_mcp_wait_reply_mode = true;
    self->_mcp_agent_active    = true;
    self->_want_arm_listen     = true;
    send_http_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppGemini::handle_mcp_reply(httpd_req* req)
{
    auto* self  = static_cast<AppGemini*>(req->user_ctx);
    cJSON* root = cJSON_CreateObject();
    if (self->_mcp_reply_ready.exchange(false)) {
        std::string txt;
        {
            std::lock_guard<std::mutex> lk(self->_mcp_mtx);
            txt = self->_mcp_reply_text;
            self->_mcp_reply_text.clear();
        }
        cJSON_AddBoolToObject(root, "ready", true);
        cJSON_AddStringToObject(root, "text", txt.c_str());
    } else {
        cJSON_AddBoolToObject(root, "ready", false);
    }
    char* s = cJSON_PrintUnformatted(root);
    send_http_json(req, s ? s : "{}");
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

int AppGemini::handle_agent_mode(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    std::string body;
    if (read_http_body(req, body, 512)) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root) {
            cJSON* jm = cJSON_GetObjectItem(root, "mode");
            std::string m = (cJSON_IsString(jm) && jm->valuestring) ? jm->valuestring : "";
            cJSON_Delete(root);
            if (m == "engineer" || m == "agent") {
                self->_mcp_agent_active = true;
                self->set_state(SessionState::AgentWait);
            } else if (m == "live" || m == "free") {
                self->_mcp_agent_active = false;
                self->_want_arm_listen  = true;
            }
        }
    }
    send_http_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppGemini::handle_listen_resume(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    self->_mcp_agent_active    = false;
    self->_mcp_wait_reply_mode = false;
    self->_want_arm_listen     = true;
    send_http_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppGemini::handle_move(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    std::string body;
    if (read_http_body(req, body, 512)) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root) {
            MotionCmd cmd;
            cmd.kind = "move";
            if (auto* jx = cJSON_GetObjectItem(root, "x"); cJSON_IsNumber(jx)) cmd.x_deg = static_cast<int>(jx->valuedouble);
            if (auto* jy = cJSON_GetObjectItem(root, "y"); cJSON_IsNumber(jy)) cmd.y_deg = static_cast<int>(jy->valuedouble);
            if (auto* js = cJSON_GetObjectItem(root, "speed"); cJSON_IsNumber(js)) cmd.speed = static_cast<int>(js->valuedouble) * 10;
            cJSON_Delete(root);
            std::lock_guard<std::mutex> lk(self->_motion_mtx);
            self->_motion_cmds.push_back(cmd);
        }
    }
    send_http_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppGemini::handle_nod(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    std::lock_guard<std::mutex> lk(self->_motion_mtx);
    self->_motion_cmds.push_back({"nod", 0, Y_HOME_DEG, 700});
    send_http_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppGemini::handle_shake(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    std::lock_guard<std::mutex> lk(self->_motion_mtx);
    self->_motion_cmds.push_back({"shake", 0, Y_HOME_DEG, 700});
    send_http_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppGemini::handle_home(httpd_req* req)
{
    auto* self = static_cast<AppGemini*>(req->user_ctx);
    std::lock_guard<std::mutex> lk(self->_motion_mtx);
    self->_motion_cmds.push_back({"home", 0, Y_HOME_DEG, 500});
    send_http_json(req, "{\"success\":true}");
    return ESP_OK;
}

int AppGemini::handle_face(httpd_req* req)
{
    send_http_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                               User Actions                                 */
/* -------------------------------------------------------------------------- */

void AppGemini::on_primary_button()
{
    SessionState st = static_cast<SessionState>(_state.load());
    switch (st) {
        case SessionState::NeedKey:
            on_retry_button();
            break;
        case SessionState::Error:
            _muted        = false;
            _want_connect = true;
            break;
        case SessionState::Ready:
            // Unmute & resume hands-free listening
            _muted           = false;
            _want_arm_listen = true;
            break;
        case SessionState::Listening:
            // Mute hands-free listening to pause conversation
            _want_mute = true;
            break;
        case SessionState::Hearing:
            // Optional early send before 1.8 s silence timeout
            _want_stop_talk = true;
            break;
        case SessionState::Speaking:
        case SessionState::Thinking:
            // Instant barge-in: stop playback & re-arm listening immediately
            _muted           = false;
            _want_arm_listen = true;
            break;
        case SessionState::AgentWait:
            // Allow user to tap to cancel waiting and return to Live listening
            _mcp_agent_active = false;
            _want_arm_listen  = true;
            break;
        case SessionState::ConnectingWifi:
        case SessionState::ConnectingApi:
        default:
            break;
    }
}

void AppGemini::on_retry_button()
{
    if (load_api_config()) {
        set_state(SessionState::ConnectingWifi);
        set_subtitle("API key loaded! Connecting...");
        _want_connect = true;
    } else {
        set_state(SessionState::NeedKey);
        set_subtitle("Still no key in /sd/gemini.txt");
    }
}

void AppGemini::change_volume(int delta)
{
    _volume = std::clamp(_volume + delta, 10, 100);
    GetHAL().setSpeakerVolume(static_cast<uint8_t>(_volume), true);
}

void AppGemini::open_settings()
{
    if (!_settings_cont) {
        return;
    }
    _settings_open = true;
    _voice_dirty   = false;
    refresh_settings_ui();
    lv_obj_remove_flag(_settings_cont, LV_OBJ_FLAG_HIDDEN);
}

void AppGemini::close_settings()
{
    if (!_settings_cont) {
        return;
    }
    _settings_open = false;
    lv_obj_add_flag(_settings_cont, LV_OBJ_FLAG_HIDDEN);
    save_voice_pitch_to_nvs();

    if (_voice_dirty && !_api_key.empty()) {
        _voice_dirty = false;
        _pcm_ring.clear();
        set_subtitle("Applying voice: " + _voice_name + "...");
        _want_connect = true;
    }
}

void AppGemini::cycle_voice(int delta)
{
    _voice_index = (_voice_index + delta + kVoicePresetCount) % kVoicePresetCount;
    _voice_name  = kVoicePresets[_voice_index].name;
    _voice_dirty = true;
    save_voice_pitch_to_nvs();
    refresh_settings_ui();
}

void AppGemini::change_pitch(int delta_pct)
{
    int next = std::clamp(_pitch_pct.load() + delta_pct, MIN_PITCH_PCT, MAX_PITCH_PCT);
    _pitch_pct.store(next);
    save_voice_pitch_to_nvs();
    refresh_settings_ui();
}

void AppGemini::set_mic_sens(int sens)
{
    int clamped = std::clamp<int>(sens, 0, 2);
    _mic_sens   = clamped;
    nvs_handle_t h{};
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, KEY_MIC_SENS, static_cast<int32_t>(clamped));
        nvs_commit(h);
        nvs_close(h);
    }
    refresh_settings_ui();
}

void AppGemini::set_speak_move(bool enabled)
{
    _speak_move = enabled;
    nvs_handle_t h{};
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, KEY_SPEAK_MV, enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    refresh_settings_ui();
}

void AppGemini::update_speak_motion(uint32_t now)
{
    if (_gesture_type != 0) {
        return;
    }
    auto& motion = GetStackChan().motion();
    bool spk     = (static_cast<SessionState>(_state.load()) == SessionState::Speaking);
    if (spk && _speak_move.load()) {
        if (!_speak_moving) {
            _base_yaw_deg    = std::clamp(motion.getCurrentYawAngle() / 10, -120, 120);
            _base_pitch_deg  = std::clamp(45 - (motion.getCurrentPitchAngle() / 10), 15, 75);
            _speak_moving    = true;
            _speak_move_step = 0;
            _speak_move_next = now;
        }
        if (static_cast<int32_t>(now - _speak_move_next) >= 0) {
            if (_audio_rms.load() > 450) {
                static const int dx_pat[4] = {-5, 4, 6, -4};
                static const int dy_pat[4] = {-5, 4, -4, 5};
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
    }
}

void AppGemini::refresh_settings_ui()
{
    if (_lbl_voice_val) {
        char vbuf[48];
        std::snprintf(vbuf, sizeof(vbuf), "%s\n(%s)", kVoicePresets[_voice_index].name,
                      kVoicePresets[_voice_index].desc);
        lv_label_set_text(_lbl_voice_val, vbuf);
    }
    if (_lbl_pitch_val) {
        int pct  = _pitch_pct.load();
        int diff = pct - 100;
        char pbuf[48];
        if (diff == 0) {
            std::snprintf(pbuf, sizeof(pbuf), "1.00x\n(Normal)");
        } else {
            std::snprintf(pbuf, sizeof(pbuf), "%.2fx\n(%+d%%)", static_cast<double>(pct) / 100.0, diff);
        }
        lv_label_set_text(_lbl_pitch_val, pbuf);
    }
    int sens = std::clamp<int>(_mic_sens.load(), 0, 2);
    for (int i = 0; i < 3; ++i) {
        if (_btn_mic_sens[i]) {
            lv_obj_set_style_bg_color(_btn_mic_sens[i], lv_color_hex(i == sens ? COL_GEMINI_BLUE : COL_CARD_HI), 0);
        }
    }
    if (_btn_move_toggle && _lbl_move_toggle) {
        bool mv = _speak_move.load();
        lv_obj_set_style_bg_color(_btn_move_toggle, lv_color_hex(mv ? COL_GREEN_DK : COL_CARD_HI), 0);
        lv_label_set_text(_lbl_move_toggle, mv ? "MOVE: ON" : "MOVE: OFF");
    }
}

void AppGemini::set_state(SessionState st)
{
    _state.store(static_cast<int>(st));
}

void AppGemini::set_subtitle(const std::string& text, bool append)
{
    std::lock_guard<std::mutex> lock(_text_mtx);
    if (append) {
        _subtitle += text;
        // Keep subtitle bounded to the latest ~140 chars so LVGL stays snappy
        if (_subtitle.size() > 140) {
            _subtitle = "..." + _subtitle.substr(_subtitle.size() - 135);
        }
    } else {
        _subtitle = text;
    }
}

void AppGemini::set_error(const std::string& msg)
{
    {
        std::lock_guard<std::mutex> lock(_text_mtx);
        _error_msg = msg;
        _subtitle  = msg;
    }
    set_state(SessionState::Error);
}

void AppGemini::update_leds(SessionState st, int rms)
{
    uint8_t r = 0, g = 0, b = 0;
    uint32_t now = GetHAL().millis();

    switch (st) {
        case SessionState::Ready:
            r = 8;
            g = 12;
            b = 26;
            break;
        case SessionState::Listening: {
            float p = 0.5f + 0.5f * std::sin(static_cast<float>(now % 2400) / 2400.0f * 6.28318f);
            r       = 0;
            g       = static_cast<uint8_t>(12 + p * 18);
            b       = static_cast<uint8_t>(16 + p * 16);
            break;
        }
        case SessionState::Hearing: {
            int level = std::clamp(32 + rms / 70, 32, 105);
            r         = 0;
            g         = static_cast<uint8_t>(level);
            b         = static_cast<uint8_t>(level / 4);
            break;
        }
        case SessionState::Thinking:
        case SessionState::AgentWait:
        case SessionState::ConnectingWifi:
        case SessionState::ConnectingApi: {
            float p = 0.5f + 0.5f * std::sin(static_cast<float>(now % 1200) / 1200.0f * 6.28318f);
            r       = static_cast<uint8_t>(20 + p * 45);
            g       = static_cast<uint8_t>(12 + p * 28);
            b       = 0;
            break;
        }
        case SessionState::Speaking: {
            int level = std::clamp(28 + rms / 70, 28, 110);
            r         = static_cast<uint8_t>(level / 3);
            g         = static_cast<uint8_t>(level / 2);
            b         = static_cast<uint8_t>(level);
            break;
        }
        case SessionState::Error:
        case SessionState::NeedKey:
            r = 45;
            g = 6;
            b = 6;
            break;
    }

    for (int i = 0; i < 12; ++i) {
        GetHAL().setRgbColor(i, r, g, b);
    }
    GetHAL().refreshRgb();
}

/* -------------------------------------------------------------------------- */
/*                                 LVGL UI                                    */
/* -------------------------------------------------------------------------- */

void AppGemini::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    _root_cont    = lv_obj_create(scr);
    lv_obj_set_size(_root_cont, W, H);
    lv_obj_set_pos(_root_cont, 0, 0);
    style_flat_box(_root_cont, COL_BG, 0);

    const int top_y   = view::get_status_bar_top_offset();
    const bool is_fix = (top_y > 0);
    const int hdr_y   = top_y + (is_fix ? 1 : 4);
    const int stage_y = top_y + (is_fix ? 18 : 24);
    const int stage_h = 152 - stage_y;

    // App header row below system status bar (voice name on the right; volume is in the top bar)
    _lbl_badge = lv_label_create(_root_cont);
    lv_obj_set_pos(_lbl_badge, 10, hdr_y);
    lv_obj_set_width(_lbl_badge, 200);
    lv_label_set_long_mode(_lbl_badge, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_badge, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_badge, lv_color_hex(COL_GEMINI_BLUE), 0);
    lv_label_set_text(_lbl_badge, "GEMINI");

    _lbl_vol_batt = lv_label_create(_root_cont);
    lv_obj_set_pos(_lbl_vol_batt, 214, hdr_y + 1);
    lv_obj_set_width(_lbl_vol_batt, 96);
    lv_obj_set_style_text_align(_lbl_vol_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(_lbl_vol_batt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_vol_batt, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(_lbl_vol_batt, _voice_name.c_str());

    // Center stage
    _stage_cont = lv_obj_create(_root_cont);
    lv_obj_set_pos(_stage_cont, 8, stage_y);
    lv_obj_set_size(_stage_cont, 304, stage_h);
    style_flat_box(_stage_cont, COL_BG, 12);

    auto on_stage_click = [](lv_event_t* e) {
        auto* self = static_cast<AppGemini*>(lv_event_get_user_data(e));
        self->on_primary_button();
    };

    _avatar_img = lv_image_create(_stage_cont);
    if (_img_naruto_calm.data != nullptr) {
        lv_image_set_src(_avatar_img, &_img_naruto_calm);
        int target_h = std::max<int>(76, stage_h - 4);
        if (_img_naruto_calm.header.h > 0) {
            uint16_t scale = static_cast<uint16_t>((target_h * 256) / _img_naruto_calm.header.h);
            lv_image_set_scale(_avatar_img, scale);
        }
    }
    lv_obj_align(_avatar_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_avatar_img, on_stage_click, LV_EVENT_CLICKED, this);

    _face_card = lv_obj_create(_stage_cont);
    lv_obj_set_size(_face_card, 176, std::max<int>(78, stage_h - 8));
    lv_obj_align(_face_card, LV_ALIGN_CENTER, 0, 0);
    style_flat_box(_face_card, COL_CARD, 18);
    lv_obj_set_style_border_width(_face_card, 2, 0);
    lv_obj_set_style_border_color(_face_card, lv_color_hex(COL_GEMINI_BLUE), 0);
    lv_obj_add_flag(_face_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_face_card, on_stage_click, LV_EVENT_CLICKED, this);

    _face_eye_l = lv_obj_create(_face_card);
    lv_obj_set_size(_face_eye_l, 18, 18);
    lv_obj_align(_face_eye_l, LV_ALIGN_CENTER, -28, -8);
    style_flat_box(_face_eye_l, COL_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(_face_eye_l, LV_OBJ_FLAG_CLICKABLE);

    _face_eye_r = lv_obj_create(_face_card);
    lv_obj_set_size(_face_eye_r, 18, 18);
    lv_obj_align(_face_eye_r, LV_ALIGN_CENTER, 28, -8);
    style_flat_box(_face_eye_r, COL_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(_face_eye_r, LV_OBJ_FLAG_CLICKABLE);

    _face_mouth = lv_obj_create(_face_card);
    lv_obj_set_size(_face_mouth, 38, 6);
    lv_obj_align(_face_mouth, LV_ALIGN_CENTER, 0, 18);
    style_flat_box(_face_mouth, COL_WHITE, 4);
    lv_obj_remove_flag(_face_mouth, LV_OBJ_FLAG_CLICKABLE);

    // Dual-avatar MCP stage (hidden in standalone mode; shown automatically while _mcp_attached == true)
    _dual_stage = lv_obj_create(_stage_cont);
    lv_obj_set_pos(_dual_stage, 0, 0);
    lv_obj_set_size(_dual_stage, 304, stage_h);
    style_flat_box(_dual_stage, COL_CARD, 12);
    lv_obj_set_style_border_width(_dual_stage, 1, 0);
    lv_obj_set_style_border_color(_dual_stage, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_add_flag(_dual_stage, LV_OBJ_FLAG_HIDDEN);

    const int slot_w        = 146;
    const int slot_h        = std::max<int>(74, stage_h - 4);
    const int slot_x[2]     = {4, 154};
    const char* slot_txt[2] = {"LIVE", "AGENT $"};
    for (int i = 0; i < 2; ++i) {
        _slot_card[i] = lv_obj_create(_dual_stage);
        lv_obj_set_pos(_slot_card[i], slot_x[i], 2);
        lv_obj_set_size(_slot_card[i], slot_w, slot_h);
        style_flat_box(_slot_card[i], 0x0F172A, 10);
        lv_obj_set_style_border_width(_slot_card[i], 1, 0);
        lv_obj_set_style_border_color(_slot_card[i], lv_color_hex(0x24314F), 0);
        lv_obj_remove_flag(_slot_card[i], LV_OBJ_FLAG_CLICKABLE);

        _slot_dot[i] = lv_obj_create(_slot_card[i]);
        lv_obj_set_size(_slot_dot[i], 7, 7);
        lv_obj_set_pos(_slot_dot[i], 7, 6);
        style_flat_box(_slot_dot[i], 0x64748B, LV_RADIUS_CIRCLE);
        lv_obj_remove_flag(_slot_dot[i], LV_OBJ_FLAG_CLICKABLE);

        _slot_lbl[i] = lv_label_create(_slot_card[i]);
        lv_obj_set_pos(_slot_lbl[i], 17, 3);
        lv_obj_set_style_text_font(_slot_lbl[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(_slot_lbl[i], lv_color_hex(0x94A3B8), 0);
        lv_label_set_text(_slot_lbl[i], slot_txt[i]);

        _dual_img[i] = lv_image_create(_slot_card[i]);
        if (_img_naruto_calm.data != nullptr) {
            lv_image_set_src(_dual_img[i], &_img_naruto_calm);
            int target_h = std::max<int>(64, slot_h - 18);
            uint16_t sc  = (_img_naruto_calm.header.h > 0)
                               ? static_cast<uint16_t>((target_h * 256) / _img_naruto_calm.header.h)
                               : static_cast<uint16_t>(is_fix ? 120 : 136);
            lv_image_set_scale(_dual_img[i], sc);
        }
        lv_obj_align(_dual_img[i], LV_ALIGN_CENTER, 0, 8);
        lv_obj_remove_flag(_dual_img[i], LV_OBJ_FLAG_CLICKABLE);

        _dual_face[i] = lv_obj_create(_slot_card[i]);
        lv_obj_set_size(_dual_face[i], 116, std::max<int>(54, slot_h - 24));
        lv_obj_align(_dual_face[i], LV_ALIGN_CENTER, 0, 8);
        style_flat_box(_dual_face[i], COL_BG, 12);
        lv_obj_set_style_border_width(_dual_face[i], 2, 0);
        lv_obj_set_style_border_color(_dual_face[i], lv_color_hex(0x24314F), 0);
        lv_obj_remove_flag(_dual_face[i], LV_OBJ_FLAG_CLICKABLE);

        _dual_eye_l[i] = lv_obj_create(_dual_face[i]);
        lv_obj_set_size(_dual_eye_l[i], 15, 15);
        lv_obj_align(_dual_eye_l[i], LV_ALIGN_CENTER, -24, -6);
        style_flat_box(_dual_eye_l[i], COL_WHITE, LV_RADIUS_CIRCLE);

        _dual_eye_r[i] = lv_obj_create(_dual_face[i]);
        lv_obj_set_size(_dual_eye_r[i], 15, 15);
        lv_obj_align(_dual_eye_r[i], LV_ALIGN_CENTER, 24, -6);
        style_flat_box(_dual_eye_r[i], COL_WHITE, LV_RADIUS_CIRCLE);

        _dual_mouth[i] = lv_obj_create(_dual_face[i]);
        lv_obj_set_size(_dual_mouth[i], 30, 5);
        lv_obj_align(_dual_mouth[i], LV_ALIGN_CENTER, 0, 14);
        style_flat_box(_dual_mouth[i], COL_WHITE, 3);
    }

    // Setup card shown only when no API key is configured
    _setup_card = lv_obj_create(_stage_cont);
    lv_obj_set_size(_setup_card, 296, stage_h - 4);
    lv_obj_align(_setup_card, LV_ALIGN_CENTER, 0, 0);
    style_flat_box(_setup_card, COL_CARD, 12);
    lv_obj_set_style_border_width(_setup_card, 1, 0);
    lv_obj_set_style_border_color(_setup_card, lv_color_hex(COL_GEMINI_BLUE), 0);
    lv_obj_add_flag(_setup_card, LV_OBJ_FLAG_HIDDEN);

    _lbl_setup_body = lv_label_create(_setup_card);
    lv_obj_set_width(_lbl_setup_body, 276);
    lv_obj_align(_lbl_setup_body, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_long_mode(_lbl_setup_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_lbl_setup_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_setup_body, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_setup_body, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(
        _lbl_setup_body,
        "Gemini Free API Key Required\n"
        "1. Get free key: aistudio.google.com\n"
        "2. Save in /sd/gemini.txt on microSD\n"
        "3. Insert SD card and tap RETRY");

    // Live caption / status box (y = 154 .. 188)
    _sub_box = lv_obj_create(_root_cont);
    lv_obj_set_pos(_sub_box, 8, 154);
    lv_obj_set_size(_sub_box, 304, 34);
    style_flat_box(_sub_box, COL_CARD, 8);

    _lbl_subtitle = lv_label_create(_sub_box);
    lv_obj_set_pos(_lbl_subtitle, 8, 2);
    lv_obj_set_size(_lbl_subtitle, 288, 30);
    lv_label_set_long_mode(_lbl_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_lbl_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_subtitle, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_subtitle, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(_lbl_subtitle, "Starting...");

    // Bottom 2-button control bar (y = 190 .. 234): TALK/MUTE/STOP | Settings
    // (VOL-, VOL+, and Home are integrated in the 38px top status bar)
    auto make_btn = [&](lv_obj_t* parent, int x, int y, int w, int h, uint32_t bg, const char* txt,
                        const lv_font_t* font) -> lv_obj_t* {
        lv_obj_t* b = lv_button_create(parent);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_size(b, w, h);
        style_flat_box(b, bg, 10);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(COL_CARD_HI), 0);
        lv_obj_t* l = lv_label_create(b);
        lv_obj_center(l);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_WHITE), 0);
        lv_label_set_text(l, txt);
        return b;
    };

    _btn_vol_dn = nullptr;
    _btn_vol_up = nullptr;
    _btn_exit   = nullptr;

    _btn_primary = lv_button_create(_root_cont);
    lv_obj_set_pos(_btn_primary, 8, 190);
    lv_obj_set_size(_btn_primary, 218, 44);
    style_flat_box(_btn_primary, COL_GEMINI_BLUE, 12);
    _lbl_primary = lv_label_create(_btn_primary);
    lv_obj_center(_lbl_primary);
    lv_obj_set_style_text_font(_lbl_primary, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_primary, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(_lbl_primary, "TALK");
    lv_obj_add_event_cb(
        _btn_primary,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->on_primary_button();
        },
        LV_EVENT_CLICKED, this);

    _btn_settings = make_btn(_root_cont, 234, 190, 78, 44, COL_CARD, LV_SYMBOL_SETTINGS " SET", &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        _btn_settings,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->open_settings();
        },
        LV_EVENT_CLICKED, this);

    /* ====================== SETTINGS OVERLAY (VOICE, PITCH, MIC, MOVE) ====================== */
    _settings_cont = lv_obj_create(_root_cont);
    lv_obj_set_size(_settings_cont, W, H);
    lv_obj_set_pos(_settings_cont, 0, 0);
    style_flat_box(_settings_cont, COL_BG, 0);
    lv_obj_add_flag(_settings_cont, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* lbl_shdr = lv_label_create(_settings_cont);
    lv_obj_set_pos(lbl_shdr, 10, top_y + 2);
    lv_obj_set_size(lbl_shdr, 200, 20);
    lv_obj_set_style_text_font(lbl_shdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_shdr, lv_color_hex(COL_GEMINI_BLUE), 0);
    lv_label_set_text(lbl_shdr, LV_SYMBOL_SETTINGS " GEMINI SET");

    lv_obj_t* btn_sback =
        make_btn(_settings_cont, 224, top_y, 86, 26, COL_GEMINI_BLUE, LV_SYMBOL_LEFT " APPLY", &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        btn_sback,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->close_settings();
        },
        LV_EVENT_CLICKED, this);

    const int r_y0   = top_y + (is_fix ? 30 : 34);
    const int r_step = is_fix ? 42 : 46;
    const int r_h    = is_fix ? 36 : 40;

    // Row 1: Voice selector (< | VOICE | >)
    lv_obj_t* btn_vprev = make_btn(_settings_cont, 10, r_y0, 48, r_h, COL_CARD_HI, LV_SYMBOL_LEFT, &lv_font_montserrat_16);
    lv_obj_add_event_cb(
        btn_vprev,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->cycle_voice(-1);
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* vcard = lv_obj_create(_settings_cont);
    lv_obj_set_pos(vcard, 64, r_y0);
    lv_obj_set_size(vcard, 192, r_h);
    style_flat_box(vcard, COL_CARD, 10);
    _lbl_voice_val = lv_label_create(vcard);
    lv_obj_center(_lbl_voice_val);
    lv_obj_set_style_text_align(_lbl_voice_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_voice_val, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_voice_val, lv_color_hex(COL_WHITE), 0);

    lv_obj_t* btn_vnext =
        make_btn(_settings_cont, 262, r_y0, 48, r_h, COL_CARD_HI, LV_SYMBOL_RIGHT, &lv_font_montserrat_16);
    lv_obj_add_event_cb(
        btn_vnext,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->cycle_voice(1);
        },
        LV_EVENT_CLICKED, this);

    // Row 2: Pitch selector (- | PITCH | +)
    lv_obj_t* btn_pdn = make_btn(_settings_cont, 10, r_y0 + r_step, 48, r_h, COL_CARD_HI, "-", &lv_font_montserrat_20);
    lv_obj_add_event_cb(
        btn_pdn,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->change_pitch(-PITCH_STEP_PCT);
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* pcard = lv_obj_create(_settings_cont);
    lv_obj_set_pos(pcard, 64, r_y0 + r_step);
    lv_obj_set_size(pcard, 192, r_h);
    style_flat_box(pcard, COL_CARD, 10);
    _lbl_pitch_val = lv_label_create(pcard);
    lv_obj_center(_lbl_pitch_val);
    lv_obj_set_style_text_align(_lbl_pitch_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_pitch_val, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_pitch_val, lv_color_hex(COL_WHITE), 0);

    lv_obj_t* btn_pup = make_btn(_settings_cont, 262, r_y0 + r_step, 48, r_h, COL_CARD_HI, "+", &lv_font_montserrat_20);
    lv_obj_add_event_cb(
        btn_pup,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->change_pitch(PITCH_STEP_PCT);
        },
        LV_EVENT_CLICKED, this);

    // Row 3: Mic sensitivity (LOW / MED / HIGH)
    const int r3_y          = r_y0 + r_step * 2;
    const char* mic_lbls[3] = {"MIC LOW", "MIC MED", "MIC HIGH"};
    for (int i = 0; i < 3; ++i) {
        _btn_mic_sens[i] = make_btn(_settings_cont, 10 + i * 102, r3_y, 96, r_h, COL_CARD_HI, mic_lbls[i], &lv_font_montserrat_12);
        lv_obj_set_user_data(_btn_mic_sens[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            _btn_mic_sens[i],
            [](lv_event_t* e) {
                auto* self = static_cast<AppGemini*>(lv_event_get_user_data(e));
                int idx    = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
                self->set_mic_sens(idx);
            },
            LV_EVENT_CLICKED, this);
    }

    // Row 4: Speak head motion toggle (MOVE: ON / OFF)
    const int r4_y   = r_y0 + r_step * 3;
    _btn_move_toggle = lv_button_create(_settings_cont);
    lv_obj_set_pos(_btn_move_toggle, 10, r4_y);
    lv_obj_set_size(_btn_move_toggle, 300, r_h);
    style_flat_box(_btn_move_toggle, COL_GREEN_DK, 10);
    _lbl_move_toggle = lv_label_create(_btn_move_toggle);
    lv_obj_center(_lbl_move_toggle);
    lv_obj_set_style_text_font(_lbl_move_toggle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_move_toggle, lv_color_hex(COL_WHITE), 0);
    lv_obj_add_event_cb(
        _btn_move_toggle,
        [](lv_event_t* e) {
            auto* self = static_cast<AppGemini*>(lv_event_get_user_data(e));
            self->set_speak_move(!self->_speak_move.load());
        },
        LV_EVENT_CLICKED, this);

    refresh_settings_ui();
}

void AppGemini::destroy_ui()
{
    if (_root_cont) {
        lv_obj_delete(_root_cont);
        _root_cont      = nullptr;
        _lbl_badge      = nullptr;
        _lbl_vol_batt   = nullptr;
        _stage_cont     = nullptr;
        _avatar_img     = nullptr;
        _face_card      = nullptr;
        _face_eye_l     = nullptr;
        _face_eye_r     = nullptr;
        _face_mouth     = nullptr;
        _dual_stage     = nullptr;
        for (int i = 0; i < 2; ++i) {
            _slot_card[i]  = nullptr;
            _slot_dot[i]   = nullptr;
            _slot_lbl[i]   = nullptr;
            _dual_img[i]   = nullptr;
            _dual_face[i]  = nullptr;
            _dual_eye_l[i] = nullptr;
            _dual_eye_r[i] = nullptr;
            _dual_mouth[i] = nullptr;
        }
        _setup_card     = nullptr;
        _lbl_setup_body = nullptr;
        _sub_box        = nullptr;
        _lbl_subtitle   = nullptr;
        _btn_vol_dn     = nullptr;
        _btn_primary    = nullptr;
        _lbl_primary    = nullptr;
        _btn_vol_up     = nullptr;
        _btn_settings   = nullptr;
        _btn_exit       = nullptr;
        _settings_cont  = nullptr;
        _lbl_voice_val   = nullptr;
        _lbl_pitch_val   = nullptr;
        for (int i = 0; i < 3; ++i) _btn_mic_sens[i] = nullptr;
        _btn_move_toggle = nullptr;
        _lbl_move_toggle = nullptr;
    }
}

void AppGemini::refresh_ui(uint32_t now)
{
    if (!_root_cont) {
        return;
    }

    SessionState st = static_cast<SessionState>(_state.load());

    // Keep speaker volume synchronized with the top status bar; show active voice preset on the right
    _volume = std::clamp(static_cast<int>(GetHAL().getSpeakerVolume()), 0, 100);
    lv_label_set_text(_lbl_vol_batt, _voice_name.c_str());

    const bool mcp_on   = _mcp_attached.load();
    const bool agent_on = _mcp_agent_active.load();

    // State-dependent badge, primary button, setup card, and 1-avatar vs 2-avatar stage
    if (_last_rendered_state != static_cast<int>(st) || _last_rendered_mcp != mcp_on ||
        _last_rendered_agent != agent_on) {
        _last_rendered_state = static_cast<int>(st);
        _last_rendered_mcp   = mcp_on;
        _last_rendered_agent = agent_on;

        bool need_key   = (st == SessionState::NeedKey);
        bool use_naruto = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto &&
                           _img_naruto_calm.data != nullptr);
        if (need_key) {
            lv_obj_remove_flag(_setup_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
            if (_avatar_img) lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
            if (_dual_stage) lv_obj_add_flag(_dual_stage, LV_OBJ_FLAG_HIDDEN);
        } else if (mcp_on && _dual_stage) {
            lv_obj_add_flag(_setup_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
            if (_avatar_img) lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(_dual_stage, LV_OBJ_FLAG_HIDDEN);

            const bool left_on    = !agent_on;
            const uint32_t bg[2]  = {left_on ? 0x0E2235U : 0x0F172AU, agent_on ? 0x2A1E10U : 0x0F172AU};
            const uint32_t brd[2] = {left_on ? 0x38BDF8U : 0x24314FU, agent_on ? 0xFBBF24U : 0x24314FU};
            const uint32_t txt[2] = {left_on ? 0x38BDF8U : 0x64748BU, agent_on ? 0xFBBF24U : 0x64748BU};
            const int bw[2]       = {left_on ? 2 : 1, agent_on ? 2 : 1};
            const lv_opa_t opa[2] = {left_on ? LV_OPA_COVER : LV_OPA_40, agent_on ? LV_OPA_COVER : LV_OPA_40};
            std::string left_lbl  = "LIVE " + _voice_name;
            if (left_lbl.size() > 14) left_lbl.resize(14);
            lv_label_set_text(_slot_lbl[0], left_lbl.c_str());
            for (int i = 0; i < 2; ++i) {
                lv_obj_set_style_bg_color(_slot_card[i], lv_color_hex(bg[i]), 0);
                lv_obj_set_style_border_color(_slot_card[i], lv_color_hex(brd[i]), 0);
                lv_obj_set_style_border_width(_slot_card[i], bw[i], 0);
                lv_obj_set_style_opa(_slot_card[i], opa[i], 0);
                lv_obj_set_style_bg_color(_slot_dot[i], lv_color_hex(txt[i]), 0);
                lv_obj_set_style_text_color(_slot_lbl[i], lv_color_hex(txt[i]), 0);
                if (use_naruto && _dual_img[i]) {
                    lv_obj_remove_flag(_dual_img[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(_dual_face[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    if (_dual_img[i]) lv_obj_add_flag(_dual_img[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_remove_flag(_dual_face[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_border_color(_dual_face[i], lv_color_hex(brd[i]), 0);
                }
            }
            lv_obj_set_style_border_color(_dual_stage, lv_color_hex(agent_on ? 0xB45309U : 0x0284C7U), 0);
        } else {
            lv_obj_add_flag(_setup_card, LV_OBJ_FLAG_HIDDEN);
            if (_dual_stage) lv_obj_add_flag(_dual_stage, LV_OBJ_FLAG_HIDDEN);
            if (use_naruto && _avatar_img) {
                lv_obj_remove_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
                if (_avatar_img) {
                    lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        const char* badge_txt = "GEMINI";
        uint32_t badge_col    = COL_GEMINI_BLUE;
        const char* btn_txt   = "TALK";
        uint32_t btn_col      = COL_GEMINI_BLUE;

        switch (st) {
            case SessionState::NeedKey:
                badge_txt = "GEMINI  SETUP";
                badge_col = COL_AMBER;
                btn_txt   = "RETRY";
                btn_col   = COL_GEMINI_BLUE;
                break;
            case SessionState::ConnectingWifi:
                badge_txt = "GEMINI  WI-FI...";
                badge_col = COL_AMBER;
                btn_txt   = "WAIT...";
                btn_col   = COL_CARD_HI;
                break;
            case SessionState::ConnectingApi:
                badge_txt = "GEMINI  LINK...";
                badge_col = COL_AMBER;
                btn_txt   = "WAIT...";
                btn_col   = COL_CARD_HI;
                break;
            case SessionState::Ready:
                badge_txt = "GEMINI  MUTED";
                badge_col = COL_DIM;
                btn_txt   = LV_SYMBOL_AUDIO " RESUME";
                btn_col   = COL_GEMINI_BLUE;
                break;
            case SessionState::Listening:
                badge_txt = "GEMINI  LISTENING";
                badge_col = COL_GEMINI_BLUE;
                btn_txt   = LV_SYMBOL_MUTE " MUTE";
                btn_col   = COL_CARD_HI;
                break;
            case SessionState::Hearing:
                badge_txt = "GEMINI  HEARING";
                badge_col = COL_GREEN;
                btn_txt   = LV_SYMBOL_OK " SEND";
                btn_col   = COL_GREEN;
                break;
            case SessionState::Thinking:
                badge_txt = "GEMINI  THINKING";
                badge_col = COL_AMBER;
                btn_txt   = "CANCEL";
                btn_col   = COL_AMBER;
                break;
            case SessionState::Speaking:
                badge_txt = mcp_on ? (agent_on ? "AGENT $  SPEAKING" : "LIVE + MCP  SPEAKING") : "GEMINI  SPEAKING";
                badge_col = mcp_on ? (agent_on ? COL_AMBER : COL_CYAN) : COL_GEMINI_PURP;
                btn_txt   = LV_SYMBOL_STOP " STOP";
                btn_col   = mcp_on ? (agent_on ? COL_AMBER : COL_CYAN) : COL_GEMINI_PURP;
                break;
            case SessionState::AgentWait:
                badge_txt = "AGENT $  WORKING";
                badge_col = COL_AMBER;
                btn_txt   = LV_SYMBOL_REFRESH " WAITING";
                btn_col   = COL_AMBER;
                break;
            case SessionState::Error:
                badge_txt = "GEMINI  ERROR";
                badge_col = COL_RED;
                btn_txt   = LV_SYMBOL_REFRESH " RETRY";
                btn_col   = COL_RED_DK;
                break;
        }

        lv_label_set_text(_lbl_badge, badge_txt);
        lv_obj_set_style_text_color(_lbl_badge, lv_color_hex(badge_col), 0);
        lv_label_set_text(_lbl_primary, btn_txt);
        lv_obj_set_style_bg_color(_btn_primary, lv_color_hex(btn_col), 0);
    }

    // Live caption / subtitle text
    std::string sub_copy;
    {
        std::lock_guard<std::mutex> lock(_text_mtx);
        sub_copy = _subtitle;
    }
    if (sub_copy != _last_rendered_sub) {
        _last_rendered_sub = sub_copy;
        lv_label_set_text(_lbl_subtitle, sub_copy.c_str());
    }

    if (st != SessionState::NeedKey) {
        refresh_face_anim(now, st);
    }
}

void AppGemini::refresh_face_anim(uint32_t now, SessionState st)
{
    bool use_naruto = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto &&
                       _img_naruto_calm.data != nullptr);

    if (_mcp_attached.load() && _dual_stage) {
        const int active = _mcp_agent_active.load() ? 1 : 0;
        const bool spk   = (st == SessionState::Speaking);
        const int rms    = _audio_rms.load();
        for (int i = 0; i < 2; ++i) {
            const bool is_act = (i == active);
            if (use_naruto && _dual_img[i]) {
                bool happy = (is_act && spk && rms > 500 && ((now / 180) % 2 == 0));
                lv_image_set_src(_dual_img[i], (happy && _img_naruto_happy.data) ? &_img_naruto_happy : &_img_naruto_calm);
                int dy = (is_act && spk) ? -std::clamp(rms / 400, 0, 4) : 0;
                lv_obj_align(_dual_img[i], LV_ALIGN_CENTER, 0, 8 + dy);
            } else if (_dual_face[i]) {
                bool blink = is_act && ((now % 3800) > 3650);
                lv_obj_set_size(_dual_eye_l[i], 15, blink ? 3 : 15);
                lv_obj_set_size(_dual_eye_r[i], 15, blink ? 3 : 15);
                int mh = (is_act && spk) ? (5 + std::clamp(rms / 240, 0, 13)) : 5;
                lv_obj_set_size(_dual_mouth[i], 30, mh);
                lv_obj_set_style_radius(_dual_mouth[i], (mh > 9) ? 6 : 3, 0);
            }
        }
        return;
    }

    if (use_naruto && _avatar_img) {
        bool want_happy = (st == SessionState::Speaking);
        if (want_happy != _avatar_showing_happy) {
            _avatar_showing_happy = want_happy;
            if (want_happy && _img_naruto_happy.data != nullptr) {
                lv_image_set_src(_avatar_img, &_img_naruto_happy);
            } else if (_img_naruto_calm.data != nullptr) {
                lv_image_set_src(_avatar_img, &_img_naruto_calm);
            }
        }
        int oy = 0;
        if (st == SessionState::Speaking) {
            int rms = _audio_rms.load();
            oy      = -std::clamp(rms / 400, 0, 5);
        } else if (st == SessionState::Hearing) {
            oy = static_cast<int>(std::sin(static_cast<float>(now % 700) / 700.0f * 6.28318f) * 2.0f);
        } else {
            oy = static_cast<int>(std::sin(static_cast<float>(now % 3000) / 3000.0f * 6.28318f) * 2.0f);
        }
        lv_obj_align(_avatar_img, LV_ALIGN_CENTER, 0, oy);
        return;
    }

    if (!_face_card) {
        return;
    }

    int rms        = _audio_rms.load();
    int offset_y   = 0;
    int mouth_w    = 38;
    int mouth_h    = 6;
    uint32_t bcol  = COL_CARD_HI;

    if (st == SessionState::Hearing) {
        bcol    = COL_GREEN;
        int lvl = std::clamp(rms / 250, 0, 10);
        mouth_w = 24 + lvl;
        mouth_h = 8 + lvl;
    } else if (st == SessionState::Listening) {
        bcol     = COL_GEMINI_BLUE;
        offset_y = static_cast<int>(std::sin(static_cast<float>(now % 2400) / 2400.0f * 6.28318f) * 2.0f);
    } else if (st == SessionState::Speaking) {
        bcol    = COL_GEMINI_PURP;
        int lvl = std::clamp(rms / 220, 0, 18);
        mouth_w = 26 + lvl;
        mouth_h = 8 + lvl;
        offset_y = static_cast<int>(std::sin(static_cast<float>(now % 600) / 600.0f * 6.28318f) * 2.0f);
    } else if (st == SessionState::Thinking || st == SessionState::ConnectingWifi ||
               st == SessionState::ConnectingApi) {
        bcol    = COL_AMBER;
        mouth_w = 18;
        mouth_h = 6;
        offset_y = static_cast<int>(std::sin(static_cast<float>(now % 1400) / 1400.0f * 6.28318f) * 2.0f);
    } else if (st == SessionState::Error) {
        bcol    = COL_RED;
        mouth_w = 26;
        mouth_h = 4;
    } else {
        bcol     = COL_GEMINI_BLUE;
        offset_y = static_cast<int>(std::sin(static_cast<float>(now % 3000) / 3000.0f * 6.28318f) * 2.0f);
    }

    lv_obj_set_style_border_color(_face_card, lv_color_hex(bcol), 0);
    lv_obj_align(_face_card, LV_ALIGN_CENTER, 0, offset_y);

    bool blink = ((now % 3500) > 3350);
    int eye_h  = blink ? 4 : 18;
    if (_face_eye_l) {
        lv_obj_set_size(_face_eye_l, 18, eye_h);
    }
    if (_face_eye_r) {
        lv_obj_set_size(_face_eye_r, 18, eye_h);
    }
    if (_face_mouth) {
        lv_obj_set_size(_face_mouth, mouth_w, mouth_h);
        lv_obj_set_style_radius(_face_mouth, (st == SessionState::Speaking) ? 10 : 4, 0);
    }
}
