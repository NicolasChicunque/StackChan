/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "alarm_service.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <settings.h>

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {

constexpr const char* TAG             = "ALARM_SVC";
constexpr const char* NVS_NS          = "alarm_clock";
constexpr const char* KEY_ALARMS_JSON = "alarms_v1";
constexpr const char* KEY_SNOOZE_MIN  = "snooze_min";
constexpr const char* KEY_VOLUME      = "volume";
constexpr const char* KEY_SNOOZE_AT   = "snooze_at";
constexpr const char* KEY_SNOOZE_IDX  = "snooze_idx";
constexpr const char* KEY_BOOT_RING   = "boot_ring";
constexpr const char* KEY_TIMER_END   = "timer_end";
constexpr const char* KEY_TIMER_REM   = "timer_rem";
constexpr const char* KEY_TIMER_PRE   = "timer_pre";

constexpr time_t MIN_VALID_EPOCH      = 1704067200;  // 2024-01-01 00:00:00 UTC

const char* const LABEL_NAMES[6] = {
    "Alarm", "Wake Up", "Work", "Gym", "Study", "Meds",
};

const char* const TONE_NAMES[5] = {
    "Chime", "Classic", "Radar", "Energetic", "SD MP3",
};

}  // namespace

AlarmService& AlarmService::instance()
{
    static AlarmService svc;
    return svc;
}

const char* AlarmService::labelName(uint8_t idx)
{
    return LABEL_NAMES[idx % 6];
}

const char* AlarmService::toneName(uint8_t idx)
{
    return TONE_NAMES[idx % 5];
}

std::string AlarmService::formatDays(uint8_t mask)
{
    if (mask == 0) {
        return "Once";
    }
    if (mask == 0x7F) {
        return "Daily";
    }
    if (mask == 0x3E) {
        return "Mon-Fri";
    }
    if (mask == 0x41) {
        return "Weekends";
    }
    static const char* const D[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    std::string s;
    for (int i = 0; i < 7; ++i) {
        if (mask & (1u << i)) {
            if (!s.empty()) {
                s += ' ';
            }
            s += D[i];
        }
    }
    return s;
}

void AlarmService::start(int launcher_page_index, int app_id)
{
    _launcher_page_idx = launcher_page_index;
    _app_id            = app_id;

    if (_started.exchange(true)) {
        return;
    }

    int boot_ring = -1;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        load_from_nvs_locked();
        boot_ring = consume_boot_ring_nvs();
    }

    if (boot_ring >= 0) {
        _pending_ring_idx  = boot_ring;
        _launcher_open_req = true;
        mclog::tagInfo(TAG, "Restored pending ring alarm #{} after warm reboot", boot_ring);
    }

    // Internal SRAM stack (safe for NVS access from background task)
    xTaskCreatePinnedToCore(bg_task_entry, "alarm_bg", 3584, this, 2, nullptr, 0);
}

void AlarmService::load_from_nvs_locked()
{
    Settings s(NVS_NS, false);
    _snooze_min       = std::clamp<int>(s.GetInt(KEY_SNOOZE_MIN, 5), 1, 30);
    _volume           = std::clamp<int>(s.GetInt(KEY_VOLUME, 80), 30, 100);
    _snooze_alarm_idx = s.GetInt(KEY_SNOOZE_IDX, -1);

    std::string snooze_str = s.GetString(KEY_SNOOZE_AT, "0");
    _snooze_until          = std::strtoll(snooze_str.c_str(), nullptr, 10);

    _timer_preset_sec      = static_cast<uint32_t>(std::clamp<int>(s.GetInt(KEY_TIMER_PRE, 300), 10, 7200));
    _timer_rem_sec         = static_cast<uint32_t>(std::clamp<int>(s.GetInt(KEY_TIMER_REM, static_cast<int>(_timer_preset_sec)), 0, 7200));
    std::string tend_str   = s.GetString(KEY_TIMER_END, "0");
    _timer_end_epoch       = std::strtoll(tend_str.c_str(), nullptr, 10);
    time_t now_t           = std::time(nullptr);
    if (_timer_end_epoch > 0 && now_t >= MIN_VALID_EPOCH) {
        if (static_cast<int64_t>(now_t) < _timer_end_epoch) {
            _timer_running   = true;
            _timer_rem_sec   = static_cast<uint32_t>(_timer_end_epoch - static_cast<int64_t>(now_t));
            _timer_target_ms = GetHAL().millis() + _timer_rem_sec * 1000;
        } else {
            _timer_running   = true;
            _timer_rem_sec   = 0;
            _timer_target_ms = GetHAL().millis();
        }
    } else {
        _timer_running   = false;
        _timer_end_epoch = 0;
    }

    _alarms.clear();
    std::string raw = s.GetString(KEY_ALARMS_JSON, "");
    if (!raw.empty()) {
        cJSON* arr = cJSON_Parse(raw.c_str());
        if (cJSON_IsArray(arr)) {
            int n = std::min<int>(cJSON_GetArraySize(arr), static_cast<int>(MAX_ALARMS));
            for (int i = 0; i < n; ++i) {
                cJSON* obj = cJSON_GetArrayItem(arr, i);
                if (!cJSON_IsObject(obj)) continue;
                AlarmItem it;
                if (auto* j = cJSON_GetObjectItem(obj, "e"); cJSON_IsBool(j)) it.enabled = cJSON_IsTrue(j);
                if (auto* j = cJSON_GetObjectItem(obj, "h"); cJSON_IsNumber(j)) it.hour = static_cast<uint8_t>(std::clamp<int>(j->valueint, 0, 23));
                if (auto* j = cJSON_GetObjectItem(obj, "m"); cJSON_IsNumber(j)) it.minute = static_cast<uint8_t>(std::clamp<int>(j->valueint, 0, 59));
                if (auto* j = cJSON_GetObjectItem(obj, "d"); cJSON_IsNumber(j)) it.days_mask = static_cast<uint8_t>(j->valueint & 0x7F);
                if (auto* j = cJSON_GetObjectItem(obj, "t"); cJSON_IsNumber(j)) it.tone = static_cast<uint8_t>(std::clamp<int>(j->valueint, 0, 4));
                if (auto* j = cJSON_GetObjectItem(obj, "l"); cJSON_IsNumber(j)) it.label_idx = static_cast<uint8_t>(std::clamp<int>(j->valueint, 0, 5));
                if (auto* j = cJSON_GetObjectItem(obj, "p"); cJSON_IsString(j) && j->valuestring) it.mp3_path = j->valuestring;
                _alarms.push_back(it);
            }
        }
        if (arr) {
            cJSON_Delete(arr);
        }
    }

    // Seed two helpful default alarms (disabled) on first boot
    if (_alarms.empty()) {
        AlarmItem a1;
        a1.enabled   = false;
        a1.hour      = 7;
        a1.minute    = 0;
        a1.days_mask = 0x3E;  // Mon-Fri
        a1.tone      = 0;     // Chime
        a1.label_idx = 1;     // Wake Up
        _alarms.push_back(a1);

        AlarmItem a2;
        a2.enabled   = false;
        a2.hour      = 8;
        a2.minute    = 30;
        a2.days_mask = 0x41;  // Weekends
        a2.tone      = 1;     // Classic
        a2.label_idx = 0;     // Alarm
        _alarms.push_back(a2);
    }

    std::stable_sort(_alarms.begin(), _alarms.end(), [](const AlarmItem& a, const AlarmItem& b) {
        return (a.hour * 60 + a.minute) < (b.hour * 60 + b.minute);
    });
}

void AlarmService::save_to_nvs_locked()
{
    cJSON* arr = cJSON_CreateArray();
    for (const auto& it : _alarms) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "e", it.enabled);
        cJSON_AddNumberToObject(obj, "h", it.hour);
        cJSON_AddNumberToObject(obj, "m", it.minute);
        cJSON_AddNumberToObject(obj, "d", it.days_mask);
        cJSON_AddNumberToObject(obj, "t", it.tone);
        cJSON_AddNumberToObject(obj, "l", it.label_idx);
        if (!it.mp3_path.empty()) {
            cJSON_AddStringToObject(obj, "p", it.mp3_path.c_str());
        }
        cJSON_AddItemToArray(arr, obj);
    }
    char* s = cJSON_PrintUnformatted(arr);
    if (s) {
        Settings nvs(NVS_NS, true);
        nvs.SetString(KEY_ALARMS_JSON, s);
        cJSON_free(s);
    }
    cJSON_Delete(arr);
}

void AlarmService::save_snooze_locked()
{
    Settings nvs(NVS_NS, true);
    nvs.SetString(KEY_SNOOZE_AT, std::to_string(_snooze_until));
    nvs.SetInt(KEY_SNOOZE_IDX, _snooze_alarm_idx);
}

void AlarmService::save_timer_locked()
{
    Settings nvs(NVS_NS, true);
    nvs.SetString(KEY_TIMER_END, std::to_string(_timer_end_epoch));
    nvs.SetInt(KEY_TIMER_REM, static_cast<int>(_timer_rem_sec));
    nvs.SetInt(KEY_TIMER_PRE, static_cast<int>(_timer_preset_sec));
}

void AlarmService::save_boot_ring_nvs(int alarm_idx)
{
    Settings nvs(NVS_NS, true);
    nvs.SetInt(KEY_BOOT_RING, alarm_idx);
}

int AlarmService::consume_boot_ring_nvs()
{
    Settings nvs(NVS_NS, false);
    int idx = nvs.GetInt(KEY_BOOT_RING, -1);
    if (idx >= 0) {
        Settings nw(NVS_NS, true);
        nw.SetInt(KEY_BOOT_RING, -1);
    }
    return idx;
}

std::vector<AlarmItem> AlarmService::getAlarms() const
{
    std::lock_guard<std::mutex> lk(_mtx);
    return _alarms;
}

void AlarmService::setAlarms(const std::vector<AlarmItem>& items)
{
    std::lock_guard<std::mutex> lk(_mtx);
    _alarms = items;
    if (_alarms.size() > MAX_ALARMS) {
        _alarms.resize(MAX_ALARMS);
    }
    std::stable_sort(_alarms.begin(), _alarms.end(), [](const AlarmItem& a, const AlarmItem& b) {
        return (a.hour * 60 + a.minute) < (b.hour * 60 + b.minute);
    });
    save_to_nvs_locked();
}

int AlarmService::getSnoozeMin() const
{
    std::lock_guard<std::mutex> lk(_mtx);
    return _snooze_min;
}

void AlarmService::setSnoozeMin(int minutes)
{
    std::lock_guard<std::mutex> lk(_mtx);
    _snooze_min = std::clamp<int>(minutes, 1, 30);
    Settings nvs(NVS_NS, true);
    nvs.SetInt(KEY_SNOOZE_MIN, _snooze_min);
}

int AlarmService::getVolume() const
{
    std::lock_guard<std::mutex> lk(_mtx);
    return _volume;
}

void AlarmService::setVolume(int vol)
{
    std::lock_guard<std::mutex> lk(_mtx);
    _volume = std::clamp<int>(vol, 30, 100);
    Settings nvs(NVS_NS, true);
    nvs.SetInt(KEY_VOLUME, _volume);
}

void AlarmService::scheduleSnooze(int alarm_idx)
{
    std::lock_guard<std::mutex> lk(_mtx);
    time_t now        = std::time(nullptr);
    _snooze_until     = static_cast<int64_t>(now) + static_cast<int64_t>(_snooze_min) * 60;
    _snooze_alarm_idx = alarm_idx;
    save_snooze_locked();
}

void AlarmService::clearSnooze()
{
    std::lock_guard<std::mutex> lk(_mtx);
    _snooze_until     = 0;
    _snooze_alarm_idx = -1;
    save_snooze_locked();
}

int64_t AlarmService::getSnoozeUntil() const
{
    std::lock_guard<std::mutex> lk(_mtx);
    return _snooze_until;
}

int AlarmService::getSnoozeAlarmIdx() const
{
    std::lock_guard<std::mutex> lk(_mtx);
    return _snooze_alarm_idx;
}

void AlarmService::startTimer(uint32_t duration_sec)
{
    std::lock_guard<std::mutex> lk(_mtx);
    uint32_t sec      = std::clamp<uint32_t>(duration_sec, 1, 7200);
    _timer_rem_sec    = sec;
    _timer_running    = true;
    _timer_target_ms  = GetHAL().millis() + sec * 1000;
    time_t now        = std::time(nullptr);
    _timer_end_epoch  = (now >= MIN_VALID_EPOCH) ? (static_cast<int64_t>(now) + sec) : 0;
    save_timer_locked();
}

void AlarmService::pauseTimer()
{
    std::lock_guard<std::mutex> lk(_mtx);
    if (_timer_running) {
        uint32_t now_ms = GetHAL().millis();
        int32_t diff_ms = static_cast<int32_t>(_timer_target_ms - now_ms);
        _timer_rem_sec  = (diff_ms > 0) ? static_cast<uint32_t>((diff_ms + 999) / 1000) : 0;
        _timer_running  = false;
        _timer_end_epoch = 0;
        save_timer_locked();
    }
}

void AlarmService::resetTimer(uint32_t preset_sec)
{
    std::lock_guard<std::mutex> lk(_mtx);
    _timer_preset_sec = std::clamp<uint32_t>(preset_sec, 10, 7200);
    _timer_rem_sec    = _timer_preset_sec;
    _timer_running    = false;
    _timer_end_epoch  = 0;
    save_timer_locked();
}

bool AlarmService::isTimerRunning() const
{
    std::lock_guard<std::mutex> lk(_mtx);
    return _timer_running;
}

uint32_t AlarmService::getTimerRemainingSec() const
{
    std::lock_guard<std::mutex> lk(_mtx);
    if (!_timer_running) {
        return _timer_rem_sec;
    }
    uint32_t now_ms = GetHAL().millis();
    int32_t diff_ms = static_cast<int32_t>(_timer_target_ms - now_ms);
    return (diff_ms > 0) ? static_cast<uint32_t>((diff_ms + 999) / 1000) : 0;
}

uint32_t AlarmService::getTimerPresetSec() const
{
    std::lock_guard<std::mutex> lk(_mtx);
    return _timer_preset_sec;
}

void AlarmService::setAppOpen(bool open)
{
    _app_open = open;
}

void AlarmService::setLauncherActive(bool active)
{
    _launcher_active = active;
}

bool AlarmService::hasLauncherOpenRequest() const
{
    return _launcher_open_req.load();
}

void AlarmService::clearLauncherOpenRequest()
{
    _launcher_open_req = false;
}

int AlarmService::consumePendingRingIdx()
{
    return _pending_ring_idx.exchange(-1);
}

bool AlarmService::hasPendingRing() const
{
    return _pending_ring_idx.load() >= 0;
}

bool AlarmService::findNextAlarmText(std::string& out_text) const
{
    std::lock_guard<std::mutex> lk(_mtx);
    if (_timer_running) {
        uint32_t now_ms = GetHAL().millis();
        int32_t diff_ms = static_cast<int32_t>(_timer_target_ms - now_ms);
        uint32_t rem    = (diff_ms > 0) ? static_cast<uint32_t>((diff_ms + 999) / 1000) : 0;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "Timer %02u:%02u",
                      static_cast<unsigned>(rem / 60), static_cast<unsigned>(rem % 60));
        out_text = buf;
        return true;
    }
    time_t now = std::time(nullptr);
    if (_snooze_until > static_cast<int64_t>(now)) {
        int64_t rem = _snooze_until - static_cast<int64_t>(now);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "Snooze in %02lld:%02lld",
                      static_cast<long long>(rem / 60), static_cast<long long>(rem % 60));
        out_text = buf;
        return true;
    }

    if (now < MIN_VALID_EPOCH) {
        for (const auto& a : _alarms) {
            if (a.enabled) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Alarm %02u:%02u %s", a.hour, a.minute, labelName(a.label_idx));
                out_text = buf;
                return true;
            }
        }
        out_text = "No active alarms";
        return false;
    }

    struct tm lt = {};
    localtime_r(&now, &lt);
    int cur_min_of_day = lt.tm_hour * 60 + lt.tm_min;
    int best_delta     = 8 * 24 * 60;
    const AlarmItem* best = nullptr;

    for (const auto& a : _alarms) {
        if (!a.enabled) continue;
        int alarm_min = a.hour * 60 + a.minute;
        for (int day_off = 0; day_off <= 7; ++day_off) {
            int wday = (lt.tm_wday + day_off) % 7;
            if (a.days_mask != 0 && ((a.days_mask & (1u << wday)) == 0)) {
                continue;
            }
            int delta = day_off * 1440 + (alarm_min - cur_min_of_day);
            if (delta <= 0) {
                if (a.days_mask == 0 && day_off == 0) {
                    continue;
                }
                continue;
            }
            if (delta < best_delta) {
                best_delta = delta;
                best       = &a;
            }
            break;
        }
    }

    if (!best) {
        out_text = "No active alarms";
        return false;
    }

    int hrs = best_delta / 60;
    int mns = best_delta % 60;
    char buf[64];
    if (hrs >= 24) {
        std::snprintf(buf, sizeof(buf), "Next %02u:%02u (%dd %dh)", best->hour, best->minute, hrs / 24, hrs % 24);
    } else if (hrs > 0) {
        std::snprintf(buf, sizeof(buf), "Next %02u:%02u (in %dh %dm)", best->hour, best->minute, hrs, mns);
    } else {
        std::snprintf(buf, sizeof(buf), "Next %02u:%02u (in %dm)", best->hour, best->minute, std::max(1, mns));
    }
    out_text = buf;
    return true;
}

void AlarmService::bg_task_entry(void* arg)
{
    static_cast<AlarmService*>(arg)->bg_task_loop();
    vTaskDelete(nullptr);
}

void AlarmService::bg_task_loop()
{
    // Wait briefly for system boot to settle
    vTaskDelay(pdMS_TO_TICKS(1500));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));

        time_t now      = std::time(nullptr);
        uint32_t now_ms = GetHAL().millis();

        int trigger_idx = -1;
        {
            std::lock_guard<std::mutex> lk(_mtx);

            // 0. Check background countdown timer (works even without valid NTP epoch)
            if (_timer_running) {
                bool timer_expired = (static_cast<int32_t>(_timer_target_ms - now_ms) <= 0) ||
                                     (_timer_end_epoch > 0 && now >= MIN_VALID_EPOCH && static_cast<int64_t>(now) >= _timer_end_epoch);
                if (timer_expired) {
                    trigger_idx      = RING_IDX_TIMER;
                    _timer_running   = false;
                    _timer_rem_sec   = _timer_preset_sec;
                    _timer_end_epoch = 0;
                    save_timer_locked();
                }
            }

            if (trigger_idx < 0 && now >= MIN_VALID_EPOCH) {
                // 1. Check active snooze deadline
                if (_snooze_until > 0 && static_cast<int64_t>(now) >= _snooze_until) {
                    trigger_idx       = (_snooze_alarm_idx >= 0) ? _snooze_alarm_idx : 0;
                    _snooze_until     = 0;
                    _snooze_alarm_idx = -1;
                    save_snooze_locked();
                } else {
                    // 2. Check scheduled alarms once per minute
                    int64_t epoch_min = static_cast<int64_t>(now) / 60;
                    if (epoch_min != _last_fire_min) {
                        struct tm lt = {};
                        localtime_r(&now, &lt);
                        for (size_t i = 0; i < _alarms.size(); ++i) {
                            auto& a = _alarms[i];
                            if (!a.enabled) continue;
                            if (a.hour != lt.tm_hour || a.minute != lt.tm_min) continue;
                            if (a.days_mask != 0 && ((a.days_mask & (1u << lt.tm_wday)) == 0)) continue;

                            trigger_idx    = static_cast<int>(i);
                            _last_fire_min = epoch_min;
                            // One-shot alarm auto-disables after firing
                            if (a.days_mask == 0) {
                                a.enabled = false;
                                save_to_nvs_locked();
                            }
                            break;
                        }
                    }
                }
            }
        }

        if (trigger_idx < 0) {
            continue;
        }

        mclog::tagInfo(TAG, "Alarm #{} triggered!", trigger_idx);
        _pending_ring_idx = trigger_idx;

        if (_app_open.load()) {
            // AppAlarm is already foreground -> rings immediately in 0ms
            continue;
        }
        if (_launcher_active.load()) {
            // Launcher is foreground -> open AppAlarm directly without reboot
            _launcher_open_req = true;
            continue;
        }

        // Another app (Music, MCP, Gemini, Avatar, AI.AGENT, etc.) owns hardware/codec:
        // persist ring request and perform clean warm reboot straight into AppAlarm.
        save_boot_ring_nvs(trigger_idx);
        vTaskDelay(pdMS_TO_TICKS(80));
        GetHAL().requestWarmReboot(_launcher_page_idx.load());
    }
}
