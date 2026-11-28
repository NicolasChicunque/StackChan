#pragma once
#include <lvgl.h>
#include <cstdint>

namespace sim {

void create_music_preview(lv_obj_t* parent);
void update_music_preview(uint32_t now_ms);
void destroy_music_preview();

void create_alarm_preview(lv_obj_t* parent);
void update_alarm_preview(uint32_t now_ms);
void destroy_alarm_preview();

void create_gemini_preview(lv_obj_t* parent);
void update_gemini_preview(uint32_t now_ms);
void destroy_gemini_preview();

void create_speaker_preview(lv_obj_t* parent);
void update_speaker_preview(uint32_t now_ms);
void destroy_speaker_preview();

}  // namespace sim
