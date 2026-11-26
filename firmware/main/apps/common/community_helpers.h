#pragma once
#include <board.h>
#include <driver/sdspi_host.h>
#include <esp_vfs_fat.h>
#include <lvgl.h>
#include <sdmmc_cmd.h>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace community {

inline constexpr uint32_t COL_BG      = 0x0B0F1A;
inline constexpr uint32_t COL_CARD    = 0x151B2E;
inline constexpr uint32_t COL_CARD_HI = 0x232C4A;
inline constexpr uint32_t COL_TEAL    = 0x2DD4BF;
inline constexpr uint32_t COL_TEAL_DK = 0x0F766E;
inline constexpr uint32_t COL_AMBER   = 0xF59E0B;
inline constexpr uint32_t COL_GREEN   = 0x22C55E;
inline constexpr uint32_t COL_BLUE    = 0x3B82F6;
inline constexpr uint32_t COL_PURPLE  = 0x8B5CF6;
inline constexpr uint32_t COL_RED     = 0xEF4444;
inline constexpr uint32_t COL_TEXT    = 0xE2E8F0;
inline constexpr uint32_t COL_DIM     = 0x94A3B8;
inline constexpr uint32_t COL_WHITE   = 0xFFFFFF;

inline void style_box(lv_obj_t* obj, uint32_t bg, int radius = 10)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

inline lv_obj_t* make_btn(lv_obj_t* parent, int x, int y, int w, int h, const char* txt, uint32_t bg,
                          uint32_t fg = COL_WHITE, const lv_font_t* font = &lv_font_montserrat_14)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    style_box(b, bg, 10);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, txt);
    return b;
}

inline std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

inline bool mount_sd_temp(sdmmc_card_t*& out_card, bool& out_mounted_by_us)
{
    out_card          = nullptr;
    out_mounted_by_us = false;
    struct stat st    = {};
    if (stat("/sd", &st) == 0) {
        return true;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SPI3_HOST;
    host.max_freq_khz = 12000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = GPIO_NUM_4;
    slot_config.host_id               = static_cast<spi_host_device_t>(host.slot);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed           = false;
    mount_config.max_files                        = 3;
    mount_config.allocation_unit_size             = 16 * 1024;

    if (esp_vfs_fat_sdspi_mount("/sd", &host, &slot_config, &mount_config, &out_card) == ESP_OK) {
        out_mounted_by_us = true;
        return true;
    }
    return false;
}

inline void unmount_sd_temp(sdmmc_card_t* card, bool mounted_by_us)
{
    if (mounted_by_us && card) {
        esp_vfs_fat_sdcard_unmount("/sd", card);
    }
}

inline bool read_sd_config_lines(const char* filename, std::vector<std::string>& out_lines, size_t max_lines = 32)
{
    out_lines.clear();
    sdmmc_card_t* card = nullptr;
    bool mounted_by_us = false;
    if (!mount_sd_temp(card, mounted_by_us)) {
        return false;
    }

    std::string p1 = std::string("/sd/config/") + filename;
    std::string p2 = std::string("/sd/") + filename;
    FILE* f        = std::fopen(p1.c_str(), "r");
    if (!f) {
        f = std::fopen(p2.c_str(), "r");
    }
    bool found = false;
    if (f) {
        found = true;
        char buf[384];
        while (std::fgets(buf, sizeof(buf), f) && out_lines.size() < max_lines) {
            std::string line = trim(buf);
            if (!line.empty() && line[0] != '#') {
                out_lines.push_back(std::move(line));
            }
        }
        std::fclose(f);
    }
    unmount_sd_temp(card, mounted_by_us);
    return found;
}

inline bool http_request(const std::string& method, const std::string& url, std::string& out_body,
                         const std::vector<std::pair<std::string, std::string>>& headers = {},
                         const std::string& req_body                                     = "")
{
    out_body.clear();
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return false;
    }
    auto http = network->CreateHttp(0);
    if (!http) {
        return false;
    }
    for (const auto& kv : headers) {
        http->SetHeader(kv.first, kv.second);
    }
    if (!req_body.empty()) {
        http->SetContent(std::string(req_body));
    }
    if (!http->Open(method, url)) {
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status < 200 || *status >= 300) {
        http->Close();
        return false;
    }
    out_body = http->ReadAll();
    http->Close();
    return true;
}

}  // namespace community
