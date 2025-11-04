#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

#include "freertos/FreeRTOS.h"

#include <cJSON.h>
#include <driver/uart.h>
#include "esp_log.h"
#include <esp_http_server.h>
#include "esp_system.h"
#include "esp_timer.h"

#include "ser2net_opts.h"
#include "runtime.h"
#include "session_ops.h"
#include "control_port.h"
#include "adapters.h"
#include "net_manager.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#define MAX_REQUEST_BODY 1024

static const char WEB_INDEX_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"  <meta charset=\"utf-8\" />\n"
"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n"
"  <title>ser2net MCU – Wi-Fi Setup</title>\n"
"  <link rel=\"stylesheet\" href=\"/static/app.css\" />\n"
"  <script src=\"/static/app.js\" defer></script>\n"
"</head>\n"
"<body>\n"
"  <header class=\"topbar\">\n"
"    <div class=\"brand\">ser2net MCU</div>\n"
"    <div class=\"subtitle\">Embedded RFC2217 Gateway</div>\n"
"  </header>\n"
"  <main class=\"content\">\n"
"    <section class=\"card\" id=\"wifi-status-card\">\n"
"      <h2>Wi-Fi Status</h2>\n"
"      <div class=\"status-grid\">\n"
"        <div>\n"
"          <span class=\"label\">Station SSID</span>\n"
"          <span class=\"value\" id=\"sta-ssid\">–</span>\n"
"        </div>\n"
"        <div>\n"
"          <span class=\"label\">Station IP</span>\n"
"          <span class=\"value\" id=\"sta-ip\">–</span>\n"
"        </div>\n"
"        <div>\n"
"          <span class=\"label\">Station State</span>\n"
"          <span class=\"value\" id=\"sta-state\">–</span>\n"
"        </div>\n"
"        <div>\n"
"          <span class=\"label\">Provisioning SoftAP</span>\n"
"          <span class=\"value\" id=\"ap-state\">–</span>\n"
"        </div>\n"
"        <div>\n"
"          <span class=\"label\">SoftAP Timeout</span>\n"
"          <span class=\"value\" id=\"ap-timeout\">–</span>\n"
"        </div>\n"
"      </div>\n"
"      <div class=\"actions\">\n"
"        <button id=\"toggle-ap\" class=\"button secondary\">Toggle SoftAP</button>\n"
"        <button id=\"forget-wifi\" class=\"button danger\">Forget Credentials</button>\n"
"      </div>\n"
"    </section>\n"
"    <section class=\"card\">\n"
"      <h2>Configure Station Wi-Fi</h2>\n"
"      <form id=\"wifi-form\" autocomplete=\"off\">\n"
"        <div class=\"form-row\">\n"
"          <label for=\"ssid\">SSID</label>\n"
"          <input type=\"text\" id=\"ssid\" name=\"ssid\" maxlength=\"32\" required placeholder=\"Network name\" />\n"
"        </div>\n"
"        <div class=\"form-row\">\n"
"          <label for=\"password\">Password</label>\n"
"          <input type=\"password\" id=\"password\" name=\"password\" maxlength=\"64\" placeholder=\"leave empty for open network\" />\n"
"        </div>\n"
"        <div class=\"form-row inline\">\n"
"          <label for=\"keep-ap\">\n"
"            <input type=\"checkbox\" id=\"keep-ap\" name=\"keep_ap\" checked />\n"
"            Keep provisioning SoftAP enabled after applying credentials\n"
"          </label>\n"
"        </div>\n"
"        <div class=\"form-row\">\n"
"          <button type=\"submit\" class=\"button primary\">Save &amp; Connect</button>\n"
"        </div>\n"
"      </form>\n"
"      <div id=\"message\" class=\"message\" hidden></div>\n"
"    </section>\n"
"    <section class=\"card\" id=\"ports-card\">\n"
"      <h2>Serial Ports</h2>\n"
"      <div class=\"table-wrapper\">\n"
"        <table class=\"status-table\">\n"
"          <thead>\n"
"            <tr>\n"
"              <th>TCP Port</th>\n"
"              <th>UART</th>\n"
"              <th>Mode</th>\n"
"              <th>Enabled</th>\n"
"              <th>Baud</th>\n"
"              <th>Frame</th>\n"
"              <th>Flow</th>\n"
"              <th>Sessions</th>\n"
"            </tr>\n"
"          </thead>\n"
"          <tbody id=\"ports-table-body\">\n"
"            <tr><td colspan=\"8\">Loading…</td></tr>\n"
"          </tbody>\n"
"        </table>\n"
"      </div>\n"
"    </section>\n"
"  </main>\n"
"</body>\n"
"</html>\n";

static const char WEB_STYLE_CSS[] =
"@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;600&display=swap');\n"
"*{box-sizing:border-box;margin:0;padding:0;font-family:'Inter',sans-serif;color:#1c1c1c;}\n"
"body{background:#f2f4f8;}\n"
".topbar{background:linear-gradient(135deg,#0f5fb6,#0b3d82);color:#fff;padding:1.5rem 2rem;display:flex;flex-direction:column;gap:.3rem;box-shadow:0 4px 12px rgba(0,0,0,.2);}\n"
".brand{font-size:1.6rem;font-weight:600;letter-spacing:.04em;text-transform:uppercase;}\n"
".subtitle{opacity:.85;font-size:.9rem;}\n"
".content{display:grid;gap:1.5rem;padding:2rem;max-width:960px;margin:0 auto;}\n"
".card{background:#fff;border-radius:16px;padding:2rem;box-shadow:0 12px 30px rgba(15,50,90,.12);border:1px solid rgba(12,53,109,.06);}\n"
".card h2{margin-bottom:1.2rem;color:#0b3d82;font-size:1.25rem;font-weight:600;}\n"
".status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:1rem;margin-bottom:1.5rem;}\n"
".label{display:block;font-size:.75rem;text-transform:uppercase;letter-spacing:.08em;color:#6b778c;margin-bottom:.35rem;}\n"
".value{font-size:1rem;font-weight:600;color:#172b4d;}\n"
".actions{display:flex;gap:.75rem;flex-wrap:wrap;}\n"
".button{border:none;padding:.6rem 1.3rem;border-radius:999px;font-size:.95rem;font-weight:600;cursor:pointer;transition:transform .15s ease,box-shadow .15s ease;}\n"
".button:hover{transform:translateY(-1px);box-shadow:0 8px 18px rgba(10,30,60,.18);}\n"
".button.primary{background:#0f5fb6;color:#fff;}\n"
".button.secondary{background:#e1ebf8;color:#10396f;}\n"
".button.danger{background:#ff6f61;color:#fff;}\n"
".form-row{display:flex;flex-direction:column;margin-bottom:1.2rem;}\n"
".form-row.inline{flex-direction:row;align-items:center;gap:.75rem;}\n"
"label{font-size:.85rem;font-weight:600;color:#344563;margin-bottom:.45rem;}\n"
"input[type=text],input[type=password]{border:1px solid rgba(15,63,118,.18);border-radius:10px;padding:.65rem .9rem;font-size:1rem;background:#f9fbff;transition:border-color .2s ease,box-shadow .2s ease;}\n"
"input[type=text]:focus,input[type=password]:focus{outline:none;border-color:#0f5fb6;box-shadow:0 0 0 3px rgba(15,95,182,.18);}\n"
".message{margin-top:1rem;padding:.85rem 1rem;border-radius:12px;font-size:.95rem;font-weight:500;background:#e8f4ff;color:#0b3d82;border:1px solid rgba(15,95,182,.25);}\n"
".message.error{background:#ffeceb;color:#7d1b1b;border-color:rgba(200,40,40,.35);}\n"
".message.success{background:#ecfff0;color:#17613b;border-color:rgba(34,139,76,.35);}\n"
".table-wrapper{overflow-x:auto;}\n"
".status-table{width:100%;border-collapse:collapse;font-size:.92rem;}\n"
".status-table th,.status-table td{padding:.6rem .75rem;text-align:left;border-bottom:1px solid rgba(15,63,118,.12);}\n"
".status-table th{font-size:.75rem;text-transform:uppercase;letter-spacing:.08em;color:#5c6c80;background:#f5f8ff;}\n"
".status-table tbody tr:hover{background:#f2f7ff;}\n"
"@media(max-width:640px){.card{padding:1.5rem;} .topbar{padding:1.2rem 1.5rem;}}\n";

static const char WEB_APP_JS[] =
"const statusElements={ssid:document.getElementById('sta-ssid'),ip:document.getElementById('sta-ip'),state:document.getElementById('sta-state'),ap:document.getElementById('ap-state'),apTimeout:document.getElementById('ap-timeout')};\n"
"const messageBox=document.getElementById('message');\n"
"const form=document.getElementById('wifi-form');\n"
"const toggleApBtn=document.getElementById('toggle-ap');\n"
"const forgetBtn=document.getElementById('forget-wifi');\n"
"const portsTableBody=document.getElementById('ports-table-body');\n"
"\n"
"function showMessage(text,type='info'){\n"
"  messageBox.textContent=text;\n"
"  messageBox.classList.remove('error','success');\n"
"  if(type==='error') messageBox.classList.add('error');\n"
"  if(type==='success') messageBox.classList.add('success');\n"
"  messageBox.hidden=false;\n"
"}\n"
"\n"
"function hideMessage(){messageBox.hidden=true;}\n"
"\n"
"function describeState(connected,configured){\n"
"  if(!configured) return 'Not configured';\n"
"  return connected?'Connected':'Disconnected';\n"
"}\n"
"\n"
"function describeAp(active,forcedDisable,remaining){\n"
"  if(forcedDisable) return 'Disabled (forced)';\n"
"  if(!active) return 'Standby';\n"
"  if(remaining>0) return `Active (${remaining}s left)`;\n"
"  return 'Active';\n"
"}\n"
"\n"
"async function fetchWifiStatus(){\n"
"  const res=await fetch('/api/wifi',{cache:'no-store'});\n"
"  if(!res.ok) throw new Error('Unable to retrieve Wi-Fi status');\n"
"  return res.json();\n"
"}\n"
"\n"
"async function fetchSystem(){\n"
"  const res=await fetch('/api/system',{cache:'no-store'});\n"
"  if(!res.ok) throw new Error('Unable to retrieve system status');\n"
"  return res.json();\n"
"}\n"
"\n"
"async function fetchPorts(){\n"
"  const res=await fetch('/api/ports',{cache:'no-store'});\n"
"  if(!res.ok) throw new Error('Unable to retrieve port list');\n"
"  return res.json();\n"
"}\n"
"\n"
"function formatFrame(port){\n"
"  return `${port.data_bits}/${port.parity}/${port.stop_bits}`;\n"
"}\n"
"\n"
"function renderPorts(ports){\n"
"  if(!Array.isArray(ports)){return;}\n"
"  portsTableBody.innerHTML='';\n"
"  if(ports.length===0){\n"
"    const row=document.createElement('tr');\n"
"    const cell=document.createElement('td');\n"
"    cell.colSpan=8;\n"
"    cell.textContent='No serial ports configured';\n"
"    row.appendChild(cell);\n"
"    portsTableBody.appendChild(row);\n"
"    return;\n"
"  }\n"
"  ports.forEach(port=>{\n"
"    const row=document.createElement('tr');\n"
"    const cells=[\n"
"      port.tcp_port,\n"
"      `UART${port.uart}`,\n"
"      port.mode,\n"
"      port.enabled?'Yes':'No',\n"
"      port.baud,\n"
"      formatFrame(port),\n"
"      port.flow_control===0?'None':'RTS/CTS',\n"
"      port.active_sessions\n"
"    ];\n"
"    cells.forEach(value=>{\n"
"      const cell=document.createElement('td');\n"
"      cell.textContent=value;\n"
"      row.appendChild(cell);\n"
"    });\n"
"    portsTableBody.appendChild(row);\n"
"  });\n"
"}\n"
"\n"
"async function refreshStatus(){\n"
"  try{\n"
"    const [wifi,sys,ports]=await Promise.allSettled([fetchWifiStatus(),fetchSystem(),fetchPorts()]);\n"
"    if(wifi.status==='fulfilled'){\n"
"      const data=wifi.value;\n"
"      statusElements.ssid.textContent=data.sta_ssid||'–';\n"
"      statusElements.ip.textContent=data.sta_ip||'–';\n"
"      statusElements.state.textContent=describeState(data.sta_connected,data.sta_configured);\n"
"      statusElements.ap.textContent=describeAp(data.softap_active,data.softap_force_disabled,data.softap_remaining_seconds);\n"
"      statusElements.apTimeout.textContent=data.softap_remaining_seconds?`${data.softap_remaining_seconds}s`:'–';\n"
"      toggleApBtn.textContent=data.softap_force_disabled?'Enable SoftAP':'Disable SoftAP';\n"
"    }\n"
"    if(sys.status==='fulfilled'){\n"
"      const uptime=document.getElementById('sta-state');\n"
"      const seconds=Math.floor(sys.value.uptime_ms/1000);\n"
"      uptime.dataset.uptime=`Uptime: ${seconds}s`;\n"
"    }\n"
"    if(ports.status==='fulfilled'){\n"
"      renderPorts(ports.value);\n"
"    }\n"
"  }catch(err){\n"
"    console.error(err);\n"
"    showMessage(err.message,'error');\n"
"  }\n"
"}\n"
"\n"
"form.addEventListener('submit',async(e)=>{\n"
"  e.preventDefault();\n"
"  hideMessage();\n"
"  const ssid=document.getElementById('ssid').value.trim();\n"
"  const password=document.getElementById('password').value;\n"
"  const keepAp=document.getElementById('keep-ap').checked;\n"
"  if(!ssid){\n"
"    showMessage('SSID must not be empty','error');\n"
"    return;\n"
"  }\n"
"  try{\n"
"    const payload={ssid,password,softap_enabled:keepAp};\n"
"    const res=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});\n"
"    if(!res.ok){\n"
"      const text=await res.text();\n"
"      throw new Error(text||'Failed to apply credentials');\n"
"    }\n"
"    showMessage('Credentials saved. Connecting…','success');\n"
"    form.reset();\n"
"    document.getElementById('keep-ap').checked=keepAp;\n"
"    await refreshStatus();\n"
"  }catch(err){\n"
"    console.error(err);\n"
"    showMessage(err.message,'error');\n"
"  }\n"
"});\n"
"\n"
"toggleApBtn.addEventListener('click',async()=>{\n"
"  hideMessage();\n"
"  try{\n"
"    const current=await fetchWifiStatus();\n"
"    const desired=current.softap_force_disabled;\n"
"    const res=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({softap_enabled:desired})});\n"
"    if(!res.ok){\n"
"      const text=await res.text();\n"
"      throw new Error(text||'Failed to toggle SoftAP');\n"
"    }\n"
"    showMessage(`SoftAP ${desired?'enabled':'disabled'}.`,'success');\n"
"    await refreshStatus();\n"
"  }catch(err){\n"
"    console.error(err);\n"
"    showMessage(err.message,'error');\n"
"  }\n"
"});\n"
"\n"
"forgetBtn.addEventListener('click',async()=>{\n"
"  hideMessage();\n"
"  if(!confirm('Forget stored Wi-Fi credentials and return to provisioning mode?')) return;\n"
"  try{\n"
"    const res=await fetch('/api/wifi',{method:'DELETE'});\n"
"    if(!res.ok){\n"
"      const text=await res.text();\n"
"      throw new Error(text||'Failed to clear credentials');\n"
"    }\n"
"    showMessage('Credentials cleared. Device is now in provisioning mode.','success');\n"
"    await refreshStatus();\n"
"  }catch(err){\n"
"    console.error(err);\n"
"    showMessage(err.message,'error');\n"
"  }\n"
"});\n"
"\n"
"document.addEventListener('DOMContentLoaded',refreshStatus);\n"
"window.setInterval(refreshStatus,5000);\n";
static esp_err_t web_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, WEB_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_style_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, WEB_STYLE_CSS, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_script_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=60");
    return httpd_resp_send(req, WEB_APP_JS, HTTPD_RESP_USE_STRLEN);
}
static const char *port_mode_to_str(enum ser2net_port_mode mode)
{
    switch (mode) {
    case SER2NET_PORT_MODE_RAW: return "raw";
    case SER2NET_PORT_MODE_RAWLP: return "rawlp";
    case SER2NET_PORT_MODE_TELNET:
    default: return "telnet";
    }
}

static enum ser2net_port_mode port_mode_from_str(const char *str)
{
    if (!str)
        return SER2NET_PORT_MODE_TELNET;
    if (strcasecmp(str, "raw") == 0)
        return SER2NET_PORT_MODE_RAW;
    if (strcasecmp(str, "rawlp") == 0)
        return SER2NET_PORT_MODE_RAWLP;
    return SER2NET_PORT_MODE_TELNET;
}

static int data_bits_to_int(uart_word_length_t len)
{
    switch (len) {
    case UART_DATA_5_BITS: return 5;
    case UART_DATA_6_BITS: return 6;
    case UART_DATA_7_BITS: return 7;
    case UART_DATA_8_BITS:
    default:
        return 8;
    }
}

static double stop_bits_to_value(uart_stop_bits_t stop)
{
    switch (stop) {
    case UART_STOP_BITS_2: return 2.0;
#ifdef UART_STOP_BITS_1_5
    case UART_STOP_BITS_1_5: return 1.5;
#endif
    case UART_STOP_BITS_1:
    default:
        return 1.0;
    }
}

static const char *parity_to_str(uart_parity_t parity)
{
    switch (parity) {
    case UART_PARITY_ODD: return "odd";
    case UART_PARITY_EVEN: return "even";
    case UART_PARITY_DISABLE:
    default:
        return "none";
    }
}

static int sessions_for_port(uint16_t tcp_port,
                             const struct ser2net_active_session *sessions,
                             size_t count)
{
    int total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (sessions[i].tcp_port == tcp_port)
            total++;
    }
    return total;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    if (status)
        httpd_resp_set_status(req, status);
    if (!message)
        message = "error";

    char buf[128];
    int written = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    if (written < 0 || written >= (int)sizeof(buf))
        return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static bool read_json_body(httpd_req_t *req, cJSON **out_json)
{
    if (!out_json)
        return false;

    if (req->content_len <= 0) {
        send_json_error(req, "400 Bad Request", "body required");
        return false;
    }
    if (req->content_len > MAX_REQUEST_BODY) {
        send_json_error(req, "413 Payload Too Large", "request too large");
        return false;
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return false;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return false;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        send_json_error(req, "400 Bad Request", "invalid json");
        return false;
    }

    *out_json = root;
    return true;
}

static const struct ser2net_esp32_serial_port_cfg *
find_port_by_tcp(uint16_t tcp_port,
                 const struct ser2net_esp32_serial_port_cfg *ports,
                 size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (ports[i].tcp_port == tcp_port)
            return &ports[i];
    }
    return NULL;
}

static bool
parse_port_and_action(const char *uri, uint16_t *out_port, const char **out_action)
{
    static const char base[] = "/api/ports/";

    if (!uri)
        return false;

    size_t base_len = strlen(base);
    if (strncmp(uri, base, base_len) != 0)
        return false;

    const char *cursor = uri + base_len;
    if (!*cursor)
        return false;

    char port_buf[8];
    size_t idx = 0;
    while (*cursor && *cursor != '/' && *cursor != '?') {
        if (idx >= sizeof(port_buf) - 1)
            return false;
        if (*cursor < '0' || *cursor > '9')
            return false;
        port_buf[idx++] = *cursor++;
    }
    port_buf[idx] = '\0';

    if (idx == 0)
        return false;

    char *end = NULL;
    long tcp_val = strtol(port_buf, &end, 10);
    if (!end || *end != '\0' || tcp_val <= 0 || tcp_val > 65535)
        return false;

    if (out_port)
        *out_port = (uint16_t) tcp_val;

    while (*cursor == '/')
        cursor++;

    if (out_action)
        *out_action = cursor;

    return true;
}

static void fill_params_from_cfg(const struct ser2net_esp32_serial_port_cfg *cfg,
                                 struct ser2net_serial_params *params)
{
    if (!cfg || !params)
        return;
    params->baud = cfg->baud_rate;
    switch (cfg->data_bits) {
    case UART_DATA_5_BITS: params->data_bits = 5; break;
    case UART_DATA_6_BITS: params->data_bits = 6; break;
    case UART_DATA_7_BITS: params->data_bits = 7; break;
    default: params->data_bits = 8; break;
    }
    if (cfg->parity == UART_PARITY_ODD)
        params->parity = 1;
    else if (cfg->parity == UART_PARITY_EVEN)
        params->parity = 2;
    else
        params->parity = 0;

    switch (cfg->stop_bits) {
    case UART_STOP_BITS_2: params->stop_bits = 2; break;
#ifdef UART_STOP_BITS_1_5
    case UART_STOP_BITS_1_5: params->stop_bits = 15; break;
#endif
    default: params->stop_bits = 1; break;
    }

    params->flow_control = (cfg->flow_ctrl == UART_HW_FLOWCTRL_CTS_RTS) ? 1 : 0;
}

static cJSON *port_to_json(const struct ser2net_esp32_serial_port_cfg *cfg, int active_sessions)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;

    cJSON_AddNumberToObject(obj, "tcp_port", cfg->tcp_port);
    cJSON_AddNumberToObject(obj, "uart", cfg->uart_num);
    cJSON_AddNumberToObject(obj, "tx_pin", cfg->tx_pin);
    cJSON_AddNumberToObject(obj, "rx_pin", cfg->rx_pin);
    if (cfg->rts_pin != UART_PIN_NO_CHANGE)
        cJSON_AddNumberToObject(obj, "rts_pin", cfg->rts_pin);
    if (cfg->cts_pin != UART_PIN_NO_CHANGE)
        cJSON_AddNumberToObject(obj, "cts_pin", cfg->cts_pin);
    cJSON_AddStringToObject(obj, "mode", port_mode_to_str(cfg->mode));
    cJSON_AddBoolToObject(obj, "enabled", cfg->enabled);
    cJSON_AddNumberToObject(obj, "baud", cfg->baud_rate);
    cJSON_AddNumberToObject(obj, "data_bits", data_bits_to_int(cfg->data_bits));
    cJSON_AddStringToObject(obj, "parity", parity_to_str(cfg->parity));
    cJSON_AddNumberToObject(obj, "stop_bits", stop_bits_to_value(cfg->stop_bits));
    cJSON_AddNumberToObject(obj, "flow_control", cfg->flow_ctrl);
    cJSON_AddNumberToObject(obj, "idle_timeout_ms", cfg->idle_timeout_ms);
    cJSON_AddNumberToObject(obj, "active_sessions", active_sessions);
    return obj;
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *json, int status)
{
    char *payload = cJSON_PrintUnformatted(json);
    if (!payload)
        return httpd_resp_send_500(req);

    if (status == 201)
        httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    cJSON_free(payload);
    return res;
}

static esp_err_t health_handler(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return httpd_resp_send_500(req);
    cJSON_AddStringToObject(obj, "status", "ok");
    esp_err_t res = send_json_response(req, obj, 200);
    cJSON_Delete(obj);
    return res;
}

static esp_err_t ports_get_handler(httpd_req_t *req)
{
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);

    struct ser2net_active_session sessions[SER2NET_MAX_PORTS];
    size_t session_count = ser2net_runtime_list_sessions(sessions, SER2NET_MAX_PORTS);

    cJSON *root = cJSON_CreateArray();
    if (!root)
        return httpd_resp_send_500(req);

    for (size_t i = 0; i < count; ++i) {
        int active = sessions_for_port(ports[i].tcp_port, sessions, session_count);
        cJSON *item = port_to_json(&ports[i], active);
        if (!item) {
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }
        cJSON_AddItemToArray(root, item);
    }

    esp_err_t res = send_json_response(req, root, 200);
    cJSON_Delete(root);
    return res;
}

static esp_err_t system_get_handler(httpd_req_t *req)
{
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t port_count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    struct ser2net_active_session sessions[SER2NET_MAX_PORTS];
    size_t session_count = ser2net_runtime_list_sessions(sessions, SER2NET_MAX_PORTS);

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return httpd_resp_send_500(req);

    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(root, "free_heap", (double) esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", (double) esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "configured_ports", (double) port_count);
    cJSON_AddNumberToObject(root, "active_sessions", (double) session_count);

    esp_err_t res = send_json_response(req, root, 200);
    cJSON_Delete(root);
    return res;
}

static cJSON *wifi_status_to_json(const struct net_manager_status *status)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON_AddBoolToObject(root, "sta_configured", status->sta_configured);
    cJSON_AddBoolToObject(root, "sta_connected", status->sta_connected);
    cJSON_AddStringToObject(root, "sta_ssid", status->sta_ssid);
    cJSON_AddStringToObject(root, "sta_ip", status->sta_ip);
    cJSON_AddBoolToObject(root, "softap_active", status->ap_active);
    cJSON_AddBoolToObject(root, "softap_force_disabled", status->ap_force_disabled);
    cJSON_AddNumberToObject(root, "softap_remaining_seconds", status->ap_remaining_seconds);
    return root;
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    struct net_manager_status status;
    if (!net_manager_get_status(&status))
        return httpd_resp_send_500(req);

    cJSON *root = wifi_status_to_json(&status);
    if (!root)
        return httpd_resp_send_500(req);

    esp_err_t res = send_json_response(req, root, 200);
    cJSON_Delete(root);
    return res;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (!read_json_body(req, &root))
        return ESP_OK;

    bool changed = false;
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *softap_enabled = cJSON_GetObjectItemCaseSensitive(root, "softap_enabled");

    if (cJSON_IsString(ssid) && ssid->valuestring && ssid->valuestring[0]) {
        const char *pass_str = "";
        if (cJSON_IsString(password) && password->valuestring)
            pass_str = password->valuestring;
        if (!net_manager_apply_credentials(ssid->valuestring, pass_str)) {
            cJSON_Delete(root);
            return send_json_error(req, "409 Conflict", "failed to apply credentials");
        }
        changed = true;
    }

    if (cJSON_IsBool(softap_enabled)) {
        bool enable = cJSON_IsTrue(softap_enabled);
        if (!net_manager_set_softap_forced_disable(!enable)) {
            cJSON_Delete(root);
            return send_json_error(req, "409 Conflict", "failed to toggle softap");
        }
        changed = true;
    }

    cJSON_Delete(root);

    if (!changed)
        return send_json_error(req, "400 Bad Request", "no changes supplied");

    struct net_manager_status status;
    if (!net_manager_get_status(&status))
        return httpd_resp_send_500(req);

    cJSON *resp = wifi_status_to_json(&status);
    if (!resp)
        return httpd_resp_send_500(req);

    esp_err_t res = send_json_response(req, resp, 200);
    cJSON_Delete(resp);
    return res;
}

static esp_err_t wifi_delete_handler(httpd_req_t *req)
{
    net_manager_forget_credentials();

    struct net_manager_status status;
    if (!net_manager_get_status(&status))
        return httpd_resp_send_500(req);

    cJSON *resp = wifi_status_to_json(&status);
    if (!resp)
        return httpd_resp_send_500(req);

    esp_err_t res = send_json_response(req, resp, 200);
    cJSON_Delete(resp);
    return res;
}

static bool parse_port_config(cJSON *root, struct ser2net_esp32_serial_port_cfg *cfg)
{
    cJSON *port_id = cJSON_GetObjectItem(root, "port_id");
    cJSON *tcp_port = cJSON_GetObjectItem(root, "tcp_port");
    cJSON *uart = cJSON_GetObjectItem(root, "uart");
    cJSON *tx_pin = cJSON_GetObjectItem(root, "tx_pin");
    cJSON *rx_pin = cJSON_GetObjectItem(root, "rx_pin");

    if (!cJSON_IsNumber(tcp_port) ||
        !cJSON_IsNumber(uart) || !cJSON_IsNumber(tx_pin) || !cJSON_IsNumber(rx_pin))
        return false;

    cfg->port_id = cJSON_IsNumber(port_id) ? port_id->valueint : -1;
    cfg->tcp_port = (uint16_t) tcp_port->valueint;
    cfg->uart_num = (uart_port_t) uart->valueint;
    cfg->tx_pin = tx_pin->valueint;
    cfg->rx_pin = rx_pin->valueint;

    cJSON *rts = cJSON_GetObjectItem(root, "rts_pin");
    if (cJSON_IsNumber(rts))
        cfg->rts_pin = rts->valueint >= 0 ? rts->valueint : UART_PIN_NO_CHANGE;

    cJSON *cts = cJSON_GetObjectItem(root, "cts_pin");
    if (cJSON_IsNumber(cts))
        cfg->cts_pin = cts->valueint >= 0 ? cts->valueint : UART_PIN_NO_CHANGE;

    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(mode))
        cfg->mode = port_mode_from_str(mode->valuestring);

    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(enabled))
        cfg->enabled = cJSON_IsTrue(enabled);

    cJSON *baud = cJSON_GetObjectItem(root, "baud");
    if (cJSON_IsNumber(baud) && baud->valueint > 0)
        cfg->baud_rate = baud->valueint;

    cJSON *data_bits = cJSON_GetObjectItem(root, "data_bits");
    if (cJSON_IsNumber(data_bits)) {
        switch (data_bits->valueint) {
        case 5: cfg->data_bits = UART_DATA_5_BITS; break;
        case 6: cfg->data_bits = UART_DATA_6_BITS; break;
        case 7: cfg->data_bits = UART_DATA_7_BITS; break;
        default: cfg->data_bits = UART_DATA_8_BITS; break;
        }
    }

    cJSON *parity = cJSON_GetObjectItem(root, "parity");
    if (cJSON_IsString(parity)) {
        if (strcasecmp(parity->valuestring, "odd") == 0)
            cfg->parity = UART_PARITY_ODD;
        else if (strcasecmp(parity->valuestring, "even") == 0)
            cfg->parity = UART_PARITY_EVEN;
        else
            cfg->parity = UART_PARITY_DISABLE;
    }

    cJSON *stop = cJSON_GetObjectItem(root, "stop_bits");
    if (cJSON_IsNumber(stop)) {
        if (stop->valuedouble >= 2.0)
            cfg->stop_bits = UART_STOP_BITS_2;
#ifdef UART_STOP_BITS_1_5
        else if (stop->valuedouble > 1.0 && stop->valuedouble < 2.0)
            cfg->stop_bits = UART_STOP_BITS_1_5;
#endif
        else
            cfg->stop_bits = UART_STOP_BITS_1;
    }

    cJSON *flow = cJSON_GetObjectItem(root, "flow_control");
    if (cJSON_IsString(flow)) {
        if (strcasecmp(flow->valuestring, "rtscts") == 0)
            cfg->flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
        else
            cfg->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    }

    cJSON *idle = cJSON_GetObjectItem(root, "idle_timeout_ms");
    if (cJSON_IsNumber(idle) && idle->valueint >= 0)
        cfg->idle_timeout_ms = idle->valueint;

    return cfg->tcp_port > 0 && cfg->uart_num >= 0 &&
           cfg->tx_pin >= 0 && cfg->rx_pin >= 0;
}

#if ENABLE_DYNAMIC_SESSIONS
static esp_err_t ports_post_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (!read_json_body(req, &root))
        return ESP_OK;

    struct ser2net_esp32_serial_port_cfg cfg = {
        .port_id = -1,
        .uart_num = UART_NUM_MAX,
        .tx_pin = -1,
        .rx_pin = -1,
        .rts_pin = UART_PIN_NO_CHANGE,
        .cts_pin = UART_PIN_NO_CHANGE,
        .tcp_port = 0,
        .tcp_backlog = 4,
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .mode = SER2NET_PORT_MODE_TELNET,
        .idle_timeout_ms = 0,
        .enabled = true
    };
    bool ok = parse_port_config(root, &cfg);
    cJSON_Delete(root);
    if (!ok)
        return send_json_error(req, "400 Bad Request", "invalid port parameters");

    if (ser2net_runtime_add_port(&cfg) != pdPASS) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "{\"error\":\"port exists or invalid\"}", HTTPD_RESP_USE_STRLEN);
    }

    struct ser2net_esp32_serial_port_cfg snapshot[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(snapshot, SER2NET_MAX_PORTS);
    struct ser2net_active_session sessions[SER2NET_MAX_PORTS];
    size_t session_count = ser2net_runtime_list_sessions(sessions, SER2NET_MAX_PORTS);

    const struct ser2net_esp32_serial_port_cfg *added = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (snapshot[i].tcp_port == cfg.tcp_port) {
            added = &snapshot[i];
            break;
        }
    }

    if (!added)
        return httpd_resp_send_500(req);

    cJSON *resp = port_to_json(added, sessions_for_port(added->tcp_port, sessions, session_count));
    if (!resp)
        return httpd_resp_send_500(req);

esp_err_t res = send_json_response(req, resp, 201);
    cJSON_Delete(resp);
    return res;
}
#else
static esp_err_t ports_post_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (read_json_body(req, &root))
        cJSON_Delete(root);
    return send_json_error(req, "403 Forbidden", "dynamic sessions disabled");
}
#endif

#if ENABLE_DYNAMIC_SESSIONS
static esp_err_t port_config_handler(httpd_req_t *req, uint16_t tcp_port)
{
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    const struct ser2net_esp32_serial_port_cfg *base = find_port_by_tcp(tcp_port, ports, count);
    if (!base)
        return send_json_error(req, "404 Not Found", "port not found");

    cJSON *root = NULL;
    if (!read_json_body(req, &root))
        return ESP_OK;

    struct ser2net_serial_params params;
    fill_params_from_cfg(base, &params);
    uint32_t idle_timeout = base->idle_timeout_ms;
    bool apply_active = false;
    struct ser2net_pin_config pins = {
        .uart_num = base->uart_num,
        .tx_pin = base->tx_pin,
        .rx_pin = base->rx_pin,
        .rts_pin = base->rts_pin,
        .cts_pin = base->cts_pin
    };
    bool pins_updated = false;

    cJSON *baud = cJSON_GetObjectItemCaseSensitive(root, "baud");
    if (cJSON_IsNumber(baud) && baud->valueint > 0)
        params.baud = baud->valueint;

    cJSON *data_bits = cJSON_GetObjectItemCaseSensitive(root, "data_bits");
    if (cJSON_IsNumber(data_bits)) {
        switch (data_bits->valueint) {
        case 5: params.data_bits = 5; break;
        case 6: params.data_bits = 6; break;
        case 7: params.data_bits = 7; break;
        case 8: params.data_bits = 8; break;
        default:
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "data_bits must be 5-8");
        }
    }

    cJSON *parity = cJSON_GetObjectItemCaseSensitive(root, "parity");
    if (cJSON_IsString(parity) && parity->valuestring) {
        if (strcasecmp(parity->valuestring, "odd") == 0)
            params.parity = 1;
        else if (strcasecmp(parity->valuestring, "even") == 0)
            params.parity = 2;
        else if (strcasecmp(parity->valuestring, "none") == 0)
            params.parity = 0;
        else {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "parity must be none/odd/even");
        }
    }

    cJSON *stop_bits = cJSON_GetObjectItemCaseSensitive(root, "stop_bits");
    if (stop_bits) {
        if (cJSON_IsNumber(stop_bits)) {
            double v = stop_bits->valuedouble;
            if (v >= 1.9)
                params.stop_bits = 2;
#ifdef UART_STOP_BITS_1_5
            else if (v > 1.0 && v < 2.0)
                params.stop_bits = 15;
#endif
            else if (v >= 0.9 && v <= 1.1)
                params.stop_bits = 1;
            else {
                cJSON_Delete(root);
                return send_json_error(req, "400 Bad Request", "stop_bits must be 1/1.5/2");
            }
        } else if (cJSON_IsString(stop_bits) && stop_bits->valuestring) {
            if (strcmp(stop_bits->valuestring, "2") == 0)
                params.stop_bits = 2;
#ifdef UART_STOP_BITS_1_5
            else if (strcmp(stop_bits->valuestring, "1.5") == 0)
                params.stop_bits = 15;
#endif
            else if (strcmp(stop_bits->valuestring, "1") == 0)
                params.stop_bits = 1;
            else {
                cJSON_Delete(root);
                return send_json_error(req, "400 Bad Request", "stop_bits must be 1/1.5/2");
            }
        }
    }

    cJSON *flow = cJSON_GetObjectItemCaseSensitive(root, "flow_control");
    if (cJSON_IsString(flow) && flow->valuestring) {
        if (strcasecmp(flow->valuestring, "rtscts") == 0)
            params.flow_control = 1;
        else if (strcasecmp(flow->valuestring, "none") == 0)
            params.flow_control = 0;
        else {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "flow_control must be none or rtscts");
        }
    }

    cJSON *idle = cJSON_GetObjectItemCaseSensitive(root, "idle_timeout_ms");
    if (cJSON_IsNumber(idle)) {
        if (idle->valueint < 0) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "idle_timeout_ms must be >= 0");
        }
        idle_timeout = (uint32_t) idle->valueint;
    }

    cJSON *apply = cJSON_GetObjectItemCaseSensitive(root, "apply_active");
    if (cJSON_IsBool(apply))
        apply_active = cJSON_IsTrue(apply);

    cJSON *tx = cJSON_GetObjectItemCaseSensitive(root, "tx_pin");
    if (cJSON_IsNumber(tx)) {
        pins.tx_pin = tx->valueint;
        pins_updated = true;
    }
    cJSON *rx = cJSON_GetObjectItemCaseSensitive(root, "rx_pin");
    if (cJSON_IsNumber(rx)) {
        pins.rx_pin = rx->valueint;
        pins_updated = true;
    }
    cJSON *rts = cJSON_GetObjectItemCaseSensitive(root, "rts_pin");
    if (cJSON_IsNumber(rts)) {
        pins.rts_pin = rts->valueint >= 0 ? rts->valueint : INT_MIN;
        pins_updated = true;
    }
    cJSON *cts = cJSON_GetObjectItemCaseSensitive(root, "cts_pin");
    if (cJSON_IsNumber(cts)) {
        pins.cts_pin = cts->valueint >= 0 ? cts->valueint : INT_MIN;
        pins_updated = true;
    }

    cJSON *uart_new = cJSON_GetObjectItemCaseSensitive(root, "uart");
    if (cJSON_IsNumber(uart_new)) {
        pins.uart_num = uart_new->valueint;
        pins_updated = true;
    }

    cJSON_Delete(root);

    if (ser2net_runtime_update_serial_config(tcp_port,
                                             &params,
                                             idle_timeout,
                                             apply_active,
                                             pins_updated ? &pins : NULL) != pdPASS)
        return send_json_error(req, "409 Conflict", "unable to update port");

    struct ser2net_active_session sessions[SER2NET_MAX_PORTS];
    size_t session_count = ser2net_runtime_list_sessions(sessions, SER2NET_MAX_PORTS);
    count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    base = find_port_by_tcp(tcp_port, ports, count);
    if (!base)
        return httpd_resp_send_500(req);

    cJSON *resp = port_to_json(base, sessions_for_port(base->tcp_port, sessions, session_count));
    if (!resp)
        return httpd_resp_send_500(req);

    esp_err_t res = send_json_response(req, resp, 200);
    cJSON_Delete(resp);
    return res;
}
#else
static esp_err_t port_config_handler(httpd_req_t *req, uint16_t tcp_port)
{
    (void) tcp_port;
    cJSON *root = NULL;
    if (read_json_body(req, &root))
        cJSON_Delete(root);
    return send_json_error(req, "403 Forbidden", "dynamic sessions disabled");
}
#endif

#if ENABLE_DYNAMIC_SESSIONS
static esp_err_t port_mode_handler(httpd_req_t *req, uint16_t tcp_port)
{
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    const struct ser2net_esp32_serial_port_cfg *base = find_port_by_tcp(tcp_port, ports, count);
    if (!base)
        return send_json_error(req, "404 Not Found", "port not found");

    cJSON *root = NULL;
    if (!read_json_body(req, &root))
        return ESP_OK;

    enum ser2net_port_mode mode = base->mode;
    bool enabled = base->enabled;
    bool touched = false;

    cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (cJSON_IsString(mode_item) && mode_item->valuestring) {
        mode = port_mode_from_str(mode_item->valuestring);
        touched = true;
    }

    cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsBool(enabled_item)) {
        enabled = cJSON_IsTrue(enabled_item);
        touched = true;
    }

    cJSON_Delete(root);

    if (!touched)
        return send_json_error(req, "400 Bad Request", "mode or enabled required");

    if (ser2net_runtime_set_port_mode(tcp_port, mode, enabled) != pdPASS)
        return send_json_error(req, "409 Conflict", "unable to update mode");

    struct ser2net_active_session sessions[SER2NET_MAX_PORTS];
    size_t session_count = ser2net_runtime_list_sessions(sessions, SER2NET_MAX_PORTS);
    count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    base = find_port_by_tcp(tcp_port, ports, count);
    if (!base)
        return httpd_resp_send_500(req);

    cJSON *resp = port_to_json(base, sessions_for_port(base->tcp_port, sessions, session_count));
    if (!resp)
        return httpd_resp_send_500(req);
    esp_err_t res = send_json_response(req, resp, 200);
    cJSON_Delete(resp);
    return res;
}
#else
static esp_err_t port_mode_handler(httpd_req_t *req, uint16_t tcp_port)
{
    (void) tcp_port;
    cJSON *root = NULL;
    if (read_json_body(req, &root))
        cJSON_Delete(root);
    return send_json_error(req, "403 Forbidden", "dynamic sessions disabled");
}
#endif

static esp_err_t port_disconnect_handler(httpd_req_t *req, uint16_t tcp_port)
{
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    if (!find_port_by_tcp(tcp_port, ports, count))
        return send_json_error(req, "404 Not Found", "port not found");

    bool disconnected = ser2net_runtime_disconnect_tcp_port(tcp_port);

    char buf[96];
    int written = snprintf(buf, sizeof(buf),
                           "{\"tcp_port\":%u,\"disconnected\":%s}",
                           tcp_port, disconnected ? "true" : "false");
    if (written < 0 || written >= (int)sizeof(buf))
        return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ports_action_handler(httpd_req_t *req)
{
    uint16_t tcp_port = 0;
    const char *action_ptr = NULL;
    if (!parse_port_and_action(req->uri, &tcp_port, &action_ptr))
        return send_json_error(req, "404 Not Found", "invalid path");

    char action_buf[16] = {0};
    size_t action_len = 0;
    if (action_ptr) {
        while (action_ptr[action_len] && action_ptr[action_len] != '/' && action_ptr[action_len] != '?') {
            if (action_len >= sizeof(action_buf) - 1)
                return send_json_error(req, "404 Not Found", "unsupported operation");
            action_buf[action_len] = action_ptr[action_len];
            action_len++;
        }
        action_buf[action_len] = '\0';
    }

    if (action_len == 0 || strcmp(action_buf, "config") == 0)
        return port_config_handler(req, tcp_port);
    if (strcmp(action_buf, "mode") == 0)
        return port_mode_handler(req, tcp_port);
    if (strcmp(action_buf, "disconnect") == 0)
        return port_disconnect_handler(req, tcp_port);

    return send_json_error(req, "404 Not Found", "unsupported operation");
}

#if ENABLE_DYNAMIC_SESSIONS
static esp_err_t ports_delete_handler(httpd_req_t *req)
{
    uint16_t tcp_port = 0;
    const char *action_ptr = NULL;
    if (!parse_port_and_action(req->uri, &tcp_port, &action_ptr))
        return send_json_error(req, "404 Not Found", "invalid path");

    if (action_ptr && *action_ptr)
        return send_json_error(req, "404 Not Found", "unsupported operation");

    if (ser2net_runtime_remove_port(tcp_port) != pdPASS)
        return send_json_error(req, "409 Conflict", "unable to remove port");

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}
#else
static esp_err_t ports_delete_handler(httpd_req_t *req)
{
    return send_json_error(req, "403 Forbidden", "dynamic sessions disabled");
}
#endif

bool web_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (config.max_uri_handlers < 16)
        config.max_uri_handlers = 16;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        s_server = NULL;
        return false;
    }

    httpd_uri_t ui_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_index_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &ui_root);

    httpd_uri_t ui_style = {
        .uri = "/static/app.css",
        .method = HTTP_GET,
        .handler = web_style_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &ui_style);

    httpd_uri_t ui_script = {
        .uri = "/static/app.js",
        .method = HTTP_GET,
        .handler = web_script_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &ui_script);

    httpd_uri_t uri_health = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = health_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_health);

    httpd_uri_t uri_system = {
        .uri = "/api/system",
        .method = HTTP_GET,
        .handler = system_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_system);

    httpd_uri_t uri_wifi_get = {
        .uri = "/api/wifi",
        .method = HTTP_GET,
        .handler = wifi_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wifi_get);

    httpd_uri_t uri_wifi_post = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = wifi_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wifi_post);

    httpd_uri_t uri_wifi_delete = {
        .uri = "/api/wifi",
        .method = HTTP_DELETE,
        .handler = wifi_delete_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wifi_delete);

    httpd_uri_t uri_ports_get = {
        .uri = "/api/ports",
        .method = HTTP_GET,
        .handler = ports_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ports_get);

    httpd_uri_t uri_ports_post = {
        .uri = "/api/ports",
        .method = HTTP_POST,
        .handler = ports_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ports_post);

    httpd_uri_t uri_ports_action = {
        .uri = "/api/ports/*",
        .method = HTTP_POST,
        .handler = ports_action_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ports_action);

    httpd_uri_t uri_ports_delete = {
        .uri = "/api/ports/*",
        .method = HTTP_DELETE,
        .handler = ports_delete_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ports_delete);

    return true;
}

void web_server_stop(void)
{
    if (!s_server)
        return;

    ESP_LOGI(TAG, "Stopping HTTP server");
    httpd_stop(s_server);
    s_server = NULL;
}
