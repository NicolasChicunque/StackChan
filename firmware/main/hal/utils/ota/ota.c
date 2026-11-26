/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "string.h"

static const char *TAG = "ota";

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADERS_COMPLETE:
        case HTTP_EVENT_ON_STATUS_CODE:
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER");
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

static esp_err_t my_esp_https_ota(const esp_https_ota_config_t *ota_config, void (*on_progress)(int progress))
{
    if (ota_config == NULL || ota_config->http_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err                           = esp_https_ota_begin(ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    int last_progress = -1;
    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int total_size = esp_https_ota_get_image_size(https_ota_handle);
        int read_size  = esp_https_ota_get_image_len_read(https_ota_handle);

        if (total_size > 0) {
            int progress = (read_size * 100) / total_size;

            if (progress != last_progress) {
                last_progress = progress;
                if (on_progress) {
                    on_progress(progress);
                }
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
        return err;
    }

    esp_err_t ota_finish_err = esp_https_ota_finish(https_ota_handle);
    if (ota_finish_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(ota_finish_err));
        return ota_finish_err;
    }
    return ESP_OK;
}

bool start_ota_update(const char *url, void (*on_progress)(int progress))
{
    if (url == NULL || url[0] == '\0') {
        ESP_LOGE(TAG, "Invalid OTA URL");
        return false;
    }

    esp_http_client_config_t config = {
        .url                   = url,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .event_handler         = _http_event_handler,
        .buffer_size           = 4096,
        .buffer_size_tx        = 4096,
        .timeout_ms            = 15000,
        .max_redirection_count = 5,
        .keep_alive_enable     = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    ESP_LOGI(TAG, "Attempting to download update from %s", config.url);
    esp_err_t ret = my_esp_https_ota(&ota_config, on_progress);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Succeed, Rebooting...");
        esp_restart();
        return true;
    }

    ESP_LOGE(TAG, "Firmware upgrade failed: %s", esp_err_to_name(ret));
    return false;
}
