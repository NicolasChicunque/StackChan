#pragma once
#include <lvgl.h>
#include <mooncake.h>
#include <string>
#include <vector>

class AppNfc : public mooncake::AppAbility {
public:
    AppNfc();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void build_ui();
    void load_bindings();

    lv_obj_t* _root       = nullptr;
    lv_obj_t* _lbl_status = nullptr;
    lv_obj_t* _lbl_list   = nullptr;
    std::vector<std::string> _bindings;
};
