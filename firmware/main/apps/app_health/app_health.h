#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <cstdint>

class AppHealth : public mooncake::AppAbility {
public:
    AppHealth();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void build_ui();
    void refresh_ui();
    void log_water();
    void ack_break();

    lv_obj_t* _root       = nullptr;
    lv_obj_t* _lbl_timer  = nullptr;
    lv_obj_t* _lbl_water  = nullptr;
    lv_obj_t* _lbl_prompt = nullptr;

    int _break_min        = 30;
    int _water_cups       = 0;
    bool _alert_active    = false;
    uint32_t _next_due_ms = 0;
    uint32_t _last_ui_ms  = 0;
    int _head_conn        = -1;
    std::atomic<bool> _head_pet{false};
};
