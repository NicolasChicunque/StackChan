#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <cstdint>

class AppGames : public mooncake::AppAbility {
public:
    AppGames();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void build_ui();
    void roll_dice();
    void refresh_ui();

    lv_obj_t* _root      = nullptr;
    lv_obj_t* _lbl_die   = nullptr;
    lv_obj_t* _lbl_val   = nullptr;
    lv_obj_t* _lbl_score = nullptr;

    int _die_idx            = 1;  // D4, D6, D8, D10, D12, D20
    int _last_roll          = 6;
    int _scores[2]          = {0, 0};
    int _imu_conn           = -1;
    std::atomic<bool> _shake{false};
    uint32_t _last_roll_ms  = 0;
};
