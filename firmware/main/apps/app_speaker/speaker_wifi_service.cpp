/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "speaker_wifi_service.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <wifi_manager.h>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

constexpr const char* TAG = "SPK_WIFI";

void send_json(httpd_req_t* req, const std::string& json, const char* status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.size()));
}

bool read_req_body(httpd_req_t* req, std::string& out, size_t max_bytes = 512)
{
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > max_bytes) {
        return false;
    }
    out.resize(static_cast<size_t>(req->content_len));
    size_t got = 0;
    while (got < out.size()) {
        int r = httpd_req_recv(req, out.data() + got, out.size() - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

// Self-contained Web Audio Streamer UI (stored in Flash, zero external CDN dependencies)
const char SPEAKER_WEB_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>StackChan Speaker</title>
<style>
:root{--bg:#0b0f1a;--card:#151b2e;--hi:#232c4a;--teal:#2dd4bf;--blue:#3b82f6;--red:#ef4444;--txt:#e2e8f0;--dim:#94a3b8}
*{box-sizing:border-box;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif}
body{margin:0;background:var(--bg);color:var(--txt);padding:16px;max-width:480px;margin:0 auto}
h1{font-size:20px;margin:0 0 4px;color:var(--teal);display:flex;align-items:center;justify-content:space-between}
.sub{font-size:12px;color:var(--dim);margin-bottom:14px}
.badge{font-size:11px;padding:3px 9px;border-radius:99px;background:var(--hi);color:var(--dim)}
.badge.live{background:#0f766e;color:#fff}
.card{background:var(--card);border-radius:14px;padding:14px;margin-bottom:12px}
.row{display:flex;gap:8px;flex-wrap:wrap}
button{flex:1;min-width:110px;border:0;border-radius:10px;padding:12px 10px;font-size:14px;font-weight:600;color:#fff;background:var(--hi);cursor:pointer}
button.primary{background:#0f766e}
button.blue{background:#2563eb}
button.danger{background:#dc2626}
button:disabled{opacity:.45;cursor:not-allowed}
input[type=range]{width:100%;accent-color:var(--teal);margin:8px 0}
input[type=file]{width:100%;color:var(--dim);font-size:13px;margin-top:6px}
.lbl{font-size:12px;color:var(--dim);display:flex;justify-content:space-between}
.meter{height:8px;background:var(--hi);border-radius:99px;overflow:hidden;margin-top:8px}
.meter>div{height:100%;width:0;background:linear-gradient(90deg,var(--teal),var(--blue));transition:width 60ms linear}
.hint{font-size:11px;color:var(--dim);line-height:1.45;margin-top:8px}
</style>
</head>
<body>
<h1>StackChan Speaker <span id="bStatus" class="badge">READY</span></h1>
<div class="sub">Wi-Fi Audio Streamer &middot; 24 kHz PCM</div>

<div class="card">
  <div class="lbl"><span>LIVE STREAM SOURCE</span><span id="srcLbl">Idle</span></div>
  <div class="row" style="margin-top:10px">
    <button id="btnTab" class="primary" onclick="startTabAudio()">PC / Tab Audio</button>
    <button id="btnMic" class="blue" onclick="startMic()">Live Mic</button>
  </div>
  <div class="row" style="margin-top:8px">
    <button id="btnStop" class="danger" onclick="stopStream()" disabled>Stop Stream</button>
  </div>
  <div class="meter"><div id="vu"></div></div>
  <div class="hint">On PC (Chrome/Edge), click <b>PC / Tab Audio</b> and check "Share audio" to stream YouTube, Spotify or system sound.</div>
</div>

<div class="card">
  <div class="lbl"><span>PLAY LOCAL AUDIO FILE</span><span id="fileLbl">MP3 / WAV / OGG</span></div>
  <input id="fileIn" type="file" accept="audio/*" onchange="playFile(this.files[0])">
  <div class="hint">Works on mobile &amp; desktop. Decodes in browser and streams live to StackChan with lip-sync.</div>
</div>

<div class="card">
  <div class="lbl"><span>VOLUME</span><span id="volVal">60%</span></div>
  <input id="vol" type="range" min="0" max="100" value="60" oninput="onVolInput(this.value)">
  <div class="row" style="margin-top:8px">
    <button id="btnMute" onclick="sendCmd('mute',0)">Mute</button>
    <button id="btnMove" onclick="sendCmd('move',0)">Dance</button>
    <button id="btnLights" onclick="sendCmd('lights',0)">Lights</button>
  </div>
</div>

<script>
const SR = 24000;
let ws = null, actx = null, proc = null, mediaStream = null, fileSrc = null;
let volTimer = null;

function setBadge(txt, live){
  const b = document.getElementById('bStatus');
  b.textContent = txt;
  b.className = 'badge' + (live ? ' live' : '');
}

function ensureWs(){
  return new Promise((resolve, reject) => {
    if (ws && ws.readyState === 1) return resolve(ws);
    const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
    ws = new WebSocket(proto + location.host + '/ws');
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => { setBadge('CONNECTED', true); resolve(ws); };
    ws.onclose = () => { setBadge('READY', false); };
    ws.onerror = (e) => reject(e);
  });
}

function downsampleTo24k(input, inRate){
  if (inRate === SR) {
    const out = new Int16Array(input.length);
    for (let i = 0; i < input.length; i++){
      let s = Math.max(-1, Math.min(1, input[i]));
      out[i] = s < 0 ? s * 32768 : s * 32767;
    }
    return out;
  }
  const ratio = inRate / SR;
  const outLen = Math.floor(input.length / ratio);
  const out = new Int16Array(outLen);
  for (let i = 0; i < outLen; i++){
    const idx = i * ratio;
    const i0 = Math.floor(idx);
    const i1 = Math.min(i0 + 1, input.length - 1);
    const frac = idx - i0;
    let s = input[i0] * (1 - frac) + input[i1] * frac;
    s = Math.max(-1, Math.min(1, s));
    out[i] = s < 0 ? s * 32768 : s * 32767;
  }
  return out;
}

function attachNode(srcNode, label){
  const inRate = actx.sampleRate;
  proc = actx.createScriptProcessor(1024, 1, 1);
  const silentGain = actx.createGain();
  silentGain.gain.value = 0;
  proc.onaudioprocess = (ev) => {
    const ch0 = ev.inputBuffer.getChannelData(0);
    let sum = 0;
    for (let i = 0; i < ch0.length; i++) sum += ch0[i] * ch0[i];
    const rms = Math.sqrt(sum / ch0.length);
    document.getElementById('vu').style.width = Math.min(100, Math.round(rms * 320)) + '%';
    if (ws && ws.readyState === 1 && ws.bufferedAmount < 32768){
      const pcm = downsampleTo24k(ch0, inRate);
      ws.send(pcm.buffer);
    }
  };
  srcNode.connect(proc);
  proc.connect(silentGain);
  silentGain.connect(actx.destination);
  document.getElementById('srcLbl').textContent = label;
  document.getElementById('btnStop').disabled = false;
  setBadge('STREAMING', true);
}

async function startTabAudio(){
  stopStream();
  try {
    await ensureWs();
    mediaStream = await navigator.mediaDevices.getDisplayMedia({video: true, audio: true});
    const audioTracks = mediaStream.getAudioTracks();
    if (!audioTracks.length) { alert('No audio track shared. Check "Share audio".'); stopStream(); return; }
    mediaStream.getVideoTracks().forEach(t => t.stop());
    actx = new (window.AudioContext || window.webkitAudioContext)({sampleRate: SR});
    const src = actx.createMediaStreamSource(new MediaStream(audioTracks));
    audioTracks[0].onended = () => stopStream();
    attachNode(src, 'PC / Tab Audio');
  } catch (e) { alert('Capture cancelled or unsupported: ' + e.message); stopStream(); }
}

async function startMic(){
  stopStream();
  try {
    await ensureWs();
    mediaStream = await navigator.mediaDevices.getUserMedia({audio: true});
    actx = new (window.AudioContext || window.webkitAudioContext)({sampleRate: SR});
    const src = actx.createMediaStreamSource(mediaStream);
    attachNode(src, 'Live Microphone');
  } catch (e) { alert('Mic error: ' + e.message); stopStream(); }
}

async function playFile(file){
  if (!file) return;
  stopStream();
  try {
    await ensureWs();
    document.getElementById('fileLbl').textContent = 'Decoding...';
    const arr = await file.arrayBuffer();
    actx = new (window.AudioContext || window.webkitAudioContext)({sampleRate: SR});
    const abuf = await actx.decodeAudioData(arr);
    fileSrc = actx.createBufferSource();
    fileSrc.buffer = abuf;
    fileSrc.onended = () => stopStream();
    attachNode(fileSrc, file.name.slice(0, 24));
    fileSrc.start(0);
    document.getElementById('fileLbl').textContent = file.name.slice(0, 28);
  } catch (e) { alert('Decode error: ' + e.message); stopStream(); }
}

function stopStream(){
  if (fileSrc){ try{ fileSrc.stop(); fileSrc.disconnect(); }catch(_){} fileSrc = null; }
  if (proc){ try{ proc.disconnect(); }catch(_){} proc = null; }
  if (mediaStream){ mediaStream.getTracks().forEach(t => t.stop()); mediaStream = null; }
  if (actx){ try{ actx.close(); }catch(_){} actx = null; }
  document.getElementById('vu').style.width = '0%';
  document.getElementById('srcLbl').textContent = 'Idle';
  document.getElementById('btnStop').disabled = true;
  setBadge(ws && ws.readyState === 1 ? 'CONNECTED' : 'READY', ws && ws.readyState === 1);
}

function onVolInput(v){
  document.getElementById('volVal').textContent = v + '%';
  clearTimeout(volTimer);
  volTimer = setTimeout(() => sendCmd('vol', parseInt(v, 10)), 120);
}

async function sendCmd(action, value){
  try {
    const r = await fetch('/api/cmd', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({action, value})});
    const s = await r.json();
    applyStatus(s);
  } catch(_){}
}

function applyStatus(s){
  if (!s) return;
  if (typeof s.vol === 'number'){
    document.getElementById('vol').value = s.vol;
    document.getElementById('volVal').textContent = s.vol + '%';
  }
  document.getElementById('btnMute').textContent = s.muted ? 'Muted' : 'Mute';
  document.getElementById('btnMove').textContent = s.move ? 'Dance: ON' : 'Dance: OFF';
  document.getElementById('btnLights').textContent = s.lights ? 'Lights: ON' : 'Lights: OFF';
}

fetch('/api/status').then(r => r.json()).then(applyStatus).catch(()=>{});
</script>
</body>
</html>)HTML";

}  // namespace

SpeakerWifiService::SpeakerWifiService()  = default;
SpeakerWifiService::~SpeakerWifiService() { stop(); }

void SpeakerWifiService::start(const Callbacks& cbs)
{
    if (_running.load()) {
        return;
    }
    _cbs           = cbs;
    _running       = true;
    _worker_exited = false;
    _udp_exited    = true;
    _web_running   = false;
    _udp_running   = false;
    _ws_clients    = 0;

    {
        std::lock_guard<std::mutex> lk(_state_mtx);
        _status_text = "Connecting Wi-Fi...";
    }

    // Internal-SRAM stack for Wi-Fi / LWIP / httpd safety
    BaseType_t ok = xTaskCreatePinnedToCore(worker_task_entry, "spk_wifi", 6144, this, 4, nullptr, 0);
    if (ok != pdPASS) {
        mclog::tagError(TAG, "failed to create spk_wifi task");
        _running       = false;
        _worker_exited = true;
    }
}

void SpeakerWifiService::stop()
{
    _running = false;
    if (_udp_sock >= 0) {
        int s     = _udp_sock;
        _udp_sock = -1;
        shutdown(s, SHUT_RDWR);
        close(s);
    }
    for (int i = 0; i < 60 && (!_worker_exited.load() || !_udp_exited.load()); ++i) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    stop_httpd();
}

bool SpeakerWifiService::is_wifi_connected() const
{
    return WifiManager::GetInstance().IsConnected();
}

std::string SpeakerWifiService::get_ip_address() const
{
    if (!WifiManager::GetInstance().IsConnected()) {
        return "";
    }
    return WifiManager::GetInstance().GetIpAddress();
}

std::string SpeakerWifiService::get_status_text() const
{
    std::lock_guard<std::mutex> lk(_state_mtx);
    return _status_text;
}

void SpeakerWifiService::ensure_wifi_connected()
{
    if (WifiManager::GetInstance().IsConnected()) {
        return;
    }
    GetHAL().startNetwork([this](std::string_view msg) {
        std::lock_guard<std::mutex> lk(_state_mtx);
        _status_text.assign(msg.data(), msg.size());
    });
}

bool SpeakerWifiService::start_httpd()
{
    if (_httpd) {
        return true;
    }
    httpd_config_t cfg      = HTTPD_DEFAULT_CONFIG();
    cfg.server_port         = 80;
    cfg.max_uri_handlers    = 8;
    cfg.max_open_sockets    = 5;
    cfg.lru_purge_enable    = true;
    cfg.stack_size          = 6144;
    cfg.recv_wait_timeout   = 5;
    cfg.send_wait_timeout   = 5;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        mclog::tagError(TAG, "httpd_start failed");
        return false;
    }

    auto reg = [&](const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*), bool is_ws = false) {
        httpd_uri_t u   = {};
        u.uri           = uri;
        u.method        = method;
        u.handler       = handler;
        u.user_ctx      = this;
        u.is_websocket  = is_ws;
        httpd_register_uri_handler(server, &u);
    };

    reg("/", HTTP_GET, handle_index);
    reg("/favicon.ico", HTTP_GET, handle_favicon);
    reg("/api/status", HTTP_GET, handle_status);
    reg("/api/cmd", HTTP_POST, handle_cmd);
    reg("/ws", HTTP_GET, handle_ws, true);

    _httpd       = server;
    _web_running = true;
    mclog::tagInfo(TAG, "Web + WS speaker server started on http://%s/", get_ip_address().c_str());
    return true;
}

void SpeakerWifiService::stop_httpd()
{
    if (_httpd) {
        httpd_stop(static_cast<httpd_handle_t>(_httpd));
        _httpd       = nullptr;
        _web_running = false;
        _ws_clients  = 0;
    }
}

void SpeakerWifiService::worker_task_entry(void* arg)
{
    static_cast<SpeakerWifiService*>(arg)->worker_task_loop();
}

void SpeakerWifiService::worker_task_loop()
{
    ensure_wifi_connected();

    if (_running.load() && is_wifi_connected()) {
        // Disable Wi-Fi modem sleep so live audio packets don't stall for 100-300ms DTIM intervals
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (start_httpd()) {
            std::lock_guard<std::mutex> lk(_state_mtx);
            _status_text = "http://" + get_ip_address();
        } else {
            std::lock_guard<std::mutex> lk(_state_mtx);
            _status_text = "HTTP start failed";
        }

        _udp_exited = false;
        if (xTaskCreatePinnedToCore(udp_task_entry, "spk_udp", 4096, this, 5, nullptr, 0) != pdPASS) {
            _udp_exited = true;
        }
    } else if (_running.load()) {
        std::lock_guard<std::mutex> lk(_state_mtx);
        _status_text = "Wi-Fi not connected";
    }

    while (_running.load()) {
        if (!is_wifi_connected()) {
            if (_web_running.load()) {
                stop_httpd();
            }
            std::lock_guard<std::mutex> lk(_state_mtx);
            _status_text = "Wi-Fi disconnected";
        } else if (!_web_running.load()) {
            if (start_httpd()) {
                std::lock_guard<std::mutex> lk(_state_mtx);
                _status_text = "http://" + get_ip_address();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    stop_httpd();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    _worker_exited = true;
    vTaskDelete(nullptr);
}

void SpeakerWifiService::udp_task_entry(void* arg)
{
    static_cast<SpeakerWifiService*>(arg)->udp_task_loop();
}

void SpeakerWifiService::udp_task_loop()
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        _udp_exited = true;
        vTaskDelete(nullptr);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(kUdpPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        _udp_exited = true;
        vTaskDelete(nullptr);
        return;
    }

    _udp_sock    = sock;
    _udp_running = true;
    mclog::tagInfo(TAG, "UDP PCM listener ready on port %u (24kHz 16-bit mono)", kUdpPort);

    std::vector<uint8_t> rx_buf(1500);
    while (_running.load()) {
        int len = recvfrom(sock, rx_buf.data(), rx_buf.size(), 0, nullptr, nullptr);
        if (len >= 2 && _cbs.on_pcm_24k) {
            size_t samples = static_cast<size_t>(len) / sizeof(int16_t);
            _cbs.on_pcm_24k(reinterpret_cast<const int16_t*>(rx_buf.data()), samples);
        }
    }

    _udp_running = false;
    if (_udp_sock == sock) {
        _udp_sock = -1;
        close(sock);
    }
    _udp_exited = true;
    vTaskDelete(nullptr);
}

int SpeakerWifiService::handle_index(httpd_req* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, SPEAKER_WEB_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

int SpeakerWifiService::handle_favicon(httpd_req* req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

int SpeakerWifiService::handle_status(httpd_req* req)
{
    auto* self = static_cast<SpeakerWifiService*>(req->user_ctx);
    std::string json = (self && self->_cbs.get_status_json) ? self->_cbs.get_status_json() : "{}";
    send_json(req, json);
    return ESP_OK;
}

int SpeakerWifiService::handle_cmd(httpd_req* req)
{
    auto* self = static_cast<SpeakerWifiService*>(req->user_ctx);
    std::string body;
    if (read_req_body(req, body)) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root) {
            cJSON* act = cJSON_GetObjectItemCaseSensitive(root, "action");
            cJSON* val = cJSON_GetObjectItemCaseSensitive(root, "value");
            if (cJSON_IsString(act) && act->valuestring && self && self->_cbs.on_remote_cmd) {
                self->_cbs.on_remote_cmd(act->valuestring, cJSON_IsNumber(val) ? val->valueint : 0);
            }
            cJSON_Delete(root);
        }
    }
    std::string json = (self && self->_cbs.get_status_json) ? self->_cbs.get_status_json() : "{}";
    send_json(req, json);
    return ESP_OK;
}

int SpeakerWifiService::handle_ws(httpd_req* req)
{
    auto* self = static_cast<SpeakerWifiService*>(req->user_ctx);
    if (req->method == HTTP_GET) {
        int fd  = httpd_req_to_sockfd(req);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (self) {
            self->_ws_clients++;
        }
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {};
    esp_err_t ret        = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (self && self->_ws_clients.load() > 0) {
            self->_ws_clients--;
        }
        return ESP_OK;
    }
    if (pkt.len == 0 || pkt.len > 16384) {
        return ESP_OK;
    }

    static uint8_t s_ws_buf[4096];
    uint8_t* buf  = (pkt.len <= sizeof(s_ws_buf)) ? s_ws_buf : nullptr;
    bool heap_buf = false;
    if (!buf) {
        buf = static_cast<uint8_t*>(heap_caps_malloc(pkt.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buf) {
            buf = static_cast<uint8_t*>(heap_caps_malloc(pkt.len, MALLOC_CAP_8BIT));
        }
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
        heap_buf = true;
    }

    pkt.payload = buf;
    ret         = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret == ESP_OK && pkt.type == HTTPD_WS_TYPE_BINARY && pkt.len >= 2 && self && self->_cbs.on_pcm_24k) {
        self->_cbs.on_pcm_24k(reinterpret_cast<const int16_t*>(buf), pkt.len / sizeof(int16_t));
    }
    if (heap_buf) {
        heap_caps_free(buf);
    }
    return ret;
}
