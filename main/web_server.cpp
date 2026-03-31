#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>
#include <nvs.h>

#include "activity_log.h"
#include "bridge_action.h"
#include "ir_engine.h"
#include "local_discovery.h"
#include "status_led.h"
#include "test_signals.h"
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
    ".card{background:#fff;border-radius:12px;padding:16px;margin-bottom:16px;box-shadow:0 2px 8px rgba(0,0,0,.06)}"
    "button{padding:8px 12px;border:0;border-radius:8px;background:#0a84ff;color:#fff;cursor:pointer}"
    "button:disabled{background:#98a2b3;cursor:not-allowed}"
    ".gr{background:#a6e3a1;color:#1e1e2e}"
    ".sm{padding:4px 8px;font-size:12px;background:#dc3545}.sm.u{background:#0a84ff}.sm.gr{background:#a6e3a1;color:#1e1e2e}"
    "input{padding:8px;border:1px solid #cfd6dd;border-radius:8px;margin-right:8px}"
    "table{width:100%;border-collapse:collapse}th,td{padding:8px;border-bottom:1px solid #e9edf0;text-align:left}"
    ".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.mu{color:#667085;font-size:13px}"
    ".pill{display:inline-block;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:600}"
    ".pill.w{background:#fff4ce;color:#7a5d00}.pill.ok{background:#dcfce7;color:#166534}.pill.er{background:#fee2e2;color:#991b1b}"
    ".sys{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:8px}"
    ".sys div{background:#f9fafb;padding:8px 12px;border-radius:8px}"
    ".sl{font-size:11px;color:#667085}.sv{font-size:18px;font-weight:600}"
    ".ta{width:100%;height:48px;font-size:11px;font-family:monospace;background:#f9fafb;border:1px solid #cfd6dd;border-radius:6px;padding:6px;resize:none;margin-top:4px}"
    ".pulse{animation:P .7s ease-in-out}@keyframes P{0%,100%{transform:scale(1)}50%{transform:scale(1.06)}}"
    ".dm{position:absolute;z-index:99;background:#fff;border:1px solid #cfd6dd;border-radius:8px;min-width:180px;max-height:180px;overflow-y:auto}"
    ".dm div{padding:6px 10px;cursor:pointer;font-size:13px}.dm div:hover{background:#f0f4ff}"
    ".g2{display:grid;grid-template-columns:auto 1fr;gap:4px 10px;font-size:13px}.g2 .l{color:#667085}"
    "input[type=number]::-webkit-inner-spin-button,input[type=number]::-webkit-outer-spin-button{-webkit-appearance:none;margin:0}"
    "input[type=number]{-moz-appearance:textfield}"
    "summary{cursor:pointer;color:#0a84ff;font-size:12px}"
    "</style></head><body>"
    "<h1>ESP Matter Hub <span class='mu' style='font-size:14px'>v3.5</span></h1>"
    "<div class='card' id='aC'><h3>API Key</h3><div class='row'>"
    "<input id='apiKey' type='password' placeholder='enter key from serial console' style='width:240px'/>"
    "<button onclick='vKey()'>Unlock</button><span id='aS' class='mu'></span></div>"
    "<p class='mu'>Enter the API key shown in serial log on boot to unlock the dashboard.</p></div>"
    "<div id='mC' style='display:none'>"
    "<div class='card'><h3>System</h3><div class='sys' id='sI'><div><div class='sl'>Status</div><div class='sv'>-</div></div></div></div>"
    "<div class='card'><h3>Endpoint Slots</h3><p class='mu'>Configure button type and signal mapping per slot.</p>"
    "<button onclick='rSlots()' style='margin-bottom:8px'>Refresh</button>"
    "<table><thead><tr><th>Slot</th><th>EP</th><th>Type</th><th>Name</th><th>Signal A</th><th>Signal B</th><th></th></tr></thead>"
    "<tbody id='sl'></tbody></table></div>"
    "<div class='card'><h3>IR Learn</h3><div class='row'>"
    "<input id='tSec' type='number' value='15' min='1' style='width:60px'/>"
    "<button id='slB' onclick='sLearn()'>Start</button>"
    "</div><p id='cH' class='pill w'>Idle</p>"
    "<div id='lR' style='display:none;margin-top:8px'>"
    "<div class='g2'><div class='l'>Carrier Hz</div><div id='lC'>-</div>"
    "<div class='l'>Ticks</div><div id='lL'>-</div>"
    "<div class='l'>Quality</div><div id='lQ'>-</div></div>"
    "<details><summary>Ticks hex</summary>"
    "<textarea id='lT' readonly class='ta'></textarea>"
    "</details>"
    "<div class='row' style='margin-top:8px'>"
    "<input id='rRep' type='number' value='3' min='1' max='10' style='width:50px'/>"
    "<button class='gr' onclick='rLearn()'>Replay</button>"
    "</div>"
    "<div class='row' style='margin-top:6px;align-items:center'>"
    "<label style='font-size:12px'><input id='rInt' type='checkbox'/> Interval</label>"
    "<input id='rDur' type='number' value='1000' min='100' max='30000' step='100' style='width:70px' disabled/>"
    "<span class='mu' style='font-size:11px'>ms</span>"
    "</div>"
    "</div></div>"
    "<div class='card'><h3>Signal Buffer <span class='mu' style='font-size:12px'>(RAM, last 16)</span></h3>"
    "<button onclick='rBuf()' style='margin-bottom:8px'>Refresh</button>"
    "<table><thead><tr><th>Signal ID</th><th>Carrier</th><th>Repeat</th><th>Ticks</th><th></th></tr></thead>"
    "<tbody id='bT'><tr><td colspan='5' class='mu'>SendSignalWithRaw 수신 시 여기에 표시됩니다</td></tr></tbody></table></div>"
    "<div class='card'><h3>Saved Test Signals</h3>"
    "<table><thead><tr><th>Name</th><th>Signal ID</th><th>Carrier</th><th>Repeat</th><th></th></tr></thead>"
    "<tbody id='tT'><tr><td colspan='5' class='mu'>Signal Buffer에서 Use as Test로 저장하세요</td></tr></tbody></table></div>"
    "<div class='card'><h3>Activity Log</h3>"
    "<button onclick='rLogs()' style='margin-bottom:8px'>Refresh</button>"
    "<table><thead><tr><th>Time</th><th>Action</th><th>Detail</th></tr></thead>"
    "<tbody id='lgT'><tr><td colspan='3' class='mu'>작업 수행 시 자동으로 기록됩니다</td></tr></tbody></table></div>"
    "</div><script>"
    "const g=id=>document.getElementById(id);"
    "const jp=(u,b)=>j(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});"
    "const nm=s=>{const v=(s||'').trim();return v?v.substring(0,31):null;};"
    "function gKey(){return sessionStorage.getItem('apiKey')||'';}"
    "async function tKey(k){try{return(await fetch('/api/key/verify',{method:'POST',headers:{'X-Api-Key':k}})).status===200;}catch{return false;}}"
    "async function vKey(){const k=g('apiKey').value;if(!k){g('aS').textContent='Key required';return;}"
    "sessionStorage.setItem('apiKey',k);"
    "if(await tKey(k)){uUI();}else{sessionStorage.removeItem('apiKey');g('aS').textContent='Invalid key';}}"
    "function uUI(){g('mC').style.display='';g('aC').style.display='none';iDB();}"
    "async function tryAutoUnlock(){const k=gKey();if(k&&await tKey(k)){g('apiKey').value=k;uUI();}else if(k)sessionStorage.removeItem('apiKey');}"
    "async function j(u,o){o=o||{};o.headers=Object.assign({'X-Api-Key':gKey()},o.headers||{});const r=await fetch(u,o);return[r.status,await r.json().catch(()=>({}))];}"
    "const sv=(l,v,x)=>`<div><div class='sl'>${l}</div><div class='sv'${x?' style=\\'font-size:12px\\'':''}>${v}</div></div>`;"
    "async function rSys(){const [s,d]=await j('/api/health');if(s!==200)return;"
    "g('sI').innerHTML=sv('Status',d.status)+sv('Slots',d.slots)+sv('Heap Free',d.heap_free?Math.round(d.heap_free/1024)+'K':'-')+sv('Heap Min',d.heap_min?Math.round(d.heap_min/1024)+'K':'-')+sv('mDNS',d.mdns)+sv('LED',d.led_state)+sv('Hostname',d.fqdn||'-',1);}"
    "const BT=['ONOFF','LEVEL','ONLYON'];const LA=['ON','UP','Sig'];const LB=['OFF','DOWN',''];"
    "const ni=(id,v)=>Number(g(id+'-'+v).value)||0;"
    "async function rSlots(){const [,d]=await j('/api/slots');const list=d.slots||[];"
    "const t=g('sl');t.innerHTML='';for(const x of list){const tr=document.createElement('tr');"
    "const sel=BT.map((n,i)=>`<option value='${i}'${i===x.button_type?' selected':''}>${n}</option>`).join('');"
    "const h2=x.button_type===2?'display:none':'';"
    "tr.innerHTML=`<td>${x.slot_id}</td><td>${x.endpoint_id}</td>"
    "<td><select id='bt-${x.slot_id}' onchange='oBt(${x.slot_id})'>${sel}</select></td>"
    "<td><input id='dn-${x.slot_id}' value='${x.display_name||''}' style='width:80px'/></td>"
    "<td><span id='la-${x.slot_id}'>${LA[x.button_type]||'A'}</span> "
    "<input id='sa-${x.slot_id}' type='number' min='0' value='${x.signal_id_a}' style='width:60px'/>"
    "<button class='sm u' onclick='sDD(${x.slot_id},\"a\")'>&#9660;</button>"
    "<div id='dda-${x.slot_id}' class='dm' style='display:none'></div></td>"
    "<td><span id='lb-${x.slot_id}' style='${h2}'>${LB[x.button_type]||'B'}</span> "
    "<input id='sb-${x.slot_id}' type='number' min='0' value='${x.signal_id_b}' style='width:60px;${h2}'/>"
    "<button id='dbb-${x.slot_id}' class='sm u' style='${h2}' onclick='sDD(${x.slot_id},\"b\")'>&#9660;</button>"
    "<div id='ddb-${x.slot_id}' class='dm' style='display:none'></div></td>"
    "<td><button onclick='svSl(${x.slot_id})'>Save</button></td>`;t.appendChild(tr);}}"
    "function oBt(id){const v=Number(g('bt-'+id).value);const h=v===2?'none':'';"
    "['la','lb','sb','dbb'].forEach(p=>{const e=g(p+'-'+id);if(e)e.style.display=h;});"
    "g('la-'+id).textContent=LA[v]||'A';g('lb-'+id).textContent=LB[v]||'B';}"
    "async function svSl(id){const bt=ni('bt',id),sa=ni('sa',id),sb=ni('sb',id),dn=g('dn-'+id).value;"
    "const [s,d]=await jp('/api/slots/'+id+'/config',{button_type:bt,signal_id_a:sa,signal_id_b:sb,display_name:dn});"
    "if(s===200)rSlots();else alert('Error: '+JSON.stringify(d));}"
    "async function sDD(si,f){const di='dd'+f+'-'+si;const dd=g(di);"
    "if(!dd)return;dd.textContent='...';dd.style.display='block';"
    "const [,list]=await j('/api/test-signals');"
    "if(!list||!list.length){dd.textContent='No saved signals';return;}"
    "dd.innerHTML=list.map(e=>`<div onclick='pDD(${si},\"${f}\",${e.signal_id},\"${di}\")'>&ldquo;${e.name}&rdquo; (sig ${e.signal_id})</div>`).join('');}"
    "function pDD(si,f,id,di){const i=g('s'+f+'-'+si);if(i)i.value=id;const d=g(di);if(d)d.style.display='none';}"
    "document.addEventListener('click',function(ev){document.querySelectorAll('.dm').forEach(d=>{if(d.style.display!=='none'&&!d.contains(ev.target)&&!ev.target.classList.contains('u'))d.style.display='none';});});"
    "let lCK='',lTk='',lCr=38000;"
    "async function rSt(){const [s,d]=await j('/api/learn/status');"
    "const b=g('slB');if(b)b.disabled=(d.state==='in_progress');"
    "const h=g('cH');"
    "if(d.state==='in_progress'){h.className='pill w';h.textContent='Listening...';}"
    "else if(d.state==='ready'&&d.captured_len>0){const k=`${d.rx_source}-${d.captured_len}-${d.quality_score}`;"
    "h.className='pill ok';h.textContent=`Captured! len=${d.captured_len}`;"
    "if(lCK!==k){h.classList.add('pulse');setTimeout(()=>h.classList.remove('pulse'),700);lCK=k;fLP();}}"
    "else if(d.state==='failed'){h.className='pill er';h.textContent='Timeout';g('lR').style.display='none';}"
    "else{h.className='pill w';h.textContent='Idle';}}"
    "async function fLP(){const [s,d]=await j('/api/learn/payload');if(s!==200||!d.ticks)return;"
    "lTk=d.ticks;lCr=d.carrier||38000;"
    "g('lC').textContent=lCr;g('lL').textContent=d.len||0;g('lQ').textContent=d.quality_score||'-';"
    "g('lT').value=lTk;g('lR').style.display='';}"
    "async function rLearn(){if(!lTk)return;const rep=Number(g('rRep').value)||3;"
    "const body={repeat:rep};"
    "if(g('rInt').checked){body.duration_ms=Number(g('rDur').value)||1000;}"
    "const [s]=await jp('/api/learn/replay',body);"
    "g('cH').textContent=s===200?'Replayed! (x'+rep+(body.duration_ms?', '+body.duration_ms+'ms interval':'')+')':"
    "'Replay failed';}"
    "async function sLearn(){const t=Number(g('tSec').value)||15;g('lR').style.display='none';"
    "await jp('/api/learn/start',{timeout_s:t});"
    "g('cH').className='pill w';g('cH').textContent='Listening...';setTimeout(rSt,300);}"
    ""
    "const mn='font-family:monospace;font-size:11px';"
    "function eT(id,c,msg){const t=g(id);if(!c||!c.length){t.innerHTML=`<tr><td colspan='9' class='mu'>${msg}</td></tr>`;return false;}return true;}"
    "async function rBuf(){const [,list]=await j('/api/signal-buffer');"
    "if(!eT('bT',list,'Empty'))return;"
    "g('bT').innerHTML=list.map((e,i)=>`<tr><td>${e.signal_id}</td><td>${e.carrier_hz}</td><td>${e.repeat}</td>"
    "<td style='${mn}'>${(e.ticks_hex||'').substring(0,20)}...</td>"
    "<td><button class='sm u' onclick='savBuf(${i})'>Use as Test</button></td></tr>`).join('');}"
    "async function savBuf(idx){const n=nm(prompt('Name:'));if(!n)return;"
    "const [rs]=await jp('/api/save-from-buffer',{name:n,buffer_index:idx});"
    "if(rs===200)rTS();else if(rs===409)alert('Duplicate name');else alert('Save failed');}"
    "async function rTS(){const [,list]=await j('/api/test-signals');"
    "if(!eT('tT',list,'No saved signals'))return;"
    "g('tT').innerHTML=list.map((e,i)=>`<tr><td>${e.name}</td><td>${e.signal_id}</td><td>${e.carrier_hz}</td><td>${e.repeat}</td>"
    "<td><button class='sm gr' style='min-width:40px' onclick='tstTS(${i},this)'>Test</button> <button class='sm' onclick='delTS(${i})'>Del</button></td></tr>`).join('');}"
    "async function tstTS(idx,btn){if(btn){btn.textContent='\\u00B7\\u00B7\\u00B7';btn.disabled=true;}"
    "const [s]=await jp('/api/test-signal-replay/'+idx,{});"
    "if(btn){btn.textContent=s===200?'\\u2713':'\\u2717';btn.disabled=false;"
    "setTimeout(()=>{btn.textContent='Test';},1000);}}"
    "async function delTS(idx){if(!confirm('Delete #'+idx+'?'))return;"
    "const [s]=await j('/api/test-signals/'+idx,{method:'DELETE'});"
    "if(s===200)rTS();else alert('Delete failed');}"
    "const AN=['IR Learn','Send Raw','Replay','Slot Config','Commissioning','Dump NVS','Fct Reset','API Key'];"
    "async function rLogs(){const [,list]=await j('/api/logs');"
    "if(!eT('lgT',list,'No entries'))return;"
    "function fmtTs(t){if(!t||t<1700000000)return'-';const d=new Date(t*1000);"
    "const M=(d.getMonth()+1).toString().padStart(2,'0'),D=d.getDate().toString().padStart(2,'0');"
    "const h=d.getHours().toString().padStart(2,'0'),m=d.getMinutes().toString().padStart(2,'0'),s=d.getSeconds().toString().padStart(2,'0');"
    "return M+'/'+D+' '+h+':'+m+':'+s;}"
    "function fmtD(v){if(!v||typeof v==='string')return v||'';try{return JSON.stringify(v);}catch{return String(v);}}"
    "g('lgT').innerHTML=list.map(e=>`<tr><td>${fmtTs(e.ts)}</td><td>${AN[e.act]||e.act}</td>"
    "<td style='${mn}'>${fmtD(e.d)}</td></tr>`).join('');}"
    "function iDB(){rSys();rSlots();rSt();rBuf();rTS();rLogs();"
    "setInterval(rSt,1000);setInterval(rSys,5000);setInterval(rBuf,5000);setInterval(rLogs,10000);"
    "g('rInt').onchange=function(){g('rDur').disabled=!this.checked;};}"
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
    (void)activity_log_append(ACT_API_KEY_CHANGE, "{}");
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

    char act_detail[40];
    snprintf(act_detail, sizeof(act_detail), "{\"slot\":%u,\"type\":%lu}",
             slot_id, static_cast<unsigned long>(type_val));
    (void)activity_log_append(ACT_SLOT_CONFIG, act_detail);
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
    char comm_detail[40];
    snprintf(comm_detail, sizeof(comm_detail), "{\"timeout_s\":%lu}", static_cast<unsigned long>(timeout_s));
    (void)activity_log_append(ACT_COMMISSIONING, comm_detail);
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

    char learn_detail[40];
    snprintf(learn_detail, sizeof(learn_detail), "{\"timeout_ms\":%lu}", static_cast<unsigned long>(timeout_ms));
    (void)activity_log_append(ACT_IR_LEARN, learn_detail);
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
    uint32_t duration_ms = 0;
    if (req->content_len > 0 && req->content_len < 128) {
        char body[128];
        int read_len = httpd_req_recv(req, body, req->content_len);
        if (read_len > 0) {
            body[read_len] = '\0';
            uint32_t parsed_repeat = 0;
            if (parse_u32_field(body, "repeat", &parsed_repeat) && parsed_repeat > 0) {
                repeat = static_cast<uint8_t>(parsed_repeat);
            }
            parse_u32_field(body, "duration_ms", &duration_ms);
        }
    }

    // Clamp interval mode: max 5s interval, max 10 repeats (worst case ~50s httpd block)
    if (duration_ms > 5000) duration_ms = 5000;
    if (repeat > 10) repeat = 10;

    if (duration_ms > 0 && repeat > 1) {
        // Interval mode: send once every duration_ms, repeat times total
        for (uint8_t i = 0; i < repeat; ++i) {
            esp_err_t err = ir_engine_send_raw(0, carrier, 1, ticks, tick_len);
            if (err != ESP_OK) {
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "replay failed");
            }
            if (i + 1U < repeat) {
                vTaskDelay(pdMS_TO_TICKS(duration_ms));
            }
        }
    } else {
        // Default: rapid burst repeat
        esp_err_t err = ir_engine_send_raw(0, carrier, repeat, ticks, tick_len);
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "replay failed");
        }
    }

    char replay_detail[64];
    snprintf(replay_detail, sizeof(replay_detail), "{\"carrier\":%lu,\"repeat\":%u,\"dur\":%lu}",
             static_cast<unsigned long>(carrier), (unsigned)repeat, static_cast<unsigned long>(duration_ms));
    (void)activity_log_append(ACT_REPLAY, replay_detail);
    return send_json(req, "{\"status\":\"ok\",\"replayed\":true}");
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    int count = activity_log_get_count();
    char entry_buf[256];
    for (int i = 0; i < count; i++) {
        if (i > 0) httpd_resp_sendstr_chunk(req, ",");
        activity_log_read_entry_json(i, entry_buf, sizeof(entry_buf));
        httpd_resp_sendstr_chunk(req, entry_buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t signal_buffer_get_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    int count = signal_buffer_get_count();
    char entry_buf[256];
    for (int i = 0; i < count; i++) {
        if (i > 0) httpd_resp_sendstr_chunk(req, ",");
        signal_buffer_read_entry_json(i, entry_buf, sizeof(entry_buf));
        httpd_resp_sendstr_chunk(req, entry_buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t test_signals_get_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    int count = test_signals_get_count();
    char entry_buf[256];
    for (int i = 0; i < count; i++) {
        if (i > 0) httpd_resp_sendstr_chunk(req, ",");
        test_signals_read_entry_json(i, entry_buf, sizeof(entry_buf));
        httpd_resp_sendstr_chunk(req, entry_buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t test_signals_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }

    if (req->content_len == 0 || req->content_len >= 640) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    char body[640];
    int read_len = httpd_req_recv(req, body, req->content_len);
    if (read_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    body[read_len] = '\0';

    char name[32] = {};
    parse_string_field(body, "name", name, sizeof(name));
    if (name[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name required");
    }

    uint32_t signal_id = 0, carrier_hz = 0, repeat = 1;
    parse_u32_field(body, "signal_id", &signal_id);
    parse_u32_field(body, "carrier_hz", &carrier_hz);
    parse_u32_field(body, "repeat", &repeat);

    char ticks_hex[520] = {};
    parse_string_field(body, "ticks_hex", ticks_hex, sizeof(ticks_hex));

    esp_err_t err = test_signals_save(name, signal_id, carrier_hz,
                                      static_cast<uint8_t>(repeat), ticks_hex);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"error\":\"duplicate name\"}");
    }
    if (err == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "507 Insufficient Storage");
        return send_json(req, "{\"error\":\"NVS full\"}");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }

    return send_json(req, "{\"status\":\"ok\"}");
}

static esp_err_t test_signals_delete_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }

    const char *prefix = "/api/test-signals/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(req->uri, prefix, prefix_len) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid URI");
    }

    const char *idx_str = req->uri + prefix_len;
    char *end = nullptr;
    unsigned long idx = strtoul(idx_str, &end, 10);
    if (end == idx_str || idx > 255) {
        httpd_resp_set_status(req, "404 Not Found");
        return send_json(req, "{\"error\":\"invalid index\"}");
    }

    esp_err_t err = test_signals_delete(static_cast<uint8_t>(idx));
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "404 Not Found");
        return send_json(req, "{\"error\":\"index out of range\"}");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
    }

    return send_json(req, "{\"status\":\"ok\"}");
}

static esp_err_t save_from_buffer_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }

    if (req->content_len == 0 || req->content_len >= 128) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char body[128];
    int len = httpd_req_recv(req, body, req->content_len);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    }
    body[len] = '\0';

    char name[32] = {};
    parse_string_field(body, "name", name, sizeof(name));
    if (!name[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name required");
    }

    uint32_t buffer_index = 0;
    parse_u32_field(body, "buffer_index", &buffer_index);

    esp_err_t err = test_signals_save_from_buffer(name, static_cast<uint8_t>(buffer_index));
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"error\":\"duplicate name\"}");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "404 Not Found");
        return send_json(req, "{\"error\":\"invalid buffer index\"}");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }

    return send_json(req, "{\"status\":\"ok\"}");
}

static esp_err_t test_signals_test_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) { return ESP_OK; }

    // Parse index from URI: /api/test-signal-replay/{idx}
    const char *prefix = "/api/test-signal-replay/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(req->uri, prefix, prefix_len) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid URI");
    }
    const char *idx_str = req->uri + prefix_len;
    char *end = nullptr;
    unsigned long idx = strtoul(idx_str, &end, 10);
    if (end == idx_str || idx > 255) {
        httpd_resp_set_status(req, "404 Not Found");
        return send_json(req, "{\"error\":\"invalid URI\"}");
    }

    uint32_t carrier_hz = 0;
    uint8_t repeat = 1;
    char ticks_hex[520];
    ticks_hex[0] = '\0';

    esp_err_t err = test_signals_get_replay_data(static_cast<uint8_t>(idx),
                                                  &carrier_hz, &repeat, ticks_hex, sizeof(ticks_hex));
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "404 Not Found");
        return send_json(req, "{\"error\":\"index out of range\"}");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
    }

    // Decode hex string to uint16_t ticks (signal buffer stores %04X per tick)
    size_t hex_len = strlen(ticks_hex);
    if (hex_len < 4 || (hex_len % 4) != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "invalid ticks data");
    }
    uint16_t ticks[128];
    size_t tick_count = 0;
    for (size_t i = 0; i + 3 < hex_len && tick_count < 128; i += 4) {
        unsigned val;
        if (sscanf(ticks_hex + i, "%4x", &val) == 1) {
            ticks[tick_count++] = (uint16_t)val;
        }
    }

    if (tick_count == 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ticks decoded");
    }

    err = ir_engine_send_raw(0, carrier_hz, repeat, ticks, tick_count);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "send failed");
    }

    return send_json(req, "{\"status\":\"ok\",\"tested\":true}");
}

esp_err_t app_web_server_start()
{
    if (s_server) {
        return ESP_OK;
    }

    activity_log_init();
    test_signals_init();

    generate_api_key();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 24;
    config.stack_size = 8192;

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

    const httpd_uri_t logs_uri = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = logs_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &logs_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t signal_buffer_uri = {
        .uri = "/api/signal-buffer",
        .method = HTTP_GET,
        .handler = signal_buffer_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &signal_buffer_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t test_signals_get_uri = {
        .uri = "/api/test-signals",
        .method = HTTP_GET,
        .handler = test_signals_get_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &test_signals_get_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t test_signals_post_uri = {
        .uri = "/api/test-signals",
        .method = HTTP_POST,
        .handler = test_signals_post_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &test_signals_post_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t test_signals_delete_uri = {
        .uri = "/api/test-signals/*",
        .method = HTTP_DELETE,
        .handler = test_signals_delete_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &test_signals_delete_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t save_from_buffer_uri = {
        .uri = "/api/save-from-buffer",
        .method = HTTP_POST,
        .handler = save_from_buffer_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &save_from_buffer_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t test_signals_test_uri = {
        .uri = "/api/test-signal-replay/*",
        .method = HTTP_POST,
        .handler = test_signals_test_handler,
        .user_ctx = nullptr,
    };
    err = register_uri_handler_checked(s_server, &test_signals_test_uri);
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
