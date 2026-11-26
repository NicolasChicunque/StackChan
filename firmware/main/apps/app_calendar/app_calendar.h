#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <string>
#include <vector>

class AppCalendar : public mooncake::AppAbility {
public:
    AppCalendar();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void build_ui();
    void fetch_calendar();
    void refresh_ui();

    lv_obj_t* _root       = nullptr;
    lv_obj_t* _lbl_hdr    = nullptr;
    lv_obj_t* _lbl_events = nullptr;

    std::string _ics_url;
    std::string _status = "Configure /sd/config/calendar.txt";
    std::vector<std::string> _events;
    std::atomic<bool> _sync_req{false};
};
