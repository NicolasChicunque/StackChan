#pragma once
#include <lvgl.h>
#include <mooncake.h>

class AppEnglish : public mooncake::AppAbility {
public:
    AppEnglish();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void build_ui();
    void refresh_ui();
    void complete_card();

    lv_obj_t* _root       = nullptr;
    lv_obj_t* _lbl_streak = nullptr;
    lv_obj_t* _lbl_en     = nullptr;
    lv_obj_t* _lbl_es     = nullptr;

    int _card_idx   = 0;
    bool _show_hint = false;
    int _streak     = 0;
    int _xp         = 0;
    int _last_day   = 0;
};
