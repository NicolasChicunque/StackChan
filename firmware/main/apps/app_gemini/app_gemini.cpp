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
#include <apps/common/common.h>
#include <assets/assets.h>
#include <board.h>
#include <audio/audio_codec.h>
#include <web_socket.h>
#include <network_interface.h>

#include <cJSON.h>
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
#include <vector>

using namespace mooncake;

LV_IMAGE_DECLARE(icon_gemini);

namespace {

constexpr const char* TAG           = "GEMINI";
constexpr const char* NVS_NS        = "gemini_live";
constexpr const char* KEY_API_KEY   = "api_key";
constexpr const char* KEY_MODEL     = "model";
constexpr const char* DEFAULT_MODEL = "gemini-3.8-live";
constexpr const char* SD_KEY_PATH   = "/sd/gemini.txt";
constexpr const char* SD_MOUNT_PT   = "/sd";

constexpr int W                     = 320;
constexpr int H                     = 240;
constexpr int VOLUME_STEP           = 10;
constexpr uint32_t MAX_LISTEN_MS    = 30000;
constexpr size_t PCM_RING_SAMPLES   = 24000 * 24;   // 24 s of 24 kHz 16-bit mono (~1.15 MB in PSRAM)
constexpr size_t B64_DECODE_CAP     = 192 * 1024;   // 192 KB PSRAM scratch for decoded PCM chunks

// Dark Gemini palette
constexpr uint32_t COL_BG           = 0x0B0F1A;
constexpr uint32_t COL_CARD         = 0x151B2E;
constexpr uint32_t COL_CARD_HI      = 0x232C4A;
constexpr uint32_t COL_GEMINI_BLUE  = 0x4285F4;
constexpr uint32_t COL_GEMINI_PURP  = 0x8E54E9;
constexpr uint32_t COL_GREEN        = 0x22C55E;
constexpr uint32_t COL_GREEN_DK     = 0x14532D;
constexpr uint32_t COL_AMBER        = 0xF59E0B;
constexpr uint32_t COL_RED          = 0xEF4444;
constexpr uint32_t COL_RED_DK       = 0x7F1D1D;
constexpr uint32_t COL_TEXT         = 0xE2E8F0;
constexpr uint32_t COL_DIM          = 0x94A3B8;
constexpr uint32_t COL_WHITE        = 0xFFFFFF;

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
    _want_start_talk      = false;
    _want_stop_talk       = false;
    _setup_complete       = false;
    _server_turn_done     = false;
    _new_turn_subtitle    = true;
    _audio_rms            = 0;
    _last_ui_ms           = 0;
    _last_led_ms          = 0;
    _listen_start_ms      = 0;
    _last_rendered_state  = -1;
    _last_rendered_sub.clear();

    _volume = std::clamp(static_cast<int>(GetHAL().getSpeakerVolume()), 10, 100);

    // Allocate PSRAM playback ring buffer & Base64 decode scratch buffer
    if (!_pcm_ring.allocate(PCM_RING_SAMPLES)) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM PCM ring buffer");
    }
    _b64_decode_buf = static_cast<uint8_t*>(heap_caps_malloc(B64_DECODE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _b64_decode_cap = _b64_decode_buf ? B64_DECODE_CAP : 0;

    // Load API key from /sd/gemini.txt (updating NVS) or cached NVS
    bool has_key = load_api_config();

    {
        LvglLockGuard lock;
        build_ui();
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

    // Auto-stop listening safety cap (30 s)
    SessionState st = static_cast<SessionState>(_state.load());
    if (st == SessionState::Listening && _listen_start_ms > 0 && (now - _listen_start_ms) >= MAX_LISTEN_MS) {
        _want_stop_talk = true;
    }

    if (now - _last_ui_ms >= 60) {
        _last_ui_ms = now;
        LvglLockGuard lock;
        refresh_ui(now);
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

    _closing = true;
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
        destroy_ui();
    }

    _pcm_ring.release();
    if (_b64_decode_buf) {
        heap_caps_free(_b64_decode_buf);
        _b64_decode_buf = nullptr;
        _b64_decode_cap = 0;
    }
}

/* -------------------------------------------------------------------------- */
/*                          Config Loading (SD + NVS)                         */
/* -------------------------------------------------------------------------- */

bool AppGemini::read_key_from_sd(std::string& out_key, std::string& out_model)
{
    sdmmc_host_t host   = SDSPI_HOST_DEFAULT();
    host.slot           = SPI3_HOST;
    host.max_freq_khz   = 12000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = GPIO_NUM_4;
    slot_config.host_id               = static_cast<spi_host_device_t>(host.slot);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed           = false;
    mount_config.max_files                        = 3;
    mount_config.allocation_unit_size             = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    esp_err_t ret      = esp_vfs_fat_sdspi_mount(SD_MOUNT_PT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "microSD not mounted (%s); will check NVS", esp_err_to_name(ret));
        return false;
    }

    bool found = false;
    FILE* f    = std::fopen(SD_KEY_PATH, "r");
    if (f) {
        char line[256];
        std::vector<std::string> lines;
        while (std::fgets(line, sizeof(line), f)) {
            std::string trimmed = trim_line(line);
            if (!trimmed.empty() && trimmed[0] != '#') {
                lines.push_back(trimmed);
                if (lines.size() >= 2) {
                    break;
                }
            }
        }
        std::fclose(f);

        if (!lines.empty() && lines[0].size() >= 10) {
            out_key   = lines[0];
            out_model = (lines.size() >= 2 && !lines[1].empty()) ? lines[1] : DEFAULT_MODEL;
            found     = true;
            ESP_LOGI(TAG, "Loaded Gemini API key from %s (model=%s)", SD_KEY_PATH, out_model.c_str());
        }
    }

    esp_vfs_fat_sdcard_unmount(SD_MOUNT_PT, card);
    return found;
}

bool AppGemini::load_key_from_nvs(std::string& out_key, std::string& out_model)
{
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

    out_model = DEFAULT_MODEL;
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

    nvs_close(h);
    return ok;
}

void AppGemini::save_key_to_nvs(const std::string& key, const std::string& model)
{
    nvs_handle_t h{};
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_API_KEY, key.c_str());
        nvs_set_str(h, KEY_MODEL, model.c_str());
        nvs_commit(h);
        nvs_close(h);
    }
}

bool AppGemini::load_api_config()
{
    std::string key;
    std::string model = DEFAULT_MODEL;

    if (read_key_from_sd(key, model)) {
        _api_key    = key;
        _model_name = model;
        save_key_to_nvs(_api_key, _model_name);
        return true;
    }

    if (load_key_from_nvs(key, model)) {
        _api_key    = key;
        _model_name = model;
        ESP_LOGI(TAG, "Loaded cached Gemini API key from NVS (model=%s)", _model_name.c_str());
        return true;
    }

    _api_key.clear();
    _model_name = DEFAULT_MODEL;
    return false;
}

/* -------------------------------------------------------------------------- */
/*                       Background Network & Audio Tasks                     */
/* -------------------------------------------------------------------------- */

void AppGemini::start_tasks()
{
    _net_task_exited = false;
    _spk_task_exited = false;

    BaseType_t ok_net = xTaskCreatePinnedToCoreWithCaps(
        net_task_entry, "gemini_net", 16384, this, 4, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok_net != pdPASS) {
        _net_task_exited = true;
        ESP_LOGE(TAG, "Failed to create gemini_net task");
    }

    BaseType_t ok_spk = xTaskCreatePinnedToCoreWithCaps(
        spk_task_entry, "gemini_spk", 12288, this, 5, nullptr, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

bool AppGemini::send_setup_message()
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

    cJSON* sys_inst = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON* parts    = cJSON_AddArrayToObject(sys_inst, "parts");
    cJSON* part0    = cJSON_CreateObject();
    cJSON_AddStringToObject(
        part0, "text",
        "You are StackChan, a friendly little desktop robot companion. "
        "Keep answers concise, natural, and lively in the user's language.");
    cJSON_AddItemToArray(parts, part0);

    // Disable automatic VAD so the robot uses clean tap-to-talk (zero echo, minimal Free Tier quota)
    cJSON* rt_cfg = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON* aad    = cJSON_AddObjectToObject(rt_cfg, "automaticActivityDetection");
    cJSON_AddBoolToObject(aad, "disabled", true);

    // Enable output audio transcription for live captions on the 2.0-inch display
    cJSON_AddObjectToObject(setup, "outputAudioTranscription");

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return false;
    }

    std::string payload(printed);
    cJSON_free(printed);
    ESP_LOGI(TAG, "Sending Gemini Live setup (%s)", model_path.c_str());
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
            if (cur != SessionState::Error && cur != SessionState::NeedKey) {
                set_error("Session closed. Tap RETRY.");
            }
        }
    });

    std::string url =
        "wss://generativelanguage.googleapis.com:443/ws/"
        "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" +
        _api_key;

    auto res = _ws->Connect(url.c_str());
    if (!res) {
        ESP_LOGE(TAG, "Connect failed: %s", res.error().ToString().c_str());
        set_error("Connect failed (check Wi-Fi / API key)");
        disconnect_live_session();
        return false;
    }

    if (!send_setup_message()) {
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

    if (_closing.load() || !_setup_complete.load()) {
        return false;
    }

    set_state(SessionState::Ready);
    set_subtitle("Ready! Tap TALK to speak");
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
                        size_t samples = out_len / sizeof(int16_t);
                        _pcm_ring.push(reinterpret_cast<const int16_t*>(_b64_decode_buf), samples);
                        SessionState cur = static_cast<SessionState>(_state.load());
                        if (cur == SessionState::Thinking || cur == SessionState::Ready) {
                            set_state(SessionState::Speaking);
                        }
                    } else if (dec_ret != 0) {
                        ESP_LOGW(TAG, "Base64 decode failed (ret=%d, in=%u)", dec_ret, static_cast<unsigned>(b64_len));
                    }
                }
            }
        }

        // Output audio transcription (live captions)
        cJSON* out_tr = cJSON_GetObjectItem(sc, "outputTranscription");
        if (out_tr && cJSON_IsObject(out_tr)) {
            cJSON* txt = cJSON_GetObjectItem(out_tr, "text");
            if (txt && cJSON_IsString(txt) && txt->valuestring && txt->valuestring[0] != '\0') {
                bool reset = _new_turn_subtitle.exchange(false);
                set_subtitle(txt->valuestring, !reset);
            }
        }

        cJSON* tc = cJSON_GetObjectItem(sc, "turnComplete");
        if (tc && cJSON_IsTrue(tc)) {
            _server_turn_done = true;
            _new_turn_subtitle = true;
        }
    }

    cJSON_Delete(root);
}

void AppGemini::stream_mic_chunk()
{
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !_ws || !_ws->IsConnected()) {
        return;
    }

    // Read 100 ms @ 24 kHz = 2400 frames; CoreS3 has 2 input channels (ch0=mic, ch1=ref)
    constexpr size_t kInFrames  = 2400;
    constexpr size_t kOutFrames = 1600;   // 100 ms @ 16 kHz (3:2 downsample)
    const size_t in_ch          = std::max(codec->input_channels(), 1);

    std::vector<int16_t> raw_in(kInFrames * in_ch);
    if (!codec->InputData(raw_in)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    // Extract mic channel (ch0) & 3:2 downsample 24 kHz -> 16 kHz + compute RMS
    int16_t pcm16k[kOutFrames];
    int64_t sum_sq = 0;
    for (size_t g = 0; g < kInFrames / 3; ++g) {
        int32_t s0 = raw_in[(g * 3 + 0) * in_ch];
        int32_t s1 = raw_in[(g * 3 + 1) * in_ch];
        int32_t s2 = raw_in[(g * 3 + 2) * in_ch];
        int16_t o0 = static_cast<int16_t>(s0);
        int16_t o1 = static_cast<int16_t>((s1 + s2) / 2);
        pcm16k[g * 2 + 0] = o0;
        pcm16k[g * 2 + 1] = o1;
        sum_sq += static_cast<int64_t>(o0) * o0 + static_cast<int64_t>(o1) * o1;
    }
    int rms = static_cast<int>(std::sqrt(static_cast<double>(sum_sq) / static_cast<double>(kOutFrames)));
    _audio_rms.store(rms);

    // Base64-encode 3200 bytes -> ~4268 chars
    constexpr size_t kRawBytes = kOutFrames * sizeof(int16_t);
    unsigned char b64_out[4400];
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64_out, sizeof(b64_out) - 1, &b64_len,
                              reinterpret_cast<const unsigned char*>(pcm16k), kRawBytes) != 0) {
        return;
    }
    b64_out[b64_len] = '\0';

    std::string msg;
    msg.reserve(b64_len + 96);
    msg.append("{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"");
    msg.append(reinterpret_cast<const char*>(b64_out), b64_len);
    msg.append("\"}}}");

    if (!_ws->Send(msg)) {
        ESP_LOGW(TAG, "Failed to send audio chunk");
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

        if (_want_start_talk.exchange(false)) {
            if (!_ws || !_ws->IsConnected() || !_setup_complete.load()) {
                if (!connect_live_session()) {
                    continue;
                }
            }

            // Barge-in: flush any queued speaker audio immediately
            _pcm_ring.clear();
            _server_turn_done  = false;
            _new_turn_subtitle = true;
            _audio_rms         = 0;

            if (codec && codec->output_enabled()) {
                codec->EnableOutput(false);
            }

            if (_ws && _ws->IsConnected()) {
                _ws->Send("{\"realtimeInput\":{\"activityStart\":{}}}");
            }

            if (codec) {
                codec->EnableInput(true);
            }
            _listen_start_ms = GetHAL().millis();
            set_state(SessionState::Listening);
            set_subtitle("Listening... tap SEND when done");
        }

        if (_want_stop_talk.exchange(false)) {
            if (codec && codec->input_enabled()) {
                codec->EnableInput(false);
            }
            _audio_rms       = 0;
            _listen_start_ms = 0;

            if (_ws && _ws->IsConnected()) {
                _ws->Send("{\"realtimeInput\":{\"activityEnd\":{}}}");
                set_state(SessionState::Thinking);
                set_subtitle("Thinking...");
            }
        }

        SessionState st = static_cast<SessionState>(_state.load());
        if (st == SessionState::Listening) {
            stream_mic_chunk();
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
    auto* codec = Board::GetInstance().GetAudioCodec();
    constexpr size_t kChunkSamples = 480;   // 20 ms @ 24 kHz
    std::vector<int16_t> chunk(kChunkSamples);
    bool output_active = false;

    while (!_closing.load()) {
        size_t got = _pcm_ring.pop(chunk.data(), kChunkSamples);
        if (got > 0 && codec) {
            if (!output_active) {
                codec->SetOutputVolume(_volume);
                codec->EnableOutput(true);
                output_active = true;
            }
            if (got < kChunkSamples) {
                std::fill(chunk.begin() + got, chunk.end(), 0);
            }

            // Compute voice RMS for live mouth animation & LED pulse
            int64_t sum_sq = 0;
            for (size_t i = 0; i < got; ++i) {
                int32_t s = chunk[i];
                sum_sq += static_cast<int64_t>(s) * s;
            }
            int rms = static_cast<int>(std::sqrt(static_cast<double>(sum_sq) / static_cast<double>(got)));
            _audio_rms.store(rms);

            codec->OutputData(chunk);
        } else {
            if (output_active && _pcm_ring.available() == 0) {
                // Keep output open briefly between network packets unless turn is complete or interrupted
                if (_server_turn_done.load() ||
                    static_cast<SessionState>(_state.load()) != SessionState::Speaking) {
                    if (codec) {
                        codec->EnableOutput(false);
                    }
                    output_active = false;
                    _audio_rms.store(0);
                    if (_server_turn_done.exchange(false) &&
                        static_cast<SessionState>(_state.load()) == SessionState::Speaking) {
                        set_state(SessionState::Ready);
                    }
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
            _want_connect = true;
            break;
        case SessionState::Ready:
            _want_start_talk = true;
            break;
        case SessionState::Listening:
            _want_stop_talk = true;
            break;
        case SessionState::Speaking:
        case SessionState::Thinking:
            // Instant barge-in: stop playback & start listening again
            _want_start_talk = true;
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
            g = 16;
            b = 36;
            break;
        case SessionState::Listening: {
            int level = std::clamp(24 + rms / 80, 24, 90);
            r         = 0;
            g         = static_cast<uint8_t>(level);
            b         = static_cast<uint8_t>(level / 3);
            break;
        }
        case SessionState::Thinking:
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

    // Top status bar (y = 0 .. 28)
    _lbl_badge = lv_label_create(_root_cont);
    lv_obj_set_pos(_lbl_badge, 10, 6);
    lv_obj_set_width(_lbl_badge, 185);
    lv_label_set_long_mode(_lbl_badge, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_badge, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_badge, lv_color_hex(COL_GEMINI_BLUE), 0);
    lv_label_set_text(_lbl_badge, "GEMINI");

    _lbl_vol_batt = lv_label_create(_root_cont);
    lv_obj_set_pos(_lbl_vol_batt, 200, 7);
    lv_obj_set_width(_lbl_vol_batt, 110);
    lv_obj_set_style_text_align(_lbl_vol_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(_lbl_vol_batt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_vol_batt, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(_lbl_vol_batt, "VOL 75%");

    // Center stage (y = 30 .. 142)
    _stage_cont = lv_obj_create(_root_cont);
    lv_obj_set_pos(_stage_cont, 8, 30);
    lv_obj_set_size(_stage_cont, 304, 112);
    style_flat_box(_stage_cont, COL_BG, 12);

    _face_card = lv_obj_create(_stage_cont);
    lv_obj_set_size(_face_card, 146, 98);
    lv_obj_align(_face_card, LV_ALIGN_CENTER, 0, 0);
    style_flat_box(_face_card, COL_CARD, 18);
    lv_obj_set_style_border_width(_face_card, 2, 0);
    lv_obj_set_style_border_color(_face_card, lv_color_hex(COL_GEMINI_BLUE), 0);
    lv_obj_add_flag(_face_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        _face_card,
        [](lv_event_t* e) {
            auto* self = static_cast<AppGemini*>(lv_event_get_user_data(e));
            self->on_primary_button();
        },
        LV_EVENT_CLICKED, this);

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

    // Setup card shown only when no API key is configured
    _setup_card = lv_obj_create(_stage_cont);
    lv_obj_set_size(_setup_card, 296, 108);
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
        "Gemini Free API Key Required\n\n"
        "1. Get a free key at aistudio.google.com\n"
        "2. Save it in /sd/gemini.txt on microSD\n"
        "3. Insert SD card and tap RETRY");

    // Live caption / status box (y = 146 .. 184)
    _sub_box = lv_obj_create(_root_cont);
    lv_obj_set_pos(_sub_box, 8, 146);
    lv_obj_set_size(_sub_box, 304, 38);
    style_flat_box(_sub_box, COL_CARD, 8);

    _lbl_subtitle = lv_label_create(_sub_box);
    lv_obj_set_pos(_lbl_subtitle, 8, 4);
    lv_obj_set_size(_lbl_subtitle, 288, 30);
    lv_label_set_long_mode(_lbl_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_lbl_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_subtitle, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_subtitle, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(_lbl_subtitle, "Starting...");

    // Bottom 4-button control bar (y = 190 .. 234): VOL- | TALK/SEND | VOL+ | EXIT
    auto make_btn = [&](int x, int w, uint32_t bg, const char* txt, const lv_font_t* font) -> lv_obj_t* {
        lv_obj_t* b = lv_button_create(_root_cont);
        lv_obj_set_pos(b, x, 190);
        lv_obj_set_size(b, w, 44);
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

    _btn_vol_dn = make_btn(8, 52, COL_CARD, "VOL-", &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        _btn_vol_dn,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->change_volume(-VOLUME_STEP);
        },
        LV_EVENT_CLICKED, this);

    _btn_primary = lv_button_create(_root_cont);
    lv_obj_set_pos(_btn_primary, 66, 190);
    lv_obj_set_size(_btn_primary, 132, 44);
    style_flat_box(_btn_primary, COL_GEMINI_BLUE, 12);
    _lbl_primary = lv_label_create(_btn_primary);
    lv_obj_center(_lbl_primary);
    lv_obj_set_style_text_font(_lbl_primary, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_primary, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(_lbl_primary, "TALK");
    lv_obj_add_event_cb(
        _btn_primary,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->on_primary_button();
        },
        LV_EVENT_CLICKED, this);

    _btn_vol_up = make_btn(204, 52, COL_CARD, "VOL+", &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        _btn_vol_up,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->change_volume(VOLUME_STEP);
        },
        LV_EVENT_CLICKED, this);

    _btn_exit = make_btn(262, 50, COL_CARD_HI, "EXIT", &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        _btn_exit,
        [](lv_event_t* e) {
            static_cast<AppGemini*>(lv_event_get_user_data(e))->close();
        },
        LV_EVENT_CLICKED, this);
}

void AppGemini::destroy_ui()
{
    if (_root_cont) {
        lv_obj_delete(_root_cont);
        _root_cont      = nullptr;
        _lbl_badge      = nullptr;
        _lbl_vol_batt   = nullptr;
        _stage_cont     = nullptr;
        _face_card      = nullptr;
        _face_eye_l     = nullptr;
        _face_eye_r     = nullptr;
        _face_mouth     = nullptr;
        _setup_card     = nullptr;
        _lbl_setup_body = nullptr;
        _sub_box        = nullptr;
        _lbl_subtitle   = nullptr;
        _btn_vol_dn     = nullptr;
        _btn_primary    = nullptr;
        _lbl_primary    = nullptr;
        _btn_vol_up     = nullptr;
        _btn_exit       = nullptr;
    }
}

void AppGemini::refresh_ui(uint32_t now)
{
    if (!_root_cont) {
        return;
    }

    SessionState st = static_cast<SessionState>(_state.load());

    // Top right: volume + battery
    int batt = GetHAL().getBatteryLevel();
    char vbuf[32];
    if (batt >= 0) {
        std::snprintf(vbuf, sizeof(vbuf), "VOL %d%%  %d%%", _volume, batt);
    } else {
        std::snprintf(vbuf, sizeof(vbuf), "VOL %d%%", _volume);
    }
    lv_label_set_text(_lbl_vol_batt, vbuf);

    // State-dependent badge, primary button, and setup card
    if (_last_rendered_state != static_cast<int>(st)) {
        _last_rendered_state = static_cast<int>(st);

        bool need_key = (st == SessionState::NeedKey);
        if (need_key) {
            lv_obj_remove_flag(_setup_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_setup_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
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
                badge_txt = "GEMINI  READY";
                badge_col = COL_GEMINI_BLUE;
                btn_txt   = LV_SYMBOL_AUDIO " TALK";
                btn_col   = COL_GEMINI_BLUE;
                break;
            case SessionState::Listening:
                badge_txt = "GEMINI  LISTENING";
                badge_col = COL_GREEN;
                btn_txt   = LV_SYMBOL_STOP " SEND";
                btn_col   = COL_GREEN;
                break;
            case SessionState::Thinking:
                badge_txt = "GEMINI  THINKING";
                badge_col = COL_AMBER;
                btn_txt   = "CANCEL";
                btn_col   = COL_AMBER;
                break;
            case SessionState::Speaking:
                badge_txt = "GEMINI  SPEAKING";
                badge_col = COL_GEMINI_PURP;
                btn_txt   = LV_SYMBOL_AUDIO " REPLY";
                btn_col   = COL_GEMINI_PURP;
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
    if (!_face_card) {
        return;
    }

    int rms        = _audio_rms.load();
    int offset_y   = 0;
    int mouth_w    = 38;
    int mouth_h    = 6;
    uint32_t bcol  = COL_CARD_HI;

    if (st == SessionState::Listening) {
        bcol    = COL_GREEN;
        int lvl = std::clamp(rms / 250, 0, 10);
        mouth_w = 24 + lvl;
        mouth_h = 8 + lvl;
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
