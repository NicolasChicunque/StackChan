#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <string>

class AppFx : public mooncake::AppAbility {
public:
    AppFx();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void build_ui();
    void refresh_ui();
    void fetch_rates();

    lv_obj_t* _root       = nullptr;
    lv_obj_t* _lbl_pair   = nullptr;
    lv_obj_t* _lbl_input  = nullptr;
    lv_obj_t* _lbl_result = nullptr;
    lv_obj_t* _lbl_status = nullptr;

    int _pair_idx        = 0;  // 0=USD->COP, 1=COP->USD, 2=EUR->COP, 3=USD->MXN, 4=USD->EUR
    double _amount       = 1.0;
    double _rates[5]     = {4050.0, 0.000247, 4380.0, 19.8, 0.92};
    std::string _status  = "Cached rates (tap SYNC)";
    std::atomic<bool> _sync_req{false};
};
