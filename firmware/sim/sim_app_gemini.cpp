#include "sim_apps.h"
#include "sim_ui_helpers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

/**
 * Preview of StackChan's real GEMINI app UI (mirroring app_gemini.cpp).
 */
namespace sim {

namespace {

constexpr int W                = 320;
constexpr int H                = 240;
constexpr uint32_t COL_GEMINI  = 0x4285F4;
constexpr uint32_t COL_AGENT   = 0xF59E0B;

enum class VoiceState : uint8_t { Idle = 0, Listening, Speaking };

const char* const VOICES[] = {"Aoede", "Puck", "Charon", "Kore", "Fenrir"};
constexpr int VOICE_COUNT  = 5;

VoiceState s_vstate   = VoiceState::Idle;
bool s_mcp_attached   = false;
int s_voice_idx       = 0;

lv_obj_t* s_root          = nullptr;
lv_obj_t* s_lbl_voice     = nullptr;
lv_obj_t* s_lbl_subtitle  = nullptr;
lv_obj_t* s_btn_primary   = nullptr;
lv_obj_t* s_lbl_primary   = nullptr;
lv_obj_t* s_slot_card[2]  = {};
lv_obj_t* s_slot_dot[2]   = {};
lv_obj_t* s_dual_mouth[2] = {};
lv_obj_t* s_lbl_agent_h   = nullptr;

void refresh_gemini_ui()
{
    if (!s_root) return;

    if (s_lbl_voice) lv_label_set_text(s_lbl_voice, VOICES[s_voice_idx]);

    uint32_t btn_col = COL_GEMINI;
    const char* btn_txt = "TALK";
    const char* sub_txt = "Tap TALK or either avatar to chat";

    if (s_vstate == VoiceState::Listening) {
        btn_col = community::COL_GREEN;
        btn_txt = "LISTENING... (Tap to reply)";
        sub_txt = "Listening to microphone (VAD active)...";
    } else if (s_vstate == VoiceState::Speaking) {
        btn_col = community::COL_PURPLE;
        btn_txt = "STOP SPEAKING";
        sub_txt = s_mcp_attached ? "Agent MCP: Checking your workspace..." : "Gemini: Hi! How can I help today?";
    }

    if (s_btn_primary) lv_obj_set_style_bg_color(s_btn_primary, lv_color_hex(btn_col), 0);
    if (s_lbl_primary) lv_label_set_text(s_lbl_primary, btn_txt);
    if (s_lbl_subtitle) lv_label_set_text(s_lbl_subtitle, sub_txt);

    // Highlight active avatar card
    bool left_active  = (s_vstate != VoiceState::Idle && !s_mcp_attached);
    bool right_active = s_mcp_attached;

    if (s_slot_card[0]) {
        lv_obj_set_style_border_color(s_slot_card[0], lv_color_hex(left_active ? COL_GEMINI : 0x24314F), 0);
    }
    if (s_slot_dot[0]) {
        lv_obj_set_style_bg_color(s_slot_dot[0], lv_color_hex(left_active ? community::COL_GREEN : 0x64748B), 0);
    }
    if (s_slot_card[1]) {
        lv_obj_set_style_border_color(s_slot_card[1], lv_color_hex(right_active ? COL_AGENT : 0x24314F), 0);
    }
    if (s_slot_dot[1]) {
        lv_obj_set_style_bg_color(s_slot_dot[1], lv_color_hex(right_active ? COL_AGENT : 0x64748B), 0);
    }
    if (s_lbl_agent_h) {
        lv_label_set_text(s_lbl_agent_h, s_mcp_attached ? "MCP ATTACHED" : "PC OFF - Tap");
        lv_obj_set_style_text_color(s_lbl_agent_h, lv_color_hex(s_mcp_attached ? COL_AGENT : community::COL_DIM), 0);
    }
}

void cycle_voice_state()
{
    int next = (static_cast<int>(s_vstate) + 1) % 3;
    s_vstate = static_cast<VoiceState>(next);
    refresh_gemini_ui();
}

}  // namespace

void create_gemini_preview(lv_obj_t* parent)
{
    destroy_gemini_preview();

    const int top_y   = view::get_status_bar_top_offset();
    const bool is_fix = (top_y > 0);
    const int hdr_y   = top_y + (is_fix ? 1 : 4);
    const int stage_y = top_y + (is_fix ? 18 : 24);
    const int stage_h = 152 - stage_y;

    s_root = lv_obj_create(parent);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, W, H);
    community::style_box(s_root, community::COL_BG, 0);

    lv_obj_t* badge = lv_label_create(s_root);
    lv_obj_set_pos(badge, 10, hdr_y);
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(badge, lv_color_hex(COL_GEMINI), 0);
    lv_label_set_text(badge, "GEMINI");

    s_lbl_voice = lv_label_create(s_root);
    lv_obj_set_pos(s_lbl_voice, 214, hdr_y + 1);
    lv_obj_set_width(s_lbl_voice, 96);
    lv_obj_set_style_text_align(s_lbl_voice, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_lbl_voice, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_voice, lv_color_hex(community::COL_DIM), 0);

    // Dual-Avatar Stage
    lv_obj_t* dual = lv_obj_create(s_root);
    lv_obj_set_pos(dual, 8, stage_y);
    lv_obj_set_size(dual, 304, stage_h);
    community::style_box(dual, community::COL_CARD, 12);
    lv_obj_set_style_border_width(dual, 1, 0);
    lv_obj_set_style_border_color(dual, lv_color_hex(community::COL_CARD_HI), 0);

    const int slot_w        = 146;
    const int slot_h        = std::max<int>(74, stage_h - 4);
    const int slot_x[2]     = {4, 154};
    const char* slot_txt[2] = {"LIVE", "AGENT $"};

    for (int i = 0; i < 2; ++i) {
        s_slot_card[i] = lv_obj_create(dual);
        lv_obj_set_pos(s_slot_card[i], slot_x[i], 2);
        lv_obj_set_size(s_slot_card[i], slot_w, slot_h);
        community::style_box(s_slot_card[i], 0x0F172A, 10);
        lv_obj_set_style_border_width(s_slot_card[i], 2, 0);
        lv_obj_add_flag(s_slot_card[i], LV_OBJ_FLAG_CLICKABLE);

        if (i == 0) {
            lv_obj_add_event_cb(s_slot_card[i], [](lv_event_t*) { cycle_voice_state(); }, LV_EVENT_CLICKED, nullptr);
        } else {
            lv_obj_add_event_cb(
                s_slot_card[i],
                [](lv_event_t*) {
                    s_mcp_attached = !s_mcp_attached;
                    refresh_gemini_ui();
                },
                LV_EVENT_CLICKED, nullptr);
        }

        s_slot_dot[i] = lv_obj_create(s_slot_card[i]);
        lv_obj_set_size(s_slot_dot[i], 7, 7);
        lv_obj_set_pos(s_slot_dot[i], 7, 6);
        community::style_box(s_slot_dot[i], 0x64748B, LV_RADIUS_CIRCLE);
        lv_obj_remove_flag(s_slot_dot[i], LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(s_slot_card[i]);
        lv_obj_set_pos(lbl, 17, 3);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x94A3B8), 0);
        lv_label_set_text(lbl, slot_txt[i]);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* face = lv_obj_create(s_slot_card[i]);
        lv_obj_set_size(face, 116, std::max<int>(54, slot_h - 24));
        lv_obj_align(face, LV_ALIGN_CENTER, 0, 6);
        community::style_box(face, community::COL_BG, 12);
        lv_obj_set_style_border_width(face, 2, 0);
        lv_obj_set_style_border_color(face, lv_color_hex(0x24314F), 0);
        lv_obj_remove_flag(face, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* el = lv_obj_create(face);
        lv_obj_set_size(el, 15, 15);
        lv_obj_align(el, LV_ALIGN_CENTER, -24, -6);
        community::style_box(el, community::COL_WHITE, LV_RADIUS_CIRCLE);

        lv_obj_t* er = lv_obj_create(face);
        lv_obj_set_size(er, 15, 15);
        lv_obj_align(er, LV_ALIGN_CENTER, 24, -6);
        community::style_box(er, community::COL_WHITE, LV_RADIUS_CIRCLE);

        s_dual_mouth[i] = lv_obj_create(face);
        lv_obj_set_size(s_dual_mouth[i], 30, 5);
        lv_obj_align(s_dual_mouth[i], LV_ALIGN_CENTER, 0, 14);
        community::style_box(s_dual_mouth[i], community::COL_WHITE, 3);
    }

    s_lbl_agent_h = lv_label_create(s_slot_card[1]);
    lv_obj_set_width(s_lbl_agent_h, slot_w - 8);
    lv_obj_align(s_lbl_agent_h, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_align(s_lbl_agent_h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_lbl_agent_h, &lv_font_montserrat_12, 0);
    lv_obj_remove_flag(s_lbl_agent_h, LV_OBJ_FLAG_CLICKABLE);

    // Subtitle box
    lv_obj_t* sub_box = lv_obj_create(s_root);
    lv_obj_set_pos(sub_box, 8, 154);
    lv_obj_set_size(sub_box, 304, 34);
    community::style_box(sub_box, community::COL_CARD, 8);

    s_lbl_subtitle = lv_label_create(sub_box);
    lv_obj_set_pos(s_lbl_subtitle, 8, 2);
    lv_obj_set_size(s_lbl_subtitle, 288, 30);
    lv_label_set_long_mode(s_lbl_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_lbl_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_lbl_subtitle, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_subtitle, lv_color_hex(community::COL_TEXT), 0);

    // Bottom bar: TALK | SET (cycles voice)
    s_btn_primary = community::make_btn(s_root, 8, 190, 218, 44, "TALK", COL_GEMINI, community::COL_WHITE,
                                        &lv_font_montserrat_14);
    s_lbl_primary = lv_obj_get_child(s_btn_primary, 0);
    lv_obj_add_event_cb(s_btn_primary, [](lv_event_t*) { cycle_voice_state(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_set = community::make_btn(s_root, 234, 190, 78, 44, LV_SYMBOL_SETTINGS " VOICE",
                                            community::COL_CARD, community::COL_WHITE, &lv_font_montserrat_12);
    lv_obj_add_event_cb(
        btn_set,
        [](lv_event_t*) {
            s_voice_idx = (s_voice_idx + 1) % VOICE_COUNT;
            refresh_gemini_ui();
        },
        LV_EVENT_CLICKED, nullptr);

    refresh_gemini_ui();
}

void update_gemini_preview(uint32_t now_ms)
{
    if (!s_root) return;
    int active_slot = s_mcp_attached ? 1 : 0;
    for (int i = 0; i < 2; ++i) {
        if (!s_dual_mouth[i]) continue;
        if (s_vstate == VoiceState::Speaking && i == active_slot) {
            int mh = 6 + static_cast<int>(std::abs(std::sin(now_ms * 0.015f)) * 12.0f);
            lv_obj_set_size(s_dual_mouth[i], 34, mh);
        } else {
            lv_obj_set_size(s_dual_mouth[i], 30, 5);
        }
    }
}

void destroy_gemini_preview()
{
    if (s_root) {
        lv_obj_delete(s_root);
        s_root          = nullptr;
        s_lbl_voice     = nullptr;
        s_lbl_subtitle  = nullptr;
        s_btn_primary   = nullptr;
        s_lbl_primary   = nullptr;
        s_lbl_agent_h   = nullptr;
        for (int i = 0; i < 2; ++i) {
            s_slot_card[i]  = nullptr;
            s_slot_dot[i]   = nullptr;
            s_dual_mouth[i] = nullptr;
        }
    }
}

}  // namespace sim
