#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <atomic>
#include <string>
#include <vector>

class AppDeck : public mooncake::AppAbility {
public:
    AppDeck();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct MacroBtn {
        std::string label;
        std::string url;
        uint32_t color;
    };

    void load_macros();
    void build_ui();

    lv_obj_t* _root       = nullptr;
    lv_obj_t* _lbl_status = nullptr;
    std::vector<MacroBtn> _macros;
    std::atomic<int> _pending_idx{-1};
};
