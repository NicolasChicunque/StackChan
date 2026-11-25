// Adapted from ThisIsBenny's StackChan PR #31.
#include "hal_toggles.h"
#include <esp_log.h>
#include <nvs.h>

namespace hal_toggles {
namespace {
constexpr const char* TAG       = "AppVisibility";
constexpr const char* LEGACY_V3 = "visible_mask_v3";
constexpr uint16_t VALID_MASK   = static_cast<uint16_t>((1u << TOGGLE_COUNT) - 1u);

bool fail(esp_err_t error)
{
    ESP_LOGW(TAG, "Preference storage failed: %s", esp_err_to_name(error));
    return false;
}
}  // namespace

bool load_states(ToggleStates& states)
{
    states = default_states();
    nvs_handle_t handle;
    auto error = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return true;
    if (error != ESP_OK) return fail(error);

    uint16_t mask = VALID_MASK;
    error         = nvs_get_u16(handle, MASK_KEY, &mask);
    auto loaded   = default_states();
    if (error == ESP_OK) {
        if ((mask & ~VALID_MASK) != 0) {
            nvs_close(handle);
            return fail(ESP_ERR_INVALID_STATE);
        }
        for (size_t i = 0; i < TOGGLE_COUNT; ++i) loaded[i] = (mask & (1u << i)) != 0;
    } else if (error == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t legacy_mask = 0xFF;
        if (nvs_get_u8(handle, LEGACY_V3, &legacy_mask) == ESP_OK) {
            for (size_t i = 0; i < 8 && i < TOGGLE_COUNT; ++i) {
                loaded[i] = (legacy_mask & (1u << i)) != 0;
            }
        } else {
            for (size_t i = 0; i < TOGGLE_COUNT; ++i) {
                uint8_t value = 1;
                error         = nvs_get_u8(handle, TOGGLE_LIST[i].key, &value);
                if (error == ESP_ERR_NVS_NOT_FOUND) continue;
                if (error != ESP_OK || value > 1) {
                    nvs_close(handle);
                    return fail(error == ESP_OK ? ESP_ERR_INVALID_STATE : error);
                }
                loaded[i] = value != 0;
            }
        }
    } else {
        nvs_close(handle);
        return fail(error);
    }
    nvs_close(handle);
    states = loaded;
    return true;
}

bool save_states(const ToggleStates& states)
{
    nvs_handle_t handle;
    auto error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return fail(error);

    uint16_t mask = 0;
    for (size_t i = 0; i < TOGGLE_COUNT; ++i) {
        if (states[i]) mask |= static_cast<uint16_t>(1u << i);
    }
    error = nvs_set_u16(handle, MASK_KEY, mask);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK ? true : fail(error);
}
}  // namespace hal_toggles
