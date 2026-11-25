/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_music_player.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

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
#include <settings.h>

#include <cJSON.h>
#include <esp_aac_dec.h>
#include <esp_heap_caps.h>
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
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <sys/stat.h>

using namespace mooncake;
using namespace stackchan;

LV_IMAGE_DECLARE(icon_music);

namespace {

constexpr const char* TAG        = "MUSIC";
constexpr const char* APP_VERSION = "v4.1";
constexpr const char* TRACK_DIR  = "/sd";

// NVS keys (single namespace for all player preferences and last track)
constexpr const char* NVS_NS         = "music_player";
constexpr const char* KEY_Y_HOME     = "y_home";
constexpr const char* KEY_VOLUME     = "volume";
constexpr const char* KEY_LOOP_MODE  = "loop_mode";
constexpr const char* KEY_SHUFFLE    = "shuffle";
constexpr const char* KEY_LIGHTS     = "lights";
constexpr const char* KEY_MOVE       = "move";
constexpr const char* KEY_LAST_IDX   = "last_idx";
constexpr const char* KEY_LAST_PATH  = "last_path";

constexpr int W                  = 320;
constexpr int H                  = 240;
constexpr uint32_t BEAT_MS       = 600;
constexpr int VOLUME_STEP        = 10;
constexpr int X_HOME_DEG         = 0;
constexpr int Y_HOME_DEFAULT_DEG = 45;
constexpr int X_SWAY_DEG         = 6;
constexpr int Y_SWAY_DEG         = 5;
constexpr int SWAY_STILL_PCT     = 15;
constexpr int HEAD_STEP_DEG      = 5;

constexpr int MAX_TRACKS         = 2000;
constexpr int PAGE_ROWS          = 5;
constexpr int MAX_SCAN_DEPTH     = 3;

// Dark UI color palette
constexpr uint32_t COL_BG        = 0x0B0F1A;
constexpr uint32_t COL_CARD      = 0x151B2E;
constexpr uint32_t COL_CARD_HI   = 0x232C4A;
constexpr uint32_t COL_ACCENT    = 0x3D7DFF;
constexpr uint32_t COL_ACCENT_DK = 0x24417F;
constexpr uint32_t COL_YELLOW    = 0xFFC940;
constexpr uint32_t COL_TEXT      = 0xE8EEF9;
constexpr uint32_t COL_DIM       = 0x7C89A8;
constexpr uint32_t COL_WHITE     = 0xFFFFFF;
constexpr uint32_t COL_RED       = 0xE74C3C;
constexpr uint32_t COL_GREEN     = 0x34C759;
constexpr uint32_t COL_PLAY_ON   = 0x1F6B3A;
constexpr uint32_t COL_PURPLE    = 0x7B2CBF;

constexpr uint32_t PALETTE[8] = {
    0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759,
    0x00C7BE, 0x007AFF, 0x5856D6, 0xAF52DE,
};

// Top status/progress geometry (relative to dynamic top offset)
constexpr int BAR_X    = 8;
constexpr int BAR_W    = 304;
constexpr int BAR_H    = 6;

// Center stage area
constexpr int STAGE_X  = 8;
constexpr int STAGE_W  = 304;

// Bottom 4-button control bar (y = 192 .. 234): PREV | PLAY/PAUSE | NEXT | SETTINGS
// (Volume -/+ and Home are integrated in the 38px top status bar)
constexpr int BAR4_Y   = 192;
constexpr int BAR4_H   = 42;
constexpr int BAR4_W   = 71;
constexpr int BAR4_GAP = 6;
constexpr int BAR4_X0  = 9; // 9 + 4*71 + 3*6 = 311 <= 320

const char* const LOOP_NAMES[3] = {"OFF", "1", "ALL"};

const char* const MAIN_BTN_NAMES[4] = {
    "prev", "play", "next", "settings",
};

struct CfgBtnSpec {
    const char* name;
    int x;
    int y;
    int w;
    int h;
};

// Settings screen layout:
// Row 1: LOOP | SHUFFLE | LIGHTS | MOVE
// Row 2: SD LIST | RADIO | WEB/QR | HEAD
// Footer: AVATAR (Classic/Naruto toggle) + BACK (Exit is in the top status bar)
constexpr CfgBtnSpec CFG_BTN_SPECS[9] = {
    {"loop",     8,   66,  73,  54},
    {"shuf",     85,  66,  73,  54},
    {"lights",   162, 66,  73,  54},
    {"move",     239, 66,  73,  54},
    {"playlist", 8,   126, 73,  54},
    {"radio",    85,  126, 73,  54},
    {"web",      162, 126, 73,  54},
    {"head",     239, 126, 73,  54},
    {"avatar",   8,   188, 148, 44},
};

uint16_t read_u16_le(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read_u32_le(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

int py_y_deg_to_pitch(int y_deg)
{
    return (45 - y_deg) * 10;
}

int pitch_to_py_y_deg(int pitch_tenths)
{
    int deg = 45 - (pitch_tenths / 10);
    return std::clamp(deg, 0, 90);
}

int py_x_deg_to_yaw(int x_deg)
{
    return x_deg * 10;
}

void style_flat_box(lv_obj_t* obj, uint32_t bg_hex, int radius = 0)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                           Mooncake Lifecycle                               */
/* -------------------------------------------------------------------------- */

AppMusicPlayer::AppMusicPlayer()
{
    setAppInfo().name = "MUSIC";

    static lv_image_dsc_t icon_from_assets = assets::get_image("icon_music.bin");
    if (icon_from_assets.data != nullptr && icon_from_assets.data_size > 0) {
        setAppInfo().icon = (void*)&icon_from_assets;
    } else {
        setAppInfo().icon = (void*)&icon_music;
    }

    static uint32_t theme_color = 0x3D7DFF;
    setAppInfo().userData       = (void*)&theme_color;
}

AppMusicPlayer::~AppMusicPlayer()
{
    stop_audio_task();
    if (_wifi_svc) {
        _wifi_svc->shutdown();
        _wifi_svc.reset();
    }
    unmount_sd();
}

void AppMusicPlayer::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppMusicPlayer::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _mode                 = ScreenMode::Main;
    _status_msg.clear();
    _audio_err.clear();
    _last_beat            = 0xFFFFFFFF;
    _playing              = false;
    _started              = false;
    _radio_mode           = false;
    _sd_upload_active     = false;
    _track_eof            = false;
    _cmd_reload           = false;
    _cmd_close_file       = false;
    _audio_file_open      = false;
    _elapsed_ms           = 0;
    _duration_ms          = 0;
    _byte_rate            = 0;
    _decoded_bytes        = 0;
    _avatar_showing_happy = false;
    _last_qr_url.clear();

    // 1. Load saved preferences and last played track from NVS
    load_saved_prefs();
    GetHAL().setSpeakerVolume(static_cast<uint8_t>(_volume), false);

    // 2. Initialize servos and LEDs
    GetHAL().setServoPowerEnabled(true);
    rgb_off();
    auto& stackchan = GetStackChan();
    stackchan.clearModifiers();
    stackchan.motion().setAutoAngleSyncEnabled(false);
    stackchan.motion().setAutoTorqueReleaseEnabled(false);
    move_home(600);

    // 3. Load Naruto sprites from the assets partition
    _img_naruto_calm  = assets::get_image("naruto_calm_music.bin");
    _img_naruto_happy = assets::get_image("naruto_happy_music.bin");

    // 4. Mount microSD card and scan tracks (.wav and .mp3)
    _sd_mounted = mount_sd();
    scan_tracks();

    // 4. Restore last played track (by exact path, falling back to last index)
    if (!_tracks.empty()) {
        int restored = -1;
        if (!_saved_track_path.empty()) {
            for (int i = 0; i < static_cast<int>(_tracks.size()); ++i) {
                if (_tracks[i].path.size() == _saved_track_path.size() &&
                    std::memcmp(_tracks[i].path.c_str(), _saved_track_path.c_str(), _saved_track_path.size()) == 0) {
                    restored = i;
                    break;
                }
            }
        }
        if (restored < 0 && _saved_track_idx >= 0 && _saved_track_idx < static_cast<int>(_tracks.size())) {
            restored = _saved_track_idx;
        }
        _index     = (restored >= 0) ? restored : 0;
        _list_page = _index / PAGE_ROWS;

        auto info    = probe_audio(std::string(_tracks[_index].path.c_str()), _tracks[_index].is_mp3);
        _duration_ms = info.duration_ms;
        _byte_rate   = info.byte_rate;
    }
    update_now_playing_title();

    // 4b. Initialize companion Wi-Fi Radio & Web Remote service (lazy Wi-Fi connect)
    init_wifi_service();

    // 5. Start audio decoding task (stack in PSRAM)
    start_audio_task();

    // 6. Build LVGL UI
    {
        LvglLockGuard lock;
        build_ui();
        uint32_t now = GetHAL().millis();
        refresh_main_ui(now);
        refresh_avatar_anim(now);

        view::create_home_indicator([&]() { close(); }, 0x3D7DFF, 0x0B0F1A);
        view::create_status_bar(0x3D7DFF, 0x0B0F1A);
    }
}

void AppMusicPlayer::onRunning()
{
    uint32_t now = GetHAL().millis();

    process_web_commands();

    if (!_status_msg.empty() && static_cast<int32_t>(now - _status_until) >= 0) {
        _status_msg.clear();
        LvglLockGuard lock;
        refresh_main_ui(now);
    }

    if (_track_eof.exchange(false)) {
        track_finished();
        LvglLockGuard lock;
        refresh_main_ui(now);
        if (_mode == ScreenMode::List) {
            refresh_list_ui();
        } else if (_mode == ScreenMode::Radio) {
            refresh_radio_ui();
        }
    }

    beat_update(now);
    GetStackChan().update();

    {
        LvglLockGuard lock;
        int hal_vol = static_cast<int>(GetHAL().getSpeakerVolume());
        if (hal_vol != _volume) {
            _volume = hal_vol;
            save_settings();
            refresh_main_ui(now);
        }

        if (_mode == ScreenMode::Main) {
            if (now - _last_anim_tick >= 120) {
                _last_anim_tick = now;
                refresh_avatar_anim(now);
            }
            if (now - _last_time_tick >= 500) {
                _last_time_tick = now;
                refresh_main_ui(now);
            }
            if (now - _last_batt_tick >= 5000) {
                _last_batt_tick = now;
                refresh_battery_ui();
            }
        } else if (_mode == ScreenMode::Head) {
            if (now - _last_time_tick >= 300) {
                _last_time_tick = now;
                refresh_head_ui();
            }
        } else if (_mode == ScreenMode::Radio) {
            if (now - _last_time_tick >= 600) {
                _last_time_tick = now;
                refresh_radio_ui();
            }
        } else if (_mode == ScreenMode::WebQr) {
            if (now - _last_time_tick >= 500) {
                _last_time_tick = now;
                refresh_web_qr_ui();
            }
        }

        view::update_home_indicator();
        view::update_status_bar();
    }
}

void AppMusicPlayer::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    save_settings();
    save_last_track();

    stop_playback();
    stop_audio_task();
    if (_wifi_svc) {
        _wifi_svc->shutdown();
        _wifi_svc.reset();
    }

    move_home(500);
    rgb_off();

    {
        LvglLockGuard lock;
        view::destroy_home_indicator();
        view::destroy_status_bar();
        destroy_ui();
    }

    unmount_sd();

    GetHAL().requestWarmReboot(6);
}

/* -------------------------------------------------------------------------- */
/*                               microSD Card                                 */
/* -------------------------------------------------------------------------- */

bool AppMusicPlayer::mount_sd()
{
    struct stat st = {};
    if (stat(TRACK_DIR, &st) == 0) {
        _sd_mounted = true;
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed           = false;
    mount_config.max_files                        = 5;
    mount_config.allocation_unit_size             = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SPI3_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = GPIO_NUM_4;
    slot_config.host_id               = SPI3_HOST;

    sdmmc_card_t* card = nullptr;
    esp_err_t ret      = esp_vfs_fat_sdspi_mount(TRACK_DIR, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        mclog::tagWarn(TAG, "SD mount failed on SPI3_HOST: {}", esp_err_to_name(ret));
        _scan_first_err = esp_err_to_name(ret);
        _sd_mounted     = false;
        _sd_card_handle = nullptr;
        return false;
    }

    _sd_card_handle = card;
    _sd_mounted     = true;
    mclog::tagInfo(TAG, "SD mounted at /sd");
    return true;
}

void AppMusicPlayer::unmount_sd()
{
    if (_sd_card_handle != nullptr) {
        esp_vfs_fat_sdcard_unmount(TRACK_DIR, static_cast<sdmmc_card_t*>(_sd_card_handle));
        _sd_card_handle = nullptr;
    }
    _sd_mounted = false;
}

bool AppMusicPlayer::is_audio_name(const std::string& name)
{
    if (name.size() <= 4 || name[0] == '.') {
        return false;
    }
    std::string ext = ext_of(name);
    return ext == ".wav" || ext == ".mp3";
}

std::string AppMusicPlayer::ext_of(const std::string& name)
{
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return "";
    }
    std::string ext = name.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

std::string AppMusicPlayer::clip_text(const std::string& s, size_t max_chars)
{
    if (s.size() <= max_chars) {
        return s;
    }
    return s.substr(0, max_chars);
}

std::string AppMusicPlayer::base_title(const std::string& path)
{
    auto slash       = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (name.size() > 4) {
        std::string ext = ext_of(name);
        if (ext == ".wav" || ext == ".mp3") {
            name = name.substr(0, name.size() - 4);
        }
    }
    for (char& c : name) {
        if (c == '_') {
            c = ' ';
        }
    }
    return clip_text(name, 24);
}

std::string AppMusicPlayer::fmt_time(int32_t ms)
{
    if (ms < 0) {
        ms = 0;
    }
    int32_t s = ms / 1000;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", static_cast<int>(s / 60), static_cast<int>(s % 60));
    return std::string(buf);
}

void AppMusicPlayer::scan_tracks()
{
    std::vector<TrackEntry, PsramAllocator<TrackEntry>> found;
    _scan_error   = false;
    _scan_entries = 0;
    _scan_other_ext.clear();
    _scan_other_n = 0;

    if (!_sd_mounted) {
        std::lock_guard<std::mutex> lk(_tracks_mutex);
        _tracks.clear();
        return;
    }

    std::map<std::string, int> exts;
    std::vector<std::pair<std::string, int>> stack;
    stack.push_back({TRACK_DIR, 0});

    while (!stack.empty() && static_cast<int>(found.size()) < MAX_TRACKS) {
        auto [dir_path, depth] = stack.back();
        stack.pop_back();

        DIR* dir = opendir(dir_path.c_str());
        if (!dir) {
            if (depth == 0) {
                _scan_error = true;
            }
            continue;
        }

        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            const char* dname = ent->d_name;
            if (!dname || dname[0] == '\0' || std::strcmp(dname, ".") == 0 || std::strcmp(dname, "..") == 0) {
                continue;
            }
            if (dname[0] == '.') {
                continue;
            }

            _scan_entries++;
            std::string name(dname);
            std::string full_path = dir_path + "/" + name;

            if (is_audio_name(name)) {
                if (static_cast<int>(found.size()) < MAX_TRACKS) {
                    TrackEntry te;
                    te.path.assign(full_path.c_str(), full_path.size());
                    std::string title_tmp = base_title(full_path);
                    te.title.assign(title_tmp.c_str(), title_tmp.size());
                    te.is_mp3 = (ext_of(name) == ".mp3");
                    found.push_back(std::move(te));
                }
                continue;
            }

            bool is_dir = (ent->d_type == DT_DIR);
            if (ent->d_type == DT_UNKNOWN) {
                struct stat st = {};
                if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    is_dir = true;
                }
            }

            if (is_dir) {
                if (depth < MAX_SCAN_DEPTH) {
                    stack.push_back({full_path, depth + 1});
                }
            } else {
                std::string e = ext_of(name);
                if (!e.empty() && e != ".mp3" && e != ".wav") {
                    exts[e]++;
                }
            }
        }
        closedir(dir);
    }

    std::sort(found.begin(), found.end(), [](const TrackEntry& a, const TrackEntry& b) {
        return a.path < b.path;
    });

    {
        std::lock_guard<std::mutex> lk(_tracks_mutex);
        _tracks.swap(found);
    }

    for (const auto& kv : exts) {
        if (kv.second > _scan_other_n) {
            _scan_other_n   = kv.second;
            _scan_other_ext = kv.first;
        }
    }

    mclog::tagInfo(TAG, "SD entries: {}, tracks: {}", _scan_entries, _tracks.size());
}

/* -------------------------------------------------------------------------- */
/*                         WAV / MP3 Probing & Audio                          */
/* -------------------------------------------------------------------------- */

AppMusicPlayer::AudioProbeInfo AppMusicPlayer::probe_wav(const std::string& path)
{
    AudioProbeInfo info;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return info;
    }

    uint8_t head[12];
    if (std::fread(head, 1, 12, f) < 12 || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0) {
        std::fclose(f);
        return info;
    }

    while (true) {
        uint8_t ch[8];
        if (std::fread(ch, 1, 8, f) < 8) {
            break;
        }
        uint32_t size = read_u32_le(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            uint8_t data[32] = {};
            size_t to_read   = std::min<size_t>(size, sizeof(data));
            if (std::fread(data, 1, to_read, f) >= 16) {
                info.format_tag  = read_u16_le(data + 0);
                info.channels    = read_u16_le(data + 2);
                info.sample_rate = read_u32_le(data + 4);
                info.byte_rate   = read_u32_le(data + 8);
                info.bits        = read_u16_le(data + 14);
            }
            if (size > to_read) {
                std::fseek(f, static_cast<long>(size - to_read), SEEK_CUR);
            }
            if (size % 2 != 0) {
                std::fseek(f, 1, SEEK_CUR);
            }
        } else if (std::memcmp(ch, "data", 4) == 0) {
            info.data_offset = static_cast<uint32_t>(std::ftell(f));
            info.data_size   = size;
            break;
        } else {
            std::fseek(f, static_cast<long>(size + (size % 2)), SEEK_CUR);
        }
    }

    std::fclose(f);

    if (info.byte_rate > 0 && info.data_size > 0 && info.format_tag == 1 && info.channels >= 1) {
        info.duration_ms = static_cast<uint32_t>((static_cast<uint64_t>(info.data_size) * 1000ULL) / info.byte_rate);
        info.valid       = true;
    }
    return info;
}

AppMusicPlayer::AudioProbeInfo AppMusicPlayer::probe_mp3(const std::string& path)
{
    AudioProbeInfo info;
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        return info;
    }
    uint32_t file_size = static_cast<uint32_t>(st.st_size);

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return info;
    }

    uint8_t head[10] = {};
    uint32_t start   = 0;
    if (std::fread(head, 1, 10, f) == 10 && std::memcmp(head, "ID3", 3) == 0) {
        uint32_t tag = ((head[6] & 0x7F) << 21) | ((head[7] & 0x7F) << 14) | ((head[8] & 0x7F) << 7) | (head[9] & 0x7F);
        start        = 10 + tag;
    }

    std::fseek(f, static_cast<long>(start), SEEK_SET);
    std::vector<uint8_t> chunk(4096);
    size_t nread = std::fread(chunk.data(), 1, chunk.size(), f);
    std::fclose(f);

    info.data_offset = start;
    info.data_size   = (file_size > start) ? (file_size - start) : file_size;
    info.valid       = true;

    for (size_t i = 0; i + 4 <= nread; ++i) {
        if (chunk[i] == 0xFF && (chunk[i + 1] & 0xE0) == 0xE0) {
            uint8_t b1     = chunk[i + 1];
            uint8_t b2     = chunk[i + 2];
            uint8_t ver    = (b1 >> 3) & 0x03;
            uint8_t layer  = (b1 >> 1) & 0x03;
            uint8_t br_idx = (b2 >> 4) & 0x0F;
            if (layer == 1 && br_idx > 0 && br_idx < 15) {
                static const int table_v1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
                static const int table_v2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
                int kbps                      = (ver == 3) ? table_v1[br_idx] : table_v2[br_idx];
                if (kbps > 0) {
                    info.byte_rate   = static_cast<uint32_t>(kbps * 125);
                    info.duration_ms = static_cast<uint32_t>((static_cast<uint64_t>(info.data_size) * 1000ULL) / info.byte_rate);
                    break;
                }
            }
        }
    }

    return info;
}

AppMusicPlayer::AudioProbeInfo AppMusicPlayer::probe_audio(const std::string& path, bool is_mp3)
{
    return is_mp3 ? probe_mp3(path) : probe_wav(path);
}

void AppMusicPlayer::start_audio_task()
{
    if (_task_running.load()) {
        return;
    }
    _task_running = true;
    _task_exited  = false;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(audio_task_entry, "music_audio", 32768, this, 5, nullptr, 0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        mclog::tagWarn(TAG, "PSRAM audio task failed, falling back to internal RAM (20KB)");
        ok = xTaskCreatePinnedToCore(audio_task_entry, "music_audio", 20480, this, 5, nullptr, 0);
    }
    if (ok != pdPASS) {
        mclog::tagError(TAG, "Failed to create audio task");
        _task_running = false;
        _task_exited  = true;
    }
}

void AppMusicPlayer::stop_audio_task()
{
    if (!_task_running.load()) {
        return;
    }
    _playing      = false;
    _task_running = false;
    for (int i = 0; i < 100 && !_task_exited.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void AppMusicPlayer::audio_task_entry(void* arg)
{
    static_cast<AppMusicPlayer*>(arg)->audio_task_loop();
    vTaskDelete(nullptr);
}

void AppMusicPlayer::audio_task_loop()
{
    auto& board      = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();

    FILE* file = nullptr;
    AudioProbeInfo probe;
    bool is_mp3          = false;
    bool is_radio        = false;
    bool radio_buffering = false;
    int radio_codec      = 0;  // 0 = auto-detect, 1 = MP3, 2 = AAC (ADTS)
    void* aac_dec        = nullptr;

    mp3dec_t* mp3d = static_cast<mp3dec_t*>(heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!mp3d) {
        mp3d = static_cast<mp3dec_t*>(std::malloc(sizeof(mp3dec_t)));
    }
    if (mp3d) {
        mp3dec_init(mp3d);
    }

    constexpr size_t MP3_BUF_SIZE  = 16384;
    constexpr size_t AAC_PCM_BYTES = 16384;
    uint8_t* mp3_buf = static_cast<uint8_t*>(heap_caps_malloc(MP3_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!mp3_buf) {
        mp3_buf = static_cast<uint8_t*>(std::malloc(MP3_BUF_SIZE));
    }
    int16_t* mp3_pcm = static_cast<int16_t*>(
        heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!mp3_pcm) {
        mp3_pcm = static_cast<int16_t*>(std::malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)));
    }
    int16_t* aac_pcm = static_cast<int16_t*>(heap_caps_malloc(AAC_PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t* wav_raw = static_cast<uint8_t*>(heap_caps_malloc(512 * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int16_t* wav_pcm = static_cast<int16_t*>(heap_caps_malloc(512 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t mp3_buf_filled = 0;

    std::vector<int16_t> out_24k;
    out_24k.reserve(2048);
    uint32_t played_out_samples = 0;
    int32_t base_start_ms       = 0;

    auto close_file = [&]() {
        if (file) {
            std::fclose(file);
            file = nullptr;
        }
        _audio_file_open.store(false);
        mp3_buf_filled = 0;
    };

    auto resample_and_output = [&](const int16_t* pcm, int frames, int channels, uint32_t src_rate) {
        if (!audio_codec || frames <= 0 || src_rate == 0) {
            return;
        }
        if (!audio_codec->output_enabled()) {
            if (audio_codec->input_enabled()) {
                audio_codec->EnableInput(false);
            }
            audio_codec->EnableOutput(true);
        }

        const uint32_t dst_rate = AUDIO_OUTPUT_SAMPLE_RATE;
        int out_frames          = static_cast<int>((static_cast<uint64_t>(frames) * dst_rate) / src_rate);
        if (out_frames <= 0) {
            return;
        }
        out_24k.resize(out_frames);

        for (int i = 0; i < out_frames; ++i) {
            uint64_t src_pos_fp = (static_cast<uint64_t>(i) * src_rate << 12) / dst_rate;
            int idx0            = static_cast<int>(src_pos_fp >> 12);
            int frac            = static_cast<int>(src_pos_fp & 0xFFF);
            if (idx0 >= frames) {
                idx0 = frames - 1;
            }
            int idx1 = (idx0 + 1 < frames) ? (idx0 + 1) : idx0;

            int32_t s0 = 0;
            int32_t s1 = 0;
            if (channels >= 2) {
                s0 = (static_cast<int32_t>(pcm[idx0 * channels]) + static_cast<int32_t>(pcm[idx0 * channels + 1])) / 2;
                s1 = (static_cast<int32_t>(pcm[idx1 * channels]) + static_cast<int32_t>(pcm[idx1 * channels + 1])) / 2;
            } else {
                s0 = pcm[idx0];
                s1 = pcm[idx1];
            }
            int32_t interp = s0 + (((s1 - s0) * frac) >> 12);
            out_24k[i]     = static_cast<int16_t>(std::clamp<int32_t>(interp, -32768, 32767));
        }

        audio_codec->OutputData(out_24k);
        played_out_samples += static_cast<uint32_t>(out_frames);
        int32_t cur_ms = base_start_ms + static_cast<int32_t>((static_cast<uint64_t>(played_out_samples) * 1000ULL) / dst_rate);
        _elapsed_ms.store(cur_ms);
    };

    while (_task_running.load()) {
        if (_cmd_close_file.exchange(false)) {
            close_file();
        }

        if (_cmd_reload.exchange(false)) {
            close_file();
            std::string path;
            int32_t start_ms = 0;
            {
                std::lock_guard<std::mutex> lk(_audio_mutex);
                path     = _active_path;
                is_mp3   = _active_is_mp3;
                is_radio = _active_is_radio;
                start_ms = _target_start_ms;
                probe    = _active_probe;
            }

            if (is_radio) {
                is_mp3             = true;
                radio_buffering    = true;
                radio_codec        = 0;
                base_start_ms      = 0;
                played_out_samples = 0;
                _elapsed_ms.store(0);
                _decoded_bytes.store(0);
                if (mp3d) {
                    mp3dec_init(mp3d);
                }
                if (aac_dec) {
                    esp_aac_dec_reset(aac_dec);
                }
            } else if (!path.empty() && probe.valid) {
                file = std::fopen(path.c_str(), "rb");
                if (file) {
                    _audio_file_open.store(true);
                    uint32_t seek_byte = probe.data_offset;
                    if (start_ms > 0 && probe.byte_rate > 0) {
                        uint32_t offset = static_cast<uint32_t>((static_cast<uint64_t>(start_ms) * probe.byte_rate) / 1000ULL);
                        if (!is_mp3) {
                            uint32_t frame_align = std::max<uint32_t>(1, (probe.channels * (probe.bits / 8)));
                            offset               = (offset / frame_align) * frame_align;
                        }
                        if (offset < probe.data_size) {
                            seek_byte += offset;
                        }
                    }
                    std::fseek(file, static_cast<long>(seek_byte), SEEK_SET);
                    _decoded_bytes.store(seek_byte - probe.data_offset);
                    base_start_ms      = start_ms;
                    played_out_samples = 0;
                    _elapsed_ms.store(start_ms);
                    if (mp3d) {
                        mp3dec_init(mp3d);
                    }
                }
            }
        }

        if (!_playing.load()) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        if (is_radio) {
            if (!_wifi_svc || !mp3d || !mp3_buf || !mp3_pcm) {
                _playing.store(false);
                continue;
            }

            if (radio_buffering) {
                if (_wifi_svc->radio_buffered_bytes() < 6144 && !_wifi_svc->is_radio_eof()) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                radio_buffering = false;
            }

            if (mp3_buf_filled < 4096) {
                size_t space = MP3_BUF_SIZE - mp3_buf_filled;
                size_t got   = _wifi_svc->pop_radio_bytes(mp3_buf + mp3_buf_filled, space);
                mp3_buf_filled += got;
            }

            if (mp3_buf_filled == 0) {
                if (_wifi_svc->is_radio_eof()) {
                    _playing.store(false);
                    _track_eof.store(true);
                    continue;
                }
                radio_buffering = true;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            // Auto-detect AAC (ADTS) vs MP3 once headers or first bytes arrive
            if (radio_codec == 0) {
                if (_wifi_svc->is_radio_aac()) {
                    radio_codec = 2;
                } else {
                    size_t scan_lim = std::min<size_t>(mp3_buf_filled, 512);
                    for (size_t i = 0; i + 2 <= scan_lim; ++i) {
                        if (mp3_buf[i] == 0xFF && (mp3_buf[i + 1] & 0xF6) == 0xF0) {
                            radio_codec = 2;  // ADTS AAC sync (layer == 00)
                            break;
                        }
                        if (mp3_buf[i] == 0xFF && (mp3_buf[i + 1] & 0xE0) == 0xE0 && (mp3_buf[i + 1] & 0x06) != 0) {
                            radio_codec = 1;  // MPEG audio (MP3)
                            break;
                        }
                    }
                    if (radio_codec == 0) {
                        radio_codec = 1;
                    }
                }
                mclog::tagInfo(TAG, "Radio codec detected: {}", (radio_codec == 2) ? "AAC" : "MP3");
            }

            if (radio_codec == 2) {
                if (!aac_dec) {
                    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
                    aac_cfg.aac_plus_enable   = true;
                    if (esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &aac_dec) != ESP_AUDIO_ERR_OK) {
                        mclog::tagError(TAG, "Failed to open AAC decoder");
                        _playing.store(false);
                        _track_eof.store(true);
                        continue;
                    }
                }
                if (!aac_pcm) {
                    _playing.store(false);
                    continue;
                }

                // Align to ADTS sync header (0xFF 0xF0..0xF9) before feeding decoder
                if (mp3_buf_filled >= 2 && !(mp3_buf[0] == 0xFF && (mp3_buf[1] & 0xF6) == 0xF0)) {
                    size_t sync_pos = mp3_buf_filled;
                    for (size_t i = 1; i + 2 <= mp3_buf_filled; ++i) {
                        if (mp3_buf[i] == 0xFF && (mp3_buf[i + 1] & 0xF6) == 0xF0) {
                            sync_pos = i;
                            break;
                        }
                    }
                    if (sync_pos < mp3_buf_filled) {
                        std::memmove(mp3_buf, mp3_buf + sync_pos, mp3_buf_filled - sync_pos);
                        mp3_buf_filled -= sync_pos;
                    } else {
                        mp3_buf_filled = 0;
                        continue;
                    }
                }

                esp_audio_dec_in_raw_t raw      = {};
                raw.buffer                      = mp3_buf;
                raw.len                         = static_cast<uint32_t>(mp3_buf_filled);
                esp_audio_dec_out_frame_t frame = {};
                frame.buffer                    = reinterpret_cast<uint8_t*>(aac_pcm);
                frame.len                       = AAC_PCM_BYTES;
                esp_audio_dec_info_t dec_info   = {};

                esp_audio_err_t ret = esp_aac_dec_decode(aac_dec, &raw, &frame, &dec_info);
                if (raw.consumed > 0) {
                    size_t consumed = std::min<size_t>(raw.consumed, mp3_buf_filled);
                    if (consumed < mp3_buf_filled) {
                        std::memmove(mp3_buf, mp3_buf + consumed, mp3_buf_filled - consumed);
                        mp3_buf_filled -= consumed;
                    } else {
                        mp3_buf_filled = 0;
                    }
                    _decoded_bytes.fetch_add(static_cast<uint32_t>(consumed));
                }

                if (ret == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
                    int ch        = (dec_info.channel > 0) ? dec_info.channel : 2;
                    uint32_t sr   = (dec_info.sample_rate > 0) ? dec_info.sample_rate : 44100;
                    int out_frms  = static_cast<int>(frame.decoded_size / (ch * sizeof(int16_t)));
                    if (out_frms > 0) {
                        resample_and_output(aac_pcm, out_frms, ch, sr);
                    }
                } else if (raw.consumed == 0) {
                    if (_wifi_svc->is_radio_eof()) {
                        mp3_buf_filled = 0;
                        _playing.store(false);
                        _track_eof.store(true);
                        continue;
                    }
                    size_t got = _wifi_svc->pop_radio_bytes(mp3_buf + mp3_buf_filled, MP3_BUF_SIZE - mp3_buf_filled);
                    mp3_buf_filled += got;
                    if (got == 0) {
                        if (mp3_buf_filled >= 2048) {
                            // Drop 1 byte to resync if stuck on malformed sync word
                            std::memmove(mp3_buf, mp3_buf + 1, --mp3_buf_filled);
                        } else {
                            vTaskDelay(pdMS_TO_TICKS(15));
                        }
                    }
                }
                continue;
            }

            mp3dec_frame_info_t frame_info = {};
            int samples = mp3dec_decode_frame(mp3d, mp3_buf, static_cast<int>(mp3_buf_filled), mp3_pcm, &frame_info);
            if (frame_info.frame_bytes > 0) {
                size_t consumed = static_cast<size_t>(frame_info.frame_bytes);
                if (consumed < mp3_buf_filled) {
                    std::memmove(mp3_buf, mp3_buf + consumed, mp3_buf_filled - consumed);
                    mp3_buf_filled -= consumed;
                } else {
                    mp3_buf_filled = 0;
                }
                _decoded_bytes.fetch_add(static_cast<uint32_t>(consumed));
            } else {
                if (_wifi_svc->is_radio_eof()) {
                    mp3_buf_filled = 0;
                    _playing.store(false);
                    _track_eof.store(true);
                    continue;
                }
                if (mp3_buf_filled >= MP3_BUF_SIZE) {
                    mp3_buf_filled = 0;
                } else {
                    size_t got = _wifi_svc->pop_radio_bytes(mp3_buf + mp3_buf_filled, MP3_BUF_SIZE - mp3_buf_filled);
                    mp3_buf_filled += got;
                    if (got == 0) {
                        vTaskDelay(pdMS_TO_TICKS(15));
                    }
                }
                continue;
            }

            if (samples > 0 && frame_info.hz > 0 && frame_info.channels > 0) {
                resample_and_output(mp3_pcm, samples, frame_info.channels, static_cast<uint32_t>(frame_info.hz));
            }
            continue;
        }

        if (!file) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        if (!is_mp3) {
            constexpr int WAV_CHUNK_FRAMES = 512;
            int bytes_per_sample           = std::max<int>(1, probe.bits / 8);
            int frame_bytes                = bytes_per_sample * std::max<int>(1, probe.channels);
            uint32_t cur_rel               = _decoded_bytes.load();
            if (cur_rel >= probe.data_size) {
                close_file();
                _playing.store(false);
                _track_eof.store(true);
                continue;
            }

            uint32_t rem_bytes = probe.data_size - cur_rel;
            size_t to_read     = std::min<size_t>(WAV_CHUNK_FRAMES * frame_bytes, rem_bytes);
            if (!wav_raw || !wav_pcm) {
                close_file();
                _playing.store(false);
                continue;
            }
            if (to_read > 512 * 4) {
                to_read = 512 * 4;
            }
            size_t n = std::fread(wav_raw, 1, to_read, file);
            if (n < static_cast<size_t>(frame_bytes)) {
                close_file();
                _playing.store(false);
                _track_eof.store(true);
                continue;
            }

            _decoded_bytes.fetch_add(static_cast<uint32_t>(n));
            int frames_read = static_cast<int>(n / frame_bytes);

            if (probe.bits == 16) {
                std::memcpy(wav_pcm, wav_raw, frames_read * frame_bytes);
            } else if (probe.bits == 8) {
                int total_samples = frames_read * probe.channels;
                for (int i = 0; i < total_samples; ++i) {
                    wav_pcm[i] = static_cast<int16_t>((static_cast<int>(wav_raw[i]) - 128) << 8);
                }
            } else {
                close_file();
                _playing.store(false);
                _track_eof.store(true);
                continue;
            }

            resample_and_output(wav_pcm, frames_read, probe.channels, probe.sample_rate);
        } else {
            if (!mp3d || !mp3_buf || !mp3_pcm) {
                close_file();
                _playing.store(false);
                continue;
            }

            if (mp3_buf_filled < 4096) {
                size_t space = MP3_BUF_SIZE - mp3_buf_filled;
                size_t n     = std::fread(mp3_buf + mp3_buf_filled, 1, space, file);
                mp3_buf_filled += n;
            }

            if (mp3_buf_filled == 0) {
                close_file();
                _playing.store(false);
                _track_eof.store(true);
                continue;
            }

            mp3dec_frame_info_t frame_info = {};
            int samples = mp3dec_decode_frame(mp3d, mp3_buf, static_cast<int>(mp3_buf_filled), mp3_pcm, &frame_info);

            if (frame_info.frame_bytes > 0) {
                size_t consumed = static_cast<size_t>(frame_info.frame_bytes);
                if (consumed < mp3_buf_filled) {
                    std::memmove(mp3_buf, mp3_buf + consumed, mp3_buf_filled - consumed);
                    mp3_buf_filled -= consumed;
                } else {
                    mp3_buf_filled = 0;
                }
                _decoded_bytes.fetch_add(static_cast<uint32_t>(consumed));
            } else {
                if (std::feof(file)) {
                    close_file();
                    _playing.store(false);
                    _track_eof.store(true);
                    continue;
                }
                mp3_buf_filled = 0;
                continue;
            }

            if (samples > 0 && frame_info.hz > 0 && frame_info.channels > 0) {
                resample_and_output(mp3_pcm, samples, frame_info.channels, static_cast<uint32_t>(frame_info.hz));
            }
        }
    }

    close_file();
    if (audio_codec && audio_codec->output_enabled()) {
        audio_codec->EnableOutput(false);
    }
    if (aac_dec) {
        esp_aac_dec_close(aac_dec);
        aac_dec = nullptr;
    }
    if (aac_pcm) {
        heap_caps_free(aac_pcm);
    }
    if (mp3d) {
        heap_caps_free(mp3d);
    }
    if (mp3_buf) {
        heap_caps_free(mp3_buf);
    }
    if (mp3_pcm) {
        heap_caps_free(mp3_pcm);
    }
    if (wav_raw) {
        heap_caps_free(wav_raw);
    }
    if (wav_pcm) {
        heap_caps_free(wav_pcm);
    }
    _task_exited = true;
}

/* -------------------------------------------------------------------------- */
/*                            Playback Controls                               */
/* -------------------------------------------------------------------------- */

void AppMusicPlayer::play_track(int idx, int32_t start_ms)
{
    if (_tracks.empty()) {
        return;
    }

    if (_wifi_svc) {
        _wifi_svc->stop_radio();
    }
    _radio_mode = false;

    int n  = static_cast<int>(_tracks.size());
    _index = ((idx % n) + n) % n;

    const auto& tr = _tracks[_index];
    std::string tr_path(tr.path.c_str(), tr.path.size());
    auto info = probe_audio(tr_path, tr.is_mp3);
    if (!info.valid) {
        set_status(tr.is_mp3 ? "INVALID MP3" : "INVALID WAV");
        _playing = false;
        _started = false;
        return;
    }

    if (start_ms < 0) {
        start_ms = 0;
    }
    if (info.duration_ms > 0 && start_ms > static_cast<int32_t>(info.duration_ms)) {
        start_ms = static_cast<int32_t>(info.duration_ms);
    }

    _duration_ms.store(static_cast<int32_t>(info.duration_ms));
    _byte_rate.store(info.byte_rate);
    _elapsed_ms.store(start_ms);

    {
        std::lock_guard<std::mutex> lk(_audio_mutex);
        _active_path     = tr_path;
        _active_is_mp3   = tr.is_mp3;
        _active_is_radio = false;
        _target_start_ms = start_ms;
        _active_probe    = info;
    }

    _track_eof  = false;
    _cmd_reload = true;
    _playing    = true;
    _started    = true;
    _status_msg.clear();
    _audio_err.clear();
    _last_beat = 0xFFFFFFFF;

    update_now_playing_title();

    // Persist last played track immediately
    save_last_track();
}

void AppMusicPlayer::play_radio(int station_idx)
{
    if (!_wifi_svc) {
        return;
    }
    auto stations = _wifi_svc->get_stations();
    if (stations.empty()) {
        set_status("NO STATIONS");
        return;
    }

    int n        = static_cast<int>(stations.size());
    _radio_index = ((station_idx % n) + n) % n;
    _radio_mode  = true;

    _duration_ms.store(0);
    _byte_rate.store(0);
    _elapsed_ms.store(0);
    _decoded_bytes.store(0);

    {
        std::lock_guard<std::mutex> lk(_audio_mutex);
        _active_path     = stations[_radio_index].url;
        _active_is_mp3   = true;
        _active_is_radio = true;
        _target_start_ms = 0;
        _active_probe    = AudioProbeInfo{};
    }

    _track_eof  = false;
    _cmd_reload = true;
    _playing    = true;
    _started    = true;
    _audio_err.clear();
    _last_beat = 0xFFFFFFFF;
    set_status("TUNING RADIO...", 3500);
    update_now_playing_title();

    _wifi_svc->start_radio(stations[_radio_index].url);
}

void AppMusicPlayer::toggle_play()
{
    if (_radio_mode.load()) {
        if (_playing.load()) {
            _playing = false;
            if (_wifi_svc) {
                _wifi_svc->stop_radio();
            }
            if (_robot_move) {
                move_home(500);
            }
            if (_lights_on) {
                set_all_rgb(0x001028);
            } else {
                rgb_off();
            }
        } else {
            play_radio(_radio_index);
        }
        return;
    }

    if (_tracks.empty()) {
        return;
    }

    if (_playing.load()) {
        _playing = false;
        if (_robot_move) {
            move_home(500);
        }
        if (_lights_on) {
            set_all_rgb(0x001028);
        } else {
            rgb_off();
        }
    } else {
        int32_t dur = _duration_ms.load();
        int32_t el  = _elapsed_ms.load();
        if (!_started.load() || (dur > 0 && el >= dur - 60)) {
            play_track(_index, 0);
            return;
        }
        _playing   = true;
        _last_beat = 0xFFFFFFFF;
    }
}

void AppMusicPlayer::stop_playback()
{
    _playing = false;
    _started = false;
    if (_wifi_svc) {
        _wifi_svc->stop_radio();
    }
}

void AppMusicPlayer::seek_to(int32_t ms)
{
    if (_radio_mode.load()) {
        set_status("LIVE RADIO");
        return;
    }
    if (_tracks.empty()) {
        return;
    }
    if (_byte_rate.load() == 0) {
        set_status("SEEK UNAVAILABLE");
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    int32_t dur = _duration_ms.load();
    if (dur > 250 && ms > dur - 250) {
        ms = dur - 250;
    }
    play_track(_index, ms);
}

void AppMusicPlayer::change_track(int delta)
{
    if (_radio_mode.load()) {
        play_radio(_radio_index + delta);
        return;
    }
    if (_tracks.empty()) {
        return;
    }
    if (_shuffle_on && _tracks.size() > 1) {
        play_track(random_index(), 0);
    } else {
        int n = static_cast<int>(_tracks.size());
        play_track((_index + delta + n) % n, 0);
    }
}

void AppMusicPlayer::change_volume(int delta)
{
    set_volume(_volume + delta);
}

void AppMusicPlayer::set_volume(int vol)
{
    _volume = std::clamp(vol, 0, 100);
    GetHAL().setSpeakerVolume(static_cast<uint8_t>(_volume), true);
    save_settings();
    set_status("VOL " + std::to_string(_volume));
}

void AppMusicPlayer::cycle_loop()
{
    _loop_mode = (_loop_mode + 1) % 3;
    save_settings();
    set_status(std::string("LOOP ") + LOOP_NAMES[_loop_mode]);
}

void AppMusicPlayer::toggle_move()
{
    _robot_move = !_robot_move;
    save_settings();
    if (_robot_move) {
        _last_beat = 0xFFFFFFFF;
        set_status("MOTION ON");
    } else {
        move_home(500);
        set_status("MOTION OFF");
    }
}

void AppMusicPlayer::toggle_lights()
{
    _lights_on = !_lights_on;
    save_settings();
    _last_beat = 0xFFFFFFFF;
    if (_lights_on) {
        set_status("LIGHTS ON");
    } else {
        rgb_off();
        set_status("LIGHTS OFF");
    }
}

void AppMusicPlayer::toggle_shuffle()
{
    _shuffle_on = !_shuffle_on;
    save_settings();
    set_status(std::string("SHUFFLE ") + (_shuffle_on ? "ON" : "OFF"));
}

void AppMusicPlayer::toggle_avatar_style()
{
    bool currently_naruto =
        (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto);
    auto next_style =
        currently_naruto ? stackchan::avatar::AvatarStyle::Classic : stackchan::avatar::AvatarStyle::Naruto;
    stackchan::avatar::setAvatarStyle(next_style);
    apply_avatar_style();
    set_status(next_style == stackchan::avatar::AvatarStyle::Naruto ? "AVATAR NARUTO" : "AVATAR CLASSIC");
}

int AppMusicPlayer::random_index() const
{
    int count = static_cast<int>(_tracks.size());
    if (count <= 1) {
        return 0;
    }
    int n = _index;
    for (int t = 0; t < 6 && n == _index; ++t) {
        n = std::rand() % count;
    }
    if (n == _index) {
        n = (_index + 1) % count;
    }
    return n;
}

void AppMusicPlayer::track_finished()
{
    if (_radio_mode.load()) {
        _playing = false;
        _started = false;
        if (_wifi_svc) {
            std::string err = _wifi_svc->get_radio_error();
            if (!err.empty()) {
                _audio_err = err;
            }
        }
        rgb_off();
        if (_robot_move) {
            move_home(500);
        }
        return;
    }

    int count = static_cast<int>(_tracks.size());
    if (_loop_mode == 1) {
        play_track(_index, 0);
    } else if (_shuffle_on && count > 1) {
        play_track(random_index(), 0);
    } else if (_index + 1 < count) {
        play_track(_index + 1, 0);
    } else if (_loop_mode == 2 && count > 0) {
        play_track(0, 0);
    } else {
        _playing    = false;
        _started    = false;
        _elapsed_ms = _duration_ms.load();
        rgb_off();
        if (_robot_move) {
            move_home(500);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                      Wi-Fi Radio & Web Remote Bridge                       */
/* -------------------------------------------------------------------------- */

void AppMusicPlayer::init_wifi_service()
{
    _wifi_svc = std::make_unique<MusicWifiService>();
    _wifi_svc->load_stations(_sd_mounted);

    MusicWifiService::WebCallbacks cbs;
    cbs.get_status_json = [this]() { return build_status_json(); };
    cbs.get_tracks_json = [this](const std::string& q, int page, int limit) {
        return build_tracks_json(q, page, limit);
    };
    cbs.enqueue_cmd = [this](const MusicWifiService::WebCommand& cmd) {
        std::lock_guard<std::mutex> lk(_cmd_mutex);
        if (_web_cmds.size() < 16) {
            _web_cmds.push_back(cmd);
        }
    };
    cbs.begin_sd_write = [this]() { return handle_begin_sd_write(); };
    cbs.end_sd_write   = [this](bool ok, const std::string& filename) { handle_end_sd_write(ok, filename); };

    _wifi_svc->init(cbs);
}

void AppMusicPlayer::update_now_playing_title()
{
    std::string title;
    if (_radio_mode.load() && _wifi_svc) {
        auto stations = _wifi_svc->get_stations();
        if (_radio_index >= 0 && _radio_index < static_cast<int>(stations.size())) {
            title = stations[_radio_index].name;
        }
    } else if (!_tracks.empty() && _index >= 0 && _index < static_cast<int>(_tracks.size())) {
        title.assign(_tracks[_index].title.c_str(), _tracks[_index].title.size());
    }
    std::lock_guard<std::mutex> lk(_title_mutex);
    _now_playing_title = title;
}

void AppMusicPlayer::process_web_commands()
{
    std::deque<MusicWifiService::WebCommand> pending;
    std::string uploaded_file;
    {
        std::lock_guard<std::mutex> lk(_cmd_mutex);
        if (_web_cmds.empty() && _uploaded_notice.empty()) {
            return;
        }
        pending.swap(_web_cmds);
        uploaded_file.swap(_uploaded_notice);
    }

    bool ui_dirty = false;
    for (const auto& cmd : pending) {
        if (cmd.action == "toggle") {
            toggle_play();
            ui_dirty = true;
        } else if (cmd.action == "next") {
            change_track(1);
            ui_dirty = true;
        } else if (cmd.action == "prev") {
            change_track(-1);
            ui_dirty = true;
        } else if (cmd.action == "play") {
            play_track(cmd.index, 0);
            ui_dirty = true;
        } else if (cmd.action == "radio") {
            play_radio(cmd.index);
            ui_dirty = true;
        } else if (cmd.action == "vol") {
            set_volume(cmd.value);
            ui_dirty = true;
        } else if (cmd.action == "loop") {
            cycle_loop();
            ui_dirty = true;
        } else if (cmd.action == "shuffle") {
            toggle_shuffle();
            ui_dirty = true;
        } else if (cmd.action == "lights") {
            toggle_lights();
            ui_dirty = true;
        } else if (cmd.action == "move") {
            toggle_move();
            ui_dirty = true;
        } else if (cmd.action == "rescan") {
            scan_tracks();
            if (!_tracks.empty() && _index >= static_cast<int>(_tracks.size())) {
                _index = 0;
            }
            if (!uploaded_file.empty()) {
                set_status("ADDED " + clip_text(uploaded_file, 18), 3500);
            }
            update_now_playing_title();
            ui_dirty = true;
        }
    }

    if (ui_dirty) {
        LvglLockGuard lock;
        uint32_t now = GetHAL().millis();
        refresh_main_ui(now);
        refresh_avatar_anim(now);
        if (_mode == ScreenMode::Settings) {
            refresh_settings_ui();
        } else if (_mode == ScreenMode::List) {
            refresh_list_ui();
        } else if (_mode == ScreenMode::Radio) {
            refresh_radio_ui();
        }
    }
}

std::string AppMusicPlayer::build_status_json() const
{
    cJSON* root = cJSON_CreateObject();
    std::string title;
    {
        std::lock_guard<std::mutex> lk(_title_mutex);
        title = _now_playing_title;
    }
    int total_tracks = 0;
    {
        std::lock_guard<std::mutex> lk(_tracks_mutex);
        total_tracks = static_cast<int>(_tracks.size());
    }

    bool is_rad = _radio_mode.load();
    cJSON_AddStringToObject(root, "title", title.c_str());
    cJSON_AddBoolToObject(root, "playing", _playing.load());
    cJSON_AddBoolToObject(root, "radio", is_rad);
    cJSON_AddNumberToObject(root, "radio_idx", _radio_index);
    cJSON_AddNumberToObject(root, "index", _index);
    cJSON_AddNumberToObject(root, "total", total_tracks);
    cJSON_AddNumberToObject(root, "elapsed", _elapsed_ms.load());
    cJSON_AddNumberToObject(root, "duration", _duration_ms.load());
    cJSON_AddNumberToObject(root, "volume", _volume);
    cJSON_AddNumberToObject(root, "loop", _loop_mode);
    cJSON_AddBoolToObject(root, "shuffle", _shuffle_on);
    cJSON_AddBoolToObject(root, "lights", _lights_on);
    cJSON_AddBoolToObject(root, "move", _robot_move);
    cJSON_AddNumberToObject(root, "batt", GetHAL().getBatteryLevel());

    std::string rstat = "READY";
    if (is_rad && _wifi_svc) {
        std::string err = _wifi_svc->get_radio_error();
        if (!err.empty()) {
            rstat = err;
        } else if (_wifi_svc->is_radio_connected()) {
            rstat = "LIVE (" + std::to_string(_wifi_svc->radio_buffered_bytes() / 1024) + " KB)";
        } else if (_playing.load()) {
            rstat = "BUFFERING...";
        } else {
            rstat = "PAUSED";
        }
    }
    cJSON_AddStringToObject(root, "radio_status", rstat.c_str());

    char* str = cJSON_PrintUnformatted(root);
    std::string out(str ? str : "{}");
    if (str) {
        cJSON_free(str);
    }
    cJSON_Delete(root);
    return out;
}

std::string AppMusicPlayer::build_tracks_json(const std::string& q, int page, int limit) const
{
    std::string ql = q;
    for (char& c : ql) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* arr  = cJSON_AddArrayToObject(root, "items");

    int matched = 0;
    int start   = page * limit;
    int end     = start + limit;

    {
        std::lock_guard<std::mutex> lk(_tracks_mutex);
        for (int i = 0; i < static_cast<int>(_tracks.size()); ++i) {
            const auto& tr = _tracks[i];
            if (!ql.empty()) {
                bool hit = false;
                size_t n = tr.title.size();
                if (n >= ql.size()) {
                    for (size_t pos = 0; pos + ql.size() <= n; ++pos) {
                        bool eq = true;
                        for (size_t k = 0; k < ql.size(); ++k) {
                            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(tr.title[pos + k])));
                            if (c != ql[k]) {
                                eq = false;
                                break;
                            }
                        }
                        if (eq) {
                            hit = true;
                            break;
                        }
                    }
                }
                if (!hit) {
                    continue;
                }
            }
            if (matched >= start && matched < end) {
                cJSON* it = cJSON_CreateObject();
                cJSON_AddNumberToObject(it, "i", i);
                std::string t(tr.title.c_str(), tr.title.size());
                cJSON_AddStringToObject(it, "t", t.c_str());
                cJSON_AddBoolToObject(it, "m", tr.is_mp3);
                cJSON_AddItemToArray(arr, it);
            }
            ++matched;
        }
    }

    cJSON_AddNumberToObject(root, "total", matched);

    char* str = cJSON_PrintUnformatted(root);
    std::string out(str ? str : "{\"total\":0,\"items\":[]}");
    if (str) {
        cJSON_free(str);
    }
    cJSON_Delete(root);
    return out;
}

bool AppMusicPlayer::handle_begin_sd_write()
{
    if (_sd_upload_active.exchange(true)) {
        return false;
    }
    // Pause playback and close any open SD file cleanly before writing uploaded file
    _playing        = false;
    _cmd_close_file = true;
    for (int i = 0; i < 40 && _audio_file_open.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return !_audio_file_open.load();
}

void AppMusicPlayer::handle_end_sd_write(bool success, const std::string& filename)
{
    _sd_upload_active = false;
    if (success) {
        std::lock_guard<std::mutex> lk(_cmd_mutex);
        _uploaded_notice = filename;
        _web_cmds.push_back({"rescan", 0, 0});
    }
}

/* -------------------------------------------------------------------------- */
/*                     Robot: Servos, LEDs & Persistence                      */
/* -------------------------------------------------------------------------- */

void AppMusicPlayer::load_saved_prefs()
{
    Settings settings(NVS_NS, false);

    int saved_y = settings.GetInt(KEY_Y_HOME, Y_HOME_DEFAULT_DEG);
    _y_home_deg = std::clamp(saved_y, 0, 90);

    int saved_vol = settings.GetInt(KEY_VOLUME, -1);
    _volume       = (saved_vol >= 0 && saved_vol <= 100) ? saved_vol : GetHAL().getSpeakerVolume();

    int saved_loop = settings.GetInt(KEY_LOOP_MODE, 0);
    _loop_mode     = (saved_loop >= 0 && saved_loop <= 2) ? saved_loop : 0;

    _shuffle_on = settings.GetBool(KEY_SHUFFLE, false);
    _lights_on  = settings.GetBool(KEY_LIGHTS, true);
    _robot_move = settings.GetBool(KEY_MOVE, true);

    _saved_track_idx  = settings.GetInt(KEY_LAST_IDX, 0);
    _saved_track_path = settings.GetString(KEY_LAST_PATH, "");
}

void AppMusicPlayer::save_settings()
{
    Settings settings(NVS_NS, true);
    settings.SetInt(KEY_VOLUME, _volume);
    settings.SetInt(KEY_LOOP_MODE, _loop_mode);
    settings.SetBool(KEY_SHUFFLE, _shuffle_on);
    settings.SetBool(KEY_LIGHTS, _lights_on);
    settings.SetBool(KEY_MOVE, _robot_move);
    settings.SetInt(KEY_Y_HOME, _y_home_deg);
}

void AppMusicPlayer::save_last_track()
{
    if (_tracks.empty() || _index < 0 || _index >= static_cast<int>(_tracks.size())) {
        return;
    }
    Settings settings(NVS_NS, true);
    settings.SetInt(KEY_LAST_IDX, _index);
    std::string cur_path(_tracks[_index].path.c_str(), _tracks[_index].path.size());
    settings.SetString(KEY_LAST_PATH, cur_path);
}

bool AppMusicPlayer::save_head_cfg()
{
    save_settings();
    return true;
}

void AppMusicPlayer::move_home(int speed)
{
    auto& motion = GetStackChan().motion();
    motion.moveWithSpeed(py_x_deg_to_yaw(X_HOME_DEG), py_y_deg_to_pitch(_y_home_deg), speed);
}

void AppMusicPlayer::rgb_off()
{
    GetHAL().showRgbColor(0, 0, 0);
}

void AppMusicPlayer::set_all_rgb(uint32_t hex_color)
{
    uint8_t r = (hex_color >> 16) & 0xFF;
    uint8_t g = (hex_color >> 8) & 0xFF;
    uint8_t b = hex_color & 0xFF;
    GetHAL().showRgbColor(r, g, b);
}

void AppMusicPlayer::beat_update(uint32_t now)
{
    if (!_playing.load()) {
        return;
    }

    uint32_t beat = now / BEAT_MS;
    if (beat == _last_beat) {
        return;
    }
    _last_beat = beat;

    if (_lights_on) {
        set_all_rgb(PALETTE[beat % 8]);
    }

    if (_robot_move && (beat % 2) == 0) {
        int r = std::rand() % 100;
        if (r < SWAY_STILL_PCT) {
            return;
        }
        _sway_side = -_sway_side;
        int ax     = 3 + (std::rand() % (X_SWAY_DEG - 3 + 1));
        int ay     = 2 + (std::rand() % (Y_SWAY_DEG - 2 + 1));
        int my_deg = std::clamp(_y_home_deg + _sway_side * ay, 0, 90);
        int mx_deg = X_HOME_DEG + _sway_side * ax;

        auto& motion = GetStackChan().motion();
        motion.moveWithSpeed(py_x_deg_to_yaw(mx_deg), py_y_deg_to_pitch(my_deg), 550);
    }
}

void AppMusicPlayer::set_status(const std::string& msg, uint32_t duration_ms)
{
    _status_msg   = msg;
    _status_until = GetHAL().millis() + duration_ms;
}

std::string AppMusicPlayer::audio_state_summary() const
{
    auto& board      = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    bool spk         = audio_codec && audio_codec->output_enabled();
    bool mic         = audio_codec && audio_codec->input_enabled();
    uint32_t kb      = _decoded_bytes.load() / 1024;
    int batt         = GetHAL().getBatteryLevel();

    char buf[48];
    std::snprintf(buf, sizeof(buf), "S%d M%d V%d P%uK B%d", spk ? 1 : 0, mic ? 1 : 0, _volume,
                  static_cast<unsigned>(kb), batt);
    return std::string(buf);
}

/* -------------------------------------------------------------------------- */
/*                             LVGL UI Builder                                */
/* -------------------------------------------------------------------------- */

void AppMusicPlayer::build_ui()
{
    _root_screen = lv_screen_active();
    lv_obj_set_style_bg_color(_root_screen, lv_color_hex(COL_BG), 0);
    lv_obj_remove_flag(_root_screen, LV_OBJ_FLAG_SCROLLABLE);

    const int top_y       = view::get_status_bar_top_offset();
    const bool is_fix     = (top_y > 0);
    const int title_y     = top_y + 2;
    const int bar_y       = top_y + 22;
    const int time_y      = top_y + 30;
    const int status_y    = top_y + 42;
    const int stage_y     = top_y + 55;
    const int stage_h     = 188 - stage_y;
    const int sub_hdr_y   = top_y + 2;
    const int row_y0      = top_y + (is_fix ? 23 : 26);
    const int row_step    = is_fix ? 25 : 29;
    const int row_h       = is_fix ? 22 : 26;
    const int list_info_y = is_fix ? 186 : 178;

    /* ============================ 1. MAIN VIEW ============================= */
    _main_cont = lv_obj_create(_root_screen);
    lv_obj_set_size(_main_cont, W, H);
    lv_obj_set_pos(_main_cont, 0, 0);
    style_flat_box(_main_cont, COL_BG, 0);

    // Track title across full width (battery & Wi-Fi are already in the system status bar)
    _lbl_title = lv_label_create(_main_cont);
    lv_obj_set_pos(_lbl_title, 10, title_y);
    lv_obj_set_size(_lbl_title, 300, 20);
    lv_label_set_long_mode(_lbl_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_title, lv_color_hex(COL_TEXT), 0);
    lv_obj_add_flag(_lbl_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        _lbl_title,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            if (self->_radio_mode.load()) {
                self->open_radio();
            } else {
                self->open_list();
            }
        },
        LV_EVENT_CLICKED, this);

    // Touch-seekable progress bar
    _bar_bg = lv_obj_create(_main_cont);
    lv_obj_set_pos(_bar_bg, BAR_X, bar_y);
    lv_obj_set_size(_bar_bg, BAR_W, BAR_H);
    style_flat_box(_bar_bg, COL_CARD_HI, 3);
    lv_obj_add_flag(_bar_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(_bar_bg, 6);
    lv_obj_add_event_cb(
        _bar_bg,
        [](lv_event_t* e) {
            auto* self     = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            int32_t dur_ms = self->_duration_ms.load();
            if (dur_ms <= 0) {
                return;
            }
            lv_point_t p;
            lv_indev_get_point(lv_indev_active(), &p);
            float ratio = std::clamp(static_cast<float>(p.x - BAR_X) / static_cast<float>(BAR_W), 0.0f, 1.0f);
            self->seek_to(static_cast<int32_t>(ratio * dur_ms));
            self->refresh_main_ui(GetHAL().millis());
        },
        LV_EVENT_CLICKED, this);

    _bar_fill = lv_obj_create(_bar_bg);
    lv_obj_set_pos(_bar_fill, 0, 0);
    lv_obj_set_size(_bar_fill, 0, BAR_H);
    style_flat_box(_bar_fill, COL_ACCENT, 3);
    lv_obj_remove_flag(_bar_fill, LV_OBJ_FLAG_CLICKABLE);

    // Elapsed time, track counter, and total duration
    _lbl_elapsed = lv_label_create(_main_cont);
    lv_obj_set_pos(_lbl_elapsed, 10, time_y);
    lv_obj_set_style_text_font(_lbl_elapsed, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_elapsed, lv_color_hex(COL_DIM), 0);

    _lbl_track_num = lv_label_create(_main_cont);
    lv_obj_set_pos(_lbl_track_num, 180, time_y);
    lv_obj_set_width(_lbl_track_num, 85);
    lv_obj_set_style_text_align(_lbl_track_num, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(_lbl_track_num, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_track_num, lv_color_hex(COL_DIM), 0);

    _lbl_duration = lv_label_create(_main_cont);
    lv_obj_set_pos(_lbl_duration, 268, time_y);
    lv_obj_set_width(_lbl_duration, 44);
    lv_obj_set_style_text_align(_lbl_duration, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(_lbl_duration, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_duration, lv_color_hex(COL_DIM), 0);

    // Volume / version and playback status
    _lbl_vol_ver = lv_label_create(_main_cont);
    lv_obj_set_pos(_lbl_vol_ver, 10, status_y);
    lv_obj_set_style_text_font(_lbl_vol_ver, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_vol_ver, lv_color_hex(COL_DIM), 0);

    _lbl_status = lv_label_create(_main_cont);
    lv_obj_set_pos(_lbl_status, 110, status_y);
    lv_obj_set_width(_lbl_status, 202);
    lv_obj_set_style_text_align(_lbl_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(_lbl_status, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(COL_DIM), 0);

    // Center stage: animated Naruto or classic StackChan avatar + floating musical notes
    _stage_cont = lv_obj_create(_main_cont);
    lv_obj_set_pos(_stage_cont, STAGE_X, stage_y);
    lv_obj_set_size(_stage_cont, STAGE_W, stage_h);
    style_flat_box(_stage_cont, COL_BG, 12);

    auto on_stage_avatar_click = [](lv_event_t* e) {
        auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
        self->toggle_play();
        uint32_t now = GetHAL().millis();
        self->refresh_main_ui(now);
        self->refresh_avatar_anim(now);
    };

    _avatar_img = lv_image_create(_stage_cont);
    if (_img_naruto_calm.data != nullptr) {
        lv_image_set_src(_avatar_img, &_img_naruto_calm);
        if (is_fix && _img_naruto_calm.header.h > 88) {
            uint16_t scale = static_cast<uint16_t>((88 * 256) / _img_naruto_calm.header.h);
            lv_image_set_scale(_avatar_img, scale);
        }
    }
    lv_obj_align(_avatar_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_avatar_img, on_stage_avatar_click, LV_EVENT_CLICKED, this);

    _face_card = lv_obj_create(_stage_cont);
    lv_obj_set_size(_face_card, 128, is_fix ? 78 : 96);
    lv_obj_align(_face_card, LV_ALIGN_CENTER, 0, 0);
    style_flat_box(_face_card, COL_CARD, 18);
    lv_obj_set_style_border_width(_face_card, 2, 0);
    lv_obj_set_style_border_color(_face_card, lv_color_hex(COL_CARD_HI), 0);
    lv_obj_add_flag(_face_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_face_card, on_stage_avatar_click, LV_EVENT_CLICKED, this);

    _face_eye_l = lv_obj_create(_face_card);
    lv_obj_set_size(_face_eye_l, 18, 18);
    lv_obj_align(_face_eye_l, LV_ALIGN_CENTER, -26, -8);
    style_flat_box(_face_eye_l, COL_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(_face_eye_l, LV_OBJ_FLAG_CLICKABLE);

    _face_eye_r = lv_obj_create(_face_card);
    lv_obj_set_size(_face_eye_r, 18, 18);
    lv_obj_align(_face_eye_r, LV_ALIGN_CENTER, 26, -8);
    style_flat_box(_face_eye_r, COL_WHITE, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(_face_eye_r, LV_OBJ_FLAG_CLICKABLE);

    _face_mouth = lv_obj_create(_face_card);
    lv_obj_set_size(_face_mouth, 38, 6);
    lv_obj_align(_face_mouth, LV_ALIGN_CENTER, 0, 18);
    style_flat_box(_face_mouth, COL_WHITE, 4);
    lv_obj_remove_flag(_face_mouth, LV_OBJ_FLAG_CLICKABLE);

    apply_avatar_style();

    // 4 floating musical notes flanking the avatar
    const int note_xs[4] = {20, 56, 234, 270};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* n   = lv_label_create(_stage_cont);
        _note_lbls[i] = n;
        lv_label_set_text(n, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(PALETTE[i * 2]), 0);
        lv_obj_set_pos(n, note_xs[i], 52);
    }

    // Bottom 4-button bar: << | > | >> | Settings (VOL-/VOL+ are in the top status bar)
    for (int i = 0; i < 4; ++i) {
        const char* nm     = MAIN_BTN_NAMES[i];
        _main_btns[i].name = nm;

        lv_obj_t* btn     = lv_button_create(_main_cont);
        _main_btns[i].btn = btn;
        lv_obj_set_pos(btn, BAR4_X0 + i * (BAR4_W + BAR4_GAP), BAR4_Y);
        lv_obj_set_size(btn, BAR4_W, BAR4_H);
        style_flat_box(btn, COL_CARD, 10);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(COL_CARD_HI), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);

        lv_obj_t* lbl       = lv_label_create(btn);
        _main_btns[i].label = lbl;
        lv_obj_center(lbl);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);

        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                auto* self      = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
                lv_obj_t* t_btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
                const char* nm  = nullptr;
                for (const auto& mb : self->_main_btns) {
                    if (mb.btn == t_btn) {
                        nm = mb.name;
                        break;
                    }
                }
                if (!nm) {
                    return;
                }
                if (std::strcmp(nm, "prev") == 0) {
                    self->change_track(-1);
                } else if (std::strcmp(nm, "play") == 0) {
                    self->toggle_play();
                } else if (std::strcmp(nm, "next") == 0) {
                    self->change_track(1);
                } else if (std::strcmp(nm, "settings") == 0) {
                    self->switch_mode(ScreenMode::Settings);
                    return;
                }
                uint32_t now = GetHAL().millis();
                self->refresh_main_ui(now);
                self->refresh_avatar_anim(now);
            },
            LV_EVENT_CLICKED, this);
    }

    /* =========================== 2. SETTINGS VIEW ========================== */
    _settings_cont = lv_obj_create(_root_screen);
    lv_obj_set_size(_settings_cont, W, H);
    lv_obj_set_pos(_settings_cont, 0, 0);
    style_flat_box(_settings_cont, COL_BG, 0);
    lv_obj_add_flag(_settings_cont, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* lbl_cfg_hdr = lv_label_create(_settings_cont);
    lv_obj_set_pos(lbl_cfg_hdr, 10, sub_hdr_y + 2);
    lv_obj_set_size(lbl_cfg_hdr, 220, 24);
    lv_obj_set_style_text_font(lbl_cfg_hdr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_cfg_hdr, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(lbl_cfg_hdr, "< SETTINGS");
    lv_obj_add_flag(lbl_cfg_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        lbl_cfg_hdr,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->switch_mode(ScreenMode::Main);
        },
        LV_EVENT_CLICKED, this);

    for (int i = 0; i < 9; ++i) {
        const auto& spec  = CFG_BTN_SPECS[i];
        _cfg_btns[i].name = spec.name;

        lv_obj_t* btn    = lv_button_create(_settings_cont);
        _cfg_btns[i].btn = btn;
        lv_obj_set_pos(btn, spec.x, spec.y);
        lv_obj_set_size(btn, spec.w, spec.h);
        style_flat_box(btn, COL_CARD, 10);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(COL_CARD_HI), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);

        lv_obj_t* lbl      = lv_label_create(btn);
        _cfg_btns[i].label = lbl;
        lv_obj_center(lbl);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);

        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                auto* self      = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
                lv_obj_t* t_btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
                const char* nm  = nullptr;
                for (const auto& cb : self->_cfg_btns) {
                    if (cb.btn == t_btn) {
                        nm = cb.name;
                        break;
                    }
                }
                if (!nm) {
                    return;
                }
                if (std::strcmp(nm, "loop") == 0) {
                    self->cycle_loop();
                } else if (std::strcmp(nm, "shuf") == 0) {
                    self->toggle_shuffle();
                } else if (std::strcmp(nm, "lights") == 0) {
                    self->toggle_lights();
                } else if (std::strcmp(nm, "move") == 0) {
                    self->toggle_move();
                } else if (std::strcmp(nm, "playlist") == 0) {
                    self->open_list();
                    return;
                } else if (std::strcmp(nm, "radio") == 0) {
                    self->open_radio();
                    return;
                } else if (std::strcmp(nm, "web") == 0) {
                    self->open_web_qr();
                    return;
                } else if (std::strcmp(nm, "head") == 0) {
                    self->switch_mode(ScreenMode::Head);
                    return;
                } else if (std::strcmp(nm, "avatar") == 0) {
                    self->toggle_avatar_style();
                }
                self->refresh_settings_ui();
            },
            LV_EVENT_CLICKED, this);
    }

    // Settings footer: AVATAR (left, above) | BACK (wide right)
    lv_obj_t* btn_cfg_back = lv_button_create(_settings_cont);
    lv_obj_set_pos(btn_cfg_back, 164, 188);
    lv_obj_set_size(btn_cfg_back, 148, 44);
    style_flat_box(btn_cfg_back, COL_PLAY_ON, 10);
    lv_obj_t* lbl_back = lv_label_create(btn_cfg_back);
    lv_obj_center(lbl_back);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(lbl_back, "BACK");
    lv_obj_add_event_cb(
        btn_cfg_back,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->switch_mode(ScreenMode::Main);
        },
        LV_EVENT_CLICKED, this);

    /* =========================== 3. PLAYLIST VIEW ========================== */
    _list_cont = lv_obj_create(_root_screen);
    lv_obj_set_size(_list_cont, W, H);
    lv_obj_set_pos(_list_cont, 0, 0);
    style_flat_box(_list_cont, COL_BG, 0);
    lv_obj_add_flag(_list_cont, LV_OBJ_FLAG_HIDDEN);

    _lbl_list_hdr = lv_label_create(_list_cont);
    lv_obj_set_pos(_lbl_list_hdr, 10, sub_hdr_y);
    lv_obj_set_size(_lbl_list_hdr, 130, 22);
    lv_obj_set_style_text_font(_lbl_list_hdr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_list_hdr, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(_lbl_list_hdr, "< PLAYLIST");
    lv_obj_add_flag(_lbl_list_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        _lbl_list_hdr,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->switch_mode(ScreenMode::Main);
        },
        LV_EVENT_CLICKED, this);

    _lbl_list_count = lv_label_create(_list_cont);
    lv_obj_set_pos(_lbl_list_count, 135, sub_hdr_y + 3);
    lv_obj_set_width(_lbl_list_count, 177);
    lv_obj_set_style_text_align(_lbl_list_count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(_lbl_list_count, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_list_count, lv_color_hex(COL_DIM), 0);

    for (int i = 0; i < PAGE_ROWS; ++i) {
        lv_obj_t* row = lv_button_create(_list_cont);
        _list_rows[i] = row;
        lv_obj_set_pos(row, 8, row_y0 + i * row_step);
        lv_obj_set_size(row, 304, row_h);
        style_flat_box(row, COL_CARD, 8);

        lv_obj_t* rlbl    = lv_label_create(row);
        _list_row_lbls[i] = rlbl;
        lv_obj_set_pos(rlbl, 10, is_fix ? 2 : 4);
        lv_obj_set_width(rlbl, 284);
        lv_label_set_long_mode(rlbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(rlbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(rlbl, lv_color_hex(COL_TEXT), 0);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* self      = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
                lv_obj_t* t_row = static_cast<lv_obj_t*>(lv_event_get_target(e));
                for (int r = 0; r < PAGE_ROWS; ++r) {
                    if (self->_list_rows[r] == t_row) {
                        int track_idx = self->_list_page * PAGE_ROWS + r;
                        if (track_idx < static_cast<int>(self->_tracks.size())) {
                            self->_list_page = track_idx / PAGE_ROWS;
                            self->play_track(track_idx, 0);
                            self->switch_mode(ScreenMode::Main);
                        }
                        break;
                    }
                }
            },
            LV_EVENT_CLICKED, this);
    }

    _lbl_list_empty1 = lv_label_create(_list_cont);
    lv_obj_set_pos(_lbl_list_empty1, 22, top_y + 48);
    lv_obj_set_style_text_font(_lbl_list_empty1, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_list_empty1, lv_color_hex(COL_DIM), 0);

    _lbl_list_empty2 = lv_label_create(_list_cont);
    lv_obj_set_pos(_lbl_list_empty2, 22, top_y + 72);
    lv_obj_set_style_text_font(_lbl_list_empty2, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_list_empty2, lv_color_hex(COL_DIM), 0);

    _lbl_list_info = lv_label_create(_list_cont);
    lv_obj_set_pos(_lbl_list_info, 8, list_info_y);
    lv_obj_set_width(_lbl_list_info, 304);
    lv_label_set_long_mode(_lbl_list_info, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_list_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_list_info, lv_color_hex(COL_DIM), 0);

    auto make_list_footer_btn = [&](int x, int w, const char* txt, uint32_t bg = COL_CARD_HI) {
        lv_obj_t* b = lv_button_create(_list_cont);
        lv_obj_set_pos(b, x, 204);
        lv_obj_set_size(b, w, 30);
        style_flat_box(b, bg, 8);
        lv_obj_t* l = lv_label_create(b);
        lv_obj_center(l);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
        lv_label_set_text(l, txt);
        return b;
    };

    _btn_page_prev = make_list_footer_btn(8, 72, "PREV");
    lv_obj_add_event_cb(
        _btn_page_prev,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            if (self->_list_page > 0) {
                self->_list_page--;
                self->refresh_list_ui();
            }
        },
        LV_EVENT_CLICKED, this);

    _btn_page_next = make_list_footer_btn(85, 72, "NEXT");
    lv_obj_add_event_cb(
        _btn_page_next,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            int pages  = std::max(1, (static_cast<int>(self->_tracks.size()) + PAGE_ROWS - 1) / PAGE_ROWS);
            if (self->_list_page + 1 < pages) {
                self->_list_page++;
                self->refresh_list_ui();
            }
        },
        LV_EVENT_CLICKED, this);

    _btn_open_head = make_list_footer_btn(162, 76, "SETUP");
    lv_obj_add_event_cb(
        _btn_open_head,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->switch_mode(ScreenMode::Settings);
        },
        LV_EVENT_CLICKED, this);

    _btn_exit_app = make_list_footer_btn(243, 69, "BACK", COL_PLAY_ON);
    lv_obj_add_event_cb(
        _btn_exit_app,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->switch_mode(ScreenMode::Main);
        },
        LV_EVENT_CLICKED, this);

    /* =========================== 4. HEAD VIEW ============================== */
    _head_cont = lv_obj_create(_root_screen);
    lv_obj_set_size(_head_cont, W, H);
    lv_obj_set_pos(_head_cont, 0, 0);
    style_flat_box(_head_cont, COL_BG, 0);
    lv_obj_add_flag(_head_cont, LV_OBJ_FLAG_HIDDEN);

    _lbl_head_hdr = lv_label_create(_head_cont);
    lv_obj_set_pos(_lbl_head_hdr, 10, sub_hdr_y + 2);
    lv_obj_set_size(_lbl_head_hdr, 240, 22);
    lv_obj_set_style_text_font(_lbl_head_hdr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_head_hdr, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(_lbl_head_hdr, "< HEAD CALIBRATION");
    lv_obj_add_flag(_lbl_head_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        _lbl_head_hdr,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->switch_mode(ScreenMode::Settings);
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* head_card = lv_obj_create(_head_cont);
    lv_obj_set_pos(head_card, 20, top_y + 24);
    lv_obj_set_size(head_card, 280, is_fix ? 128 : 150);
    style_flat_box(head_card, COL_CARD, 12);

    lv_obj_t* lbl_obj_cap = lv_label_create(_head_cont);
    lv_obj_set_pos(lbl_obj_cap, 56, top_y + 34);
    lv_obj_set_style_text_font(lbl_obj_cap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_obj_cap, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(lbl_obj_cap, "TARGET");

    lv_obj_t* lbl_real_cap = lv_label_create(_head_cont);
    lv_obj_set_pos(lbl_real_cap, 206, top_y + 34);
    lv_obj_set_style_text_font(lbl_real_cap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_real_cap, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(lbl_real_cap, "ACTUAL");

    _lbl_head_target = lv_label_create(_head_cont);
    lv_obj_set_pos(_lbl_head_target, 64, top_y + 50);
    lv_obj_set_style_text_font(_lbl_head_target, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lbl_head_target, lv_color_hex(COL_YELLOW), 0);

    _lbl_head_real = lv_label_create(_head_cont);
    lv_obj_set_pos(_lbl_head_real, 206, top_y + 50);
    lv_obj_set_style_text_font(_lbl_head_real, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_lbl_head_real, lv_color_hex(COL_TEXT), 0);

    _btn_head_minus = lv_button_create(_head_cont);
    lv_obj_set_pos(_btn_head_minus, 36, top_y + 90);
    lv_obj_set_size(_btn_head_minus, 84, 50);
    style_flat_box(_btn_head_minus, COL_CARD_HI, 10);
    lv_obj_t* lbl_minus = lv_label_create(_btn_head_minus);
    lv_obj_center(lbl_minus);
    lv_obj_set_style_text_font(lbl_minus, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_minus, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(lbl_minus, "-");
    lv_obj_add_event_cb(
        _btn_head_minus,
        [](lv_event_t* e) {
            auto* self        = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->_y_home_deg = std::max(0, self->_y_home_deg - HEAD_STEP_DEG);
            self->move_home(500);
            self->refresh_head_ui();
        },
        LV_EVENT_CLICKED, this);

    _btn_head_plus = lv_button_create(_head_cont);
    lv_obj_set_pos(_btn_head_plus, 200, top_y + 90);
    lv_obj_set_size(_btn_head_plus, 84, 50);
    style_flat_box(_btn_head_plus, COL_CARD_HI, 10);
    lv_obj_t* lbl_plus = lv_label_create(_btn_head_plus);
    lv_obj_center(lbl_plus);
    lv_obj_set_style_text_font(lbl_plus, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_plus, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(lbl_plus, "+");
    lv_obj_add_event_cb(
        _btn_head_plus,
        [](lv_event_t* e) {
            auto* self        = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->_y_home_deg = std::min(90, self->_y_home_deg + HEAD_STEP_DEG);
            self->move_home(500);
            self->refresh_head_ui();
        },
        LV_EVENT_CLICKED, this);

    _btn_head_save = lv_button_create(_head_cont);
    lv_obj_set_pos(_btn_head_save, 70, 198);
    lv_obj_set_size(_btn_head_save, 180, 34);
    style_flat_box(_btn_head_save, COL_PLAY_ON, 10);
    lv_obj_t* lbl_save = lv_label_create(_btn_head_save);
    lv_obj_center(lbl_save);
    lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_save, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(lbl_save, "SAVE");
    lv_obj_add_event_cb(
        _btn_head_save,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->save_head_cfg();
            self->set_status("HEAD SAVED");
            self->switch_mode(ScreenMode::Main);
        },
        LV_EVENT_CLICKED, this);

    /* =========================== 5. RADIO VIEW ============================= */
    _radio_cont = lv_obj_create(_root_screen);
    lv_obj_set_size(_radio_cont, W, H);
    lv_obj_set_pos(_radio_cont, 0, 0);
    style_flat_box(_radio_cont, COL_BG, 0);
    lv_obj_add_flag(_radio_cont, LV_OBJ_FLAG_HIDDEN);

    _lbl_radio_hdr = lv_label_create(_radio_cont);
    lv_obj_set_pos(_lbl_radio_hdr, 10, sub_hdr_y);
    lv_obj_set_size(_lbl_radio_hdr, 150, 22);
    lv_obj_set_style_text_font(_lbl_radio_hdr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_radio_hdr, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(_lbl_radio_hdr, "< RADIO");
    lv_obj_add_flag(_lbl_radio_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        _lbl_radio_hdr,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->switch_mode(ScreenMode::Main);
        },
        LV_EVENT_CLICKED, this);

    _lbl_radio_count = lv_label_create(_radio_cont);
    lv_obj_set_pos(_lbl_radio_count, 160, sub_hdr_y + 3);
    lv_obj_set_width(_lbl_radio_count, 152);
    lv_obj_set_style_text_align(_lbl_radio_count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(_lbl_radio_count, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_radio_count, lv_color_hex(COL_DIM), 0);

    for (int i = 0; i < PAGE_ROWS; ++i) {
        lv_obj_t* row  = lv_button_create(_radio_cont);
        _radio_rows[i] = row;
        lv_obj_set_pos(row, 8, row_y0 + i * row_step);
        lv_obj_set_size(row, 304, row_h);
        style_flat_box(row, COL_CARD, 8);

        lv_obj_t* rlbl     = lv_label_create(row);
        _radio_row_lbls[i] = rlbl;
        lv_obj_set_pos(rlbl, 10, is_fix ? 2 : 4);
        lv_obj_set_width(rlbl, 284);
        lv_label_set_long_mode(rlbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(rlbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(rlbl, lv_color_hex(COL_TEXT), 0);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* self      = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
                lv_obj_t* t_row = static_cast<lv_obj_t*>(lv_event_get_target(e));
                for (int r = 0; r < PAGE_ROWS; ++r) {
                    if (self->_radio_rows[r] == t_row) {
                        int st_idx = self->_radio_page * PAGE_ROWS + r;
                        if (self->_wifi_svc && st_idx < self->_wifi_svc->station_count()) {
                            self->play_radio(st_idx);
                            self->switch_mode(ScreenMode::Main);
                        }
                        break;
                    }
                }
            },
            LV_EVENT_CLICKED, this);
    }

    _lbl_radio_info = lv_label_create(_radio_cont);
    lv_obj_set_pos(_lbl_radio_info, 8, list_info_y);
    lv_obj_set_width(_lbl_radio_info, 304);
    lv_label_set_long_mode(_lbl_radio_info, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(_lbl_radio_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_radio_info, lv_color_hex(COL_DIM), 0);

    auto make_radio_footer_btn = [&](int x, int w, const char* txt, uint32_t bg = COL_CARD_HI) {
        lv_obj_t* b = lv_button_create(_radio_cont);
        lv_obj_set_pos(b, x, 204);
        lv_obj_set_size(b, w, 30);
        style_flat_box(b, bg, 8);
        lv_obj_t* l = lv_label_create(b);
        lv_obj_center(l);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
        lv_label_set_text(l, txt);
        return b;
    };

    lv_obj_t* btn_rad_prev = make_radio_footer_btn(8, 72, "PREV");
    lv_obj_add_event_cb(
        btn_rad_prev,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            if (self->_radio_page > 0) {
                self->_radio_page--;
                self->refresh_radio_ui();
            }
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_rad_next = make_radio_footer_btn(85, 72, "NEXT");
    lv_obj_add_event_cb(
        btn_rad_next,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            int count  = self->_wifi_svc ? self->_wifi_svc->station_count() : 0;
            int pages  = std::max(1, (count + PAGE_ROWS - 1) / PAGE_ROWS);
            if (self->_radio_page + 1 < pages) {
                self->_radio_page++;
                self->refresh_radio_ui();
            }
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_rad_sd = make_radio_footer_btn(162, 72, "SD");
    lv_obj_add_event_cb(
        btn_rad_sd,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->open_list();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* btn_rad_web = make_radio_footer_btn(239, 73, "WEB/QR", COL_ACCENT_DK);
    lv_obj_add_event_cb(
        btn_rad_web,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->open_web_qr();
        },
        LV_EVENT_CLICKED, this);

    /* =========================== 6. WEB / QR VIEW ========================== */
    _web_cont = lv_obj_create(_root_screen);
    lv_obj_set_size(_web_cont, W, H);
    lv_obj_set_pos(_web_cont, 0, 0);
    style_flat_box(_web_cont, COL_BG, 0);
    lv_obj_add_flag(_web_cont, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* lbl_web_hdr = lv_label_create(_web_cont);
    lv_obj_set_pos(lbl_web_hdr, 10, sub_hdr_y + 2);
    lv_obj_set_size(lbl_web_hdr, 240, 22);
    lv_obj_set_style_text_font(lbl_web_hdr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_web_hdr, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(lbl_web_hdr, "< WEB REMOTE & UPLOAD");
    lv_obj_add_flag(lbl_web_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        lbl_web_hdr,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->switch_mode(ScreenMode::Main);
        },
        LV_EVENT_CLICKED, this);

    _web_qr_box = lv_obj_create(_web_cont);
    lv_obj_set_pos(_web_qr_box, 12, top_y + 24);
    lv_obj_set_size(_web_qr_box, 120, 120);
    style_flat_box(_web_qr_box, COL_WHITE, 8);
    lv_obj_add_flag(_web_qr_box, LV_OBJ_FLAG_HIDDEN);

    _web_qr = lv_qrcode_create(_web_qr_box);
    lv_qrcode_set_size(_web_qr, 108);
    lv_qrcode_set_dark_color(_web_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(_web_qr, lv_color_hex(0xFFFFFF));
    lv_obj_center(_web_qr);

    _lbl_web_status = lv_label_create(_web_cont);
    lv_obj_set_pos(_lbl_web_status, 144, top_y + 26);
    lv_obj_set_width(_lbl_web_status, 168);
    lv_label_set_long_mode(_lbl_web_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_web_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_web_status, lv_color_hex(COL_YELLOW), 0);
    lv_label_set_text(_lbl_web_status, "Starting Wi-Fi...");

    _lbl_web_url = lv_label_create(_web_cont);
    lv_obj_set_pos(_lbl_web_url, 144, top_y + 50);
    lv_obj_set_width(_lbl_web_url, 168);
    lv_label_set_long_mode(_lbl_web_url, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_web_url, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_web_url, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(_lbl_web_url, "");

    _lbl_web_hint = lv_label_create(_web_cont);
    lv_obj_set_pos(_lbl_web_hint, 144, top_y + 76);
    lv_obj_set_width(_lbl_web_hint, 168);
    lv_label_set_long_mode(_lbl_web_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_lbl_web_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_web_hint, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(_lbl_web_hint, "Scan QR on same Wi-Fi to browse, play radio, or upload MP3s.");

    lv_obj_t* btn_web_back = lv_button_create(_web_cont);
    lv_obj_set_pos(btn_web_back, 16, 192);
    lv_obj_set_size(btn_web_back, 288, 38);
    style_flat_box(btn_web_back, COL_PLAY_ON, 10);
    lv_obj_t* lbl_web_back = lv_label_create(btn_web_back);
    lv_obj_center(lbl_web_back);
    lv_obj_set_style_text_font(lbl_web_back, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_web_back, lv_color_hex(COL_WHITE), 0);
    lv_label_set_text(lbl_web_back, "BACK TO PLAYER (KEEP SERVER ON)");
    lv_obj_add_event_cb(
        btn_web_back,
        [](lv_event_t* e) {
            auto* self = static_cast<AppMusicPlayer*>(lv_event_get_user_data(e));
            self->switch_mode(ScreenMode::Main);
        },
        LV_EVENT_CLICKED, this);

    refresh_battery_ui();
}

void AppMusicPlayer::destroy_ui()
{
    if (_main_cont) {
        lv_obj_delete(_main_cont);
        _main_cont   = nullptr;
        _stage_cont  = nullptr;
        _avatar_img  = nullptr;
        _face_card   = nullptr;
        _face_eye_l  = nullptr;
        _face_eye_r  = nullptr;
        _face_mouth  = nullptr;
    }
    if (_settings_cont) {
        lv_obj_delete(_settings_cont);
        _settings_cont = nullptr;
    }
    if (_list_cont) {
        lv_obj_delete(_list_cont);
        _list_cont = nullptr;
    }
    if (_head_cont) {
        lv_obj_delete(_head_cont);
        _head_cont = nullptr;
    }
    if (_radio_cont) {
        lv_obj_delete(_radio_cont);
        _radio_cont = nullptr;
    }
    if (_web_cont) {
        lv_obj_delete(_web_cont);
        _web_cont = nullptr;
        _web_qr_box = nullptr;
        _web_qr     = nullptr;
    }
}

void AppMusicPlayer::switch_mode(ScreenMode new_mode)
{
    _mode = new_mode;
    if (!_main_cont || !_settings_cont || !_list_cont || !_head_cont || !_radio_cont || !_web_cont) {
        return;
    }

    lv_obj_add_flag(_main_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_settings_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_list_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_head_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_radio_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_web_cont, LV_OBJ_FLAG_HIDDEN);

    if (_mode == ScreenMode::Main) {
        lv_obj_remove_flag(_main_cont, LV_OBJ_FLAG_HIDDEN);
        uint32_t now = GetHAL().millis();
        refresh_main_ui(now);
        refresh_avatar_anim(now);
    } else if (_mode == ScreenMode::Settings) {
        lv_obj_remove_flag(_settings_cont, LV_OBJ_FLAG_HIDDEN);
        refresh_settings_ui();
    } else if (_mode == ScreenMode::List) {
        lv_obj_remove_flag(_list_cont, LV_OBJ_FLAG_HIDDEN);
        refresh_list_ui();
    } else if (_mode == ScreenMode::Head) {
        lv_obj_remove_flag(_head_cont, LV_OBJ_FLAG_HIDDEN);
        refresh_head_ui();
    } else if (_mode == ScreenMode::Radio) {
        lv_obj_remove_flag(_radio_cont, LV_OBJ_FLAG_HIDDEN);
        refresh_radio_ui();
    } else if (_mode == ScreenMode::WebQr) {
        lv_obj_remove_flag(_web_cont, LV_OBJ_FLAG_HIDDEN);
        refresh_web_qr_ui();
    }
}

void AppMusicPlayer::open_radio()
{
    if (_wifi_svc) {
        _wifi_svc->load_stations(_sd_mounted);
        _radio_page = _radio_index / PAGE_ROWS;
    }
    switch_mode(ScreenMode::Radio);
}

void AppMusicPlayer::open_web_qr()
{
    if (_wifi_svc) {
        _wifi_svc->request_web_start();
    }
    switch_mode(ScreenMode::WebQr);
}

void AppMusicPlayer::open_list()
{
    if (_tracks.empty()) {
        if (!_sd_mounted) {
            _sd_mounted = mount_sd();
        }
        scan_tracks();
        if (!_tracks.empty()) {
            auto info    = probe_audio(std::string(_tracks[0].path.c_str()), _tracks[0].is_mp3);
            _duration_ms = info.duration_ms;
            _byte_rate   = info.byte_rate;
        }
    }
    if (!_tracks.empty()) {
        _list_page = _index / PAGE_ROWS;
    }
    switch_mode(ScreenMode::List);
}

/* -------------------------------------------------------------------------- */
/*                             UI View Updates                                */
/* -------------------------------------------------------------------------- */

void AppMusicPlayer::refresh_battery_ui()
{
    if (!_lbl_batt || !_batt_fill) {
        return;
    }
    _batt_pct = GetHAL().getBatteryLevel();
    _batt_chg = GetHAL().isBatteryCharging();

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", _batt_pct);
    lv_label_set_text(_lbl_batt, buf);

    int inner = 18;
    int fw    = std::clamp((_batt_pct * inner) / 100, 1, inner);
    lv_obj_set_width(_batt_fill, fw);

    uint32_t col = COL_RED;
    if (_batt_pct > 50) {
        col = COL_GREEN;
    } else if (_batt_pct > 20) {
        col = COL_YELLOW;
    }
    lv_obj_set_style_bg_color(_batt_fill, lv_color_hex(col), 0);
}

void AppMusicPlayer::refresh_button_styles()
{
    bool is_playing = _playing.load();
    for (int i = 0; i < 4; ++i) {
        const char* nm = _main_btns[i].name;
        lv_obj_t* btn  = _main_btns[i].btn;
        lv_obj_t* lbl  = _main_btns[i].label;
        if (!btn || !lbl) {
            continue;
        }

        uint32_t bg_col  = COL_CARD;
        uint32_t txt_col = COL_TEXT;
        const char* text = "";

        if (std::strcmp(nm, "prev") == 0) {
            text = LV_SYMBOL_PREV;
        } else if (std::strcmp(nm, "play") == 0) {
            bg_col  = is_playing ? COL_PLAY_ON : COL_CARD;
            txt_col = is_playing ? COL_WHITE : COL_TEXT;
            text    = is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
        } else if (std::strcmp(nm, "next") == 0) {
            text = LV_SYMBOL_NEXT;
        } else if (std::strcmp(nm, "settings") == 0) {
            text = LV_SYMBOL_SETTINGS;
        }

        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_col), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(txt_col), 0);
        lv_label_set_text(lbl, text);
    }
}

void AppMusicPlayer::refresh_settings_ui()
{
    if (!_settings_cont) {
        return;
    }

    bool use_naruto = (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto);

    for (int i = 0; i < 9; ++i) {
        const char* nm = _cfg_btns[i].name;
        lv_obj_t* btn  = _cfg_btns[i].btn;
        lv_obj_t* lbl  = _cfg_btns[i].label;
        if (!btn || !lbl) {
            continue;
        }

        uint32_t bg_col  = COL_CARD;
        uint32_t txt_col = COL_TEXT;
        std::string text;

        if (std::strcmp(nm, "loop") == 0) {
            bg_col  = (_loop_mode != 0) ? COL_ACCENT_DK : COL_CARD;
            txt_col = (_loop_mode != 0) ? COL_YELLOW : COL_DIM;
            text    = std::string("LOOP\n") + LOOP_NAMES[_loop_mode];
        } else if (std::strcmp(nm, "shuf") == 0) {
            bg_col  = _shuffle_on ? COL_ACCENT_DK : COL_CARD;
            txt_col = _shuffle_on ? COL_WHITE : COL_DIM;
            text    = std::string("SHUFFLE\n") + (_shuffle_on ? "ON" : "OFF");
        } else if (std::strcmp(nm, "lights") == 0) {
            bg_col  = _lights_on ? COL_ACCENT_DK : COL_CARD;
            txt_col = _lights_on ? COL_WHITE : COL_DIM;
            text    = std::string("LIGHTS\n") + (_lights_on ? "ON" : "OFF");
        } else if (std::strcmp(nm, "move") == 0) {
            bg_col  = _robot_move ? COL_ACCENT_DK : COL_CARD;
            txt_col = _robot_move ? COL_WHITE : COL_DIM;
            text    = std::string("MOVE\n") + (_robot_move ? "ON" : "OFF");
        } else if (std::strcmp(nm, "playlist") == 0) {
            bg_col  = (!_radio_mode.load()) ? COL_ACCENT_DK : COL_CARD_HI;
            txt_col = COL_WHITE;
            text    = "SD LIST";
        } else if (std::strcmp(nm, "radio") == 0) {
            bg_col  = _radio_mode.load() ? COL_PURPLE : COL_CARD_HI;
            txt_col = COL_WHITE;
            text    = "RADIO";
        } else if (std::strcmp(nm, "web") == 0) {
            bool web_on = _wifi_svc && _wifi_svc->is_web_running();
            bg_col      = web_on ? COL_PLAY_ON : COL_CARD_HI;
            txt_col     = COL_WHITE;
            text        = web_on ? "WEB\nON" : "WEB/QR";
        } else if (std::strcmp(nm, "head") == 0) {
            bg_col  = COL_CARD_HI;
            txt_col = COL_TEXT;
            text    = "HEAD";
        } else if (std::strcmp(nm, "avatar") == 0) {
            bg_col  = use_naruto ? COL_ACCENT_DK : COL_CARD_HI;
            txt_col = use_naruto ? COL_YELLOW : COL_WHITE;
            text    = std::string("AVATAR\n") + (use_naruto ? "NARUTO" : "CLASSIC");
        }

        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_col), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(txt_col), 0);
        lv_label_set_text(lbl, text.c_str());
    }
}

void AppMusicPlayer::refresh_main_ui(uint32_t now)
{
    (void)now;
    if (!_main_cont) {
        return;
    }

    bool is_rad = _radio_mode.load();
    if (is_rad && _wifi_svc) {
        auto stations = _wifi_svc->get_stations();
        std::string sname = (_radio_index >= 0 && _radio_index < static_cast<int>(stations.size()))
                                ? stations[_radio_index].name
                                : "Internet Radio";
        lv_obj_set_style_text_color(_lbl_title, lv_color_hex(COL_YELLOW), 0);
        lv_label_set_text(_lbl_title, clip_text(sname, 28).c_str());

        char rnum[32];
        std::snprintf(rnum, sizeof(rnum), "FM %d/%d", _radio_index + 1, std::max(1, static_cast<int>(stations.size())));
        lv_label_set_text(_lbl_track_num, rnum);

        lv_label_set_text(_lbl_elapsed, "LIVE");
        lv_label_set_text(_lbl_duration, "RADIO");
        lv_obj_set_style_bg_color(_bar_fill, lv_color_hex(COL_PURPLE), 0);
        lv_obj_set_width(_bar_fill, _playing.load() ? BAR_W : 0);
    } else {
        lv_obj_set_style_bg_color(_bar_fill, lv_color_hex(COL_ACCENT), 0);
        if (!_tracks.empty()) {
            lv_obj_set_style_text_color(_lbl_title, lv_color_hex(COL_TEXT), 0);
            std::string cur_title(_tracks[_index].title.c_str(), _tracks[_index].title.size());
            lv_label_set_text(_lbl_title, clip_text(cur_title, 28).c_str());

            char tnum[32];
            std::snprintf(tnum, sizeof(tnum), "%d/%d", _index + 1, static_cast<int>(_tracks.size()));
            lv_label_set_text(_lbl_track_num, tnum);
        } else {
            lv_obj_set_style_text_color(_lbl_title, lv_color_hex(COL_RED), 0);
            lv_label_set_text(_lbl_title, "NO MUSIC");
            lv_label_set_text(_lbl_track_num, "");
        }

        int32_t el_ms  = _elapsed_ms.load();
        int32_t dur_ms = _duration_ms.load();
        lv_label_set_text(_lbl_elapsed, fmt_time(el_ms).c_str());
        lv_label_set_text(_lbl_duration, fmt_time(dur_ms).c_str());

        float ratio = 0.0f;
        if (dur_ms > 0) {
            ratio = std::clamp(static_cast<float>(el_ms) / static_cast<float>(dur_ms), 0.0f, 1.0f);
        }
        int fw = static_cast<int>(BAR_W * ratio);
        lv_obj_set_width(_bar_fill, std::max(0, fw));
    }

    std::string vol_line = "VOL " + std::to_string(_volume) + " " + APP_VERSION;
    if (_wifi_svc && _wifi_svc->is_web_running()) {
        vol_line += " WEB";
    }
    lv_label_set_text(_lbl_vol_ver, vol_line.c_str());

    std::string status_line;
    uint32_t status_col = COL_DIM;
    if (is_rad && _wifi_svc) {
        if (!_playing.load()) {
            status_line = "RADIO PAUSED";
        } else if (_wifi_svc->is_radio_connected()) {
            size_t kb   = _wifi_svc->radio_buffered_bytes() / 1024;
            status_line = "LIVE STREAM  BUF " + std::to_string(kb) + "K";
            status_col  = COL_GREEN;
        } else {
            status_line = "BUFFERING RADIO...";
            status_col  = COL_YELLOW;
        }
    } else {
        status_line = std::string(_playing.load() ? "PLAYING" : "PAUSED") + "  LOOP " + LOOP_NAMES[_loop_mode];
    }

    if (!_status_msg.empty()) {
        status_line = _status_msg;
        status_col  = COL_YELLOW;
    } else if (!_audio_err.empty()) {
        status_line = _audio_err;
        status_col  = COL_RED;
    }
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(status_col), 0);
    lv_label_set_text(_lbl_status, clip_text(status_line, 26).c_str());

    refresh_button_styles();
}

void AppMusicPlayer::apply_avatar_style()
{
    bool show_naruto =
        (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto &&
         _img_naruto_calm.data != nullptr);

    if (_avatar_img) {
        if (show_naruto) {
            lv_obj_remove_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_avatar_img, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (_face_card) {
        if (show_naruto) {
            lv_obj_add_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(_face_card, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AppMusicPlayer::refresh_avatar_anim(uint32_t now)
{
    if (!_avatar_img && !_face_card) {
        return;
    }

    apply_avatar_style();
    bool is_playing  = _playing.load();
    bool show_naruto =
        (stackchan::avatar::getAvatarStyle() == stackchan::avatar::AvatarStyle::Naruto &&
         _img_naruto_calm.data != nullptr);

    int offset_y = 0;
    int offset_x = 0;

    if (show_naruto && _avatar_img) {
        // Switch Naruto expression between Happy (playing) and Calm (paused)
        if (is_playing != _avatar_showing_happy) {
            _avatar_showing_happy = is_playing;
            if (is_playing && _img_naruto_happy.data != nullptr) {
                lv_image_set_src(_avatar_img, &_img_naruto_happy);
            } else if (_img_naruto_calm.data != nullptr) {
                lv_image_set_src(_avatar_img, &_img_naruto_calm);
            }
        }

        // Gentle bounce on the beat while playing, calm breathing while paused
        if (is_playing) {
            float beat_phase = static_cast<float>(now % BEAT_MS) / static_cast<float>(BEAT_MS);
            offset_y         = static_cast<int>(-std::abs(std::sin(beat_phase * 3.14159f)) * 5.0f) + 2;
            offset_x         = static_cast<int>(std::sin((now / static_cast<float>(BEAT_MS)) * 1.5708f) * 3.0f);
        } else {
            float breath = std::sin(static_cast<float>(now % 3000) / 3000.0f * 6.28318f);
            offset_y     = static_cast<int>(breath * 2.0f);
        }
        lv_obj_align(_avatar_img, LV_ALIGN_CENTER, offset_x, offset_y);
    } else if (_face_card) {
        // Animate the classic StackChan face on the beat
        int mouth_h = 6;
        int mouth_w = 38;
        if (is_playing) {
            float beat_phase = static_cast<float>(now % BEAT_MS) / static_cast<float>(BEAT_MS);
            float pulse      = std::abs(std::sin(beat_phase * 3.14159f));
            offset_y         = static_cast<int>(-pulse * 5.0f) + 2;
            offset_x         = static_cast<int>(std::sin((now / static_cast<float>(BEAT_MS)) * 1.5708f) * 3.0f);
            mouth_h          = 10 + static_cast<int>(pulse * 12.0f);
            mouth_w          = 30 + static_cast<int>(pulse * 12.0f);
            lv_obj_set_style_border_color(_face_card, lv_color_hex(COL_ACCENT), 0);
        } else {
            float breath = std::sin(static_cast<float>(now % 3000) / 3000.0f * 6.28318f);
            offset_y     = static_cast<int>(breath * 2.0f);
            lv_obj_set_style_border_color(_face_card, lv_color_hex(COL_CARD_HI), 0);
        }
        lv_obj_align(_face_card, LV_ALIGN_CENTER, offset_x, offset_y);

        // Occasional natural blink
        bool blink = ((now % 3400) > 3250);
        int eye_h  = blink ? 4 : 18;
        if (_face_eye_l) {
            lv_obj_set_size(_face_eye_l, 18, eye_h);
        }
        if (_face_eye_r) {
            lv_obj_set_size(_face_eye_r, 18, eye_h);
        }
        if (_face_mouth) {
            lv_obj_set_size(_face_mouth, mouth_w, mouth_h);
            lv_obj_set_style_radius(_face_mouth, is_playing ? 10 : 4, 0);
        }
    }

    // Floating musical notes on the sides
    const int base_xs[4] = {18, 54, 232, 268};
    for (int i = 0; i < 4; ++i) {
        if (!_note_lbls[i]) {
            continue;
        }
        if (is_playing) {
            lv_obj_remove_flag(_note_lbls[i], LV_OBJ_FLAG_HIDDEN);
            float phase = static_cast<float>((now + i * 690) % 2760) / 2760.0f;
            int ny      = 92 - static_cast<int>(phase * 72.0f);
            int nx      = base_xs[i] + static_cast<int>(std::sin(phase * 6.28318f + i) * 5.0f);
            lv_obj_set_pos(_note_lbls[i], nx, ny);
            lv_obj_set_style_text_color(_note_lbls[i], lv_color_hex(PALETTE[((now / BEAT_MS) + i) % 8]), 0);
        } else {
            lv_obj_add_flag(_note_lbls[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AppMusicPlayer::refresh_list_ui()
{
    if (!_list_cont) {
        return;
    }

    int track_count = static_cast<int>(_tracks.size());
    int pages       = std::max(1, (track_count + PAGE_ROWS - 1) / PAGE_ROWS);
    _list_page      = std::clamp(_list_page, 0, pages - 1);

    char cbuf[48];
    std::snprintf(cbuf, sizeof(cbuf), "%d TRACKS  %d/%d", track_count, _list_page + 1, pages);
    lv_label_set_text(_lbl_list_count, cbuf);

    for (int i = 0; i < PAGE_ROWS; ++i) {
        int t = _list_page * PAGE_ROWS + i;
        if (t < track_count) {
            lv_obj_remove_flag(_list_rows[i], LV_OBJ_FLAG_HIDDEN);
            bool is_cur = (t == _index);
            lv_obj_set_style_bg_color(_list_rows[i], lv_color_hex(is_cur ? COL_ACCENT_DK : COL_CARD), 0);
            lv_obj_set_style_text_color(_list_row_lbls[i], lv_color_hex(is_cur ? COL_WHITE : COL_TEXT), 0);
            std::string row_title(_tracks[t].title.c_str(), _tracks[t].title.size());
            lv_label_set_text(_list_row_lbls[i], clip_text(row_title, 26).c_str());
        } else {
            lv_obj_add_flag(_list_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (track_count == 0) {
        lv_obj_remove_flag(_lbl_list_empty1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_lbl_list_empty2, LV_OBJ_FLAG_HIDDEN);
        if (!_sd_mounted) {
            lv_label_set_text(_lbl_list_empty1, "SD NOT MOUNTED");
            lv_label_set_text(_lbl_list_empty2, "FORMAT AS FAT32");
        } else if (_scan_error) {
            lv_label_set_text(_lbl_list_empty1, "SD READ ERROR");
            lv_label_set_text(_lbl_list_empty2, "TOO MANY FILES?");
        } else {
            lv_label_set_text(_lbl_list_empty1, "NO WAV/MP3 IN /SD");
            lv_label_set_text(_lbl_list_empty2, "CHECK ROOT FOLDER");
        }
    } else {
        lv_obj_add_flag(_lbl_list_empty1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_lbl_list_empty2, LV_OBJ_FLAG_HIDDEN);
    }

    uint32_t free_kb = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    char ibuf[96];
    std::snprintf(ibuf, sizeof(ibuf), "%d ITEMS  %d AUDIO  M:%uK", _scan_entries, track_count,
                  static_cast<unsigned>(free_kb));
    std::string info_str(ibuf);
    if (_scan_other_n > 0) {
        info_str += "  " + _scan_other_ext + ":" + std::to_string(_scan_other_n);
    }
    lv_label_set_text(_lbl_list_info, clip_text(info_str, 44).c_str());
}

void AppMusicPlayer::refresh_head_ui()
{
    if (!_head_cont) {
        return;
    }

    lv_label_set_text(_lbl_head_target, std::to_string(_y_home_deg).c_str());

    int real_pitch = GetStackChan().motion().getCurrentPitchAngle();
    int real_y_deg = pitch_to_py_y_deg(real_pitch);
    lv_label_set_text(_lbl_head_real, std::to_string(real_y_deg).c_str());
}

void AppMusicPlayer::refresh_radio_ui()
{
    if (!_radio_cont || !_wifi_svc) {
        return;
    }

    auto stations = _wifi_svc->get_stations();
    int count     = static_cast<int>(stations.size());
    int pages     = std::max(1, (count + PAGE_ROWS - 1) / PAGE_ROWS);
    _radio_page   = std::clamp(_radio_page, 0, pages - 1);

    char cbuf[48];
    std::snprintf(cbuf, sizeof(cbuf), "%d STATIONS  %d/%d", count, _radio_page + 1, pages);
    lv_label_set_text(_lbl_radio_count, cbuf);

    for (int i = 0; i < PAGE_ROWS; ++i) {
        int s = _radio_page * PAGE_ROWS + i;
        if (s < count) {
            lv_obj_remove_flag(_radio_rows[i], LV_OBJ_FLAG_HIDDEN);
            bool is_cur = (_radio_mode.load() && s == _radio_index);
            lv_obj_set_style_bg_color(_radio_rows[i], lv_color_hex(is_cur ? COL_PURPLE : COL_CARD), 0);
            lv_obj_set_style_text_color(_radio_row_lbls[i], lv_color_hex(is_cur ? COL_WHITE : COL_TEXT), 0);
            lv_label_set_text(_radio_row_lbls[i], clip_text(stations[s].name, 26).c_str());
        } else {
            lv_obj_add_flag(_radio_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    std::string info = _wifi_svc->is_wifi_connected()
                           ? ("Wi-Fi: " + _wifi_svc->get_ip_address() + "  (custom: /sd/radio.txt)")
                           : "Tap any station to connect Wi-Fi & stream";
    lv_label_set_text(_lbl_radio_info, clip_text(info, 46).c_str());
}

void AppMusicPlayer::refresh_web_qr_ui()
{
    if (!_web_cont || !_wifi_svc) {
        return;
    }

    if (_wifi_svc->is_web_running()) {
        std::string ip  = _wifi_svc->get_ip_address();
        std::string url = "http://" + ip + "/";

        lv_obj_set_style_text_color(_lbl_web_status, lv_color_hex(COL_GREEN), 0);
        lv_label_set_text(_lbl_web_status, "SERVER READY");
        lv_label_set_text(_lbl_web_url, url.c_str());
        lv_obj_remove_flag(_web_qr_box, LV_OBJ_FLAG_HIDDEN);

        if (_web_qr && url != _last_qr_url) {
            lv_qrcode_update(_web_qr, url.c_str(), static_cast<uint32_t>(url.size()));
            _last_qr_url = url;
        }
    } else {
        lv_obj_add_flag(_web_qr_box, LV_OBJ_FLAG_HIDDEN);
        std::string st = _wifi_svc->get_wifi_status();
        lv_obj_set_style_text_color(_lbl_web_status, lv_color_hex(COL_YELLOW), 0);
        lv_label_set_text(_lbl_web_status, clip_text(st, 22).c_str());
        lv_label_set_text(_lbl_web_url, "Waiting for Wi-Fi...");
    }
}
