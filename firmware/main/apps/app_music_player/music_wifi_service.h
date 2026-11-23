/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct httpd_req;

/**
 * @brief Companion Wi-Fi service for AppMusicPlayer:
 *  1) Internet Radio MP3 HTTP/HTTPS stream prefetcher into a 96 KB PSRAM ring buffer
 *     (runs on an internal-SRAM FreeRTOS task so TLS/Wi-Fi never panic).
 *  2) Local HTTP Web Remote & MP3 Uploader (port 80) with QR code onboarding.
 */
class MusicWifiService {
public:
    struct RadioStation {
        std::string name;
        std::string url;
    };

    struct WebCommand {
        std::string action;   // toggle, next, prev, play, radio, vol, loop, shuffle, lights, move, rescan
        int index = 0;
        int value = 0;
    };

    struct WebCallbacks {
        std::function<std::string()> get_status_json;
        std::function<std::string(const std::string& q, int page, int limit)> get_tracks_json;
        std::function<void(const WebCommand& cmd)> enqueue_cmd;
        std::function<bool()> begin_sd_write;
        std::function<void(bool success, const std::string& filename)> end_sd_write;
    };

    MusicWifiService();
    ~MusicWifiService();

    void init(const WebCallbacks& cbs);
    void shutdown();

    /* ----------------------------- Wi-Fi / Web ---------------------------- */
    void request_web_start();
    void stop_web_server();
    bool is_wifi_connected() const;
    bool is_web_running() const;
    std::string get_ip_address() const;
    std::string get_wifi_status() const;

    /* --------------------------- Internet Radio --------------------------- */
    void load_stations(bool sd_mounted);
    bool add_station(const std::string& name, const std::string& url, bool sd_mounted);
    std::vector<RadioStation> get_stations() const;
    int station_count() const;

    void start_radio(const std::string& url);
    void stop_radio();
    bool is_radio_active() const;
    bool is_radio_connected() const;
    bool is_radio_aac() const;
    bool is_radio_eof() const;
    std::string get_radio_error() const;
    size_t radio_buffered_bytes() const;
    size_t pop_radio_bytes(uint8_t* dst, size_t max_len);

private:
    struct ByteRingBuffer {
        uint8_t* buf    = nullptr;
        size_t capacity = 0;
        size_t head     = 0;
        size_t tail     = 0;
        size_t count    = 0;
        mutable std::mutex mtx;

        bool allocate(size_t bytes);
        void release();
        void clear();
        size_t push(const uint8_t* data, size_t len);
        size_t pop(uint8_t* out, size_t max_len);
        size_t available() const;
        size_t free_space() const;
    };

    static void worker_task_entry(void* arg);
    void worker_task_loop();
    void ensure_wifi_connected();
    bool start_httpd();
    void run_radio_stream(const std::string& url);

    // HTTP handlers
    static int handle_index(httpd_req* req);
    static int handle_favicon(httpd_req* req);
    static int handle_status(httpd_req* req);
    static int handle_tracks(httpd_req* req);
    static int handle_radio_get(httpd_req* req);
    static int handle_radio_post(httpd_req* req);
    static int handle_cmd(httpd_req* req);
    static int handle_upload(httpd_req* req);

    static std::string sanitize_filename(const std::string& raw);
    static std::string url_decode(const std::string& in);

    WebCallbacks _cbs;
    void* _httpd = nullptr;

    std::atomic<bool> _running{false};
    std::atomic<bool> _worker_exited{true};
    std::atomic<bool> _want_web_start{false};
    std::atomic<bool> _web_running{false};

    std::atomic<bool> _radio_want_play{false};
    std::atomic<bool> _radio_stop_req{false};
    std::atomic<bool> _radio_active{false};
    std::atomic<bool> _radio_connected{false};
    std::atomic<bool> _radio_is_aac{false};
    std::atomic<bool> _radio_eof{false};

    mutable std::mutex _state_mtx;
    std::string _wifi_status = "Wi-Fi idle";
    std::string _radio_target_url;
    std::string _radio_error;
    std::vector<RadioStation> _stations;

    ByteRingBuffer _radio_ring;
};
