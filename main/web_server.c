#include "web_server.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "ups_data.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

// --- helpers ---------------------------------------------------------------

static int append(char *buf, size_t cap, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= cap) {
        return off;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    return n < 0 ? off : off + n;
}

// Appends `"key":1.2` or `"key":null`.
static int append_num(char *buf, size_t cap, int off, const char *key, float v, int prec)
{
    if (isnan(v)) {
        return append(buf, cap, off, "\"%s\":null", key);
    }
    return append(buf, cap, off, "\"%s\":%.*f", key, prec, v);
}

static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *out++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

// Extract a form field value from an x-www-form-urlencoded body.
static bool form_value(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            if (n >= out_len) {
                n = out_len - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    out[0] = '\0';
    return false;
}

// --- handlers --------------------------------------------------------------

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    ups_state_t s;
    ups_data_get(&s);

    char status[48];
    ups_status_string(&s, status, sizeof(status));

    char ip[16] = "";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(sta, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    char buf[1024];
    int o = 0;
    o = append(buf, sizeof(buf), o,
               "{\"variant\":\"usb\",\"connected\":%s,\"status\":\"%s\","
               "\"mfr\":\"%s\",\"model\":\"%s\",\"serial\":\"%s\",",
               s.connected ? "true" : "false", status, s.mfr, s.model, s.serial);
    o = append_num(buf, sizeof(buf), o, "battery_charge", s.battery_charge, 0);
    o = append(buf, sizeof(buf), o, ",");
    o = append_num(buf, sizeof(buf), o, "battery_runtime_s", s.battery_runtime, 0);
    o = append(buf, sizeof(buf), o, ",");
    o = append_num(buf, sizeof(buf), o, "battery_voltage", s.battery_voltage, 1);
    o = append(buf, sizeof(buf), o, ",");
    o = append_num(buf, sizeof(buf), o, "load_percent", s.load_percent, 0);
    o = append(buf, sizeof(buf), o, ",");
    o = append_num(buf, sizeof(buf), o, "load_watts", ups_load_watts(&s), 0);
    o = append(buf, sizeof(buf), o, ",");
    o = append_num(buf, sizeof(buf), o, "realpower_nominal", s.realpower_nominal, 0);
    o = append(buf, sizeof(buf), o, ",");
    o = append_num(buf, sizeof(buf), o, "input_voltage", s.input_voltage, 1);
    o = append(buf, sizeof(buf), o, ",");
    o = append_num(buf, sizeof(buf), o, "output_voltage", s.output_voltage, 1);
    o = append(buf, sizeof(buf), o,
               ",\"wifi\":{\"ip\":\"%s\",\"rssi\":%d,\"provisioning\":%s},"
               "\"uptime_s\":%lld}",
               ip, rssi, wifi_mgr_is_provisioning() ? "true" : "false",
               esp_timer_get_time() / 1000000);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, o);
}

static const char SETUP_HTML[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>esp32-nuts setup</title><style>body{font-family:system-ui;background:#14161a;color:#e8e8e8;"
    "display:flex;justify-content:center;padding-top:10vh}form{background:#1e2128;padding:2em;border-radius:12px;"
    "min-width:280px}input{width:100%;margin:.4em 0 1em;padding:.6em;border-radius:6px;border:1px solid #444;"
    "background:#14161a;color:#e8e8e8;box-sizing:border-box}button{width:100%;padding:.7em;border:0;"
    "border-radius:6px;background:#4a9eff;color:#fff;font-size:1em;cursor:pointer}</style></head><body>"
    "<form method=post action=/setup/save><h2>&#x1F95C; esp32-nuts WiFi</h2>"
    "<label>SSID</label><input name=ssid required maxlength=32>"
    "<label>Password</label><input name=pass type=password maxlength=64>"
    "<button>Save &amp; reboot</button></form></body></html>";

static esp_err_t setup_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

static void reboot_cb(void *arg)
{
    esp_restart();
}

static esp_err_t setup_save_post(httpd_req_t *req)
{
    char body[256];
    int total = 0;
    while (total < req->content_len && total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
        }
        total += r;
    }
    body[total] = '\0';

    char ssid[33], pass[65];
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }
    form_value(body, "pass", pass, sizeof(pass));

    if (wifi_mgr_save_credentials(ssid, pass) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body style='font-family:system-ui'>"
                            "Saved. Rebooting and joining your network&hellip; "
                            "find me at http://" CONFIG_ESPNUTS_HOSTNAME ".local</body></html>");

    const esp_timer_create_args_t args = { .callback = reboot_cb, .name = "reboot" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 1500000);
    }
    return ESP_OK;
}

void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start http server");
        return;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/setup",      .method = HTTP_GET,  .handler = setup_get },
        { .uri = "/setup/save", .method = HTTP_POST, .handler = setup_save_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "http server up on port 80");
}
