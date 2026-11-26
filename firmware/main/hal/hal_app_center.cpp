/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "utils/ota/ota.h"
#include "utils/secret_logic/secret_logic.h"
#include <mooncake_log.h>
#include <settings.h>
#include <board.h>
#include <cJSON.h>
#include <driver/sdspi_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sys/stat.h>

static const std::string_view _tag = "HAL-AppCenter";

static constexpr std::string_view _store_nvs_ns       = "app_center";
static constexpr std::string_view _store_source_key   = "store_src";
static constexpr std::string_view _store_custom_key   = "use_custom";
static constexpr std::string_view _store_url_key      = "custom_url";
static constexpr std::string_view _default_custom_url =
    "https://raw.githubusercontent.com/NicolasChicunque/StackChan/feat/custom-app-store/store/apps.json";

static constexpr const char *kSdMountPt = "/sd";
static constexpr const char *kSdAppsDir = "/sd/apps";

static bool ensure_sd_mounted(sdmmc_card_t *&out_card, bool &out_mounted_by_us)
{
    out_card          = nullptr;
    out_mounted_by_us = false;
    struct stat st    = {};
    if (stat(kSdMountPt, &st) == 0) {
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

    if (esp_vfs_fat_sdspi_mount(kSdMountPt, &host, &slot_config, &mount_config, &out_card) == ESP_OK) {
        out_mounted_by_us = true;
        return true;
    }
    return false;
}

static std::string read_sd_store_url_override()
{
    static constexpr const char *kPaths[] = {
        "/sd/config/store.txt",
        "/sdcard/config/store.txt",
        "/sd/store.txt",
        "/sdcard/store.txt",
    };
    for (const char *path : kPaths) {
        std::ifstream file(path);
        if (!file.is_open()) {
            continue;
        }
        std::string line;
        if (std::getline(file, line)) {
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            size_t start = 0;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
                start++;
            }
            line = line.substr(start);
            if (line.rfind("http://", 0) == 0 || line.rfind("https://", 0) == 0) {
                return line;
            }
        }
    }
    return "";
}

int Hal::getAppStoreSource()
{
    Settings settings(_store_nvs_ns.data(), false);
    int src = settings.GetInt(_store_source_key.data(), -1);
    if (src >= 0 && src <= 2) {
        return src;
    }
    return settings.GetInt(_store_custom_key.data(), 1) != 0 ? 1 : 0;
}

void Hal::setAppStoreSource(int source)
{
    source = std::clamp(source, 0, 2);
    Settings settings(_store_nvs_ns.data(), true);
    settings.SetInt(_store_source_key.data(), source);
    settings.SetInt(_store_custom_key.data(), source == 1 ? 1 : 0);
}

bool Hal::isCustomAppStoreEnabled()
{
    return getAppStoreSource() == 1;
}

void Hal::setCustomAppStoreEnabled(bool enabled)
{
    setAppStoreSource(enabled ? 1 : 0);
}

std::string Hal::getCustomAppStoreUrl()
{
    if (std::string sd_url = read_sd_store_url_override(); !sd_url.empty()) {
        return sd_url;
    }

    Settings settings(_store_nvs_ns.data(), false);
    std::string url = settings.GetString(_store_url_key.data(), "");
    if (!url.empty()) {
        return url;
    }

    return std::string(_default_custom_url);
}

void Hal::setCustomAppStoreUrl(std::string_view url)
{
    Settings settings(_store_nvs_ns.data(), true);
    settings.SetString(_store_url_key.data(), std::string(url));
}

static std::string get_app_info_list_url(Hal &hal)
{
    if (hal.getAppStoreSource() == 1) {
        return hal.getCustomAppStoreUrl();
    }
    return secret_logic::get_server_url() + "/stackChan/apps";
}

static app_center::AppInfoList_t fetch_sd_app_list()
{
    app_center::AppInfoList_t app_list;
    sdmmc_card_t *card = nullptr;
    bool mounted_by_us = false;
    if (!ensure_sd_mounted(card, mounted_by_us)) {
        mclog::tagWarn(_tag, "microSD not available for /sd/apps scan");
        return app_list;
    }

    DIR *dir = opendir(kSdAppsDir);
    if (dir) {
        struct dirent *ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            std::string fname = ent->d_name;
            if (fname.size() <= 4 || fname.substr(fname.size() - 4) != ".bin") {
                continue;
            }
            std::string full_path = std::string(kSdAppsDir) + "/" + fname;
            struct stat fst       = {};
            long kb               = 0;
            if (stat(full_path.c_str(), &fst) == 0) {
                kb = static_cast<long>(fst.st_size / 1024);
            }
            app_center::AppInfo_t info;
            info.name        = fname.substr(0, fname.size() - 4);
            info.version     = "SD";
            char desc[96];
            std::snprintf(desc, sizeof(desc), "Offline MicroSD firmware (%ld KB) from /sd/apps/%s", kb, fname.c_str());
            info.description = desc;
            info.firmwareUrl = "sd://" + full_path;
            app_list.push_back(std::move(info));
        }
        closedir(dir);
    }

    std::sort(app_list.begin(), app_list.end(),
              [](const app_center::AppInfo_t &a, const app_center::AppInfo_t &b) { return a.name < b.name; });

    if (mounted_by_us && card) {
        esp_vfs_fat_sdcard_unmount(kSdMountPt, card);
    }
    return app_list;
}

static const char *get_json_string(cJSON *item, std::initializer_list<const char *> keys)
{
    for (const auto *key : keys) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(item, key);
        if (cJSON_IsString(value) && value->valuestring != nullptr) {
            return value->valuestring;
        }
    }

    return nullptr;
}

app_center::AppInfoList_t Hal::fetchAppList()
{
    if (getAppStoreSource() == 2) {
        return fetch_sd_app_list();
    }

    app_center::AppInfoList_t app_list;
    auto &board  = Board::GetInstance();
    auto network = board.GetNetwork();

    const std::string url = get_app_info_list_url(*this);
    mclog::tagInfo(_tag, "fetching app list from: {}", url);

    auto http = network->CreateHttp(0);
    if (!http->Open("GET", url)) {
        mclog::tagError(_tag, "failed to open http connection");
        return app_list;
    }

    const auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        mclog::tagError(_tag, "failed to fetch app list, status code: {}", status.value_or(-1));
        http->Close();
        return app_list;
    }

    std::string data = http->ReadAll();
    http->Close();

    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        mclog::tagError(_tag, "failed to parse json response");
        return app_list;
    }

    cJSON *app_array = nullptr;
    if (cJSON_IsArray(root)) {
        app_array = root;
    } else if (cJSON_IsObject(root)) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (cJSON_IsNumber(code) && code->valueint != 0) {
            cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
            mclog::tagError(_tag, "failed to fetch app list, code: {}, message: {}", code->valueint,
                            cJSON_IsString(message) ? message->valuestring : "unknown");
            cJSON_Delete(root);
            return app_list;
        }

        app_array = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (!cJSON_IsArray(app_array)) {
            mclog::tagError(_tag, "invalid app list response: data is not an array");
            cJSON_Delete(root);
            return app_list;
        }
    }

    if (cJSON_IsArray(app_array)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, app_array)
        {
            app_center::AppInfo_t info;

            if (const char *name = get_json_string(item, {"appName", "name"})) {
                info.name = name;
            }

            if (const char *version = get_json_string(item, {"version", "appVersion"})) {
                info.version = version;
            }

            if (const char *icon = get_json_string(item, {"iconUrl", "appIcon", "icon"})) {
                info.iconUrl = icon;
            }

            if (const char *desc = get_json_string(item, {"description", "appDescription", "desc"})) {
                info.description = desc;
            }

            if (const char *fw_url = get_json_string(item, {"firmwareUrl", "downloadUrl", "otaUrl", "url"})) {
                info.firmwareUrl = fw_url;
            }

            app_list.push_back(info);
        }
    } else {
        mclog::tagError(_tag, "invalid app list response: unexpected json shape");
    }

    cJSON_Delete(root);
    return app_list;
}

static std::function<void(int)> _on_progress = nullptr;
static void ota_callback(int progress)
{
    if (_on_progress) {
        _on_progress(progress);
    }
}

bool Hal::launchApp(std::string_view url, std::function<void(int)> onProgress)
{
    mclog::tagInfo(_tag, "launching app from url: {}", url);
    _on_progress = onProgress;

    if (url.rfind("sd://", 0) == 0) {
        sdmmc_card_t *card = nullptr;
        bool mounted_by_us = false;
        if (!ensure_sd_mounted(card, mounted_by_us)) {
            return false;
        }
        std::string filepath(url.substr(5));
        bool ok = start_ota_from_file(filepath.c_str(), ota_callback);
        if (!ok && mounted_by_us && card) {
            esp_vfs_fat_sdcard_unmount(kSdMountPt, card);
        }
        return ok;
    }

    return start_ota_update(url.data(), ota_callback);
}
