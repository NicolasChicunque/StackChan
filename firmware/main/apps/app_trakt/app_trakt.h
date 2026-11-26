#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <string>
#include <vector>

class AppTrakt : public mooncake::AppAbility {
public:
    AppTrakt();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void build_ui();
    void fetch_trakt();
    void refresh_ui();

    lv_obj_t* _root      = nullptr;
    lv_obj_t* _lbl_hdr   = nullptr;
    lv_obj_t* _lbl_shows = nullptr;

    std::string _client_id;
    std::string _username;
    std::string _status = "Configure /sd/config/trakt.txt";
    std::vector<std::string> _shows;
    std::atomic<bool> _sync_req{false};
};
