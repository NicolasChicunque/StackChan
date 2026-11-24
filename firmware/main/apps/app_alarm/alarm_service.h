/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AlarmItem {
    bool enabled         = false;
    uint8_t hour         = 7;
    uint8_t minute       = 0;
    uint8_t days_mask    = 0x3E;  // Bit 0=Sun .. Bit 6=Sat; 0=Once
    uint8_t tone         = 0;     // 0=Chime, 1=Classic, 2=Radar, 3=Energetic, 4=SD MP3
    uint8_t label_idx    = 1;     // 0=Alarm, 1=Wake Up, 2=Work, 3=Gym, 4=Study, 5=Meds
    std::string mp3_path;         // Optional /sd/... path when tone == 4
};

/**
 * @brief Lightweight background alarm scheduler persisted in NVS.
 * Watches the hardware-backed RTC clock across all apps and wakes AppAlarm
 * directly (when on Launcher or inside AppAlarm) or via a clean warm reboot
 * (when another app owns the audio codec).
 */
class AlarmService {
public:
    static constexpr size_t MAX_ALARMS = 6;
    static constexpr uint8_t TONE_SD   = 4;

    static AlarmService& instance();

    void start(int launcher_page_index, int app_id);

    std::vector<AlarmItem> getAlarms() const;
    void setAlarms(const std::vector<AlarmItem>& items);

    int getSnoozeMin() const;
    void setSnoozeMin(int minutes);

    int getVolume() const;
    void setVolume(int vol);

    void scheduleSnooze(int alarm_idx);
    void clearSnooze();
    int64_t getSnoozeUntil() const;
    int getSnoozeAlarmIdx() const;

    void setAppOpen(bool open);
    void setLauncherActive(bool active);
    bool hasLauncherOpenRequest() const;
    void clearLauncherOpenRequest();

    int consumePendingRingIdx();
    bool hasPendingRing() const;
    int appId() const { return _app_id.load(); }
    int launcherPageIndex() const { return _launcher_page_idx.load(); }

    bool findNextAlarmText(std::string& out_text) const;

    static const char* labelName(uint8_t idx);
    static const char* toneName(uint8_t idx);
    static std::string formatDays(uint8_t mask);

private:
    AlarmService() = default;

    void load_from_nvs_locked();
    void save_to_nvs_locked();
    void save_snooze_locked();
    void save_boot_ring_nvs(int alarm_idx);
    int consume_boot_ring_nvs();

    static void bg_task_entry(void* arg);
    void bg_task_loop();

    mutable std::mutex _mtx;
    std::vector<AlarmItem> _alarms;
    int _snooze_min         = 5;
    int _volume             = 80;
    int64_t _snooze_until   = 0;
    int _snooze_alarm_idx   = -1;
    int64_t _last_fire_min  = -1;

    std::atomic<bool> _started{false};
    std::atomic<bool> _app_open{false};
    std::atomic<bool> _launcher_active{false};
    std::atomic<bool> _launcher_open_req{false};
    std::atomic<int> _pending_ring_idx{-1};
    std::atomic<int> _launcher_page_idx{6};
    std::atomic<int> _app_id{-1};
};
