/*
 * Wi-Fi station + WS2812 status indicator.
 *
 * The LED colour comes from a link-state machine written by the Wi-Fi/IP event
 * handlers and rendered by its own task, because connect and the reachability
 * probe both block.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "lwip/sockets.h"
#if CONFIG_EXAMPLE_BLE_ENABLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

static const char *TAG = "wifi_led";

#define BIT_STA_STARTED BIT0
#define BIT_GOT_IP      BIT1
#define BIT_LOST_IP     BIT2

/* 114.114.114.114:53 rather than a hostname, so the probe measures raw internet
 * reachability and cannot be masked by a broken local DNS server. */
#define PROBE_IP   "114.114.114.114"
#define PROBE_PORT 53

/* Fallback access path, off by default. The SoftAP is a pure L2 AP with its own
 * DHCP but no NAT to the STA uplink, and a joining Windows PC moves its default
 * gateway here -- which silently kills that PC's internet. Keep it only for
 * boards whose router-assigned LAN address really is unreachable. */
#if CONFIG_EXAMPLE_AP_ENABLE
#define AP_SSID    CONFIG_EXAMPLE_AP_SSID
#define AP_PASS    CONFIG_EXAMPLE_AP_PASSWORD
#define AP_MAX_STA 4
#endif

typedef enum {
    ST_CONNECTING,
    ST_LINKED_NO_NET,
    ST_ONLINE,
} link_state_t;

static volatile link_state_t s_state = ST_CONNECTING;
static volatile bool s_got_ip = false;
static volatile bool s_online = false;
static char s_ip_str[16] = "0.0.0.0";
static EventGroupHandle_t s_wifi_events;

/* An authmode threshold above WPA2 that yields reason 211 proves the AP is not
 * WPA3-class, so the ladder is ordered for a WPA2 router and covers the ways a
 * 4-way handshake (reason 204) still fails against one: wrong band assumption,
 * TKIP-only pairwise suite, and a mesh/eMesh SSID where the strongest-looking
 * BSS is not the one that will complete the handshake. */
typedef struct {
    wifi_auth_mode_t threshold;
    bool pin_bssid;
    const char *name;
} attempt_cfg_t;

static const attempt_cfg_t k_attempts[] = {
    { WIFI_AUTH_WPA2_PSK,     false, "WPA2, any BSS" },
    { WIFI_AUTH_WPA2_PSK,     true,  "WPA2, pinned BSS" },
    { WIFI_AUTH_WPA_PSK,      false, "WPA/WPA2 (TKIP allowed)" },
    { WIFI_AUTH_OPEN,         false, "accept whatever the AP offers" },
    { WIFI_AUTH_WPA3_PSK,     false, "WPA3" },
};
#define N_ATTEMPTS (sizeof(k_attempts) / sizeof(k_attempts[0]))

/* Strongest BSS advertising the target SSID, refreshed by every scan. */
static uint8_t s_target_bssid[6];
static bool s_target_seen = false;

static void report_memory(void);

#if CONFIG_EXAMPLE_MONO_LED_GPIO >= 0
static void mono_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_EXAMPLE_MONO_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}
#define MONO_ON()  gpio_set_level(CONFIG_EXAMPLE_MONO_LED_GPIO, 1)
#define MONO_OFF() gpio_set_level(CONFIG_EXAMPLE_MONO_LED_GPIO, 0)
#else
static void mono_led_init(void) {}
#define MONO_ON()  do {} while (0)
#define MONO_OFF() do {} while (0)
#endif

static const char *reason_name(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:  return "4-way handshake timeout - wrong password";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:       return "handshake timeout - password, MAC whitelist / max-clients, or a BSS that stops answering";
    case WIFI_REASON_AUTH_FAIL:               return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL:              return "association failed";
    case WIFI_REASON_CONNECTION_FAIL:         return "connection failed";
    case WIFI_REASON_NO_AP_FOUND:             return "AP not found - wrong SSID or 5GHz-only";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
                                              return "AP found but no mutually supported security mode";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
                                              return "AP found but below the RSSI threshold";
    case WIFI_REASON_BEACON_TIMEOUT:          return "beacon timeout - out of range";
    default:                                  return "(see esp_wifi_types_generic.h)";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_events, BIT_STA_STARTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_got_ip = false;
        s_state = ST_CONNECTING;
        xEventGroupClearBits(s_wifi_events, BIT_GOT_IP);
        xEventGroupSetBits(s_wifi_events, BIT_LOST_IP);
        ESP_LOGW(TAG, "disconnected, reason=%u: %s", d->reason, reason_name(d->reason));
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    s_got_ip = true;
    s_state = ST_LINKED_NO_NET;
    xEventGroupClearBits(s_wifi_events, BIT_LOST_IP);
    xEventGroupSetBits(s_wifi_events, BIT_GOT_IP);
    /* esp_ip4_addr_t and lwip's ip4_addr_t share a layout; the cast is what the
     * netif/lwip boundary needs to reuse the lwip printer. */
    strlcpy(s_ip_str, ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip), sizeof(s_ip_str));
    ESP_LOGI(TAG, "got IP: %s  ->  http://%s/", s_ip_str, s_ip_str);
}

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA+WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2+WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI_PSK";
    case WIFI_AUTH_OWE:             return "OWE";
    default:                        return "?";
    }
}

static const char *cipher_name(wifi_cipher_type_t c)
{
    switch (c) {
    case WIFI_CIPHER_TYPE_NONE:      return "NONE";
    case WIFI_CIPHER_TYPE_WEP40:     return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104:    return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP:      return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP:      return "CCMP";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP+CCMP";
    default:                         return "?";
    }
}

static void describe_ap(const wifi_ap_record_t *ap, const char *prefix)
{
    ESP_LOGI(TAG, "%s\"%s\" ch=%u rssi=%d auth=%s(%d) pairwise=%s group=%s cc=%.2s"
                  " bssid=%02x:%02x:%02x:%02x:%02x:%02x%s%s",
             prefix, (const char *)ap->ssid, (unsigned)ap->primary, (int)ap->rssi,
             auth_name(ap->authmode), (int)ap->authmode,
             cipher_name(ap->pairwise_cipher), cipher_name(ap->group_cipher),
             ap->country.cc,
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5],
             ap->wps ? " wps" : "",
             ap->phy_11ax ? " ax" : (ap->phy_11n ? " n" : " legacy"));
}

/* Reports nearby networks and records the target SSID's strongest BSS. Called
 * before every connect attempt, because the authmode/cipher/BSSID fields are what
 * turns an opaque reason 204 into a specific diagnosis, and a serial monitor that
 * joins late still gets them. */
static void scan_for_target(void)
{
    wifi_scan_config_t scan = { .show_hidden = true };
    ESP_LOGI(TAG, "scanning...");
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        ESP_LOGW(TAG, "scan found nothing");
        return;
    }
    uint16_t report = found > 12 ? 12 : found;

    wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * report);
    if (!aps) {
        return;
    }
    esp_wifi_scan_get_ap_records(&report, aps);

    s_target_seen = false;
    int8_t best_rssi = -127;
    int matches = 0;
    for (int i = 0; i < report; i++) {
        bool is_target = strcmp((const char *)aps[i].ssid, CONFIG_EXAMPLE_WIFI_SSID) == 0;
        if (is_target) {
            matches++;
            describe_ap(&aps[i], "  >");
            if ((int8_t)aps[i].rssi > best_rssi) {
                best_rssi = (int8_t)aps[i].rssi;
                memcpy(s_target_bssid, aps[i].bssid, sizeof(s_target_bssid));
                s_target_seen = true;
            }
        } else {
            describe_ap(&aps[i], "   ");
        }
    }
    free(aps);

    if (!s_target_seen) {
        ESP_LOGE(TAG, "target \"%s\" NOT in scan results", CONFIG_EXAMPLE_WIFI_SSID);
    } else {
        ESP_LOGI(TAG, "target \"%s\" seen on %d BSS, strongest pinned above",
                 CONFIG_EXAMPLE_WIFI_SSID, matches);
    }
}

static void apply_wifi_config(size_t try_index)
{
    wifi_config_t c = {0};
    snprintf((char *)c.sta.ssid, sizeof(c.sta.ssid), "%s", CONFIG_EXAMPLE_WIFI_SSID);
    snprintf((char *)c.sta.password, sizeof(c.sta.password), "%s", CONFIG_EXAMPLE_WIFI_PASSWORD);
    c.sta.threshold.authmode = k_attempts[try_index].threshold;
    /* pmf_cfg.capable is deprecated and ignored by the driver; only `required`
     * has any effect, and leaving it false accepts both PMF and non-PMF APs. */
    c.sta.pmf_cfg.capable = true;
    c.sta.pmf_cfg.required = false;
    if (k_attempts[try_index].pin_bssid && s_target_seen) {
        memcpy(c.sta.bssid, s_target_bssid, sizeof(c.sta.bssid));
        c.sta.bssid_set = true;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &c));
}

/* ---------- web-controlled LED override ---------- */

typedef enum {
    FX_SOLID,
    FX_BREATHE,
    FX_RAINBOW,
    FX_OFF,
} led_fx_t;

/* Written by the httpd task, read by led_task; copied whole under the mutex.
 * manual=false renders the link-state colours, manual=true outranks them. */
typedef struct {
    bool manual;
    led_fx_t fx;
    uint8_t r, g, b;
    uint8_t brightness; /* 0..100 */
} led_control_t;

static led_control_t s_led_ctl = {
    .manual = false, .fx = FX_SOLID, .r = 255, .g = 255, .b = 255, .brightness = 50,
};
static SemaphoreHandle_t s_led_ctl_mux;

static void led_ctl_get(led_control_t *out)
{
    xSemaphoreTake(s_led_ctl_mux, portMAX_DELAY);
    *out = s_led_ctl;
    xSemaphoreGive(s_led_ctl_mux);
}

static void led_ctl_set(const led_control_t *in)
{
    xSemaphoreTake(s_led_ctl_mux, portMAX_DELAY);
    s_led_ctl = *in;
    xSemaphoreGive(s_led_ctl_mux);
}

static const char *fx_name(led_fx_t fx)
{
    switch (fx) {
    case FX_SOLID:   return "solid";
    case FX_BREATHE: return "breathe";
    case FX_RAINBOW: return "rainbow";
    default:         return "off";
    }
}

static bool fx_from_name(const char *name, led_fx_t *out)
{
    if (!strcmp(name, "solid"))   { *out = FX_SOLID;   return true; }
    if (!strcmp(name, "breathe")) { *out = FX_BREATHE; return true; }
    if (!strcmp(name, "rainbow")) { *out = FX_RAINBOW; return true; }
    if (!strcmp(name, "off"))     { *out = FX_OFF;     return true; }
    return false;
}

static int led_ctl_to_json(char *buf, size_t len)
{
    led_control_t ctl;
    led_ctl_get(&ctl);
    return snprintf(buf, len,
                    "{\"manual\":%s,\"fx\":\"%s\",\"rgb\":[%u,%u,%u],\"brightness\":%u}",
                    ctl.manual ? "true" : "false", fx_name(ctl.fx),
                    ctl.r, ctl.g, ctl.b, ctl.brightness);
}

/* Parse and apply a partial LED-control update. Accepts the same JSON as
 * POST /api/led; absent keys keep their current values, and any value outside
 * its range makes the whole write fail. */
static bool led_ctl_from_json(const char *body, const char **errmsg)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        *errmsg = "invalid JSON";
        return false;
    }

    led_control_t ctl;
    led_ctl_get(&ctl);
    bool ok = true;

    cJSON *item = cJSON_GetObjectItem(root, "manual");
    if (cJSON_IsBool(item)) {
        ctl.manual = cJSON_IsTrue(item);
    }
    item = cJSON_GetObjectItem(root, "fx");
    if (cJSON_IsString(item)) {
        ok = fx_from_name(item->valuestring, &ctl.fx);
    } else if (item) {
        ok = false;
    }
    item = cJSON_GetObjectItem(root, "rgb");
    if (cJSON_IsArray(item) && cJSON_GetArraySize(item) == 3) {
        uint8_t *dst[3] = { &ctl.r, &ctl.g, &ctl.b };
        int idx = 0;
        cJSON *ch = NULL;
        cJSON_ArrayForEach(ch, item) {
            if (!cJSON_IsNumber(ch) || ch->valuedouble < 0 || ch->valuedouble > 255) {
                ok = false;
                break;
            }
            *dst[idx++] = (uint8_t)ch->valuedouble;
        }
    } else if (item) {
        ok = false;
    }
    item = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0 || item->valuedouble > 100) {
            ok = false;
        } else {
            ctl.brightness = (uint8_t)item->valuedouble;
        }
    }
    cJSON_Delete(root);

    if (!ok) {
        *errmsg = "fx must be solid|breathe|rainbow|off, rgb 0-255, brightness 0-100";
        return false;
    }

    led_ctl_set(&ctl);
    ESP_LOGI(TAG, "LED control: %s fx=%s rgb=(%u,%u,%u) brightness=%u",
             ctl.manual ? "manual" : "auto", fx_name(ctl.fx),
             ctl.r, ctl.g, ctl.b, ctl.brightness);
    return true;
}

/* ---------- built-in web monitor ---------- */

static const char k_index_html[] = R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="icon" href="data:,">
<title>ESP32-S3 监控</title>
<style>
:root{--bg:#0e1116;--card:#161b22;--line:#232b36;--fg:#e6edf3;--dim:#8b949e}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 system-ui,"Microsoft YaHei",sans-serif;padding:18px}
h1{font-size:17px;margin:0 0 14px;font-weight:600}
.pill{display:inline-flex;align-items:center;gap:8px;padding:6px 14px;border-radius:999px;font-weight:600;font-size:13px}
.dot{width:9px;height:9px;border-radius:50%}
.on{background:#10331f}.on .dot{background:#3fb950;box-shadow:0 0 10px #3fb950}
.warn{background:#3a2c10}.warn .dot{background:#d29922;box-shadow:0 0 10px #d29922}
.off{background:#3a1414}.off .dot{background:#f85149;box-shadow:0 0 10px #f85149}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-top:14px}
.c{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
.k{color:var(--dim);font-size:12px;margin-bottom:4px}
.v{font-size:19px;font-weight:600;font-variant-numeric:tabular-nums;word-break:break-all}
.v small{font-size:12px;color:var(--dim);font-weight:400}
.spark{display:flex;align-items:flex-end;gap:2px;height:46px;margin-top:8px}
.spark i{flex:1;background:#1f6feb;border-radius:2px 2px 0 0;min-height:2px;opacity:.85}
.btn{background:#21262d;border:1px solid var(--line);color:var(--fg);border-radius:8px;padding:5px 12px;font:inherit;cursor:pointer}
.btn:hover{background:#30363d}
.btn.active{background:#1f6feb;border-color:#1f6feb;color:#fff}
.foot{color:var(--dim);font-size:12px;margin-top:14px}
</style></head><body>
<h1>ESP32-S3 &middot; Wi-Fi 状态监控</h1>
<div id="pill" class="pill off"><span class="dot"></span><span id="st">连接中</span></div>
<div class="grid">
<div class="c"><div class="k">IP 地址</div><div class="v"><span id="ip">-</span></div></div>
<div class="c"><div class="k">SSID / 加密</div><div class="v"><span id="ssid">-</span> <small id="auth"></small></div></div>
<div class="c"><div class="k">信号强度</div><div class="v"><span id="rssi">-</span> <small>dBm</small></div></div>
<div class="c"><div class="k">信道</div><div class="v"><span id="ch">-</span></div></div>
<div class="c"><div class="k">运行时长</div><div class="v"><span id="up">-</span></div></div>
<div class="c"><div class="k">内部内存空闲</div><div class="v"><span id="hi">-</span> <small>KiB</small></div></div>
<div class="c"><div class="k">PSRAM 空闲</div><div class="v"><span id="hp">-</span> <small>MiB</small></div></div>
<div class="c"><div class="k">外网连通</div><div class="v"><span id="net">-</span></div></div>
<div class="c"><div class="k">板子热点</div><div class="v"><span id="aps">-</span> <small id="api"></small></div></div>
<div class="c"><div class="k">接入板子的设备</div><div class="v"><span id="apc">-</span></div></div>
</div>
<div class="c" style="margin-top:10px"><div class="k">信号历史（最近 60 次采样）</div><div class="spark" id="sp"></div></div>
<div class="c" style="margin-top:10px"><div class="k">连接到板子热点的终端</div><div id="cl" style="color:#8b949e;font-size:13px">无</div></div>
<div class="c" style="margin-top:10px"><div class="k">灯带控制 <small id="ledmode" style="font-weight:400"></small></div>
<div style="display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin-top:8px">
<label style="display:flex;align-items:center;gap:6px">颜色 <input type="color" id="ledcolor" value="#ffffff" style="width:40px;height:28px;border:0;background:none;padding:0;cursor:pointer"></label>
<label style="display:flex;align-items:center;gap:8px;flex:1;min-width:170px">亮度 <input type="range" id="ledbri" min="0" max="100" value="50" style="flex:1"><span id="ledbriv" style="font-variant-numeric:tabular-nums;width:38px">50%</span></label>
<button class="btn fx" data-fx="solid">常亮</button>
<button class="btn fx" data-fx="breathe">呼吸</button>
<button class="btn fx" data-fx="rainbow">彩虹</button>
<button class="btn fx" data-fx="off">熄灭</button>
<button class="btn" id="ledauto">恢复自动</button>
</div></div>
<div class="foot" id="foot">等待数据…</div>
<script>
var hist=[];
function $(i){return document.getElementById(i)}
function h(v){return Math.max(2,Math.min(46,(v+95)*46/55))}
async function tick(){
 try{
  var r=await fetch('/api/status',{cache:'no-store'});var d=await r.json();
  var m={online:['已联网','on'],linked:['仅连上路由','warn'],connecting:['连接中','off']}[d.state];
  $('pill').className='pill '+m[1];$('st').textContent=m[0];
  $('ip').textContent=d.ip;
  $('ssid').textContent=d.ssid;$('auth').textContent=d.auth;
  $('rssi').textContent=d.rssi;$('ch').textContent=d.channel||'-';
  $('up').textContent=Math.floor(d.uptime_s/60)+' 分 '+(d.uptime_s%60)+' 秒';
  $('hi').textContent=Math.round(d.heap_internal/1024);
  $('hp').textContent=(d.heap_psram/1048576).toFixed(1);
  $('net').textContent=d.internet?'通':'不通';
  $('aps').textContent=d.ap_ssid;$('api').textContent=d.ap_ip;
  $('apc').textContent=d.ap_stas;
  $('cl').innerHTML=(d.clients||[]).map(function(c){
    return '<div>'+c.mac+' &middot; '+c.rssi+' dBm</div>'}).join('')||'无';
  hist.push(d.rssi);if(hist.length>60)hist.shift();
  $('sp').innerHTML=hist.map(function(v){return '<i style="height:'+h(v)+'px"></i>'}).join('');
  $('foot').textContent='更新于 '+new Date().toLocaleTimeString()+' · MAC '+d.mac;
 }catch(e){$('pill').className='pill off';$('st').textContent='读不到板子';$('foot').textContent='请求失败：'+e}
}
tick();setInterval(tick,2000);
var ledState={manual:false,fx:'solid',rgb:[255,255,255],brightness:50};
var fxzh={solid:'常亮',breathe:'呼吸',rainbow:'彩虹',off:'熄灭'};
function ledUI(){
 var hex='#'+ledState.rgb.map(function(v){return ('0'+(v||0).toString(16)).slice(-2)}).join('');
 $('ledcolor').value=hex;$('ledbri').value=ledState.brightness;$('ledbriv').textContent=ledState.brightness+'%';
 document.querySelectorAll('.btn.fx').forEach(function(b){b.className='btn fx'+(ledState.manual&&b.dataset.fx===ledState.fx?' active':'')});
 $('ledauto').className='btn'+(ledState.manual?'':' active');
 $('ledmode').textContent=ledState.manual?('手动 · '+fxzh[ledState.fx]):'自动（跟随链路状态）';
}
async function postLed(){
 try{
  var r=await fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(ledState)});
  if(!r.ok)throw new Error('HTTP '+r.status);
  ledState=await r.json();ledUI();$('foot').textContent='灯带已更新 '+new Date().toLocaleTimeString();
 }catch(e){$('foot').textContent='灯带控制失败：'+e}
}
$('ledcolor').addEventListener('change',function(){
 ledState.rgb=[parseInt(this.value.slice(1,3),16),parseInt(this.value.slice(3,5),16),parseInt(this.value.slice(5,7),16)];
 ledState.manual=true;postLed()});
$('ledbri').addEventListener('input',function(){$('ledbriv').textContent=this.value+'%'});
$('ledbri').addEventListener('change',function(){ledState.brightness=+this.value;ledState.manual=true;postLed()});
document.querySelectorAll('.btn.fx').forEach(function(b){b.addEventListener('click',function(){
 ledState.fx=b.dataset.fx;ledState.manual=true;postLed()})});
$('ledauto').addEventListener('click',function(){ledState.manual=false;postLed()});
(async function(){
 try{var r=await fetch('/api/led',{cache:'no-store'});if(r.ok)ledState=await r.json()}catch(e){}
 ledUI();
})();
</script></body></html>)HTML";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, k_index_html, HTTPD_RESP_USE_STRLEN);
}

static int ap_station_count(wifi_sta_list_t *clients)
{
    memset(clients, 0, sizeof(*clients));
    return (esp_wifi_ap_get_sta_list(clients) == ESP_OK) ? clients->num : 0;
}

/* Status JSON shared by the web endpoint and the BLE characteristic. The web
 * version carries the hotspot's client list; BLE drops it (with_ap_details)
 * so the whole payload fits one ATT read even with a smaller negotiated MTU. */
static int build_status_json(char *buf, size_t len, bool with_ap_details)
{
    /* Read the live AP record rather than caching it, so the page cannot drift
     * from what the driver is actually associated to. */
    wifi_ap_record_t rec;
    int rssi = 0, chan = 0;
    const char *auth = "-";
    bool associated = (esp_wifi_sta_get_ap_info(&rec) == ESP_OK);
    if (associated) {
        rssi = rec.rssi;
        chan = rec.primary;
        auth = auth_name(rec.authmode);
    }
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    wifi_sta_list_t clients;
    int ap_stas = ap_station_count(&clients);
    /* Report the AP state honestly: with the fallback AP compiled out there is
     * no hotspot, and the page should not claim one. */
#if CONFIG_EXAMPLE_AP_ENABLE
    const char *ap_name = AP_SSID;
#else
    const char *ap_name = "off";
#endif

    int n = snprintf(buf, len,
             "{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
             "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
             "\"rssi\":%d,\"channel\":%d,\"auth\":\"%s\","
             "\"uptime_s\":%u,\"heap_internal\":%u,\"heap_psram\":%u,"
             "\"internet\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
             "\"ap_stas\":%d,\"clients\":[",
             s_online ? "online" : (s_got_ip ? "linked" : "connecting"),
             CONFIG_EXAMPLE_WIFI_SSID, s_ip_str,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             rssi, chan, auth,
             (unsigned)(esp_timer_get_time() / 1000000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             s_online ? "true" : "false",
             ap_name, ap_stas > 0 ? "192.168.4.1" : "-", ap_stas);

    for (int i = 0; with_ap_details && i < ap_stas && n < (int)len - 90; i++) {
        n += snprintf(buf + n, len - n,
                      "%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d}",
                      i ? "," : "",
                      clients.sta[i].mac[0], clients.sta[i].mac[1], clients.sta[i].mac[2],
                      clients.sta[i].mac[3], clients.sta[i].mac[4], clients.sta[i].mac[5],
                      (int)clients.sta[i].rssi);
    }
    strlcat(buf, "]}", len);
    return (int)strlen(buf);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char json[760];
    build_status_json(json, sizeof(json), true);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t led_get_handler(httpd_req_t *req)
{
    char json[96];
    led_ctl_to_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t led_post_handler(httpd_req_t *req)
{
    /* The UI always posts the full object, so 256 bytes is plenty; the buffer is
     * zeroed so cJSON_Parse() sees a terminated string even on short reads. */
    char buf[256] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    const char *errmsg = NULL;
    if (!led_ctl_from_json(buf, &errmsg)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, errmsg);
        return ESP_FAIL;
    }

    char json[96];
    led_ctl_to_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static void start_web_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %d", cfg.server_port);
        return;
    }
    httpd_uri_t index = { .uri = "/", .method = HTTP_GET,
                          .handler = index_handler, .user_ctx = NULL };
    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET,
                           .handler = status_handler, .user_ctx = NULL };
    httpd_uri_t led_get = { .uri = "/api/led", .method = HTTP_GET,
                            .handler = led_get_handler, .user_ctx = NULL };
    httpd_uri_t led_post = { .uri = "/api/led", .method = HTTP_POST,
                             .handler = led_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &index);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &led_get);
    httpd_register_uri_handler(server, &led_post);
#if CONFIG_EXAMPLE_AP_ENABLE
    ESP_LOGI(TAG, "web monitor on port %d:  http://192.168.4.1/  (via the \"%s\" hotspot)",
             cfg.server_port, AP_SSID);
#else
    ESP_LOGI(TAG, "web monitor on port %d:  open the URL logged at \"got IP\"",
             cfg.server_port);
#endif
}

/* ---------- BLE GATT server (LED control + status, for nRF Connect) ---------- */

#if CONFIG_EXAMPLE_BLE_ENABLE

/* One custom 128-bit service, 5d400001-7a9b-4c8e-b2f4-38a1c6e0d951, with a
 * LED characteristic (0002, same JSON as POST /api/led) and a read-only
 * status characteristic (0003, same fields as /api/status). UUID bytes are
 * listed LSB-first, as BLE_UUID128_INIT and the on-air format want. */
static const ble_uuid128_t k_ble_svc_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x01, 0x00, 0x40, 0x5d);
static const ble_uuid128_t k_ble_led_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x02, 0x00, 0x40, 0x5d);
static const ble_uuid128_t k_ble_status_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x03, 0x00, 0x40, 0x5d);

static uint8_t s_ble_addr_type;

/* The public store header does not declare this; official examples forward-
 * declare it the same way. */
extern void ble_store_config_init(void);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);

static int ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* arg names the characteristic: 2 = LED, 3 = status. */
    if ((uintptr_t)arg == 2) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            char buf[256] = {0};
            uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
            if (om_len >= sizeof(buf)) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, NULL);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            const char *errmsg = NULL;
            if (!led_ctl_from_json(buf, &errmsg)) {
                ESP_LOGW(TAG, "BLE LED write rejected: %s", errmsg);
                return 0x81; /* application error: invalid LED JSON */
            }
            return 0;
        }
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            char json[96];
            led_ctl_to_json(json, sizeof(json));
            return os_mbuf_append(ctxt->om, json, strlen(json));
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    if ((uintptr_t)arg == 3) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        /* No hotspot client list, so one ATT read carries the payload at the
         * 512 MTU; phones that negotiate smaller fall back to long reads,
         * which the NimBLE server handles by itself. */
        char json[768];
        int n = build_status_json(json, sizeof(json), false);
        return os_mbuf_append(ctxt->om, json, n);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def k_ble_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_ble_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                .uuid = &k_ble_led_uuid.u,
                .access_cb = ble_gatt_access,
                .arg = (void *)2,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &k_ble_status_uuid.u,
                .access_cb = ble_gatt_access,
                .arg = (void *)3,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 }, /* no more characteristics in this service */
        },
    },
    { 0 }, /* no more services */
};

static int ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    /* Flags are mandatory: general discoverability, BLE-only (the S3 has no
     * classic Bluetooth). The full name rides along in the same packet. */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)CONFIG_EXAMPLE_BLE_NAME;
    fields.name_len = strlen(CONFIG_EXAMPLE_BLE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE adv fields failed: %d (name too long?)", rc);
        return rc;
    }

    /* The 18-byte 128-bit service UUID does not fit next to the name in the
     * 31-byte legacy packet, so scanners find it in the scan response. */
    memset(&fields, 0, sizeof(fields));
    fields.uuids128 = &k_ble_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE scan-rsp fields failed: %d", rc);
        return rc;
    }

    struct ble_gap_adv_params adv_params = { 0 };
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_ble_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE adv start failed: %d", rc);
        return rc;
    }
    ESP_LOGI(TAG, "BLE advertising as \"%s\"", CONFIG_EXAMPLE_BLE_NAME);
    return 0;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "BLE connect failed, status=%d", event->connect.status);
            ble_advertise();
        } else {
            /* MAX_CONNECTIONS=1: no advertising while a link is up. */
            ESP_LOGI(TAG, "BLE connected (conn=%u), advertising paused",
                     event->connect.conn_handle);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason=%d), advertising again",
                 event->disconnect.reason);
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE mtu update: conn=%u mtu=%u",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void ble_on_sync(void)
{
    /* No privacy (0) → the controller's public address. */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE ensure_addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_ble_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE infer_addr failed: %d", rc);
        return;
    }
    ble_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    nimble_port_run(); /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* Mirrors the official Wi-Fi/BLE coexistence example: Wi-Fi fully up first,
 * then NimBLE. Airtime sharing between the two radios is handled by the
 * ESP-IDF coexistence layer without any configuration here. */
static void start_ble(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Security-manager fields stay zero: no pairing, plaintext characteristics.
     * Anyone in radio range can drive the LED -- the point of a demo; add
     * BLE_GATT_CHR_F_WRITE_ENC later to require encryption. */
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(k_ble_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(k_ble_gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE GATT registration failed: %d", rc);
        return;
    }
    rc = ble_svc_gap_device_name_set(CONFIG_EXAMPLE_BLE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE device name set failed: %d", rc);
        return;
    }
    ble_store_config_init();

    nimble_port_freertos_init(ble_host_task);
}

#else /* !CONFIG_EXAMPLE_BLE_ENABLE */

static void start_ble(void) {}

#endif

static void connect_task(void *arg)
{
    xEventGroupWaitBits(s_wifi_events, BIT_STA_STARTED, pdTRUE, pdFALSE, portMAX_DELAY);
    size_t try_index = 0;
    while (true) {
        xEventGroupClearBits(s_wifi_events, BIT_GOT_IP | BIT_LOST_IP);
        report_memory();
        scan_for_target();
        apply_wifi_config(try_index);
        ESP_LOGI(TAG, "connecting (attempt %u/%u: %s)",
                 (unsigned)(try_index + 1), (unsigned)N_ATTEMPTS,
                 k_attempts[try_index].name);

        if (esp_wifi_connect() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            try_index = (try_index + 1) % N_ATTEMPTS;
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, BIT_GOT_IP | BIT_LOST_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

        if (bits & BIT_GOT_IP) {
            try_index = 0;
            /* Remain in this branch until the link drops; esp_wifi_connect() must
             * not be called again while associated. */
            xEventGroupWaitBits(s_wifi_events, BIT_LOST_IP, pdTRUE, pdFALSE, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(3000));
        } else {
            try_index = (try_index + 1) % N_ATTEMPTS;
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

static bool probe_internet(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROBE_PORT);
    inet_pton(AF_INET, PROBE_IP, &addr.sin_addr);

    bool ok = (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(sock);
    return ok;
}

static void probe_task(void *arg)
{
    bool was_online = false;
    while (true) {
        if (!s_got_ip) {
            s_state = ST_CONNECTING;
            s_online = false;
            was_online = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool online = probe_internet();
        s_online = online;
        s_state = online ? ST_ONLINE : ST_LINKED_NO_NET;
        if (online != was_online) {
            ESP_LOGI(TAG, "internet probe: %s", online ? "reachable" : "unreachable");
            was_online = online;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void led_task(void *arg)
{
    led_strip_handle_t strip = NULL;
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_EXAMPLE_RGB_LED_GPIO,
        .max_leds = CONFIG_EXAMPLE_RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed: %s (is GPIO %d the right pin?)",
                 esp_err_to_name(err), CONFIG_EXAMPLE_RGB_LED_GPIO);
        vTaskDelete(NULL);
        return;
    }
    led_strip_clear(strip);
    ESP_LOGI(TAG, "WS2812 ready on GPIO %d", CONFIG_EXAMPLE_RGB_LED_GPIO);

    const float pi = 3.14159265f;
    uint32_t tick = 0;
    link_state_t shown = ST_CONNECTING;
    int devices_shown = 0;
    bool shown_manual = false;

    while (true) {
        link_state_t state = s_state;
        uint8_t r = 0, g = 0, b = 0;

        /* Once a second, not every frame: this loop refreshes the strip at 50 Hz. */
        if (tick % 50 == 0) {
            wifi_sta_list_t clients;
            int d = ap_station_count(&clients);
            if (d != devices_shown) {
                ESP_LOGI(TAG, "hotspot devices: %d", d);
                devices_shown = d;
            }
        }

        led_control_t ctl;
        led_ctl_get(&ctl);
        if (ctl.manual != shown_manual) {
            ESP_LOGI(TAG, "LED control -> %s",
                     ctl.manual ? "manual (web override)" : "auto (link state)");
            shown_manual = ctl.manual;
        }

        if (ctl.manual) {
            switch (ctl.fx) {
            case FX_SOLID:
                r = ctl.r;
                g = ctl.g;
                b = ctl.b;
                break;
            case FX_BREATHE: {
                float phase = sinf(tick * pi / 60.0f);
                float k = (phase + 1.0f) * 0.5f * ctl.brightness / 100.0f;
                r = (uint8_t)(ctl.r * k);
                g = (uint8_t)(ctl.g * k);
                b = (uint8_t)(ctl.b * k);
                break;
            }
            case FX_RAINBOW: /* resolved per pixel below */
            case FX_OFF:
                break;
            }
        } else {
            switch (state) {
            case ST_CONNECTING: {
                float phase = sinf(tick * pi / 60.0f);
                b = (uint8_t)((phase + 1.0f) * 0.5f * 200.0f);
                break;
            }
            case ST_LINKED_NO_NET:
                /* joined the router, internet probe failed */
                g = 120;
                break;
            case ST_ONLINE:
                /* router and internet both confirmed */
                g = 120;
                b = 120;
                break;
            }

            /* Something attached to the board's own hotspot outranks the WAN colour,
             * which is the "有设备接入就变色" behaviour asked for. */
            if (devices_shown > 0) {
                r = 140;
                b = 140;
                g = 0;
            }
        }

        uint8_t hue = (uint8_t)(tick * 2);
        for (int i = 0; i < CONFIG_EXAMPLE_RGB_LED_COUNT; i++) {
            if (ctl.manual && ctl.fx == FX_RAINBOW) {
                /* set_pixel_hsv() wants hue in [0,360); spread it across the
                 * strip so multi-LED setups get a travelling rainbow. */
                uint16_t h = ((uint32_t)hue * 360 / 256
                              + i * 360 / CONFIG_EXAMPLE_RGB_LED_COUNT) % 360;
                led_strip_set_pixel_hsv(strip, i, h, 100, ctl.brightness * 255 / 100);
            } else {
                led_strip_set_pixel(strip, i, r, g, b);
            }
        }
        led_strip_refresh(strip);

        if (state != shown) {
            ESP_LOGI(TAG, "LED state -> %s (r=%u g=%u b=%u)",
                     state == ST_CONNECTING ? "CONNECTING/breathing-blue"
                     : state == ST_LINKED_NO_NET ? "LINKED_NO_NET/green"
                     : "ONLINE/cyan",
                     (unsigned)r, (unsigned)g, (unsigned)b);
            shown = state;
        }

        /* Nothing above ever fires while the link stays in CONNECTING, so this is
         * the only proof that RMT is driving the strip rather than having failed
         * to init or been given the wrong GPIO. */
        if (tick % 500 == 0) {
            ESP_LOGI(TAG, "WS2812 alive on GPIO %d, driving rgb=(%u,%u,%u)",
                     CONFIG_EXAMPLE_RGB_LED_GPIO, (unsigned)r, (unsigned)g, (unsigned)b);
        }

        if (state == ST_CONNECTING) {
            if (tick % 15 == 0) { MONO_ON(); } else { MONO_OFF(); }
        } else {
            MONO_OFF();
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void report_memory(void)
{
    size_t internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "heap: internal=%u KiB, psram=%u KiB%s",
             (unsigned)(internal / 1024), (unsigned)(psram / 1024),
             psram > 0 ? " (Octal PSRAM OK)" : " (PSRAM NOT ENABLED)");

    /* Printed every attempt because a router that filters by MAC drops the
     * handshake silently, and the address to whitelist is the one below. */
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "sta mac: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Wi-Fi LED status demo ===");
    ESP_LOGI(TAG, "target SSID: \"%s\" (password %u chars)",
             CONFIG_EXAMPLE_WIFI_SSID, (unsigned)strlen(CONFIG_EXAMPLE_WIFI_PASSWORD));

    report_memory();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_events = xEventGroupCreate();
    s_led_ctl_mux = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
#if CONFIG_EXAMPLE_AP_ENABLE
    esp_netif_create_default_wifi_ap();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &ip_event_handler, NULL, NULL);

    mono_led_init();
    xTaskCreate(probe_task, "probe", 4096, NULL, 5, NULL);
    xTaskCreate(led_task, "led", 4096, NULL, 5, NULL);

#if CONFIG_EXAMPLE_AP_ENABLE
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    /* The AP config has to precede start(): after start the beacon is already
     * out with the empty default SSID and the new one does not replace it. */
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", AP_SSID);
    ap.ap.ssid_len = strlen(AP_SSID);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", AP_PASS);
    ap.ap.channel = 0;   /* follow the STA channel; AP+STA cannot straddle two */
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = AP_MAX_STA;
    ap.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Both require an enabled interface, hence after start(). RAM storage because
     * every attempt writes a fresh config and a stale credential in NVS would
     * silently override it; modem sleep can drop EAPOL frames to finicky
     * routers, so it stays off while this is a diagnostic. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    start_ble();

    xTaskCreate(connect_task, "connect", 4096, NULL, 6, NULL);
    start_web_server();

    ESP_LOGI(TAG, "LED breathing blue = still connecting; take over via the web page or POST /api/led");
}
