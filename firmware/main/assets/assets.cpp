/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "assets.h"
#include <assets.h>
#include <mooncake_log.h>
#include <driver/sdspi_host.h>
#include <esp_heap_caps.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>

static const std::string_view _tag = "Assets";

namespace assets {

namespace {

struct CachedSdAsset {
    uint8_t* data    = nullptr;
    size_t data_size = 0;
};

std::mutex s_sd_mutex;
bool s_sd_scanned = false;
std::map<std::string, CachedSdAsset> s_sd_cache;

static constexpr const char* kSdMountPoint    = "/sd";
static constexpr const char* kSdAvatarDir     = "/sd/avatars";
static constexpr const char* kAvatarFilenames[] = {
    "avatar_calm.bin",
    "avatar_calm_music.bin",
    "avatar_happy.bin",
    "avatar_happy_music.bin",
    "avatar_pouty.bin",
    "avatar_shy.bin",
    "avatar_sleepy.bin",
    "avatar_smug.bin",
    "avatar_thinking.bin",
};

void preload_sd_avatars_locked()
{
    if (s_sd_scanned) {
        return;
    }
    s_sd_scanned = true;

    bool already_mounted = false;
    sdmmc_card_t* card   = nullptr;
    struct stat st       = {};
    if (stat(kSdMountPoint, &st) == 0) {
        already_mounted = true;
    } else {
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

        if (esp_vfs_fat_sdspi_mount(kSdMountPoint, &host, &slot_config, &mount_config, &card) != ESP_OK) {
            return;
        }
    }

    int loaded = 0;
    for (const char* filename : kAvatarFilenames) {
        std::string full_path = std::string(kSdAvatarDir) + "/" + filename;
        FILE* f               = std::fopen(full_path.c_str(), "rb");
        if (!f) {
            continue;
        }
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);

        if (sz > static_cast<long>(sizeof(lv_image_header_t)) && sz < 512 * 1024) {
            uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (buf && std::fread(buf, 1, sz, f) == static_cast<size_t>(sz)) {
                s_sd_cache[filename] = {buf, static_cast<size_t>(sz)};
                loaded++;
            } else if (buf) {
                heap_caps_free(buf);
            }
        }
        std::fclose(f);
    }

    if (loaded > 0) {
        mclog::tagInfo(_tag, "loaded {} custom avatar sprite(s) from {}", loaded, kSdAvatarDir);
    }

    if (!already_mounted && card) {
        esp_vfs_fat_sdcard_unmount(kSdMountPoint, card);
    }
}

bool get_sd_avatar_data(const std::string& key, void*& out_ptr, size_t& out_size)
{
    if (key.rfind("avatar_", 0) != 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    preload_sd_avatars_locked();
    auto it = s_sd_cache.find(key);
    if (it != s_sd_cache.end() && it->second.data != nullptr) {
        out_ptr  = it->second.data;
        out_size = it->second.data_size;
        return true;
    }
    return false;
}

}  // namespace

static bool has_suffix(std::string_view str, std::string_view suffix)
{
    return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

lv_image_dsc_t get_image(std::string_view name)
{
    std::string key(name);
    lv_image_dsc_t dsc = {0};

    // 1. Retrieve data from Assets partition, or fall back to MicroSD /sd/avatars/
    void* data_ptr   = nullptr;
    size_t data_size = 0;

    if (!Assets::GetInstance().GetAssetData(key, data_ptr, data_size)) {
        if (!get_sd_avatar_data(key, data_ptr, data_size)) {
            if (key.rfind("avatar_", 0) != 0) {
                mclog::tagError(_tag, "get image asset {} failed: not found", name);
            }
            return dsc;
        }
    }

    // 2. Construct the LVGL image descriptor
    if (has_suffix(name, ".bin")) {
        // Pre-converted binary (RGB565, etc.) with 4-byte header
        if (data_size > sizeof(lv_image_header_t)) {
            memcpy(&dsc.header, data_ptr, sizeof(lv_image_header_t));
            dsc.data_size = data_size - sizeof(lv_image_header_t);
            dsc.data      = (const uint8_t*)data_ptr + sizeof(lv_image_header_t);
        } else {
            mclog::tagError(_tag, "bin asset {} size too small", name);
        }
    } else if (has_suffix(name, ".png") || has_suffix(name, ".jpg") || has_suffix(name, ".jpeg") ||
               has_suffix(name, ".gif")) {
        // Encoded standard image
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf    = LV_COLOR_FORMAT_RAW_ALPHA;
        dsc.data_size    = data_size;
        dsc.data         = (const uint8_t*)data_ptr;
    } else {
        // Fallback for unknown
        mclog::tagWarn(_tag, "unknown asset type for {}, treating as raw", name);
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf    = LV_COLOR_FORMAT_RAW;
        dsc.data_size    = data_size;
        dsc.data         = (const uint8_t*)data_ptr;
    }

    return dsc;
}

}  // namespace assets
