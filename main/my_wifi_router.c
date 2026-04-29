/*
 * ESP32 wifi pocket  — WiFi router with web-interface for ESP32-S3
 *
 * Features:
 *   - Creates WiFi Access Point with unique password (MAC-based)
 *   - Scans and connects to external WiFi networks via web-interface
 *   - NAT routing — shares internet to connected clients (up to 7)
 *   - Web-interface with two tabs: WiFi Setup & Settings
 *   - Auto-sync AP channel with external network
 *   - Password change and factory reset via web-interface
 *   - DNS forwarding from ISP to AP clients
 *   - Settings saved to NVS (survive reboots)
 *
 * Supported chips:
 *   ESP32, ESP32-S3 (tested), ESP32-C3, ESP32-C5, ESP32-C6
 *
 * Requirements:
 *   ESP-IDF v5.4+
 *   LWIP: IP forwarding + NAT enabled in menuconfig
 *
 * Web-interface: http://192.168.4.1
 *
 * License: GPL v3
 * Author: Ivan Svarkovsky, 2026
 * GitHub: https://github.com/Svarkovsky
 */




#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "lwip/lwip_napt.h"
#include "cJSON.h"

// --- НАСТРОЙКИ ТОЧКИ ДОСТУПА ESP32 ---
#define AP_SSID_PREFIX  "Pocket"
#define AP_MAX_CONN     7

static const char *TAG = "nat_router";

static esp_netif_t *ap_netif = NULL;
static esp_netif_t *sta_netif = NULL;

static char sta_ssid[33] = {0};
static char sta_password[65] = {0};

static volatile bool is_configuring = false;
static int wifi_retry_count = 0;

static char ap_ssid_full[32] = {0};
static char ap_password[64] = {0};

// ======================= NVS =======================
static void load_wifi_credentials(void) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t len = sizeof(sta_ssid);
        if (nvs_get_str(my_handle, "ssid", sta_ssid, &len) == ESP_OK) {
            len = sizeof(sta_password);
            nvs_get_str(my_handle, "password", sta_password, &len);
            ESP_LOGI(TAG, "Loaded saved network: SSID='%s'", sta_ssid);
        }
        nvs_close(my_handle);
    }
}

static void save_wifi_credentials(const char* ssid, const char* password) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "ssid", ssid);
        nvs_set_str(my_handle, "password", password);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Wi-Fi credentials saved to NVS!");
    }
}

// ======================= ОБРАБОТЧИК СОБЫТИЙ =======================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (strlen(sta_ssid) > 0) {
            ESP_LOGI(TAG, "Starting STA, connecting to: %s", sta_ssid);
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
        ESP_LOGI(TAG, "STA connected to %s on channel %d", event->ssid, event->channel);
        
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            wifi_config_t current_ap_config;
            esp_wifi_get_config(WIFI_IF_AP, &current_ap_config);
            if (current_ap_config.ap.channel != ap_info.primary) {
                current_ap_config.ap.channel = ap_info.primary;
                esp_wifi_set_config(WIFI_IF_AP, &current_ap_config);
                ESP_LOGI(TAG, "AP channel synced to %d", ap_info.primary);
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!is_configuring && strlen(sta_ssid) > 0) {
            if (wifi_retry_count < 20) {
                ESP_LOGI(TAG, "Disconnected from AP. Reconnecting...");
                esp_wifi_connect();
                wifi_retry_count++;
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got external IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;

        esp_netif_ip_info_t ap_ip_info;
        if (ap_netif) {
            esp_netif_get_ip_info(ap_netif, &ap_ip_info);
            ip_napt_enable(ap_ip_info.ip.addr, 1);
            ESP_LOGI(TAG, "NAT Routing ENABLED on IP: " IPSTR, IP2STR(&ap_ip_info.ip));

            esp_netif_dns_info_t dns_info;
            if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
                esp_netif_dhcps_stop(ap_netif);
                esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
                uint8_t dhcps_offer_dns = 1;
                esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_offer_dns, sizeof(dhcps_offer_dns));
                esp_netif_dhcps_start(ap_netif);
                ESP_LOGI(TAG, "AP DHCP DNS updated from ISP: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            }
        }
    }
}

// ======================= WEB ИНТЕРФЕЙС =======================
const char* html_page = 
"<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>ESP32 WiFi Pocket</title>"
"<link rel='icon' href='data:,'>" 
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,-apple-system,sans-serif;background:#f4f6f9;display:flex;justify-content:center;align-items:center;min-height:100vh;color:#1e293b}"
".card{background:#fff;border-radius:12px;box-shadow:0 10px 25px rgba(0,0,0,0.05);width:100%;max-width:400px;overflow:hidden}"
".header{background:linear-gradient(135deg,#1e293b,#334155);padding:20px 24px;text-align:center;color:#fff}"
".header-icon{margin-bottom:8px}"
".header-icon svg{width:32px;height:32px;fill:#60a5fa}"
".header h1{font-size:18px;font-weight:700;color:#f1f5f9;letter-spacing:0.5px}"
".header .subtitle{font-size:11px;color:#94a3b8;margin-top:4px}"
".tabs{display:flex;border-bottom:1px solid #e2e8f0}"
".tab{flex:1;text-align:center;padding:14px;font-size:13px;font-weight:600;color:#64748b;background:#f8fafc;border:none;cursor:pointer;transition:all 0.2s}"
".tab.active{color:#3b82f6;background:#fff;border-bottom:2px solid #3b82f6}"
".tab-content{display:none;padding:24px}"
".tab-content.active{display:block}"
".header-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:15px}"
"h1{font-size:20px;color:#0f172a}"
".refresh-btn{background:#e2e8f0;color:#475569;border:none;padding:8px 12px;border-radius:6px;font-size:12px;font-weight:600;cursor:pointer;transition:0.2s}"
".refresh-btn:hover{background:#cbd5e1}"
"#status-msg{text-align:center;font-size:13px;margin-bottom:15px;font-weight:500;padding:8px;border-radius:6px;background:#f8fafc}"
".network-list{border:1px solid #e2e8f0;border-radius:8px;margin-bottom:20px;max-height:200px;overflow-y:auto;background:#fafafa}"
".network-item{display:flex;justify-content:space-between;align-items:center;padding:10px 15px;border-bottom:1px solid #e2e8f0;cursor:pointer;transition:background 0.2s}"
".network-item:last-child{border-bottom:none}"
".network-item:hover{background:#f1f5f9}"
".net-info{display:flex;align-items:center;gap:10px;font-size:14px;color:#334155}"
".net-icons{display:flex;align-items:center;gap:6px}"
".icon{width:16px;height:16px}"
".icon.secure{fill:#94a3b8}"
".form-group{margin-bottom:15px}"
"label{display:block;font-size:13px;color:#475569;margin-bottom:6px;font-weight:500}"
"input{width:100%;padding:10px 12px;border:1px solid #cbd5e1;border-radius:6px;font-size:14px;outline:none;transition:border 0.2s}"
"input:focus{border-color:#3b82f6}"
".btn{width:100%;background:#3b82f6;color:white;border:none;padding:12px;border-radius:6px;font-size:14px;font-weight:600;cursor:pointer;transition:0.2s;margin-top:5px}"
".btn:hover{background:#2563eb}"
".btn.danger{background:#ef4444}"
".btn.danger:hover{background:#dc2626}"
".loading{text-align:center;padding:20px;font-size:14px;color:#64748b}"
".info-box{background:#f0fdf4;border:1px solid #bbf7d0;border-radius:8px;padding:12px;margin-bottom:15px;font-size:13px;color:#166534}"
".info-box.warn{background:#fef2f2;border-color:#fecaca;color:#991b1b}"
".footer{margin-top:20px;padding-top:15px;border-top:1px solid #e2e8f0;text-align:center;font-size:11px;color:#94a3b8}"
".footer-line{margin-bottom:3px}"
".footer a{color:#64748b;text-decoration:none}"
".footer a:hover{text-decoration:underline}"
"</style></head><body>"
"<div class='card'>"
// ---------- ШАПКА ----------
"  <div class='header'>"
"    <div class='header-icon'>"
"      <svg viewBox='0 0 24 24'><path d='M1 9l2 2c4.97-4.97 13.03-4.97 18 0l2-2C16.93 2.93 7.08 2.93 1 9zm8 8l3 3 3-3c-1.65-1.66-4.34-1.66-6 0zm-4-4l2 2c2.76-2.76 7.24-2.76 10 0l2-2C15.14 9.14 8.87 9.14 5 13z'/></svg>"
"    </div>"
"    <h1>ESP32 WiFi Pocket</h1>"
"    <div class='subtitle'>NAT Router</div>"
"  </div>"
// ---------- ВКЛАДКИ ----------
"  <div class='tabs'>"
"    <button class='tab active' onclick='switchTab(\"wifi\")'>WiFi Setup</button>"
"    <button class='tab' onclick='switchTab(\"settings\")'>Settings</button>"
"  </div>"
"  <div id='tab-wifi' class='tab-content active'>"
"    <div class='header-row'><h1>WiFi Setup</h1><button class='refresh-btn' onclick='scan()'>Refresh</button></div>"
"    <div id='status-msg'>Checking status...</div>"
"    <div class='network-list' id='list'><div class='loading'>Scanning networks...</div></div>"
"    <div class='form-group'><label>SSID</label><input type='text' id='ssid' placeholder='Network Name'></div>"
"    <div class='form-group'><label>Password</label><input type='password' id='pwd' placeholder='Password (optional)'></div>"
"    <button class='btn' onclick='connect()'>CONNECT</button>"
"  </div>"
"  <div id='tab-settings' class='tab-content'>"
"    <h1 style='margin-bottom:15px'>Router Settings</h1>"
"    <div class='form-group'><label>Current AP Password</label><input type='text' id='current-pass' readonly></div>"
"    <div class='form-group'><label>New Password</label><input type='text' id='new-pass' placeholder='New password (min 8 chars)'></div>"
"    <button class='btn' onclick='changePass()'>CHANGE PASSWORD</button>"
"    <div id='settings-msg' style='margin-top:12px;font-size:13px;text-align:center'></div>"
"    <hr style='margin:24px 0;border-color:#e2e8f0'>"
"    <button class='btn danger' onclick='resetAP()'>FACTORY RESET AP</button>"
"    <div style='margin-top:8px;font-size:11px;color:#94a3b8'>Resets AP password to generated default</div>"
"  </div>"
// ---------- ФУТЕР ----------
"  <div class='footer'>"
"    <div class='footer-line'>v.01 &nbsp;|&nbsp; ESP-IDF: 5.4.4 &nbsp;|&nbsp; <a href='https://github.com/Svarkovsky' target='_blank'>Source</a></div>"
"    <div class='footer-line'>Ivan Svarkovsky, 2026</div>"
"    <div class='footer-line'>GPL v3</div>"
"  </div>"
"</div>"
"<script>"
"const lockSvg='<svg class=\"icon secure\" viewBox=\"0 0 24 24\"><path d=\"M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zM9 6c0-1.66 1.34-3 3-3s3 1.34 3 3v2H9V6zm9 14H6V10h12v10zm-6-3c1.1 0 2-.9 2-2s-.9-2-2-2-2 .9-2 2 .9 2 2 2z\"/></svg>';"
"const wifiSvg='<svg class=\"icon\" viewBox=\"0 0 24 24\"><path d=\"M1 9l2 2c4.97-4.97 13.03-4.97 18 0l2-2C16.93 2.93 7.08 2.93 1 9zm8 8l3 3 3-3c-1.65-1.66-4.34-1.66-6 0zm-4-4l2 2c2.76-2.76 7.24-2.76 10 0l2-2C15.14 9.14 8.87 9.14 5 13z\"/></svg>';"
"let curSsid='';"
"function switchTab(tab){"
"  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));"
"  document.querySelectorAll('.tab-content').forEach(c=>c.classList.remove('active'));"
"  event.target.classList.add('active');"
"  document.getElementById('tab-'+tab).classList.add('active');"
"  if(tab==='settings') loadSettings();"
"}"
"function init(){"
"  fetch('/status').then(r=>r.json()).then(d=>{"
"    curSsid = d.ssid;"
"    const msg = document.getElementById('status-msg');"
"    if(d.status === 'connected') {"
"      msg.innerHTML = 'Connected to: <b>' + d.ssid + '</b>';"
"      msg.style.color = '#15803d'; msg.style.background = '#f0fdf4';"
"    } else {"
"      msg.innerHTML = 'Not connected';"
"      msg.style.color = '#b91c1c'; msg.style.background = '#fef2f2';"
"    }"
"    scan();"
"  });"
"}"
"function scan(){"
"  const lst=document.getElementById('list'); lst.innerHTML='<div class=\"loading\">Scanning...</div>';"
"  fetch('/scan').then(r=>r.json()).then(data=>{"
"    lst.innerHTML='';"
"    if(data.length===0) return lst.innerHTML='<div class=\"loading\">No networks</div>';"
"    data.forEach(net=>{"
"      const div=document.createElement('div'); div.className='network-item';"
"      if(net.ssid === curSsid) { div.style.background='#f0fdf4'; div.style.borderLeft='3px solid #22c55e'; }"
"      div.onclick=()=>document.getElementById('ssid').value=net.ssid;"
"      div.innerHTML=`<div class='net-info'>${net.ssid} ${net.ssid===curSsid?'<b>(Active)</b>':''}</div><div class='net-icons'>${net.sec?lockSvg:''}${wifiSvg}</div>`;"
"      lst.appendChild(div);"
"    });"
"  }).catch(()=>lst.innerHTML='<div class=\"loading\">Scan failed</div>');"
"}"
"function connect(){"
"  const s=document.getElementById('ssid').value;"
"  const p=document.getElementById('pwd').value;"
"  if(!s) return alert('Enter SSID');"
"  if(s === curSsid) return alert('Already connected to ' + s + '!');"
"  const btn=event.target; btn.innerText='CONNECTING...'; btn.style.background='#94a3b8';"
"  fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)})"
"  .then(()=>alert('Settings saved! You will get internet access shortly.'))"
"  .finally(()=>{btn.innerText='CONNECT'; btn.style.background='#3b82f6'; setTimeout(init, 3000);});"
"}"
"function loadSettings(){"
"  fetch('/api/appass').then(r=>r.json()).then(d=>{"
"    document.getElementById('current-pass').value = d.password || 'Not set';"
"  });"
"}"
"function changePass(){"
"  const pass = document.getElementById('new-pass').value;"
"  if(pass.length < 8) return alert('Password must be at least 8 characters');"
"  const btn=event.target; btn.innerText='CHANGING...'; btn.style.background='#94a3b8';"
"  fetch('/setpass',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pass='+encodeURIComponent(pass)})"
"  .then(r=>{"
"    if(r.ok){"
"      document.getElementById('settings-msg').innerHTML='<div class=\"info-box\">Password changed! You may need to reconnect.</div>';"
"      document.getElementById('current-pass').value = pass;"
"      document.getElementById('new-pass').value = '';"
"    } else {"
"      document.getElementById('settings-msg').innerHTML='<div class=\"info-box warn\">Failed to change password.</div>';"
"    }"
"  })"
"  .finally(()=>{btn.innerText='CHANGE PASSWORD'; btn.style.background='#3b82f6';});"
"}"
"function resetAP(){"
"  if(!confirm('Reset AP password to generated default? You will need to reconnect.')) return;"
"  fetch('/resetpass',{method:'POST'}).then(r=>{"
"    if(r.ok){"
"      alert('AP password reset. The page will reload.');"
"      location.reload();"
"    } else {"
"      alert('Failed to reset password.');"
"    }"
"  });"
"}"
"window.onload=init;"
"</script></body></html>";

//
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    wifi_ap_record_t ap_info;
    cJSON *root = cJSON_CreateObject();
    
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddStringToObject(root, "status", "connected");
        cJSON_AddStringToObject(root, "ssid", (char *)ap_info.ssid);
    } else {
        cJSON_AddStringToObject(root, "status", "disconnected");
        cJSON_AddStringToObject(root, "ssid", "");
    }

    const char *sys_info = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sys_info, strlen(sys_info));
    
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    wifi_scan_config_t scan_conf = { .show_hidden = false };
    esp_wifi_scan_start(&scan_conf, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_list[i].rssi);
        cJSON_AddBoolToObject(item, "sec", ap_list[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(root, item);
    }
    free(ap_list);

    const char *sys_info = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sys_info, strlen(sys_info));
    
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10); else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10); else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' '; src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

static esp_err_t connect_post_handler(httpd_req_t *req) {
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) return ESP_FAIL;

    char raw_ssid[64] = {0}, raw_pass[64] = {0};
    if (httpd_query_key_value(buf, "ssid", raw_ssid, sizeof(raw_ssid)) != ESP_OK) return ESP_FAIL;
    httpd_query_key_value(buf, "pass", raw_pass, sizeof(raw_pass));

    is_configuring = true;

    url_decode(sta_ssid, raw_ssid);
    url_decode(sta_password, raw_pass);
    ESP_LOGI(TAG, "User requested connection to: %s", sta_ssid);
    
    save_wifi_credentials(sta_ssid, sta_password);

    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500)); 

    wifi_config_t wifi_sta_config = {0};
    strncpy((char *)wifi_sta_config.sta.ssid, sta_ssid, sizeof(wifi_sta_config.sta.ssid));
    strncpy((char *)wifi_sta_config.sta.password, sta_password, sizeof(wifi_sta_config.sta.password));
    
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config);
    esp_wifi_connect();
    
    wifi_retry_count = 0;
    is_configuring = false;

    return ESP_OK;
}

static esp_err_t ap_pass_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "password", ap_password);
    
    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    
    free((void *)json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t set_pass_post_handler(httpd_req_t *req) {
    char buf[64] = {0};
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char new_pass[64] = {0};
    if (httpd_query_key_value(buf, "pass", new_pass, sizeof(new_pass)) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (strlen(new_pass) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password too short");
        return ESP_FAIL;
    }

    strncpy(ap_password, new_pass, sizeof(ap_password) - 1);
    
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ap_pass", ap_password);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "AP password changed to: %s", ap_password);
    }

    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid_len = strlen(ap_ssid_full),
            .channel = 0,
            .max_connection = AP_MAX_CONN,
            .authmode = strlen(ap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    strncpy((char *)wifi_ap_config.ap.ssid, ap_ssid_full, sizeof(wifi_ap_config.ap.ssid));
    if (strlen(ap_password) > 0) {
        strncpy((char *)wifi_ap_config.ap.password, ap_password, sizeof(wifi_ap_config.ap.password));
    }
    esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
    
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_start();
    
    ESP_LOGI(TAG, "AP restarted with new password");
    return ESP_OK;
}

static esp_err_t reset_pass_handler(httpd_req_t *req) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_password, sizeof(ap_password), "%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);

    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ap_pass", ap_password);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "AP password reset to: %s", ap_password);
    }

    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid_len = strlen(ap_ssid_full),
            .channel = 0,
            .max_connection = AP_MAX_CONN,
            .authmode = strlen(ap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    strncpy((char *)wifi_ap_config.ap.ssid, ap_ssid_full, sizeof(wifi_ap_config.ap.ssid));
    if (strlen(ap_password) > 0) {
        strncpy((char *)wifi_ap_config.ap.password, ap_password, sizeof(wifi_ap_config.ap.password));
    }
    esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
    
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_start();

    ESP_LOGI(TAG, "AP restarted with factory password");
    return ESP_OK;
}

// ======================= ИНИЦИАЛИЗАЦИЯ =======================
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_wifi_credentials();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    
    bool ap_pass_generated = false;
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        size_t len = sizeof(ap_password);
        if (nvs_get_str(nvs, "ap_pass", ap_password, &len) != ESP_OK) {
            snprintf(ap_password, sizeof(ap_password), "%02X%02X%02X%02X",
                     mac[2], mac[3], mac[4], mac[5]);
            nvs_set_str(nvs, "ap_pass", ap_password);
            nvs_commit(nvs);
            ap_pass_generated = true;
        }
        nvs_close(nvs);
    }

    snprintf(ap_ssid_full, sizeof(ap_ssid_full), "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    ESP_LOGI(TAG, "==============================");
    ESP_LOGI(TAG, "AP SSID:     %s", ap_ssid_full);
    ESP_LOGI(TAG, "AP Password: %s", ap_password);
    if (ap_pass_generated) {
        ESP_LOGW(TAG, ">>> NEW PASSWORD GENERATED <<<");
    }
    ESP_LOGI(TAG, "==============================");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();

    esp_netif_dns_info_t dns_info;
    dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
    dns_info.ip.type = IPADDR_TYPE_V4;
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    uint8_t dhcps_offer_dns = 1;
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_offer_dns, sizeof(dhcps_offer_dns));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid_len = strlen(ap_ssid_full),
            .channel = 0,
            .max_connection = AP_MAX_CONN,
            .authmode = strlen(ap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    strncpy((char *)wifi_ap_config.ap.ssid, ap_ssid_full, sizeof(wifi_ap_config.ap.ssid));
    if (strlen(ap_password) > 0) {
        strncpy((char *)wifi_ap_config.ap.password, ap_password, sizeof(wifi_ap_config.ap.password));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    if (strlen(sta_ssid) > 0) {
        wifi_config_t wifi_sta_config = {0};
        strncpy((char *)wifi_sta_config.sta.ssid, sta_ssid, sizeof(wifi_sta_config.sta.ssid));
        strncpy((char *)wifi_sta_config.sta.password, sta_password, sizeof(wifi_sta_config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi started. AP SSID: %s", ap_ssid_full);
    
    httpd_handle_t server = NULL;
    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &httpd_cfg) == ESP_OK) {
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler };
        httpd_register_uri_handler(server, &uri_status);

        httpd_uri_t uri_scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler };
        httpd_register_uri_handler(server, &uri_scan);

        httpd_uri_t uri_connect = { .uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler };
        httpd_register_uri_handler(server, &uri_connect);

        httpd_uri_t uri_ap_pass = { .uri = "/api/appass", .method = HTTP_GET, .handler = ap_pass_get_handler };
        httpd_register_uri_handler(server, &uri_ap_pass);

        httpd_uri_t uri_set_pass = { .uri = "/setpass", .method = HTTP_POST, .handler = set_pass_post_handler };
        httpd_register_uri_handler(server, &uri_set_pass);

        httpd_uri_t uri_reset_pass = { .uri = "/resetpass", .method = HTTP_POST, .handler = reset_pass_handler };
        httpd_register_uri_handler(server, &uri_reset_pass);
        
        ESP_LOGI(TAG, "HTTP Web Server started successfully!");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
