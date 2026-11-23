/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "music_wifi_service.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <wifi_manager.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace {

constexpr const char* TAG             = "MUSIC_WIFI";
constexpr const char* RADIO_FILE_PATH = "/sd/radio.txt";
constexpr size_t RADIO_RING_BYTES     = 96 * 1024;
constexpr size_t UPLOAD_CHUNK_BYTES   = 4096;

struct DefaultStation {
    const char* name;
    const char* url;
};

// Built-in MP3 / AAC streams (plus custom stations from /sd/radio.txt)
constexpr DefaultStation DEFAULT_STATIONS[] = {
    {"El Sol Bogota (RCN)", "http://mdstrm.com/audio/632c9b89aa9ace684913b815/icecast.audio"},
    {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Vaporwaves",   "http://ice1.somafm.com/vaporwaves-128-mp3"},
    {"SomaFM Lush Chill",   "http://ice1.somafm.com/lush-128-mp3"},
    {"Radio Paradise",      "http://stream.radioparadise.com/mp3-128"},
    {"SomaFM Drone Zone",   "http://ice1.somafm.com/dronezone-128-mp3"},
};

bool is_sd_present()
{
    struct stat st = {};
    return stat("/sd", &st) == 0;
}

std::string trim_str(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

bool read_req_body(httpd_req_t* req, std::string& out, size_t max_bytes = 2048)
{
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > max_bytes) {
        return false;
    }
    out.resize(static_cast<size_t>(req->content_len));
    size_t received = 0;
    while (received < out.size()) {
        int r = httpd_req_recv(req, out.data() + received, out.size() - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return false;
        }
        received += static_cast<size_t>(r);
    }
    return true;
}

std::string get_query_param(httpd_req_t* req, const char* key)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 512) {
        return "";
    }
    std::string query(qlen + 1, '\0');
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) {
        return "";
    }
    std::string val(256, '\0');
    if (httpd_query_key_value(query.c_str(), key, val.data(), val.size()) != ESP_OK) {
        return "";
    }
    val.resize(std::strlen(val.c_str()));
    return val;
}

void send_json(httpd_req_t* req, const std::string& json, const char* status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.size()));
}

// Self-contained mobile-first Web Remote & MP3 Uploader (stored in Flash, zero CDN dependencies)
const char WEB_UI_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>StackChan Music</title>
<style>
:root{--bg:#0b0f1a;--card:#151b2e;--hi:#232c4a;--acc:#3d7dff;--grn:#27ae60;--ylw:#ffc940;--txt:#e8eef9;--dim:#8391b0;--red:#e74c3c}
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
body{background:var(--bg);color:var(--txt);max-width:520px;margin:0 auto;padding:12px 12px 32px}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}
header h1{font-size:17px;font-weight:700;letter-spacing:.3px}
.badge{font-size:11px;padding:3px 8px;border-radius:99px;background:var(--hi);color:var(--dim);font-weight:600}
.badge.live{background:#8e24aa;color:#fff}
.card{background:var(--card);border:1px solid var(--hi);border-radius:14px;padding:14px;margin-bottom:12px}
.title{font-size:16px;font-weight:700;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-bottom:8px}
.bar{height:6px;background:var(--hi);border-radius:4px;overflow:hidden;margin-bottom:6px}
.bar>i{display:block;height:100%;width:0;background:var(--acc);transition:width .35s}
.meta{display:flex;justify-content:space-between;font-size:12px;color:var(--dim);margin-bottom:12px}
.row{display:flex;gap:8px;align-items:center}
button{border:1px solid var(--hi);background:var(--hi);color:var(--txt);border-radius:10px;padding:10px 12px;font-size:14px;font-weight:600;cursor:pointer;touch-action:manipulation}
button:active{transform:scale(.97)}
.tr button{flex:1;padding:13px 0;font-size:16px}
.tr .play{background:var(--acc);border-color:var(--acc);color:#fff;flex:1.5}
.tr .play.on{background:var(--grn);border-color:var(--grn)}
.vol{margin-top:12px;gap:10px}
.vol input{flex:1;accent-color:var(--acc)}
.vol span{min-width:48px;text-align:right;font-size:12px;color:var(--dim)}
.pills{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:10px}
.pills button{padding:8px 4px;font-size:11px;background:var(--bg);color:var(--dim)}
.pills button.on{background:#24417f;border-color:var(--acc);color:#fff}
.tabs{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-bottom:10px}
.tabs button{padding:10px 4px;font-size:13px;background:var(--card);color:var(--dim)}
.tabs button.on{background:var(--acc);border-color:var(--acc);color:#fff}
.panel{display:none}
.panel.on{display:block}
input[type=text],input[type=url],input[type=search]{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--hi);background:var(--bg);color:var(--txt);font-size:14px;margin-bottom:8px}
.list{display:flex;flex-direction:column;gap:6px}
.item{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;border-radius:10px;background:var(--bg);border:1px solid transparent;cursor:pointer}
.item.cur{border-color:var(--acc);background:#1b2848}
.item b{font-size:13px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:85%}
.item span{font-size:11px;color:var(--dim)}
.pager{display:flex;justify-content:space-between;align-items:center;margin-top:10px;font-size:12px;color:var(--dim)}
.pager button{padding:7px 14px;font-size:12px}
.drop{border:2px dashed var(--hi);border-radius:12px;padding:18px 12px;text-align:center;margin-bottom:10px;background:var(--bg)}
.drop input{width:100%;color:var(--dim);margin-top:8px}
.btn-full{width:100%;background:var(--acc);border-color:var(--acc);color:#fff;padding:11px}
.msg{margin-top:8px;font-size:12px;color:var(--ylw);min-height:16px}
.msg.err{color:var(--red)}
.msg.ok{color:#58d68d}
</style>
</head>
<body>
<header>
  <h1>StackChan Music</h1>
  <span id="modeBadge" class="badge">SD</span>
</header>

<div class="card">
  <div id="title" class="title">Loading...</div>
  <div class="bar"><i id="prog"></i></div>
  <div class="meta"><span id="tCur">0:00</span><span id="tInfo">--</span><span id="tDur">0:00</span></div>
  <div class="row tr">
    <button onclick="cmd('prev')">&#9198; Prev</button>
    <button id="btnPlay" class="play" onclick="cmd('toggle')">&#9654; Play</button>
    <button onclick="cmd('next')">Next &#9197;</button>
  </div>
  <div class="row vol">
    <span style="text-align:left;min-width:34px">VOL</span>
    <input id="vol" type="range" min="0" max="100" step="5" value="60" oninput="onVol(this.value)">
    <span id="volVal">60%</span>
  </div>
  <div class="pills">
    <button id="pLoop" onclick="cmd('loop')">LOOP</button>
    <button id="pShuf" onclick="cmd('shuffle')">SHUFFLE</button>
    <button id="pLights" onclick="cmd('lights')">LIGHTS</button>
    <button id="pMove" onclick="cmd('move')">MOVE</button>
  </div>
</div>

<div class="tabs">
  <button id="tabLib" class="on" onclick="showTab('lib')">Library</button>
  <button id="tabRad" onclick="showTab('rad')">Radio</button>
  <button id="tabUp" onclick="showTab('up')">Upload MP3</button>
</div>

<div id="panLib" class="card panel on">
  <input id="q" type="search" placeholder="Search songs on microSD..." oninput="onSearch()">
  <div id="trackList" class="list"></div>
  <div class="pager">
    <button onclick="chgPage(-1)">&lt; Prev</button>
    <span id="pageInfo">Page 1</span>
    <button onclick="chgPage(1)">Next &gt;</button>
  </div>
</div>

<div id="panRad" class="card panel">
  <div id="radioList" class="list" style="margin-bottom:14px"></div>
  <div style="font-size:12px;color:var(--dim);margin-bottom:6px">Add Custom MP3 / AAC Stream (saved to /sd/radio.txt)</div>
  <input id="rName" type="text" placeholder="Station name (e.g. El Sol)">
  <input id="rUrl" type="url" placeholder="http://... (direct MP3 or AAC stream URL)">
  <button class="btn-full" onclick="addRadio()">Add Station</button>
  <div id="radMsg" class="msg"></div>
</div>

<div id="panUp" class="card panel">
  <div class="drop">
    <div style="font-size:13px;font-weight:600">Send .mp3 or .wav to StackChan microSD</div>
    <div style="font-size:11px;color:var(--dim);margin-top:4px">Playback pauses briefly while saving the file</div>
    <input id="upFile" type="file" accept=".mp3,.wav,audio/mpeg,audio/wav">
  </div>
  <div class="bar"><i id="upBar"></i></div>
  <button id="btnUp" class="btn-full" onclick="uploadFile()">Upload to /sd</button>
  <div id="upMsg" class="msg"></div>
</div>

<script>
const $=id=>document.getElementById(id);
const LOOP=['OFF','1','ALL'];
let st={},page=0,totalPages=1,searchT=null,volT=null,uploading=false;

function fmt(ms){ms=Math.max(0,ms|0);let s=(ms/1000)|0;return ((s/60)|0)+':'+String(s%60).padStart(2,'0')}
function showTab(t){
  ['lib','rad','up'].forEach(k=>{
    $('tab'+k[0].toUpperCase()+k.slice(1)).classList.toggle('on',k===t);
    $('pan'+k[0].toUpperCase()+k.slice(1)).classList.toggle('on',k===t);
  });
  if(t==='lib')loadTracks();
  if(t==='rad')loadRadio();
}
async function cmd(action,extra={}){
  try{
    await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(Object.assign({action},extra))});
    setTimeout(pollStatus,180);
  }catch(e){}
}
function onVol(v){
  $('volVal').textContent=v+'%';
  clearTimeout(volT);
  volT=setTimeout(()=>cmd('vol',{value:parseInt(v,10)}),140);
}
async function pollStatus(){
  if(uploading)return;
  try{
    const r=await fetch('/api/status');
    if(!r.ok)return;
    st=await r.json();
    $('title').textContent=st.title||'Ready';
    const b=$('modeBadge');
    b.textContent=st.radio?'LIVE RADIO':('SD  '+st.batt+'%');
    b.classList.toggle('live',!!st.radio);
    $('tCur').textContent=st.radio?'LIVE':fmt(st.elapsed);
    $('tDur').textContent=st.radio?'STREAM':fmt(st.duration);
    $('tInfo').textContent=st.radio?(st.radio_status||'STREAMING'):((st.total>0?(st.index+1)+'/'+st.total:'0 tracks'));
    let pct=st.radio?(st.playing?100:0):(st.duration>0?Math.min(100,100*st.elapsed/st.duration):0);
    $('prog').style.width=pct.toFixed(1)+'%';
    const bp=$('btnPlay');
    bp.innerHTML=st.playing?'&#9208; Pause':'&#9654; Play';
    bp.classList.toggle('on',!!st.playing);
    if(document.activeElement!==$('vol')){$('vol').value=st.volume;$('volVal').textContent=st.volume+'%';}
    $('pLoop').textContent='LOOP '+LOOP[st.loop||0];
    $('pLoop').classList.toggle('on',(st.loop||0)!==0);
    $('pShuf').classList.toggle('on',!!st.shuffle);
    $('pLights').classList.toggle('on',!!st.lights);
    $('pMove').classList.toggle('on',!!st.move);
  }catch(e){}
}
function onSearch(){clearTimeout(searchT);searchT=setTimeout(()=>{page=0;loadTracks();},250);}
function chgPage(d){page=Math.max(0,Math.min(totalPages-1,page+d));loadTracks();}
async function loadTracks(){
  try{
    const q=encodeURIComponent($('q').value.trim());
    const r=await fetch('/api/tracks?q='+q+'&page='+page+'&limit=25');
    const d=await r.json();
    totalPages=Math.max(1,Math.ceil(d.total/25));
    $('pageInfo').textContent=d.total+' songs  ('+(page+1)+'/'+totalPages+')';
    const box=$('trackList');box.innerHTML='';
    if(!d.items.length){box.innerHTML='<div style="color:var(--dim);font-size:13px;padding:12px">No matching songs</div>';return;}
    d.items.forEach(it=>{
      const div=document.createElement('div');
      div.className='item'+(!st.radio&&st.index===it.i?' cur':'');
      div.innerHTML='<b>'+esc(it.t)+'</b><span>'+(it.m?'MP3':'WAV')+'</span>';
      div.onclick=()=>{cmd('play',{index:it.i});setTimeout(loadTracks,300);};
      box.appendChild(div);
    });
  }catch(e){}
}
async function loadRadio(){
  try{
    const r=await fetch('/api/radio');
    const d=await r.json();
    const box=$('radioList');box.innerHTML='';
    d.stations.forEach((s,i)=>{
      const div=document.createElement('div');
      div.className='item'+(st.radio&&st.radio_idx===i?' cur':'');
      div.innerHTML='<b>'+esc(s.n)+'</b><span>PLAY</span>';
      div.onclick=()=>{cmd('radio',{index:i});setTimeout(loadRadio,350);};
      box.appendChild(div);
    });
  }catch(e){}
}
async function addRadio(){
  const name=$('rName').value.trim(),url=$('rUrl').value.trim();
  const msg=$('radMsg');msg.className='msg';
  if(!name||!url){msg.className='msg err';msg.textContent='Enter both name and http:// MP3 URL';return;}
  try{
    const r=await fetch('/api/radio',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name,url})});
    const d=await r.json();
    if(d.ok){$('rName').value='';$('rUrl').value='';msg.className='msg ok';msg.textContent='Station added!';loadRadio();}
    else{msg.className='msg err';msg.textContent=d.err||'Failed to add station';}
  }catch(e){msg.className='msg err';msg.textContent='Network error';}
}
function uploadFile(){
  const inp=$('upFile'),msg=$('upMsg'),bar=$('upBar');
  if(!inp.files||!inp.files.length){msg.className='msg err';msg.textContent='Pick an .mp3 or .wav file first';return;}
  const f=inp.files[0];
  uploading=true;$('btnUp').disabled=true;
  msg.className='msg';msg.textContent='Uploading '+f.name+'...';
  bar.style.width='0%';
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/api/upload?name='+encodeURIComponent(f.name),true);
  xhr.setRequestHeader('Content-Type','application/octet-stream');
  xhr.upload.onprogress=e=>{
    if(e.lengthComputable){
      const p=Math.round(100*e.loaded/e.total);
      bar.style.width=p+'%';
      msg.textContent='Uploading '+p+'% ('+Math.round(e.loaded/1024)+' / '+Math.round(e.total/1024)+' KB)';
    }
  };
  xhr.onload=()=>{
    uploading=false;$('btnUp').disabled=false;
    try{
      const d=JSON.parse(xhr.responseText);
      if(xhr.status===200&&d.ok){
        bar.style.width='100%';msg.className='msg ok';
        msg.textContent='Saved '+d.file+' to /sd! Library refreshed.';
        inp.value='';loadTracks();pollStatus();
      }else{msg.className='msg err';msg.textContent=d.err||'Upload failed';}
    }catch(e){msg.className='msg err';msg.textContent='Upload error ('+xhr.status+')';}
  };
  xhr.onerror=()=>{uploading=false;$('btnUp').disabled=false;msg.className='msg err';msg.textContent='Connection lost during upload';};
  xhr.send(f);
}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
pollStatus();loadTracks();loadRadio();
setInterval(pollStatus,1800);
</script>
</body>
</html>
)HTML";

}  // namespace

/* -------------------------------------------------------------------------- */
/*                              ByteRingBuffer                                */
/* -------------------------------------------------------------------------- */

bool MusicWifiService::ByteRingBuffer::allocate(size_t bytes)
{
    std::lock_guard<std::mutex> lk(mtx);
    if (buf) {
        heap_caps_free(buf);
        buf = nullptr;
    }
    buf = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
    capacity = buf ? bytes : 0;
    head     = 0;
    tail     = 0;
    count    = 0;
    return buf != nullptr;
}

void MusicWifiService::ByteRingBuffer::release()
{
    std::lock_guard<std::mutex> lk(mtx);
    if (buf) {
        heap_caps_free(buf);
        buf = nullptr;
    }
    capacity = 0;
    head     = 0;
    tail     = 0;
    count    = 0;
}

void MusicWifiService::ByteRingBuffer::clear()
{
    std::lock_guard<std::mutex> lk(mtx);
    head  = 0;
    tail  = 0;
    count = 0;
}

size_t MusicWifiService::ByteRingBuffer::push(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lk(mtx);
    if (!buf || capacity == 0 || len == 0) {
        return 0;
    }
    size_t to_write = std::min(len, capacity - count);
    if (to_write == 0) {
        return 0;
    }
    size_t first = std::min(to_write, capacity - head);
    std::memcpy(buf + head, data, first);
    if (to_write > first) {
        std::memcpy(buf, data + first, to_write - first);
    }
    head = (head + to_write) % capacity;
    count += to_write;
    return to_write;
}

size_t MusicWifiService::ByteRingBuffer::pop(uint8_t* out, size_t max_len)
{
    std::lock_guard<std::mutex> lk(mtx);
    if (!buf || count == 0 || max_len == 0) {
        return 0;
    }
    size_t to_read = std::min(max_len, count);
    size_t first   = std::min(to_read, capacity - tail);
    std::memcpy(out, buf + tail, first);
    if (to_read > first) {
        std::memcpy(out + first, buf, to_read - first);
    }
    tail = (tail + to_read) % capacity;
    count -= to_read;
    return to_read;
}

size_t MusicWifiService::ByteRingBuffer::available() const
{
    std::lock_guard<std::mutex> lk(mtx);
    return count;
}

size_t MusicWifiService::ByteRingBuffer::free_space() const
{
    std::lock_guard<std::mutex> lk(mtx);
    return capacity - count;
}

/* -------------------------------------------------------------------------- */
/*                               Lifecycle                                    */
/* -------------------------------------------------------------------------- */

MusicWifiService::MusicWifiService() = default;

MusicWifiService::~MusicWifiService()
{
    shutdown();
}

void MusicWifiService::init(const WebCallbacks& cbs)
{
    if (_running.load()) {
        return;
    }
    _cbs = cbs;
    _radio_ring.allocate(RADIO_RING_BYTES);

    _running        = true;
    _worker_exited  = false;
    _want_web_start = false;
    _radio_want_play = false;
    _radio_stop_req  = false;

    // MUST stay on internal SRAM stack (Wi-Fi/NVS/TLS access SPI Flash!)
    BaseType_t ok = xTaskCreatePinnedToCore(worker_task_entry, "music_wifi", 8192, this, 4, nullptr, 0);
    if (ok != pdPASS) {
        mclog::tagError(TAG, "Failed to create music_wifi worker task");
        _running       = false;
        _worker_exited = true;
    }
}

void MusicWifiService::shutdown()
{
    if (!_running.load()) {
        stop_web_server();
        _radio_ring.release();
        return;
    }

    _radio_stop_req  = true;
    _radio_want_play = false;
    _want_web_start  = false;
    _running         = false;

    stop_web_server();

    for (int i = 0; i < 150 && !_worker_exited.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    _radio_ring.release();
}

/* -------------------------------------------------------------------------- */
/*                               Wi-Fi / Web                                  */
/* -------------------------------------------------------------------------- */

void MusicWifiService::request_web_start()
{
    _want_web_start = true;
}

void MusicWifiService::stop_web_server()
{
    if (_httpd) {
        httpd_stop(static_cast<httpd_handle_t>(_httpd));
        _httpd = nullptr;
    }
    _web_running = false;
}

bool MusicWifiService::is_wifi_connected() const
{
    return WifiManager::GetInstance().IsConnected();
}

bool MusicWifiService::is_web_running() const
{
    return _web_running.load();
}

std::string MusicWifiService::get_ip_address() const
{
    if (!WifiManager::GetInstance().IsConnected()) {
        return "";
    }
    return WifiManager::GetInstance().GetIpAddress();
}

std::string MusicWifiService::get_wifi_status() const
{
    std::lock_guard<std::mutex> lk(_state_mtx);
    return _wifi_status;
}

void MusicWifiService::ensure_wifi_connected()
{
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsConnected()) {
        std::lock_guard<std::mutex> lk(_state_mtx);
        _wifi_status = "Wi-Fi OK (" + wifi.GetIpAddress() + ")";
        return;
    }

    {
        std::lock_guard<std::mutex> lk(_state_mtx);
        _wifi_status = "Connecting Wi-Fi...";
    }

    GetHAL().startNetwork([this](std::string_view log) {
        std::lock_guard<std::mutex> lk(_state_mtx);
        if (!log.empty()) {
            _wifi_status.assign(log.data(), std::min<size_t>(log.size(), 40));
        }
    });

    for (int i = 0; i < 75 && _running.load() && !wifi.IsConnected(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    std::lock_guard<std::mutex> lk(_state_mtx);
    if (wifi.IsConnected()) {
        _wifi_status = "Wi-Fi OK (" + wifi.GetIpAddress() + ")";
    } else {
        _wifi_status = "No Wi-Fi (configure in SETUP)";
    }
}

bool MusicWifiService::start_httpd()
{
    if (_httpd) {
        _web_running = true;
        return true;
    }

    httpd_config_t cfg      = HTTPD_DEFAULT_CONFIG();
    cfg.server_port         = 80;
    cfg.stack_size          = 8192;
    cfg.max_uri_handlers    = 10;
    cfg.lru_purge_enable    = true;
    cfg.recv_wait_timeout   = 15;
    cfg.send_wait_timeout   = 15;
    cfg.task_priority       = 4;

    httpd_handle_t srv = nullptr;
    esp_err_t err      = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        mclog::tagError(TAG, "httpd_start failed: {}", esp_err_to_name(err));
        std::lock_guard<std::mutex> lk(_state_mtx);
        _wifi_status = "HTTP server error";
        return false;
    }

    const httpd_uri_t uris[] = {
        {"/",           HTTP_GET,  handle_index,     this},
        {"/api/status", HTTP_GET,  handle_status,    this},
        {"/api/tracks", HTTP_GET,  handle_tracks,    this},
        {"/api/radio",  HTTP_GET,  handle_radio_get, this},
        {"/api/radio",  HTTP_POST, handle_radio_post,this},
        {"/api/cmd",    HTTP_POST, handle_cmd,       this},
        {"/api/upload", HTTP_POST, handle_upload,    this},
    };
    for (const auto& u : uris) {
        httpd_register_uri_handler(srv, &u);
    }

    _httpd       = srv;
    _web_running = true;
    mclog::tagInfo(TAG, "Web Remote started at http://{}/", get_ip_address());
    return true;
}

/* -------------------------------------------------------------------------- */
/*                             Internet Radio                                 */
/* -------------------------------------------------------------------------- */

void MusicWifiService::load_stations(bool sd_mounted)
{
    std::vector<RadioStation> list;
    for (const auto& ds : DEFAULT_STATIONS) {
        list.push_back({ds.name, ds.url});
    }

    if (sd_mounted) {
        FILE* f = std::fopen(RADIO_FILE_PATH, "r");
        if (f) {
            char line[320];
            while (std::fgets(line, sizeof(line), f)) {
                std::string s = trim_str(line);
                if (s.empty() || s[0] == '#') {
                    continue;
                }
                auto sep = s.find('|');
                if (sep == std::string::npos) {
                    continue;
                }
                std::string name = trim_str(s.substr(0, sep));
                std::string url  = trim_str(s.substr(sep + 1));
                if (!name.empty() && (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)) {
                    list.push_back({name, url});
                }
            }
            std::fclose(f);
        }
    }

    std::lock_guard<std::mutex> lk(_state_mtx);
    _stations = std::move(list);
}

bool MusicWifiService::add_station(const std::string& raw_name, const std::string& raw_url, bool sd_mounted)
{
    std::string name = trim_str(raw_name);
    std::string url  = trim_str(raw_url);
    if (name.empty() || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) {
        return false;
    }
    // Strip characters that would break Name|URL line format
    for (char& c : name) {
        if (c == '|' || c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    for (char& c : url) {
        if (c == '\r' || c == '\n') {
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lk(_state_mtx);
        _stations.push_back({name, url});
    }

    if (sd_mounted) {
        FILE* f = std::fopen(RADIO_FILE_PATH, "a");
        if (f) {
            std::fprintf(f, "%s|%s\n", name.c_str(), url.c_str());
            std::fclose(f);
        }
    }
    return true;
}

std::vector<MusicWifiService::RadioStation> MusicWifiService::get_stations() const
{
    std::lock_guard<std::mutex> lk(_state_mtx);
    return _stations;
}

int MusicWifiService::station_count() const
{
    std::lock_guard<std::mutex> lk(_state_mtx);
    return static_cast<int>(_stations.size());
}

void MusicWifiService::start_radio(const std::string& url)
{
    {
        std::lock_guard<std::mutex> lk(_state_mtx);
        _radio_target_url = url;
        _radio_error.clear();
    }
    _radio_stop_req  = true;
    _radio_eof       = false;
    _radio_connected = false;
    _radio_ring.clear();
    _radio_want_play = true;
}

void MusicWifiService::stop_radio()
{
    _radio_want_play = false;
    _radio_stop_req  = true;
    _radio_ring.clear();
}

bool MusicWifiService::is_radio_active() const
{
    return _radio_active.load() || _radio_want_play.load();
}

bool MusicWifiService::is_radio_connected() const
{
    return _radio_connected.load();
}

bool MusicWifiService::is_radio_aac() const
{
    return _radio_is_aac.load();
}

bool MusicWifiService::is_radio_eof() const
{
    return _radio_eof.load();
}

std::string MusicWifiService::get_radio_error() const
{
    std::lock_guard<std::mutex> lk(_state_mtx);
    return _radio_error;
}

size_t MusicWifiService::radio_buffered_bytes() const
{
    return _radio_ring.available();
}

size_t MusicWifiService::pop_radio_bytes(uint8_t* dst, size_t max_len)
{
    return _radio_ring.pop(dst, max_len);
}

/* -------------------------------------------------------------------------- */
/*                               Worker Task                                  */
/* -------------------------------------------------------------------------- */

void MusicWifiService::worker_task_entry(void* arg)
{
    static_cast<MusicWifiService*>(arg)->worker_task_loop();
    vTaskDelete(nullptr);
}

void MusicWifiService::worker_task_loop()
{
    while (_running.load()) {
        if (_want_web_start.exchange(false)) {
            ensure_wifi_connected();
            if (is_wifi_connected()) {
                start_httpd();
            }
        }

        if (_radio_want_play.exchange(false)) {
            _radio_stop_req  = false;
            _radio_eof       = false;
            _radio_connected = false;
            _radio_active    = true;
            _radio_ring.clear();

            std::string url;
            {
                std::lock_guard<std::mutex> lk(_state_mtx);
                url = _radio_target_url;
                _radio_error.clear();
            }

            ensure_wifi_connected();
            if (!_running.load() || _radio_stop_req.load()) {
                _radio_active = false;
                continue;
            }
            if (!is_wifi_connected()) {
                {
                    std::lock_guard<std::mutex> lk(_state_mtx);
                    _radio_error = "NO WIFI";
                }
                _radio_active = false;
                _radio_eof    = true;
                continue;
            }

            run_radio_stream(url);
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    _worker_exited = true;
}

void MusicWifiService::run_radio_stream(const std::string& initial_url)
{
    struct HeaderCapture {
        std::string location;
        std::string content_type;
    };

    auto event_cb = [](esp_http_client_event_t* evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data && evt->header_key && evt->header_value) {
            auto* cap = static_cast<HeaderCapture*>(evt->user_data);
            if (strcasecmp(evt->header_key, "location") == 0) {
                cap->location = evt->header_value;
            } else if (strcasecmp(evt->header_key, "content-type") == 0) {
                cap->content_type = evt->header_value;
            }
        }
        return ESP_OK;
    };

    constexpr size_t READ_CHUNK = 2048;
    uint8_t* read_buf = static_cast<uint8_t*>(heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!read_buf) {
        read_buf = static_cast<uint8_t*>(std::malloc(READ_CHUNK));
    }
    if (!read_buf) {
        std::lock_guard<std::mutex> lk(_state_mtx);
        _radio_error  = "NO MEMORY";
        _radio_active = false;
        _radio_eof    = true;
        return;
    }

    std::string cur_url = initial_url;
    // Convenience: convert Mediastream HLS URLs (mdstrm.com/audio/<id>/live.m3u8) to direct Icecast audio
    if (cur_url.find("mdstrm.com/audio/") != std::string::npos) {
        auto pos = cur_url.find("/live.m3u8");
        if (pos != std::string::npos) {
            cur_url.replace(pos, 10, "/icecast.audio");
        }
    }

    esp_http_client_handle_t client = nullptr;
    bool stream_ok = false;
    _radio_is_aac  = false;

    for (int hop = 0; hop < 5 && _running.load() && !_radio_stop_req.load(); ++hop) {
        HeaderCapture cap;
        esp_http_client_config_t cfg = {};
        cfg.url                      = cur_url.c_str();
        cfg.method                   = HTTP_METHOD_GET;
        cfg.timeout_ms               = 8000;
        cfg.buffer_size              = 4096;
        cfg.buffer_size_tx           = 1024;
        cfg.crt_bundle_attach        = esp_crt_bundle_attach;
        cfg.user_agent               = "StackChan-Music/1.0";
        cfg.disable_auto_redirect    = true;
        cfg.event_handler            = event_cb;
        cfg.user_data                = &cap;

        client = esp_http_client_init(&cfg);
        if (!client) {
            std::lock_guard<std::mutex> lk(_state_mtx);
            _radio_error = "HTTP INIT ERR";
            break;
        }
        esp_http_client_set_header(client, "Icy-MetaData", "0");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            mclog::tagWarn(TAG, "Radio open failed ({}): {}", cur_url, esp_err_to_name(err));
            esp_http_client_cleanup(client);
            client = nullptr;
            std::lock_guard<std::mutex> lk(_state_mtx);
            _radio_error = "CONNECT FAILED";
            break;
        }

        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && !cap.location.empty()) {
            mclog::tagInfo(TAG, "Radio redirect {} -> {}", status, cap.location);
            cur_url = cap.location;
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
            continue;
        }

        if (status != 200 && status != 206) {
            mclog::tagWarn(TAG, "Radio HTTP status {}", status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
            std::lock_guard<std::mutex> lk(_state_mtx);
            _radio_error = "HTTP " + std::to_string(status);
            break;
        }

        std::string ct = cap.content_type;
        for (char& c : ct) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ct.find("aac") != std::string::npos || ct.find("mp4") != std::string::npos) {
            _radio_is_aac = true;
        }
        mclog::tagInfo(TAG, "Radio Content-Type: '{}' (aac={})", cap.content_type, _radio_is_aac.load());

        stream_ok = true;
        break;
    }

    if (stream_ok && client) {
        _radio_connected = true;
        mclog::tagInfo(TAG, "Radio streaming: {}", cur_url);

        while (_running.load() && !_radio_stop_req.load()) {
            // Also honor Web Remote start requests while streaming radio!
            if (_want_web_start.exchange(false)) {
                if (is_wifi_connected()) {
                    start_httpd();
                }
            }

            size_t free_sp = _radio_ring.free_space();
            if (free_sp < READ_CHUNK) {
                vTaskDelay(pdMS_TO_TICKS(15));
                continue;
            }

            size_t want = std::min(READ_CHUNK, free_sp);
            int n       = esp_http_client_read(client, reinterpret_cast<char*>(read_buf), static_cast<int>(want));
            if (n > 0) {
                _radio_ring.push(read_buf, static_cast<size_t>(n));
            } else if (n == 0) {
                if (esp_http_client_is_complete_data_received(client)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(15));
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    vTaskDelay(pdMS_TO_TICKS(15));
                    continue;
                }
                mclog::tagWarn(TAG, "Radio read error: {}", n);
                std::lock_guard<std::mutex> lk(_state_mtx);
                _radio_error = "STREAM DROPPED";
                break;
            }
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    heap_caps_free(read_buf);
    _radio_connected = false;
    _radio_active    = false;
    _radio_eof       = true;
}

/* -------------------------------------------------------------------------- */
/*                              HTTP Handlers                                 */
/* -------------------------------------------------------------------------- */

int MusicWifiService::handle_index(httpd_req* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, WEB_UI_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

int MusicWifiService::handle_status(httpd_req* req)
{
    auto* self = static_cast<MusicWifiService*>(req->user_ctx);
    std::string json = (self && self->_cbs.get_status_json) ? self->_cbs.get_status_json() : "{}";
    send_json(req, json);
    return ESP_OK;
}

int MusicWifiService::handle_tracks(httpd_req* req)
{
    auto* self = static_cast<MusicWifiService*>(req->user_ctx);
    std::string q = url_decode(get_query_param(req, "q"));
    int page      = std::atoi(get_query_param(req, "page").c_str());
    int limit     = std::atoi(get_query_param(req, "limit").c_str());
    if (limit <= 0 || limit > 50) {
        limit = 25;
    }
    if (page < 0) {
        page = 0;
    }
    std::string json = (self && self->_cbs.get_tracks_json) ? self->_cbs.get_tracks_json(q, page, limit) : "{\"total\":0,\"items\":[]}";
    send_json(req, json);
    return ESP_OK;
}

int MusicWifiService::handle_radio_get(httpd_req* req)
{
    auto* self    = static_cast<MusicWifiService*>(req->user_ctx);
    auto stations = self->get_stations();

    cJSON* root = cJSON_CreateObject();
    cJSON* arr  = cJSON_AddArrayToObject(root, "stations");
    for (const auto& s : stations) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "n", s.name.c_str());
        cJSON_AddStringToObject(item, "u", s.url.c_str());
        cJSON_AddItemToArray(arr, item);
    }
    char* str = cJSON_PrintUnformatted(root);
    std::string out(str ? str : "{\"stations\":[]}");
    if (str) {
        cJSON_free(str);
    }
    cJSON_Delete(root);
    send_json(req, out);
    return ESP_OK;
}

int MusicWifiService::handle_radio_post(httpd_req* req)
{
    auto* self = static_cast<MusicWifiService*>(req->user_ctx);
    std::string body;
    if (!read_req_body(req, body, 1024)) {
        send_json(req, "{\"ok\":false,\"err\":\"Invalid request\"}", "400 Bad Request");
        return ESP_OK;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        send_json(req, "{\"ok\":false,\"err\":\"Invalid JSON\"}", "400 Bad Request");
        return ESP_OK;
    }
    cJSON* jn = cJSON_GetObjectItem(root, "name");
    cJSON* ju = cJSON_GetObjectItem(root, "url");
    std::string name = (cJSON_IsString(jn) && jn->valuestring) ? jn->valuestring : "";
    std::string url  = (cJSON_IsString(ju) && ju->valuestring) ? ju->valuestring : "";
    cJSON_Delete(root);

    bool ok = self->add_station(name, url, is_sd_present());
    if (!ok) {
        send_json(req, "{\"ok\":false,\"err\":\"URL must start with http:// or https://\"}", "400 Bad Request");
        return ESP_OK;
    }

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

int MusicWifiService::handle_cmd(httpd_req* req)
{
    auto* self = static_cast<MusicWifiService*>(req->user_ctx);
    std::string body;
    if (!read_req_body(req, body, 512)) {
        send_json(req, "{\"ok\":false}", "400 Bad Request");
        return ESP_OK;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        send_json(req, "{\"ok\":false}", "400 Bad Request");
        return ESP_OK;
    }

    WebCommand cmd;
    cJSON* ja = cJSON_GetObjectItem(root, "action");
    cJSON* ji = cJSON_GetObjectItem(root, "index");
    cJSON* jv = cJSON_GetObjectItem(root, "value");
    if (cJSON_IsString(ja) && ja->valuestring) {
        cmd.action = ja->valuestring;
    }
    if (cJSON_IsNumber(ji)) {
        cmd.index = ji->valueint;
    }
    if (cJSON_IsNumber(jv)) {
        cmd.value = jv->valueint;
    }
    cJSON_Delete(root);

    if (!cmd.action.empty() && self && self->_cbs.enqueue_cmd) {
        self->_cbs.enqueue_cmd(cmd);
    }
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

int MusicWifiService::handle_upload(httpd_req* req)
{
    auto* self = static_cast<MusicWifiService*>(req->user_ctx);
    if (!is_sd_present()) {
        send_json(req, "{\"ok\":false,\"err\":\"No microSD card mounted\"}", "503 Service Unavailable");
        return ESP_OK;
    }

    std::string raw_name = url_decode(get_query_param(req, "name"));
    std::string safe     = sanitize_filename(raw_name);
    if (safe.empty()) {
        send_json(req, "{\"ok\":false,\"err\":\"Only .mp3 and .wav files are supported\"}", "400 Bad Request");
        return ESP_OK;
    }

    if (req->content_len <= 0) {
        send_json(req, "{\"ok\":false,\"err\":\"Empty upload\"}", "400 Bad Request");
        return ESP_OK;
    }

    // Ask AppMusicPlayer to pause SD playback & close any open FILE* before writing
    if (self->_cbs.begin_sd_write && !self->_cbs.begin_sd_write()) {
        send_json(req, "{\"ok\":false,\"err\":\"SD busy, try again\"}", "503 Service Unavailable");
        return ESP_OK;
    }

    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(UPLOAD_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<uint8_t*>(std::malloc(UPLOAD_CHUNK_BYTES));
    }

    std::string full_path = std::string("/sd/") + safe;
    FILE* f               = buf ? std::fopen(full_path.c_str(), "wb") : nullptr;
    bool ok               = (f != nullptr);

    size_t remaining = static_cast<size_t>(req->content_len);
    while (ok && remaining > 0) {
        size_t to_read = std::min(remaining, UPLOAD_CHUNK_BYTES);
        int r          = httpd_req_recv(req, reinterpret_cast<char*>(buf), to_read);
        if (r > 0) {
            if (std::fwrite(buf, 1, static_cast<size_t>(r), f) != static_cast<size_t>(r)) {
                ok = false;
                break;
            }
            remaining -= static_cast<size_t>(r);
        } else if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        } else {
            ok = false;
            break;
        }
    }

    if (f) {
        std::fclose(f);
    }
    if (buf) {
        heap_caps_free(buf);
    }
    if (!ok) {
        std::remove(full_path.c_str());
    }

    if (self->_cbs.end_sd_write) {
        self->_cbs.end_sd_write(ok, safe);
    }

    if (ok) {
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "file", safe.c_str());
        char* s = cJSON_PrintUnformatted(resp);
        std::string out(s ? s : "{\"ok\":true}");
        if (s) {
            cJSON_free(s);
        }
        cJSON_Delete(resp);
        send_json(req, out);
    } else {
        send_json(req, "{\"ok\":false,\"err\":\"Failed writing to microSD\"}", "500 Internal Server Error");
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                                Helpers                                     */
/* -------------------------------------------------------------------------- */

std::string MusicWifiService::url_decode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i + 1]);
            int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (in[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

std::string MusicWifiService::sanitize_filename(const std::string& raw)
{
    // Take only the basename after any '/' or '\'
    size_t slash     = raw.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? raw : raw.substr(slash + 1);
    base             = trim_str(base);
    if (base.size() <= 4 || base[0] == '.') {
        return "";
    }

    // Check extension (.mp3 or .wav)
    auto dot = base.find_last_of('.');
    if (dot == std::string::npos) {
        return "";
    }
    std::string ext = base.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext != ".mp3" && ext != ".wav") {
        return "";
    }

    // Keep FAT32-safe ASCII characters
    std::string stem = base.substr(0, dot);
    std::string clean;
    clean.reserve(stem.size());
    for (unsigned char ch : stem) {
        if (std::isalnum(ch) || ch == ' ' || ch == '-' || ch == '_' || ch == '(' || ch == ')' || ch == '.') {
            clean.push_back(static_cast<char>(ch));
        } else if (ch >= 128) {
            clean.push_back('_');
        }
    }
    clean = trim_str(clean);
    if (clean.empty()) {
        clean = "track";
    }
    if (clean.size() > 64) {
        clean.resize(64);
    }
    return clean + ext;
}
