/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_speaker.h"

#include <apps/common/common.h>
#include <apps/common/community_helpers.h>
#include <assets/assets.h>
#include <audio/audio_codec.h>
#include <board.h>
#include <cJSON.h>
#include <hal/board/config.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <settings.h>
#include <stackchan/stackchan.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include <usb_device_uac.h>
#define SPEAKER_HAS_UAC 1
#else
#define SPEAKER_HAS_UAC 0
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace community;

namespace {

constexpr const char* TAG    = "SPEAKER";
constexpr const char* NVS_NS = "app_speaker";

constexpr int W             = 320;
constexpr int H             = 240;
constexpr int BAR4_Y        = 192;
constexpr int BAR4_H        = 42;
constexpr int BAR4_W        = 71;
constexpr int BAR4_GAP      = 6;
constexpr int BAR4_X0       = 9;

constexpr size_t kRingSamples           = 48000;  // 2.0 s @ 24 kHz (96 KB PSRAM)
constexpr size_t kMaxLatencyWifiSamples = 24000;  // 1.0 s jitter headroom for Wi-Fi WS/UDP
constexpr size_t kMaxLatencyUsbSamples  = 4800;   // 200 ms tight latency for USB-C UAC
constexpr size_t kPrebufferWifiSamples  = 6000;   // 250 ms pre-buffer before playback starts
constexpr size_t kPrebufferUsbSamples   = 960;    // 40 ms pre-buffer for USB-C
constexpr size_t kChunkSamples          = 480;    // 20 ms @ 24 kHz for smooth I2S DMA
constexpr uint32_t kActiveTimeoutMs     = 1200;

constexpr uint32_t kRgbPalette[6] = {
    0x2DD4BF, 0x3B82F6, 0x8B5CF6, 0xF59E0B, 0x22C55E, 0xEC4899,
};

// Lightweight 128-point Radix-2 FFT (inspired by mongonta0716/stackchan-bluetooth-simple)
class SpeakerFft128 {
public:
    static constexpr size_t N = 128;

    SpeakerFft128()
    {
        constexpr float kPi    = 3.14159265358979323846f;
        constexpr float omega  = 2.0f * kPi / static_cast<float>(N);
        for (size_t i = 0; i < N / 2; ++i) {
            _cos[i] = std::cos(omega * static_cast<float>(i));
            _sin[i] = std::sin(omega * static_cast<float>(i));
        }
        for (size_t i = 0; i < N; ++i) {
            size_t x = i;
            size_t r = 0;
            for (int b = 0; b < 7; ++b) {
                r = (r << 1) | (x & 1u);
                x >>= 1;
            }
            _rev[i] = static_cast<uint8_t>(r);
            _win[i] = 0.5f * (1.0f - std::cos(omega * static_cast<float>(i)));
        }
    }

    void compute(const int16_t* pcm, float& out_mouth, float& out_energy, std::array<uint8_t, 8>& out_bands)
    {
        float re[N];
        float im[N];
        float sum_sq = 0.0f;

        for (size_t i = 0; i < N; ++i) {
            float v         = static_cast<float>(pcm[i]) * (1.0f / 32768.0f);
            sum_sq         += v * v;
            re[_rev[i]]     = v * _win[i];
            im[_rev[i]]     = 0.0f;
        }

        out_energy = std::min(1.0f, std::sqrt(sum_sq / static_cast<float>(N)) * 3.8f);

        for (size_t len = 2; len <= N; len <<= 1) {
            size_t half = len >> 1;
            size_t step = N / len;
            for (size_t i = 0; i < N; i += len) {
                for (size_t j = 0; j < half; ++j) {
                    size_t idx = j * step;
                    float wr   = _cos[idx];
                    float wi   = -_sin[idx];
                    float tr   = wr * re[i + j + half] - wi * im[i + j + half];
                    float ti   = wr * im[i + j + half] + wi * re[i + j + half];
                    re[i + j + half] = re[i + j] - tr;
                    im[i + j + half] = im[i + j] - ti;
                    re[i + j] += tr;
                    im[i + j] += ti;
                }
            }
        }

        // Bin ranges for 8 visual EQ bars across 0..12 kHz (bins 1..63)
        static constexpr size_t kEdges[9] = {1, 3, 6, 10, 16, 24, 34, 48, 63};
        float voice_sum                   = 0.0f;
        for (int b = 0; b < 8; ++b) {
            float acc = 0.0f;
            size_t cnt = kEdges[b + 1] - kEdges[b];
            for (size_t k = kEdges[b]; k < kEdges[b + 1]; ++k) {
                float mag = std::sqrt(re[k] * re[k] + im[k] * im[k]);
                acc += mag;
                if (b >= 1 && b <= 5) {
                    voice_sum += mag;
                }
            }
            float avg = (cnt > 0) ? (acc / static_cast<float>(cnt)) : 0.0f;
            int pct   = static_cast<int>(std::clamp(avg * 38.0f, 0.0f, 100.0f));
            out_bands[b] = static_cast<uint8_t>(pct);
        }

        out_mouth = std::clamp(voice_sum * 0.045f + out_energy * 0.65f, 0.0f, 1.0f);
    }

private:
    float _cos[N / 2];
    float _sin[N / 2];
    float _win[N];
    uint8_t _rev[N];
};

#if SPEAKER_HAS_UAC
// One-shot global UAC trampoline (uac_device_init initializes TinyUSB once per boot)
std::mutex g_uac_mtx;
AppSpeaker* g_uac_owner = nullptr;
bool g_uac_initialized  = false;

esp_err_t uac_output_trampoline(uint8_t* buf, size_t len, void* /*cb_ctx*/)
{
    if (!buf || len < 2) {
        return ESP_OK;
    }
    std::lock_guard<std::mutex> lk(g_uac_mtx);
    if (g_uac_owner) {
        g_uac_owner->on_uac_pcm_48k(reinterpret_cast<const int16_t*>(buf), len / sizeof(int16_t));
    }
    return ESP_OK;
}

void uac_volume_trampoline(uint32_t volume, void* /*cb_ctx*/)
{
    std::lock_guard<std::mutex> lk(g_uac_mtx);
    if (g_uac_owner) {
        g_uac_owner->on_uac_volume(volume);
    }
}

void uac_mute_trampoline(uint32_t mute, void* /*cb_ctx*/)
{
    std::lock_guard<std::mutex> lk(g_uac_mtx);
    if (g_uac_owner) {
        g_uac_owner->on_uac_mute(mute);
    }
}
#endif

}  // namespace

/* -------------------------------------------------------------------------- */
/*                              PCM Ring Buffer                               */
/* -------------------------------------------------------------------------- */

bool AppSpeaker::PcmRingBuffer::allocate(size_t samples)
{
    std::lock_guard<std::mutex> lk(mtx);
    if (buf) {
        heap_caps_free(buf);
        buf = nullptr;
    }
    buf = static_cast<int16_t*>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<int16_t*>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_8BIT));
    }
    if (!buf) {
        capacity = head = tail = count = 0;
        return false;
    }
    capacity = samples;
    head = tail = count = 0;
    return true;
}

void AppSpeaker::PcmRingBuffer::release()
{
    std::lock_guard<std::mutex> lk(mtx);
    if (buf) {
        heap_caps_free(buf);
        buf = nullptr;
    }
    capacity = head = tail = count = 0;
}

void AppSpeaker::PcmRingBuffer::clear()
{
    std::lock_guard<std::mutex> lk(mtx);
    head = tail = count = 0;
}

void AppSpeaker::PcmRingBuffer::push(const int16_t* data, size_t samples, size_t max_latency_samples)
{
    if (!data || samples == 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(mtx);
    if (!buf || capacity == 0) {
        return;
    }
    if (samples >= capacity) {
        data += (samples - capacity);
        samples = capacity;
        head = tail = count = 0;
    }
    // Keep live streaming latency tight by dropping oldest audio if backlog grows too high
    size_t limit = std::min(capacity, max_latency_samples);
    if (count + samples > limit) {
        size_t drop = (count + samples) - limit;
        if (drop > count) {
            drop = count;
        }
        tail = (tail + drop) % capacity;
        count -= drop;
    }
    size_t first = std::min(samples, capacity - head);
    std::memcpy(buf + head, data, first * sizeof(int16_t));
    if (samples > first) {
        std::memcpy(buf, data + first, (samples - first) * sizeof(int16_t));
    }
    head = (head + samples) % capacity;
    count += samples;
}

size_t AppSpeaker::PcmRingBuffer::pop(int16_t* out, size_t samples)
{
    std::lock_guard<std::mutex> lk(mtx);
    if (!buf || count == 0 || samples == 0) {
        return 0;
    }
    size_t n     = std::min(samples, count);
    size_t first = std::min(n, capacity - tail);
    std::memcpy(out, buf + tail, first * sizeof(int16_t));
    if (n > first) {
        std::memcpy(out + first, buf, (n - first) * sizeof(int16_t));
    }
    tail = (tail + n) % capacity;
    count -= n;
    return n;
}

size_t AppSpeaker::PcmRingBuffer::available() const
{
    std::lock_guard<std::mutex> lk(mtx);
    return count;
}

/* -------------------------------------------------------------------------- */
/*                             Mooncake Lifecycle                             */
/* -------------------------------------------------------------------------- */

AppSpeaker::AppSpeaker()
{
    setAppInfo().name         = "SPEAKER";
    static auto icon          = assets::get_image("icon_music.bin");
    setAppInfo().icon         = (void*)&icon;
    static uint32_t theme_col = COL_TEAL;
    setAppInfo().userData     = (void*)&theme_col;
}

AppSpeaker::~AppSpeaker()
{
    stop_usb_mode();
    stop_wifi_mode();
    stop_audio_task();
    _ring.release();
}

void AppSpeaker::onCreate()
{
    mclog::tagInfo(TAG, "on create");
}

void AppSpeaker::onOpen()
{
    mclog::tagInfo(TAG, "on open");

    _input_mode   = InputMode::Wifi;  // Always start in Wi-Fi so USB-Serial-JTAG stays intact until USB-C is tapped
    _view_mode    = ViewMode::Main;
    _muted        = false;
    _last_rx_ms   = 0;
    _last_sound_ms = 0;
    _rx_packets   = 0;
    _mouth_ratio  = 0.0f;
    _energy_level = 0.0f;
    _was_active   = false;
    _last_qr_url.clear();
    for (auto& b : _eq_bars) {
        b = 0;
    }

    load_prefs();
    GetHAL().setSpeakerVolume(static_cast<uint8_t>(_volume), false);

    // Servos & LEDs
    GetHAL().setServoPowerEnabled(true);
    GetHAL().showRgbColor(0, 0, 0);
    auto& sc = GetStackChan();
    sc.clearModifiers();
    sc.motion().setAutoAngleSyncEnabled(false);
    sc.motion().setAutoTorqueReleaseEnabled(false);
    move_home(600);

    // Optional Custom Avatar sprites (follows global avatar setting from main Settings)
    _img_avatar_calm  = assets::get_image("avatar_calm_music.bin");
    _img_avatar_happy = assets::get_image("avatar_happy_music.bin");

    _ring.allocate(kRingSamples);
    start_audio_task();

    // Head touch gestures: tap = mute, swipe forward = vol+, swipe backward = vol-
    _head_pet_conn = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture g) {
        if (g == HeadPetGesture::Press) {
            _pending_toggle_mute = true;
        } else if (g == HeadPetGesture::SwipeForward) {
            _pending_vol_delta = 10;
        } else if (g == HeadPetGesture::SwipeBackward) {
            _pending_vol_delta = -10;
        }
    });

    {
        LvglLockGuard lock;
        build_ui();
        view::create_home_indicator([&]() { close(); }, COL_TEAL, COL_BG);
        view::create_status_bar(COL_TEAL, COL_BG);
    }

    start_wifi_mode();

    {
        LvglLockGuard lock;
        refresh_mode_bar();
        refresh_bottom_bar();
        refresh_ui(GetHAL().millis());
    }
}

void AppSpeaker::onRunning()
{
    uint32_t now = GetHAL().millis();

    process_remote_cmds();

    bool active = (_last_sound_ms.load() != 0) && ((now - _last_sound_ms.load()) < kActiveTimeoutMs) && !_muted;
    float level = active ? _energy_level.load() : 0.0f;

    update_robot_dance(now, active, level);

    {
        LvglLockGuard lock;
        view::update_home_indicator();
        view::update_status_bar();
        if (now - _last_ui_ms >= 50) {
            _last_ui_ms = now;
            refresh_ui(now);
        }
    }

    GetStackChan().update();
}

void AppSpeaker::onClose()
{
    mclog::tagInfo(TAG, "on close");

    if (_head_pet_conn >= 0) {
        GetHAL().onHeadPetGesture.disconnect(_head_pet_conn);
        _head_pet_conn = -1;
    }

    stop_usb_mode();
    stop_wifi_mode();
    stop_audio_task();
    _ring.release();

    GetHAL().showRgbColor(0, 0, 0);
    move_home(500);
    save_prefs();

    LvglLockGuard lock;
    view::destroy_home_indicator();
    view::destroy_status_bar();
    destroy_ui();
}

/* -------------------------------------------------------------------------- */
/*                            Input Modes & Audio                             */
/* -------------------------------------------------------------------------- */

void AppSpeaker::switch_input_mode(InputMode mode)
{
    if (_input_mode == mode) {
        return;
    }
    _ring.clear();
    _last_rx_ms    = 0;
    _last_sound_ms = 0;
    _rx_packets    = 0;
    _mouth_ratio = 0.0f;
    _energy_level = 0.0f;
    for (auto& b : _eq_bars) {
        b = 0;
    }

    if (mode == InputMode::Wifi) {
        stop_usb_mode();
        _input_mode = InputMode::Wifi;
        start_wifi_mode();
    } else {
        stop_wifi_mode();
        _input_mode = InputMode::Usb;
        start_usb_mode();
    }

    refresh_mode_bar();
    if (_view_mode == ViewMode::Info) {
        refresh_info_view();
    }
}

void AppSpeaker::start_wifi_mode()
{
    if (!_wifi_svc) {
        _wifi_svc = std::make_unique<SpeakerWifiService>();
    }
    SpeakerWifiService::Callbacks cbs;
    cbs.on_pcm_24k = [this](const int16_t* samples, size_t count) {
        if (_input_mode == InputMode::Wifi) {
            push_pcm_24k(samples, count);
        }
    };
    cbs.get_status_json = [this]() { return build_status_json(); };
    cbs.on_remote_cmd   = [this](const std::string& action, int value) {
        if (action == "vol") {
            _pending_vol = std::clamp(value, 0, 100);
        } else if (action == "mute") {
            _pending_toggle_mute = true;
        } else if (action == "move") {
            _pending_toggle_move = true;
        } else if (action == "lights") {
            _pending_toggle_lights = true;
        }
    };
    _wifi_svc->start(cbs);
}

void AppSpeaker::stop_wifi_mode()
{
    if (_wifi_svc) {
        _wifi_svc->stop();
        _wifi_svc.reset();
    }
}

void AppSpeaker::start_usb_mode()
{
#if SPEAKER_HAS_UAC
    std::lock_guard<std::mutex> lk(g_uac_mtx);
    g_uac_owner = this;
    if (!g_uac_initialized) {
        uac_device_config_t cfg = {};
        cfg.skip_tinyusb_init   = false;
        cfg.output_cb           = uac_output_trampoline;
        cfg.input_cb            = nullptr;
        cfg.set_mute_cb         = uac_mute_trampoline;
        cfg.set_volume_cb       = uac_volume_trampoline;
        cfg.cb_ctx              = nullptr;

        esp_err_t err = uac_device_init(&cfg);
        if (err == ESP_OK) {
            g_uac_initialized = true;
            _usb_attached     = true;
            _usb_status_msg   = "USB-C ready: StackChan Speaker";
            mclog::tagInfo(TAG, "USB-C UAC speaker initialized");
        } else {
            g_uac_owner     = nullptr;
            _usb_attached   = false;
            _usb_status_msg = "USB-C init failed";
            mclog::tagError(TAG, "uac_device_init failed: %d", static_cast<int>(err));
        }
    } else {
        _usb_attached   = true;
        _usb_status_msg = "USB-C ready: StackChan Speaker";
    }
#else
    _usb_attached   = false;
    _usb_status_msg = "USB-C UAC not supported";
#endif
}

void AppSpeaker::stop_usb_mode()
{
#if SPEAKER_HAS_UAC
    std::lock_guard<std::mutex> lk(g_uac_mtx);
    if (g_uac_owner == this) {
        g_uac_owner = nullptr;
    }
#endif
    _usb_attached = false;
}

void AppSpeaker::on_uac_pcm_48k(const int16_t* samples, size_t count)
{
    if (_input_mode != InputMode::Usb || !samples || count < 2) {
        return;
    }
    constexpr size_t kMaxOut = 512;
    int16_t down[kMaxOut];
#if defined(CONFIG_UAC_SPEAKER_CHANNEL_NUM) && (CONFIG_UAC_SPEAKER_CHANNEL_NUM == 2)
    // 48 kHz stereo (L,R) -> 24 kHz mono: average 2 stereo frames (4 samples) per output sample
    size_t pos = 0;
    while (pos + 3 < count) {
        size_t out_n = std::min((count - pos) / 4, kMaxOut);
        for (size_t i = 0; i < out_n; ++i) {
            size_t base = pos + i * 4;
            int32_t avg = (static_cast<int32_t>(samples[base]) + static_cast<int32_t>(samples[base + 1]) +
                           static_cast<int32_t>(samples[base + 2]) + static_cast<int32_t>(samples[base + 3])) >>
                          2;
            down[i] = static_cast<int16_t>(avg);
        }
        push_pcm_24k(down, out_n);
        pos += out_n * 4;
    }
#else
    // 2:1 boxcar downsample 48 kHz mono -> 24 kHz mono (matches AW88298 codec)
    size_t pos = 0;
    while (pos + 1 < count) {
        size_t pairs = std::min((count - pos) / 2, kMaxOut);
        for (size_t i = 0; i < pairs; ++i) {
            int32_t avg = (static_cast<int32_t>(samples[pos + i * 2]) +
                           static_cast<int32_t>(samples[pos + i * 2 + 1])) >>
                          1;
            down[i] = static_cast<int16_t>(avg);
        }
        push_pcm_24k(down, pairs);
        pos += pairs * 2;
    }
#endif
}

void AppSpeaker::on_uac_volume(uint32_t volume)
{
    _pending_vol = std::clamp(static_cast<int>(volume), 0, 100);
}

void AppSpeaker::on_uac_mute(uint32_t mute)
{
    _pending_mute = (mute != 0) ? 1 : 0;
}

void AppSpeaker::push_pcm_24k(const int16_t* samples, size_t count)
{
    if (!samples || count == 0) {
        return;
    }
    const size_t max_lat = (_input_mode == InputMode::Wifi) ? kMaxLatencyWifiSamples : kMaxLatencyUsbSamples;
    _ring.push(samples, count, max_lat);
    uint32_t now = GetHAL().millis();
    _last_rx_ms  = now;
    _rx_packets++;

    // Detect non-silent audio so paused USB-C/Wi-Fi streams don't trigger dance/LEDs
    int32_t peak = 0;
    for (size_t i = 0; i < count; i += 4) {
        int32_t v = samples[i];
        if (v < 0) {
            v = -v;
        }
        if (v > peak) {
            peak = v;
        }
    }
    if (peak > 150) {
        _last_sound_ms = now;
    }
}

void AppSpeaker::start_audio_task()
{
    if (_audio_running.load()) {
        return;
    }
    _audio_running = true;
    _audio_exited  = false;
    if (xTaskCreatePinnedToCore(audio_task_entry, "spk_audio", 5120, this, 3, nullptr, 1) != pdPASS) {
        _audio_running = false;
        _audio_exited  = true;
    }
}

void AppSpeaker::stop_audio_task()
{
    _audio_running = false;
    for (int i = 0; i < 60 && !_audio_exited.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

void AppSpeaker::audio_task_entry(void* arg)
{
    static_cast<AppSpeaker*>(arg)->audio_task_loop();
}

void AppSpeaker::audio_task_loop()
{
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        mclog::tagError(TAG, "AudioCodec unavailable");
        _audio_exited = true;
        vTaskDelete(nullptr);
        return;
    }

    auto fft = std::make_unique<SpeakerFft128>();
    std::vector<int16_t> chunk(kChunkSamples, 0);
    bool output_on    = false;
    bool buffering    = true;
    uint32_t idle_ticks = 0;
    uint32_t fft_div  = 0;

    while (_audio_running.load()) {
        size_t avail = _ring.available();
        size_t prebuf = (_input_mode == InputMode::Wifi) ? kPrebufferWifiSamples : kPrebufferUsbSamples;

        if (buffering) {
            if (avail < prebuf) {
                float cur_m  = _mouth_ratio.load();
                _mouth_ratio = (cur_m > 0.02f) ? (cur_m * 0.78f) : 0.0f;
                float cur_e  = _energy_level.load();
                _energy_level = (cur_e > 0.02f) ? (cur_e * 0.78f) : 0.0f;
                for (auto& b : _eq_bars) {
                    uint8_t v = b.load();
                    b = (v > 3) ? static_cast<uint8_t>(v - 4) : 0;
                }
                if (output_on && ++idle_ticks > 40) {
                    codec->EnableOutput(false);
                    output_on = false;
                }
                vTaskDelay(std::max<TickType_t>(2, pdMS_TO_TICKS(20)));
                continue;
            }
            buffering = false;
        }

        size_t got = _ring.pop(chunk.data(), kChunkSamples);
        if (got < kChunkSamples) {
            // Underrun: re-enter pre-buffer mode to absorb Wi-Fi jitter cleanly
            buffering = true;
            if (got == 0) {
                vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
                continue;
            }
            std::memset(chunk.data() + got, 0, (kChunkSamples - got) * sizeof(int16_t));
        }

        idle_ticks = 0;

        if (_muted) {
            std::fill(chunk.begin(), chunk.end(), 0);
            _mouth_ratio  = 0.0f;
            _energy_level = 0.0f;
            for (auto& b : _eq_bars) {
                b = 0;
            }
        } else if ((++fft_div & 1u) == 0u) {
            // Run FFT every ~40 ms (matches 20-25 FPS UI refresh, saves CPU for smooth I2S)
            float mouth  = 0.0f;
            float energy = 0.0f;
            std::array<uint8_t, 8> bands{};
            fft->compute(chunk.data(), mouth, energy, bands);
            _mouth_ratio  = _mouth_ratio.load() * 0.35f + mouth * 0.65f;
            _energy_level = _energy_level.load() * 0.4f + energy * 0.6f;
            for (int i = 0; i < 8; ++i) {
                uint8_t prev = _eq_bars[i].load();
                _eq_bars[i]  = static_cast<uint8_t>((prev * 2 + bands[i] * 3) / 5);
            }
        }

        if (!output_on) {
            codec->EnableOutput(true);
            output_on = true;
        }
        codec->OutputData(chunk);
    }

    if (output_on) {
        codec->EnableOutput(false);
    }
    _audio_exited = true;
    vTaskDelete(nullptr);
}

/* -------------------------------------------------------------------------- */
/*                         Controls, Dance & Settings                         */
/* -------------------------------------------------------------------------- */

void AppSpeaker::load_prefs()
{
    Settings s(NVS_NS, false);
    int vol      = s.GetInt("vol", -1);
    _volume      = (vol >= 0 && vol <= 100) ? vol : GetHAL().getSpeakerVolume();
    _robot_move  = s.GetBool("move", true);
    _lights_on   = s.GetBool("lights", true);
    _y_home_deg  = std::clamp<int>(s.GetInt("y_home", 45), 0, 90);
}

void AppSpeaker::save_prefs()
{
    Settings s(NVS_NS, true);
    s.SetInt("vol", _volume);
    s.SetBool("move", _robot_move);
    s.SetBool("lights", _lights_on);
    s.SetInt("y_home", _y_home_deg);
}

void AppSpeaker::set_volume(int vol)
{
    _volume = std::clamp(vol, 0, 100);
    GetHAL().setSpeakerVolume(static_cast<uint8_t>(_volume), false);
    save_prefs();
}

void AppSpeaker::change_volume(int delta)
{
    set_volume(_volume + delta);
}

void AppSpeaker::toggle_mute()
{
    _muted = !_muted;
    LvglLockGuard lock;
    refresh_bottom_bar();
    refresh_mode_bar();
}

void AppSpeaker::toggle_move()
{
    _robot_move = !_robot_move;
    if (!_robot_move) {
        move_home(500);
    }
    save_prefs();
    LvglLockGuard lock;
    refresh_bottom_bar();
}

void AppSpeaker::toggle_lights()
{
    _lights_on = !_lights_on;
    if (!_lights_on) {
        GetHAL().showRgbColor(0, 0, 0);
    }
    save_prefs();
    LvglLockGuard lock;
    refresh_bottom_bar();
}

void AppSpeaker::move_home(int speed)
{
    auto& motion = GetStackChan().motion();
    int pitch    = static_cast<int>(std::lround((static_cast<float>(_y_home_deg) / 90.0f) * 900.0f));
    motion.moveWithSpeed(0, pitch, speed);
}

void AppSpeaker::update_robot_dance(uint32_t now, bool active, float level)
{
    if (!active) {
        if (_was_active) {
            _was_active = false;
            if (_lights_on) {
                GetHAL().showRgbColor(0, 0, 0);
            }
            if (_robot_move) {
                move_home(500);
            }
        }
        return;
    }

    _was_active = true;

    // Reactive RGB lights
    if (_lights_on) {
        uint32_t hex = kRgbPalette[(now / 1200) % 6];
        float scale  = std::clamp(0.15f + level * 0.85f, 0.1f, 1.0f);
        uint8_t r    = static_cast<uint8_t>(((hex >> 16) & 0xFF) * scale);
        uint8_t g    = static_cast<uint8_t>(((hex >> 8) & 0xFF) * scale);
        uint8_t b    = static_cast<uint8_t>((hex & 0xFF) * scale);
        GetHAL().showRgbColor(r, g, b);
    }

    // Reactive servo dance (nod + gentle sway on beats)
    if (_robot_move && (now - _last_dance_ms >= 520)) {
        _last_dance_ms = now;
        _sway_side     = -_sway_side;
        int amp_x      = static_cast<int>(40 + level * 70);  // 4..11 deg
        int amp_y      = static_cast<int>(25 + level * 65);  // 2.5..9 deg
        int base_pitch = static_cast<int>(std::lround((static_cast<float>(_y_home_deg) / 90.0f) * 900.0f));
        int yaw        = _sway_side * amp_x;
        int pitch      = std::clamp(base_pitch + ((_sway_side > 0) ? amp_y : -amp_y / 2), 0, 900);
        GetStackChan().motion().moveWithSpeed(yaw, pitch, 420);
    }
}

std::string AppSpeaker::build_status_json() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", _input_mode == InputMode::Wifi ? "wifi" : "usb");
    cJSON_AddNumberToObject(root, "vol", _volume);
    cJSON_AddBoolToObject(root, "muted", _muted);
    cJSON_AddBoolToObject(root, "move", _robot_move);
    cJSON_AddBoolToObject(root, "lights", _lights_on);
    char* s = cJSON_PrintUnformatted(root);
    std::string out(s ? s : "{}");
    if (s) {
        cJSON_free(s);
    }
    cJSON_Delete(root);
    return out;
}

void AppSpeaker::process_remote_cmds()
{
    int pv = _pending_vol.exchange(-1);
    if (pv >= 0) {
        set_volume(pv);
    }
    int vd = _pending_vol_delta.exchange(0);
    if (vd != 0) {
        change_volume(vd);
    }
    int pm = _pending_mute.exchange(-1);
    if (pm >= 0) {
        _muted = (pm != 0);
        LvglLockGuard lock;
        refresh_bottom_bar();
        refresh_mode_bar();
    }
    if (_pending_toggle_mute.exchange(false)) {
        toggle_mute();
    }
    if (_pending_toggle_move.exchange(false)) {
        toggle_move();
    }
    if (_pending_toggle_lights.exchange(false)) {
        toggle_lights();
    }
}

/* -------------------------------------------------------------------------- */
/*                                 LVGL UI                                    */
/* -------------------------------------------------------------------------- */

void AppSpeaker::build_ui()
{
    lv_obj_t* scr = lv_screen_active();
    _root         = lv_obj_create(scr);
    lv_obj_set_size(_root, W, H);
    style_box(_root, COL_BG, 0);

    const int top_y = view::get_status_bar_top_offset();
    const bool fix  = (top_y > 0);

    /* =========================== MAIN VIEW ================================ */
    _main_cont = lv_obj_create(_root);
    lv_obj_set_size(_main_cont, W, H);
    style_box(_main_cont, COL_BG, 0);

    // 1. Mode Switcher Bar: [ WI-FI ] [ USB-C ]  [ STATE BADGE ]
    const int mode_y = top_y + 4;
    const int mode_h = 28;

    _btn_mode_wifi = make_btn(_main_cont, 8, mode_y, 88, mode_h, "WI-FI", COL_TEAL_DK, COL_WHITE, &lv_font_montserrat_12);
    _lbl_mode_wifi = lv_obj_get_child(_btn_mode_wifi, 0);
    lv_obj_add_event_cb(
        _btn_mode_wifi,
        [](lv_event_t* e) {
            auto* self = static_cast<AppSpeaker*>(lv_event_get_user_data(e));
            self->switch_input_mode(InputMode::Wifi);
        },
        LV_EVENT_CLICKED, this);

    _btn_mode_usb = make_btn(_main_cont, 102, mode_y, 88, mode_h, "USB-C", COL_CARD, COL_DIM, &lv_font_montserrat_12);
    _lbl_mode_usb = lv_obj_get_child(_btn_mode_usb, 0);
    lv_obj_add_event_cb(
        _btn_mode_usb,
        [](lv_event_t* e) {
            auto* self = static_cast<AppSpeaker*>(lv_event_get_user_data(e));
            self->switch_input_mode(InputMode::Usb);
        },
        LV_EVENT_CLICKED, this);

    _badge_state = lv_obj_create(_main_cont);
    lv_obj_set_pos(_badge_state, 200, mode_y);
    lv_obj_set_size(_badge_state, 112, mode_h);
    style_box(_badge_state, COL_CARD, 14);
    _lbl_badge = lv_label_create(_badge_state);
    lv_obj_center(_lbl_badge);
    lv_obj_set_style_text_font(_lbl_badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_badge, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(_lbl_badge, "READY");

    // 2. Center Stage Card: Avatar (tap to switch style) + 8-bar FFT EQ
    const int stage_y = mode_y + mode_h + 6;
    const int stage_h = (BAR4_Y - 22) - stage_y;

    _stage_card = lv_obj_create(_main_cont);
    lv_obj_set_pos(_stage_card, 8, stage_y);
    lv_obj_set_size(_stage_card, 304, stage_h);
    style_box(_stage_card, COL_CARD, 14);

    // Classic StackChan Face (dynamic eyes + FFT lip-sync mouth)
    const int face_h = fix ? 64 : 84;
    _face_box        = lv_obj_create(_stage_card);
    lv_obj_set_size(_face_box, 136, face_h);
    lv_obj_align(_face_box, LV_ALIGN_TOP_MID, 0, fix ? 6 : 12);
    style_box(_face_box, COL_BG, 16);
    lv_obj_remove_flag(_face_box, LV_OBJ_FLAG_CLICKABLE);

    _eye_l = lv_obj_create(_face_box);
    lv_obj_set_size(_eye_l, 14, 14);
    lv_obj_align(_eye_l, LV_ALIGN_TOP_LEFT, 30, fix ? 14 : 20);
    style_box(_eye_l, COL_WHITE, 7);

    _eye_r = lv_obj_create(_face_box);
    lv_obj_set_size(_eye_r, 14, 14);
    lv_obj_align(_eye_r, LV_ALIGN_TOP_RIGHT, -30, fix ? 14 : 20);
    style_box(_eye_r, COL_WHITE, 7);

    _mouth = lv_obj_create(_face_box);
    lv_obj_set_size(_mouth, 36, 5);
    lv_obj_align(_mouth, LV_ALIGN_BOTTOM_MID, 0, fix ? -12 : -16);
    style_box(_mouth, COL_TEAL, 4);

    // Optional Custom Avatar Avatar Sprite
    _avatar_img = lv_image_create(_stage_card);
    if (_img_avatar_calm.data != nullptr) {
        lv_image_set_src(_avatar_img, &_img_avatar_calm);
        int max_h = fix ? 66 : 86;
        if (_img_avatar_calm.header.h > max_h) {
            uint16_t sc = static_cast<uint16_t>((max_h * 256) / _img_avatar_calm.header.h);
            lv_image_set_scale(_avatar_img, sc);
        }
    }
    lv_obj_align(_avatar_img, LV_ALIGN_TOP_MID, 0, fix ? 4 : 8);
    lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);

    // 8-bar live FFT Equalizer along the bottom of the stage card
    constexpr int kEqBarW = 24;
    constexpr int kEqGap  = 10;
    constexpr int kEqX0   = (304 - (8 * kEqBarW + 7 * kEqGap)) / 2;
    for (int i = 0; i < 8; ++i) {
        _eq_objs[i] = lv_obj_create(_stage_card);
        lv_obj_set_size(_eq_objs[i], kEqBarW, 4);
        lv_obj_align(_eq_objs[i], LV_ALIGN_BOTTOM_LEFT, kEqX0 + i * (kEqBarW + kEqGap), -6);
        style_box(_eq_objs[i], COL_TEAL, 3);
        lv_obj_remove_flag(_eq_objs[i], LV_OBJ_FLAG_CLICKABLE);
    }

    // Connection / IP / USB hint line
    _lbl_conn_hint = lv_label_create(_main_cont);
    lv_obj_set_pos(_lbl_conn_hint, 10, BAR4_Y - 18);
    lv_obj_set_size(_lbl_conn_hint, 300, 16);
    lv_obj_set_style_text_align(_lbl_conn_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_lbl_conn_hint, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_conn_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_conn_hint, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(_lbl_conn_hint, "Connecting Wi-Fi...");

    // 3. Bottom 4-Button Control Bar: MUTE | DANCE | LIGHTS | QR/INFO
    auto make_bar_btn = [&](int idx, const char* txt, lv_obj_t*& out_lbl) {
        int x       = BAR4_X0 + idx * (BAR4_W + BAR4_GAP);
        lv_obj_t* b = make_btn(_main_cont, x, BAR4_Y, BAR4_W, BAR4_H, txt, COL_CARD, COL_TEXT, &lv_font_montserrat_12);
        out_lbl     = lv_obj_get_child(b, 0);
        return b;
    };

    _btn_mute = make_bar_btn(0, "MUTE", _lbl_mute);
    lv_obj_add_event_cb(
        _btn_mute,
        [](lv_event_t* e) { static_cast<AppSpeaker*>(lv_event_get_user_data(e))->toggle_mute(); },
        LV_EVENT_CLICKED, this);

    _btn_move = make_bar_btn(1, "DANCE", _lbl_move);
    lv_obj_add_event_cb(
        _btn_move,
        [](lv_event_t* e) { static_cast<AppSpeaker*>(lv_event_get_user_data(e))->toggle_move(); },
        LV_EVENT_CLICKED, this);

    _btn_lights = make_bar_btn(2, "LIGHTS", _lbl_lights);
    lv_obj_add_event_cb(
        _btn_lights,
        [](lv_event_t* e) { static_cast<AppSpeaker*>(lv_event_get_user_data(e))->toggle_lights(); },
        LV_EVENT_CLICKED, this);

    _btn_info = make_bar_btn(3, "QR / INFO", _lbl_info);
    lv_obj_add_event_cb(
        _btn_info,
        [](lv_event_t* e) { static_cast<AppSpeaker*>(lv_event_get_user_data(e))->switch_view(ViewMode::Info); },
        LV_EVENT_CLICKED, this);

    /* =========================== INFO / QR VIEW =========================== */
    _info_cont = lv_obj_create(_root);
    lv_obj_set_size(_info_cont, W, H);
    style_box(_info_cont, COL_BG, 0);
    lv_obj_add_flag(_info_cont, LV_OBJ_FLAG_HIDDEN);

    _lbl_info_hdr = lv_label_create(_info_cont);
    lv_obj_set_pos(_lbl_info_hdr, 10, top_y + 6);
    lv_obj_set_size(_lbl_info_hdr, 240, 22);
    lv_obj_set_style_text_font(_lbl_info_hdr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_info_hdr, lv_color_hex(COL_TEAL), 0);
    lv_label_set_text(_lbl_info_hdr, "< CONNECTION");
    lv_obj_add_flag(_lbl_info_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        _lbl_info_hdr,
        [](lv_event_t* e) { static_cast<AppSpeaker*>(lv_event_get_user_data(e))->switch_view(ViewMode::Main); },
        LV_EVENT_CLICKED, this);

    // Left white box for QR (Wi-Fi mode)
    const int qr_sz = fix ? 116 : 132;
    _qr_box         = lv_obj_create(_info_cont);
    lv_obj_set_pos(_qr_box, 12, top_y + 34);
    lv_obj_set_size(_qr_box, qr_sz + 10, qr_sz + 10);
    style_box(_qr_box, COL_WHITE, 10);
    _qr_obj = lv_qrcode_create(_qr_box);
    lv_qrcode_set_size(_qr_obj, qr_sz);
    lv_qrcode_set_dark_color(_qr_obj, lv_color_hex(0x0B0F1A));
    lv_qrcode_set_light_color(_qr_obj, lv_color_hex(0xFFFFFF));
    lv_obj_center(_qr_obj);

    // Right info text
    _lbl_info_title = lv_label_create(_info_cont);
    lv_obj_set_pos(_lbl_info_title, 152, top_y + 36);
    lv_obj_set_width(_lbl_info_title, 158);
    lv_obj_set_style_text_font(_lbl_info_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_info_title, lv_color_hex(COL_WHITE), 0);

    _lbl_info_body = lv_label_create(_info_cont);
    lv_obj_set_pos(_lbl_info_body, 152, top_y + 60);
    lv_obj_set_width(_lbl_info_body, 158);
    lv_label_set_long_mode(_lbl_info_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_info_body, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_info_body, lv_color_hex(COL_DIM), 0);

    _lbl_info_foot = lv_label_create(_info_cont);
    lv_obj_set_pos(_lbl_info_foot, 12, 212);
    lv_obj_set_width(_lbl_info_foot, 296);
    lv_obj_set_style_text_align(_lbl_info_foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_lbl_info_foot, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_info_foot, lv_color_hex(COL_TEAL), 0);
}

void AppSpeaker::destroy_ui()
{
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
    _main_cont = _info_cont = nullptr;
    _btn_mode_wifi = _lbl_mode_wifi = _btn_mode_usb = _lbl_mode_usb = nullptr;
    _badge_state = _lbl_badge = _stage_card = _avatar_img = nullptr;
    _face_box = _eye_l = _eye_r = _mouth = _lbl_conn_hint = nullptr;
    _btn_mute = _lbl_mute = _btn_move = _lbl_move = nullptr;
    _btn_lights = _lbl_lights = _btn_info = _lbl_info = nullptr;
    _lbl_info_hdr = _qr_box = _qr_obj = nullptr;
    _lbl_info_title = _lbl_info_body = _lbl_info_foot = nullptr;
    for (auto*& o : _eq_objs) {
        o = nullptr;
    }
}

void AppSpeaker::switch_view(ViewMode view)
{
    _view_mode = view;
    if (!_main_cont || !_info_cont) {
        return;
    }
    if (view == ViewMode::Main) {
        lv_obj_remove_flag(_main_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_info_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_main_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_info_cont, LV_OBJ_FLAG_HIDDEN);
        refresh_info_view();
    }
}

void AppSpeaker::refresh_mode_bar()
{
    if (!_btn_mode_wifi || !_btn_mode_usb) {
        return;
    }
    bool is_wifi = (_input_mode == InputMode::Wifi);
    lv_obj_set_style_bg_color(_btn_mode_wifi, lv_color_hex(is_wifi ? COL_TEAL_DK : COL_CARD), 0);
    lv_obj_set_style_text_color(_lbl_mode_wifi, lv_color_hex(is_wifi ? COL_WHITE : COL_DIM), 0);
    lv_obj_set_style_bg_color(_btn_mode_usb, lv_color_hex(!is_wifi ? COL_BLUE : COL_CARD), 0);
    lv_obj_set_style_text_color(_lbl_mode_usb, lv_color_hex(!is_wifi ? COL_WHITE : COL_DIM), 0);

    uint32_t accent = is_wifi ? COL_TEAL : COL_BLUE;
    if (_mouth) {
        lv_obj_set_style_bg_color(_mouth, lv_color_hex(accent), 0);
    }
    for (auto* bar : _eq_objs) {
        if (bar) {
            lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0);
        }
    }
}

void AppSpeaker::refresh_bottom_bar()
{
    if (!_btn_mute || !_btn_move || !_btn_lights) {
        return;
    }
    lv_obj_set_style_bg_color(_btn_mute, lv_color_hex(_muted ? COL_RED : COL_CARD), 0);
    lv_obj_set_style_text_color(_lbl_mute, lv_color_hex(_muted ? COL_WHITE : COL_TEXT), 0);
    lv_label_set_text(_lbl_mute, _muted ? "MUTED" : "MUTE");

    uint32_t on_col = (_input_mode == InputMode::Wifi) ? COL_TEAL_DK : COL_BLUE;
    lv_obj_set_style_bg_color(_btn_move, lv_color_hex(_robot_move ? on_col : COL_CARD), 0);
    lv_obj_set_style_text_color(_lbl_move, lv_color_hex(_robot_move ? COL_WHITE : COL_DIM), 0);
    lv_label_set_text(_lbl_move, _robot_move ? "DANCE\nON" : "DANCE\nOFF");

    lv_obj_set_style_bg_color(_btn_lights, lv_color_hex(_lights_on ? on_col : COL_CARD), 0);
    lv_obj_set_style_text_color(_lbl_lights, lv_color_hex(_lights_on ? COL_WHITE : COL_DIM), 0);
    lv_label_set_text(_lbl_lights, _lights_on ? "LIGHTS\nON" : "LIGHTS\nOFF");

    lv_obj_set_style_bg_color(_btn_info, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_set_style_text_color(_lbl_info, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(_lbl_info, (_input_mode == InputMode::Wifi) ? "QR\nWEB" : "USB\nINFO");
}

void AppSpeaker::refresh_avatar_and_eq(uint32_t now)
{
    if (!_face_box || !_avatar_img) {
        return;
    }
    bool active      = (_last_sound_ms.load() != 0) && ((now - _last_sound_ms.load()) < kActiveTimeoutMs) && !_muted;
    float mr         = active ? _mouth_ratio.load() : 0.0f;
    bool show_custom_avatar = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Custom &&
                        _img_avatar_calm.data != nullptr);

    if (show_custom_avatar) {
        lv_obj_add_flag(_face_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
        bool happy = active && (mr > 0.22f);
        if (happy != _avatar_happy && _img_avatar_happy.data != nullptr) {
            _avatar_happy = happy;
            lv_image_set_src(_avatar_img, happy ? &_img_avatar_happy : &_img_avatar_calm);
        }
    } else {
        lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_face_box, LV_OBJ_FLAG_HIDDEN);

        // Periodic natural blink
        if (now - _last_blink_ms > (_eyes_closed ? 140U : 3400U)) {
            _last_blink_ms = now;
            _eyes_closed   = !_eyes_closed;
        }
        int eye_h = _eyes_closed ? 3 : (active && mr > 0.45f ? 10 : 14);
        lv_obj_set_height(_eye_l, eye_h);
        lv_obj_set_height(_eye_r, eye_h);

        // Smooth FFT-driven lip-sync mouth
        int mh = std::clamp(static_cast<int>(5 + mr * 20.0f), 4, 24);
        int mw = std::clamp(static_cast<int>(34 + mr * 16.0f), 28, 52);
        lv_obj_set_size(_mouth, mw, mh);
        lv_obj_set_style_radius(_mouth, mh / 2, 0);
    }

    // 8-bar FFT Equalizer
    constexpr int kMaxEqH = 26;
    for (int i = 0; i < 8; ++i) {
        if (!_eq_objs[i]) {
            continue;
        }
        uint8_t pct = _eq_bars[i].load();
        int h       = std::clamp(3 + (pct * (kMaxEqH - 3)) / 100, 3, kMaxEqH);
        lv_obj_set_height(_eq_objs[i], h);
    }
}

void AppSpeaker::refresh_info_view()
{
    if (!_info_cont || !_qr_box || !_lbl_info_title || !_lbl_info_body || !_lbl_info_foot) {
        return;
    }

    if (_input_mode == InputMode::Wifi) {
        lv_obj_remove_flag(_qr_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(_lbl_info_title, 152, view::get_status_bar_top_offset() + 36);
        lv_obj_set_pos(_lbl_info_body, 152, view::get_status_bar_top_offset() + 60);
        lv_obj_set_width(_lbl_info_title, 158);
        lv_obj_set_width(_lbl_info_body, 158);

        std::string ip = _wifi_svc ? _wifi_svc->get_ip_address() : "";
        if (!ip.empty()) {
            std::string url = "http://" + ip + "/";
            if (url != _last_qr_url) {
                _last_qr_url = url;
                lv_qrcode_update(_qr_obj, url.c_str(), url.size());
            }
            lv_label_set_text(_lbl_info_title, "WI-FI SPEAKER");
            lv_label_set_text(_lbl_info_body,
                              "Scan QR to stream:\n- PC / Tab Audio\n- Live Mic\n- Audio Files\n\nUDP PCM: port 8765");
            lv_label_set_text(_lbl_info_foot, url.c_str());
        } else {
            lv_label_set_text(_lbl_info_title, "NO WI-FI");
            lv_label_set_text(_lbl_info_body, "Connect StackChan to Wi-Fi in SETUP to enable Web & UDP audio.");
            lv_label_set_text(_lbl_info_foot, _wifi_svc ? _wifi_svc->get_status_text().c_str() : "Offline");
        }
    } else {
        lv_obj_add_flag(_qr_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(_lbl_info_title, 16, view::get_status_bar_top_offset() + 38);
        lv_obj_set_pos(_lbl_info_body, 16, view::get_status_bar_top_offset() + 64);
        lv_obj_set_width(_lbl_info_title, 288);
        lv_obj_set_width(_lbl_info_body, 288);

        lv_label_set_text(_lbl_info_title, "USB-C OTG SPEAKER (UAC2)");
        lv_label_set_text(_lbl_info_body,
                          "1. Plug USB-C into PC or phone.\n"
                          "2. Select \"StackChan Speaker\" as audio output (48 kHz stereo).\n"
                          "3. Audio plays live with lip-sync & dance.\n\n"
                          "Note: USB-Serial monitor pauses while USB Audio is active.");
        char foot[64];
        std::snprintf(foot, sizeof(foot), "Status: %s  |  Pkts: %lu",
                      _usb_attached.load() ? "USB READY" : "INIT ERR",
                      static_cast<unsigned long>(_rx_packets.load()));
        lv_label_set_text(_lbl_info_foot, foot);
    }
}

void AppSpeaker::refresh_ui(uint32_t now)
{
    if (!_main_cont) {
        return;
    }

    if (_view_mode == ViewMode::Info) {
        refresh_info_view();
        return;
    }

    bool active = (_last_sound_ms.load() != 0) && ((now - _last_sound_ms.load()) < kActiveTimeoutMs);

    // Update top status badge
    if (_badge_state && _lbl_badge) {
        if (_muted) {
            lv_obj_set_style_bg_color(_badge_state, lv_color_hex(COL_RED), 0);
            lv_obj_set_style_text_color(_lbl_badge, lv_color_hex(COL_WHITE), 0);
            lv_label_set_text(_lbl_badge, "MUTED");
        } else if (active) {
            uint32_t col = (_input_mode == InputMode::Wifi) ? COL_TEAL_DK : COL_BLUE;
            lv_obj_set_style_bg_color(_badge_state, lv_color_hex(col), 0);
            lv_obj_set_style_text_color(_lbl_badge, lv_color_hex(COL_WHITE), 0);
            lv_label_set_text(_lbl_badge, (_input_mode == InputMode::Wifi) ? "WI-FI LIVE" : "USB LIVE");
        } else {
            lv_obj_set_style_bg_color(_badge_state, lv_color_hex(COL_CARD), 0);
            lv_obj_set_style_text_color(_lbl_badge, lv_color_hex(COL_DIM), 0);
            if (_input_mode == InputMode::Wifi) {
                bool ok = _wifi_svc && _wifi_svc->is_web_running();
                lv_label_set_text(_lbl_badge, ok ? "WI-FI READY" : "CONNECTING");
            } else {
                lv_label_set_text(_lbl_badge, _usb_attached.load() ? "USB READY" : "USB ERR");
            }
        }
    }

    // Connection hint line
    if (_lbl_conn_hint) {
        if (_input_mode == InputMode::Wifi) {
            std::string ip = _wifi_svc ? _wifi_svc->get_ip_address() : "";
            if (!ip.empty()) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "http://%s  |  UDP:%u  |  VOL %d%%",
                              ip.c_str(), SpeakerWifiService::kUdpPort, _volume);
                lv_label_set_text(_lbl_conn_hint, buf);
            } else {
                lv_label_set_text(_lbl_conn_hint, _wifi_svc ? _wifi_svc->get_status_text().c_str() : "Wi-Fi idle");
            }
        } else {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "USB-C: StackChan Speaker 48kHz  |  VOL %d%%", _volume);
            lv_label_set_text(_lbl_conn_hint, buf);
        }
    }

    refresh_avatar_and_eq(now);
}
