#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <cstdint>
#include <string>

class AppSentinel : public mooncake::AppAbility {
public:
    AppSentinel();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void build_ui();
    void check_ci();
    void refresh_ui();

    lv_obj_t* _root       = nullptr;
    lv_obj_t* _lbl_repo   = nullptr;
    lv_obj_t* _lbl_state  = nullptr;
    lv_obj_t* _lbl_detail = nullptr;

    std::string _repo   = "NicolasChicunque/StackChan";
    std::string _state  = "IDLE";
    std::string _detail = "Tap CHECK (config: /sd/config/sentinel.txt)";
    uint32_t _color     = 0x3B82F6;
    std::atomic<bool> _check_req{false};
};
