#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <string>
#include <vector>

class AppHome : public mooncake::AppAbility {
public:
    AppHome();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct HomeAction {
        std::string label;
        std::string url;
    };
    void load_actions();
    void build_ui();

    lv_obj_t* _root       = nullptr;
    lv_obj_t* _lbl_status = nullptr;
    std::vector<HomeAction> _actions;
    std::atomic<int> _pending_idx{-1};
};
