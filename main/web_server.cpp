#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>
#include <nvs.h>

#include "bridge_action.h"
#include "ir_engine.h"
#include "local_discovery.h"
#include "status_led.h"
#include "web_server.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = nullptr;
static char s_api_key[17] = {};
extern "C" esp_err_t app_open_commissioning_window(uint16_t timeout_seconds);

static const char *kNvsNamespaceWeb = "web_config";
static const char *kNvsKeyApiKey = "api_key";

static void save_api_key_to_nvs()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespaceWeb, NVS_READWRITE, &handle);
    if (err != ESP_OK) return;
    nvs_set_str(handle, kNvsKeyApiKey, s_api_key);
    nvs_commit(handle);
    nvs_close(handle);
}

static void generate_api_key()
{
    // Try loading from NVS first
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespaceWeb, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len = sizeof(s_api_key);
        err = nvs_get_str(handle, kNvsKeyApiKey, s_api_key, &len);
        nvs_close(handle);
        if (err == ESP_OK && s_api_key[0] != '\0') {
            ESP_LOGI(TAG, "API Key loaded from NVS (persistent)");
            ESP_LOGW(TAG, "========================================");
            ESP_LOGW(TAG, "  Web API Key: %s", s_api_key);
            ESP_LOGW(TAG, "========================================");
            return;
        }
    }

    // Generate new random key and save
    uint32_t r0 = esp_random();
    uint32_t r1 = esp_random();
    snprintf(s_api_key, sizeof(s_api_key), "%08lx%08lx",
             static_cast<unsigned long>(r0), static_cast<unsigned long>(r1));
    save_api_key_to_nvs();
    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, "  Web API Key: %s (new, saved to NVS)", s_api_key);
    ESP_LOGW(TAG, "========================================");
}

static bool check_api_key(httpd_req_t *req)
{
    char key_buf[20] = {};
    esp_err_t err = httpd_req_get_hdr_value_str(req, "X-Api-Key", key_buf, sizeof(key_buf));
    if (err == ESP_OK && strcmp(key_buf, s_api_key) == 0) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"invalid or missing X-Api-Key\"}");
    return false;
}

static bool parse_u32_field(const char *json, const char *field_name, uint32_t *out_value);
static bool parse_string_field(const char *json, const char *field_name, char *out_value, size_t out_size);

static esp_err_t register_uri_handler_checked(httpd_handle_t server, const httpd_uri_t *uri)
{
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI %s (method=%d): %s", uri->uri, static_cast<int>(uri->method),
                 esp_err_to_name(err));
    }
    return err;
}
static const char *kDashboardHtml =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP Matter Hub</title><style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin:24px;background:#f4f6f8;color:#111}"
    ".card{background:#fff;border-radius:12px;padding:16px;margin-bottom:16px;box-shadow:0 2px 10px rgba(0,0,0,.06)}"
    "button{padding:8px 12px;border:0;border-radius:8px;background:#0a84ff;color:#fff;cursor:pointer}"
    "button:disabled{background:#98a2b3;cursor:not-allowed}"
    ".btn-replay{background:#a6e3a1;color:#1e1e2e}"
    "button.sm{padding:4px 8px;font-size:12px;background:#dc3545}"
    "input{padding:8px;border:1px solid #cfd6dd;border-radius:8px;margin-right:8px}"
    "table{width:100%;border-collapse:collapse}th,td{padding:8px;border-bottom:1px solid #e9edf0;text-align:left}"
    ".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
    ".muted{color:#667085;font-size:13px}"
    ".pill{display:inline-block;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:600}"
    ".pill.wait{background:#fff4ce;color:#7a5d00}.pill.ok{background:#dcfce7;color:#166534}.pill.err{background:#fee2e2;color:#991b1b}"
    ".sys{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:8px}"
    ".sys div{background:#f9fafb;padding:8px 12px;border-radius:8px}.sys .label{font-size:11px;color:#667085}.sys .val{font-size:18px;font-weight:600}"
    ".cache-bar{display:flex;gap:2px;margin:8px 0}.buf-slot{width:24px;height:24px;border-radius:4px;background:#e9edf0;display:flex;align-items:center;justify-content:center;font-size:10px;font-weight:600}"
    ".buf-slot.used{background:#0a84ff;color:#fff}"
    ".pulse{animation:pulse .7s ease-in-out}@keyframes pulse{0%{transform:scale(1)}50%{transform:scale(1.06)}100%{transform:scale(1)}}"
    "</style></head><body>"
    "<h1>ESP Matter Hub <span class='muted' style='font-size:14px'>v3.1</span></h1>"

    "<div class='card' id='authCard'><h3>API Key</h3><div class='row'>"
    "<input id='apiKey' type='password' placeholder='enter key from serial console' style='width:240px'/>"
    "<button onclick='verifyKey()'>Unlock</button>"
    "<span id='authStatus' class='muted'></span></div>"
    "<p class='muted'>Enter the API key shown in serial log on boot to unlock the dashboard.</p></div>"

    "<div id='mainContent' style='display:none'>"

    "<div class='card'><h3>System</h3><div class='sys' id='sysInfo'><div><div class='label'>Status</div><div class='val'>-</div></div></div></div>"

    "<div class='card'><h3>Signal Buffer</h3>"
    "<p class='muted'>App sends IR data via Matter (SendSignalWithRaw). Hub buffers for fast replay. Persisted to NVS when full.</p>"
    "<div id='bufferBar' class='cache-bar'></div>"
    "<p id='bufferInfo' class='muted'>-</p>"
    "<table><thead><tr><th>#</th><th>Signal ID</th><th>Carrier</th><th>Repeat</th><th>Items</th></tr></thead><tbody id='cache'></tbody></table></div>"

    "<div class='card'><h3>Endpoint Slots</h3>"
    "<p class='muted'>Configure button type and signal mapping per slot.</p>"
    "<button onclick='refreshSlots()'>Refresh</button>"
    "<table><thead><tr><th>Slot</th><th>EP</th><th>Type</th><th>Name</th><th id='thA'>Signal A</th><th id='thB'>Signal B</th><th></th></tr></thead><tbody id='slots'></tbody></table></div>"

    "<div class='card'><h3>IR Learn</h3><div class='row'>"
    "<input id='timeoutSec' type='number' value='15' min='1' step='1' style='width:60px'/>"
    "<button id='startLearnBtn' onclick='startLearn()'>Start</button>"
    "<input id='replayRepeat' type='number' value='3' min='1' max='10' style='width:50px;display:none'/>"
    "<button id='replayBtn' class='btn-replay' onclick='replayLearn()' style='display:none'>Replay</button>"
    "</div><p id='status' class='muted'>-</p><p id='captureHint' class='pill wait'>Idle</p>"
    "<div id='learnResult' style='display:none;margin-top:8px'>"
    "<p class='muted'>Carrier: <span id='lrCarrier'>-</span> Hz | Ticks: <span id='lrLen'>-</span></p>"
    "<textarea id='lrTicks' readonly style='width:100%;height:48px;font-size:11px;font-family:monospace;background:#f9fafb;border:1px solid #cfd6dd;border-radius:6px;padding:6px;resize:none'></textarea>"
    "</div></div>"

    "<div class='card'><h3>Persisted Signals</h3>"
    "<p class='muted'>Signals currently in the buffer (persisted to NVS when evicted).</p>"
    "<table><thead><tr><th>Signal ID</th><th>Carrier</th><th>Repeat</th><th>Ref Count</th><th>Last Seen</th><th>Items</th></tr></thead><tbody id='persisted'></tbody></table></div>"

    "</div>"
    "<script>"
    "function getKey(){return sessionStorage.getItem('apiKey')||'';}"
    "async function verifyKey(){const k=document.getElementById('apiKey').value;if(!k){document.getElementById('authStatus').textContent='Key required';return;}"
    "sessionStorage.setItem('apiKey',k);"
    "const ok=await testKey(k);if(ok){unlockUI();}else{sessionStorage.removeItem('apiKey');document.getElementById('authStatus').textContent='Invalid key';}}"
    "async function testKey(k){try{const r=await fetch('/api/key/verify',{method:'POST',headers:{'X-Api-Key':k}});return r.status===200;}catch{return false;}}"
    "function unlockUI(){document.getElementById('mainContent').style.display='';document.getElementById('authCard').style.display='none';initDashboard();}"
    "async function tryAutoUnlock(){const k=getKey();if(!k)return;const ok=await testKey(k);if(ok){document.getElementById('apiKey').value=k;unlockUI();}else{sessionStorage.removeItem('apiKey');}}"
    "async function j(u,o){o=o||{};o.headers=o.headers||{};const k=getKey();if(k)o.headers['X-Api-Key']=k;const r=await fetch(u,o);return[r.status,await r.json().catch(()=>({}))];}"

    "async function refreshSys(){const [s,d]=await j('/api/health');if(s!==200)return;"
    "const el=document.getElementById('sysInfo');"
    "el.innerHTML=`<div><div class='label'>Status</div><div class='val'>${d.status}</div></div>"
    "<div><div class='label'>Slots</div><div class='val'>${d.slots}</div></div>"
    "<div><div class='label'>Heap Free</div><div class='val'>${d.heap_free?Math.round(d.heap_free/1024)+'K':'-'}</div></div>"
    "<div><div class='label'>Heap Min</div><div class='val'>${d.heap_min?Math.round(d.heap_min/1024)+'K':'-'}</div></div>"
    "<div><div class='label'>mDNS</div><div class='val'>${d.mdns}</div></div>"
    "<div><div class='label'>LED</div><div class='val'>${d.led_state}</div></div>"
    "<div><div class='label'>Hostname</div><div class='val' style='font-size:12px'>${d.fqdn||'-'}</div></div>`;}"

    "async function refreshBuffer(){const [s,d]=await j('/api/buffer');if(s!==200)return;const entries=d.entries||[];"
    "const used=entries.filter(e=>e.valid).length;document.getElementById('bufferInfo').textContent=`${used}/${entries.length} buffered`;"
    "const bar=document.getElementById('bufferBar');bar.innerHTML='';for(let i=0;i<entries.length;i++){const e=entries[i];"
    "const div=document.createElement('div');div.className='buf-slot'+(e.valid?' used':'');div.textContent=e.valid?e.signal_id:'';div.title=e.valid?`sig=${e.signal_id} carrier=${e.carrier_hz} items=${e.item_count}`:'empty';bar.appendChild(div);}"
    "const t=document.getElementById('cache');t.innerHTML='';for(const e of entries){if(!e.valid)continue;const tr=document.createElement('tr');"
    "tr.innerHTML=`<td>${entries.indexOf(e)}</td><td>${e.signal_id}</td><td>${e.carrier_hz}</td><td>${e.repeat}</td><td>${e.item_count}</td>`;t.appendChild(tr);}"
    "const pt=document.getElementById('persisted');pt.innerHTML='';for(const e of entries){if(!e.valid)continue;const tr=document.createElement('tr');"
    "const ls=e.last_seen_at>0?new Date(e.last_seen_at).toLocaleString():'-';"
    "tr.innerHTML=`<td>${e.signal_id}</td><td>${e.carrier_hz}</td><td>${e.repeat}</td><td>${e.ref_count||0}</td><td>${ls}</td><td>${e.item_count}</td>`;pt.appendChild(tr);}}"

    "const BT=['ONOFF','LEVEL','ONLYON'];"
    "const LBL_A=['ON','UP','Signal'];const LBL_B=['OFF','DOWN',''];"
    "async function refreshSlots(){const [s,d]=await j('/api/slots');const list=d.slots||[];"
    "const t=document.getElementById('slots');t.innerHTML='';for(const x of list){const tr=document.createElement('tr');"
    "const sel=BT.map((n,i)=>`<option value='${i}'${i===x.button_type?' selected':''}>${n}</option>`).join('');"
    "const showB=(x.button_type!==2);const hideBs=showB?'display:inline':'display:none';const hideBl=showB?'display:inline-block':'display:none';"
    "const la=LBL_A[x.button_type]||'A';const lb=LBL_B[x.button_type]||'B';const lblW='display:inline-block;width:70px;font-size:12px';"
    "tr.innerHTML=`<td>${x.slot_id}</td><td>${x.endpoint_id}</td>"
    "<td><select id='bt-${x.slot_id}' onchange='onBtChange(${x.slot_id})'>${sel}</select></td>"
    "<td><input id='dn-${x.slot_id}' value='${x.display_name||''}' style='width:100px'/></td>"
    "<td><span id='la-${x.slot_id}' style='${lblW}'>${la}</span><input id='sa-${x.slot_id}' type='number' min='0' value='${x.signal_id_a}' style='width:70px'/></td>"
    "<td><span id='lb-${x.slot_id}' style='${lblW};${hideBl}'>${lb}</span><input id='sb-${x.slot_id}' type='number' min='0' value='${x.signal_id_b}' style='width:70px;${hideBs}'/></td>"
    "<td><button onclick='saveSlot(${x.slot_id})'>Save</button></td>`;t.appendChild(tr);}}"
    "function onBtChange(id){const v=Number(document.getElementById('bt-'+id).value);"
    "const la=document.getElementById('la-'+id);const lb=document.getElementById('lb-'+id);"
    "const sb=document.getElementById('sb-'+id);"
    "la.textContent=LBL_A[v]||'A';"
    "lb.textContent=LBL_B[v]||'B';"
    "sb.style.display=(v===2)?'none':'inline';"
    "lb.style.display=(v===2)?'none':'inline-block';}"
    "async function saveSlot(id){const bt=Number(document.getElementById('bt-'+id).value);"
    "const sa=Number(document.getElementById('sa-'+id).value)||0;const sb=Number(document.getElementById('sb-'+id).value)||0;"
    "const dn=document.getElementById('dn-'+id).value;"
    "const [s,d]=await j('/api/slots/'+id+'/config',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({button_type:bt,signal_id_a:sa,signal_id_b:sb,display_name:dn})});"
    "if(s===200){await refreshSlots();}else{alert('Error: '+JSON.stringify(d));}}"

    "let lastCaptureKey='';"
    "async function refreshStatus(){const [s,d]=await j('/api/learn/status');"
    "const b=document.getElementById('startLearnBtn');if(b)b.disabled=(d.state==='in_progress');"
    "document.getElementById('status').textContent=`state=${d.state} elapsed=${d.elapsed_ms}ms captured_len=${d.captured_len||0} quality=${d.quality_score||0}`;"
    "const h=document.getElementById('captureHint');"
    "if(d.state==='in_progress'){h.className='pill wait';h.textContent='Listening...';}"
    "else if(d.state==='ready'&&(d.captured_len||0)>0){const key=`${d.rx_source}-${d.captured_len}-${d.quality_score}`;h.className='pill ok';h.textContent=`Captured! len=${d.captured_len}`;if(lastCaptureKey!==key){h.classList.add('pulse');setTimeout(()=>h.classList.remove('pulse'),700);lastCaptureKey=key;fetchLearnedPayload();}}"
    "else if(d.state==='failed'){h.className='pill err';h.textContent='Timeout';document.getElementById('learnResult').style.display='none';document.getElementById('replayBtn').style.display='none';}"
    "else{h.className='pill wait';h.textContent='Idle';}}"

    "let learnedTicks='';"
    "let learnedCarrier=38000;"
    "async function fetchLearnedPayload(){const [s,d]=await j('/api/learn/payload');if(s!==200||!d.ticks)return;"
    "learnedTicks=d.ticks;learnedCarrier=d.carrier||38000;"
    "document.getElementById('lrCarrier').textContent=learnedCarrier;"
    "document.getElementById('lrLen').textContent=d.len||0;"
    "document.getElementById('lrTicks').value=learnedTicks;"
    "document.getElementById('learnResult').style.display='';"
    "document.getElementById('replayRepeat').style.display='';"
    "document.getElementById('replayBtn').style.display='';}"

    "async function replayLearn(){if(!learnedTicks)return;"
    "const rep=Number(document.getElementById('replayRepeat').value)||3;"
    "const [s,d]=await j('/api/learn/replay',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({carrier_hz:learnedCarrier,repeat:rep,ticks:learnedTicks})});"
    "document.getElementById('captureHint').textContent=s===200?'Replayed! (x'+rep+')':'Replay failed';}"

    "async function startLearn(){const t=Number(document.getElementById('timeoutSec').value)||15;"
    "document.getElementById('learnResult').style.display='none';document.getElementById('replayRepeat').style.display='none';document.getElementById('replayBtn').style.display='none';"
    "const [s,d]=await j('/api/learn/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timeout_s:t})});"
    "document.getElementById('captureHint').className='pill wait';document.getElementById('captureHint').textContent='Listening...';setTimeout(refreshStatus,300);}"

    "function initDashboard(){refreshSys();refreshBuffer();refreshSlots();refreshStatus();"
    "setInterval(refreshStatus,1000);setInterval(()=>{refreshSys();refreshBuffer();},5000);}"
    "tryAutoUnlock();"
    "</script></body></html>";

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t begin_json_stream(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return ESP_OK;
}

static esp_err_t end_json_stream(httpd_req_t *req)
{
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static esp_err_t dashboard_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, kDashboardHtml);
}

static esp_err_t no_content_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    size_t slot_count = 0;
    bridge_action_get_slots(&slot_count);

    const char *hostname = app_local_discovery_hostname();
    const char *fqdn = app_local_discovery_fqdn();
    const char *mdns_state = app_local_discovery_ready() ? "ready" : "disabled";
    const char *led_state = status_led_get_state_str();

    char body[384];
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"service\":\"esp-matter-hub\",\"slots\":%u,\"hostname\":\"%s\",\"fqdn\":\"%s\",\"mdns\":\"%s\",\"led_state\":\"%s\","
             "\"heap_free\":%lu,\"heap_min\":%lu}",
             static_cast<unsigned>(slot_count), hostname, fqdn, mdns_state, led_state,
             static_cast<unsigned long>(esp_get_free_heap_size()),
             static_cast<unsigned long>(esp_get_minimum_free_heap_size()));
    return send_json(req, body);
}

static esp_err_t buffer_get_handler(httpd_req_t *req)
{
    size_t count = 0;
    const signal_buffer_entry_t *entries = ir_engine_buffer_get_all(&count);

    esp_err_t err = begin_json_stream(req);
    if (err != ESP_OK) {
        return err;
    }

    err = httpd_resp_sendstr_chunk(req, "{\"entries\":[");
    if (err != ESP_OK) {
        return err;
    }

    char item[192];
    for (size_t i = 0; i < count; ++i) {
        const signal_buffer_entry_t &e = entries[i];
        snprintf(item, sizeof(item),
                 "%s{\"valid\":%s,\"signal_id\":%lu,\"carrier_hz\":%lu,\"repeat\":%u,\"item_count\":%u,\"ref_count\":%lu,\"last_seen_at\":%lld}",
                 (i == 0) ? "" : ",",
                 e.valid ? "true" : "false",
                 static_cast<unsigned long>(e.signal_id),
                 static_cast<unsigned long>(e.carrier_hz),
                 e.repeat,
                 static_cast<unsigned>(e.item_count),
                 static_cast<unsigned long>(e.ref_count),
                 static_cast<long long>(e.last_seen_at));
        err = httpd_resp_sendstr_chunk(req, item);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = httpd_resp_sendstr_chunk(req, "]}");
    if (err != ESP_OK) {
        return err;
    }
    return end_json_stream(req);
}

static esp_err_t slots_get_handler(httpd_req_t *req)
{
    size_t slot_count = 0;
    const bridge_slot_state_t *slots = bridge_action_get_slots(&slot_count);

    esp_err_t err = begin_json_stream(req);
    if (err != ESP_OK) {
        return err;
    }

    err = httpd_resp_sendstr_chunk(req, "{\"slots\":[");
    if (err != ESP_OK) {
        return err;
    }

    char item[256];
    for (size_t i = 0; i < slot_count; ++i) {
        const bridge_slot_state_t &slot = slots[i];
        snprintf(item, sizeof(item),
                 "%s{\"slot_id\":%u,\"endpoint_id\":%u,\"button_type\":%u,\"button_type_name\":\"%s\","
                 "\"display_name\":\"%s\",\"signal_id_a\":%lu,\"signal_id_b\":%lu}",
                 (i == 0) ? "" : ",", slot.slot_id, slot.endpoint_id,
                 static_cast<unsigned>(slot.button_type), bridge_action_button_type_name(slot.button_type),
                 slot.display_name,
                 static_cast<unsigned long>(slot.signal_id_a), static_cast<unsigned long>(slot.signal_id_b));
        err = httpd_resp_sendstr_chunk(req, item);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = httpd_resp_sendstr_chunk(req, "]}");
    if (err != ESP_OK) {
        return err;
    }
    return end_json_stream(req);
}

static bool parse_slot_id_from_uri(const char *uri, uint8_t *slot_id)
{
    const char *prefix = "/api/slots/";
    size_t prefix_len = strlen(prefix);
    if (!uri || !slot_id || strncmp(uri, prefix, prefix_len) != 0) {
        return false;
    }
    const char *id_str = uri + prefix_len;
    char *end = nullptr;
    unsigned long id = strtoul(id_str, &end, 10);
    if (end == id_str || id > 255) {
        return false;
    }
    *slot_id = static_cast<uint8_t>(id);
    return true;
}

static esp_err_t api_key_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }

    if (req->content_len == 0 || req->content_len >= 128) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    char body[128];
    int read_len = httpd_req_recv(req, body, req->content_len);
    if (read_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    body[read_len] = '\0';

    char new_key[20] = {};
    parse_string_field(body, "key", new_key, sizeof(new_key));
    if (strlen(new_key) < 4) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key must be at least 4 characters");
    }

    strlcpy(s_api_key, new_key, sizeof(s_api_key));
    save_api_key_to_nvs();
    ESP_LOGI(TAG, "API key updated and saved to NVS");
    return send_json(req, "{\"status\":\"ok\",\"message\":\"API key updated\"}");
}

static esp_err_t key_verify_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }
    return send_json(req, "{\"status\":\"ok\"}");
}

static esp_err_t slot_config_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }

    uint8_t slot_id = 0;
    if (!parse_slot_id_from_uri(req->uri, &slot_id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot URI");
    }

    if (req->content_len == 0 || req->content_len >= 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    char body[256];
    int read_len = httpd_req_recv(req, body, req->content_len);
    if (read_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    body[read_len] = '\0';

    uint32_t type_val = 0, sig_a = 0, sig_b = 0;
    parse_u32_field(body, "button_type", &type_val);
    parse_u32_field(body, "signal_id_a", &sig_a);
    parse_u32_field(body, "signal_id_b", &sig_b);

    char display_name[40] = {};
    parse_string_field(body, "display_name", display_name, sizeof(display_name));

    esp_err_t err = bridge_action_configure_slot(slot_id, static_cast<button_type_t>(type_val), sig_a, sig_b,
                                                  display_name[0] ? display_name : nullptr);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot_id or button_type");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to configure slot");
    }

    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"ok\",\"slot_id\":%u}", slot_id);
    return send_json(req, response);
}

static bool parse_u32_field(const char *json, const char *field_name, uint32_t *out_value)
{
    if (!json || !field_name || !out_value) {
        return false;
    }

    const char *found = strstr(json, field_name);
    if (!found) {
        return false;
    }

    const char *colon = strchr(found, ':');
    if (!colon) {
        return false;
    }
    colon++;
    while (*colon == ' ' || *colon == '\t') {
        colon++;
    }

    char *end = nullptr;
    unsigned long value = strtoul(colon, &end, 10);
    if (end == colon) {
        return false;
    }

    *out_value = static_cast<uint32_t>(value);
    return true;
}

static bool parse_string_field(const char *json, const char *field_name, char *out_value, size_t out_size)
{
    if (!json || !field_name || !out_value || out_size == 0) {
        return false;
    }

    const char *found = strstr(json, field_name);
    if (!found) {
        return false;
    }

    const char *colon = strchr(found, ':');
    if (!colon) {
        return false;
    }
    const char *first_quote = strchr(colon, '"');
    if (!first_quote) {
        return false;
    }
    first_quote++;

    const char *second_quote = strchr(first_quote, '"');
    if (!second_quote || second_quote <= first_quote) {
        return false;
    }

    size_t len = static_cast<size_t>(second_quote - first_quote);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out_value, first_quote, len);
    out_value[len] = '\0';
    return true;
}

static esp_err_t commissioning_open_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }
    uint32_t timeout_s = 300;
    if (req->content_len > 0 && req->content_len < 128) {
        char body[128];
        int read_len = httpd_req_recv(req, body, req->content_len);
        if (read_len > 0) {
            body[read_len] = '\0';
            uint32_t parsed_timeout = 0;
            if (parse_u32_field(body, "timeout_s", &parsed_timeout) && parsed_timeout > 0) {
                timeout_s = parsed_timeout;
            }
        }
    }
    esp_err_t err = app_open_commissioning_window(static_cast<uint16_t>(timeout_s));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to open commissioning window");
    }
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"ok\",\"commissioning_window\":\"opened\",\"timeout_s\":%lu}",
             static_cast<unsigned long>(timeout_s));
    return send_json(req, response);
}

static esp_err_t learn_start_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }
    uint32_t timeout_ms = 15000;
    if (req->content_len > 0 && req->content_len < 128) {
        char body[128];
        int read_len = httpd_req_recv(req, body, req->content_len);
        if (read_len > 0) {
            body[read_len] = '\0';
            uint32_t parsed_timeout = 0;
            if (parse_u32_field(body, "timeout_s", &parsed_timeout) && parsed_timeout > 0) {
                timeout_ms = parsed_timeout * 1000;
            } else if (parse_u32_field(body, "timeout_ms", &parsed_timeout) && parsed_timeout > 0) {
                timeout_ms = parsed_timeout;
            }
        }
    }

    esp_err_t err = ir_engine_start_learning(timeout_ms);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"status\":\"error\",\"message\":\"learning already in progress\"}");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to start learning");
    }

    return send_json(req, "{\"status\":\"ok\",\"learning\":\"started\"}");
}

static esp_err_t learn_status_get_handler(httpd_req_t *req)
{
    ir_learning_status_t status;
    ir_engine_get_learning_status(&status);

    const char *state_str = "unknown";
    switch (status.state) {
    case IR_LEARNING_IDLE:
        state_str = "idle";
        break;
    case IR_LEARNING_IN_PROGRESS:
        state_str = "in_progress";
        break;
    case IR_LEARNING_READY:
        state_str = "ready";
        break;
    case IR_LEARNING_FAILED:
        state_str = "failed";
        break;
    default:
        break;
    }

    char response[256];
    snprintf(response, sizeof(response),
             "{\"state\":\"%s\",\"elapsed_ms\":%lu,\"timeout_ms\":%lu,\"last_signal_id\":%lu,\"rx_source\":%u,\"captured_len\":%u,\"quality_score\":%u}",
             state_str, static_cast<unsigned long>(status.elapsed_ms), static_cast<unsigned long>(status.timeout_ms),
             static_cast<unsigned long>(status.last_signal_id), status.rx_source, status.captured_len,
             status.quality_score);
    return send_json(req, response);
}

static esp_err_t learn_payload_get_handler(httpd_req_t *req)
{
    uint8_t tick_len = 0;
    uint32_t carrier = 0;
    const uint16_t *ticks = ir_engine_get_learned_ticks(&tick_len, &carrier);

    if (!ticks || tick_len == 0) {
        return send_json(req, "{\"state\":\"not_ready\"}");
    }

    char response[1280];
    int off = snprintf(response, sizeof(response),
                       "{\"state\":\"ready\",\"carrier\":%lu,\"len\":%u,\"ticks\":\"",
                       static_cast<unsigned long>(carrier), tick_len);
    for (uint8_t i = 0; i < tick_len && off < (int)(sizeof(response) - 8); ++i) {
        off += snprintf(response + off, sizeof(response) - off, "%02X%02X",
                        ticks[i] & 0xFF, (ticks[i] >> 8) & 0xFF);
    }
    off += snprintf(response + off, sizeof(response) - off, "\"}");
    return send_json(req, response);
}

static esp_err_t learn_replay_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }

    uint8_t tick_len = 0;
    uint32_t carrier = 0;
    const uint16_t *ticks = ir_engine_get_learned_ticks(&tick_len, &carrier);

    if (!ticks || tick_len == 0) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"error\":\"no learned signal\"}");
    }

    uint8_t repeat = 1;
    if (req->content_len > 0 && req->content_len < 128) {
        char body[128];
        int read_len = httpd_req_recv(req, body, req->content_len);
        if (read_len > 0) {
            body[read_len] = '\0';
            uint32_t parsed_repeat = 0;
            if (parse_u32_field(body, "repeat", &parsed_repeat) && parsed_repeat > 0) {
                repeat = static_cast<uint8_t>(parsed_repeat);
            }
        }
    }

    esp_err_t err = ir_engine_send_raw(0, carrier, repeat, ticks, tick_len);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "replay failed");
    }

    return send_json(req, "{\"status\":\"ok\",\"replayed\":true}");
}

esp_err_t app_web_server_start()
{
    if (s_server) {
        return ESP_OK;
    }

    generate_api_key();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 20;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", err);
        return err;
    }

    const httpd_uri_t health_uri = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &health_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t slots_uri = {
        .uri = "/api/slots",
        .method = HTTP_GET,
        .handler = slots_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &slots_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t learn_start_uri = {
        .uri = "/api/learn/start",
        .method = HTTP_POST,
        .handler = learn_start_post_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &learn_start_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t learn_status_uri = {
        .uri = "/api/learn/status",
        .method = HTTP_GET,
        .handler = learn_status_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &learn_status_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t learn_payload_uri = {
        .uri = "/api/learn/payload",
        .method = HTTP_GET,
        .handler = learn_payload_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &learn_payload_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t learn_replay_uri = {
        .uri = "/api/learn/replay",
        .method = HTTP_POST,
        .handler = learn_replay_post_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &learn_replay_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t cache_uri = {
        .uri = "/api/buffer",
        .method = HTTP_GET,
        .handler = buffer_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &cache_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t api_key_uri = {
        .uri = "/api/key",
        .method = HTTP_POST,
        .handler = api_key_post_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &api_key_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t key_verify_uri = {
        .uri = "/api/key/verify",
        .method = HTTP_POST,
        .handler = key_verify_post_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &key_verify_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t slot_config_uri = {
        .uri = "/api/slots/*",
        .method = HTTP_POST,
        .handler = slot_config_post_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &slot_config_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t commissioning_open_uri = {
        .uri = "/api/commissioning/open",
        .method = HTTP_POST,
        .handler = commissioning_open_post_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &commissioning_open_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t dashboard_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = dashboard_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &dashboard_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = no_content_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &favicon_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t apple_icon_uri = {
        .uri = "/apple-touch-icon.png",
        .method = HTTP_GET,
        .handler = no_content_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &apple_icon_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t apple_icon_precomposed_uri = {
        .uri = "/apple-touch-icon-precomposed.png",
        .method = HTTP_GET,
        .handler = no_content_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &apple_icon_precomposed_uri);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "HTTP API started: GET /, /api/health, /api/slots, /api/buffer, POST /api/slots/*/config, /api/learn/*, /api/commissioning/open");
    return ESP_OK;
}
