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

struct httpd_req;

/**
 * @brief Wi-Fi audio receiver service for AppSpeaker:
 *  1) Local HTTP + WebSocket server (port 80) serving a self-contained Web App
 *     (PC tab/system audio capture, live mic, and local audio file streamer)
 *     that streams 24 kHz 16-bit mono PCM over WebSocket (/ws).
 *  2) Low-latency UDP PCM receiver (port 8765, 24 kHz 16-bit mono LE PCM)
 *     for direct PC/host audio streaming (ffmpeg / Python).
 */
class SpeakerWifiService {
public:
    static constexpr uint16_t kUdpPort = 8765;

    struct Callbacks {
        std::function<void(const int16_t* samples, size_t count)> on_pcm_24k;
        std::function<std::string()> get_status_json;
        std::function<void(const std::string& action, int value)> on_remote_cmd;
    };

    SpeakerWifiService();
    ~SpeakerWifiService();

    void start(const Callbacks& cbs);
    void stop();

    bool is_running() const { return _running.load(); }
    bool is_wifi_connected() const;
    bool is_web_running() const { return _web_running.load(); }
    bool is_udp_running() const { return _udp_running.load(); }
    bool has_ws_client() const { return _ws_clients.load() > 0; }

    std::string get_ip_address() const;
    std::string get_status_text() const;

private:
    static void worker_task_entry(void* arg);
    void worker_task_loop();
    static void udp_task_entry(void* arg);
    void udp_task_loop();

    void ensure_wifi_connected();
    bool start_httpd();
    void stop_httpd();

    static int handle_index(httpd_req* req);
    static int handle_favicon(httpd_req* req);
    static int handle_status(httpd_req* req);
    static int handle_cmd(httpd_req* req);
    static int handle_ws(httpd_req* req);

    Callbacks _cbs;
    void* _httpd = nullptr;
    int _udp_sock = -1;

    std::atomic<bool> _running{false};
    std::atomic<bool> _worker_exited{true};
    std::atomic<bool> _udp_exited{true};
    std::atomic<bool> _web_running{false};
    std::atomic<bool> _udp_running{false};
    std::atomic<int> _ws_clients{0};

    mutable std::mutex _state_mtx;
    std::string _status_text = "Connecting Wi-Fi...";
};
