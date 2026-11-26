#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <sdmmc_cmd.h>
#include <string>
#include <vector>

class AppFiles : public mooncake::AppAbility {
public:
    AppFiles();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Entry {
        std::string name;
        bool is_dir = false;
        long size_kb = 0;
    };

    void scan_dir(const std::string& path);
    void build_ui();
    void refresh_ui();

    lv_obj_t* _root      = nullptr;
    lv_obj_t* _lbl_path  = nullptr;
    lv_obj_t* _lbl_list  = nullptr;
    lv_obj_t* _lbl_info  = nullptr;

    sdmmc_card_t* _card    = nullptr;
    bool _mounted_by_us    = false;
    std::string _cur_path  = "/sd";
    std::vector<Entry> _entries;
    int _cursor            = 0;
};
