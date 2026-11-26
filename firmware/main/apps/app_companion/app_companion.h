#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <cstdint>

class AppCompanion : public mooncake::AppAbility {
public:
    AppCompanion();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void build_ui();
    void refresh_ui();
    void interact_pet();
    void next_story();

    lv_obj_t* _root       = nullptr;
    lv_obj_t* _lbl_mood   = nullptr;
    lv_obj_t* _lbl_story  = nullptr;
    lv_obj_t* _lbl_stats  = nullptr;

    int _bond             = 50;   // 0..100, never decays harshly (low-maintenance!)
    int _curiosity        = 70;
    int _story_idx        = 0;
    uint32_t _next_glance = 0;
    int _head_conn        = -1;
    std::atomic<bool> _head_pet{false};
};
