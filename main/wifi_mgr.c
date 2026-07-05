#include "wifi_mgr.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE   "espnuts"
#define MAX_STA_FAILURES 20  // then bring up the setup AP alongside STA

static bool s_provisioning;
static bool s_ap_started;
static int s_sta_failures;
static bool s_mdns_started;

static void start_setup_ap(void)
{
    if (s_ap_started) {
        return;
    }
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "esp32-nuts-setup",
            .ssid_len = 0,
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 2,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_provisioning ? WIFI_MODE_AP : WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    s_ap_started = true;
    ESP_LOGW(TAG, "setup AP started: connect to 'esp32-nuts-setup', open http://192.168.4.1/setup");
}

static void start_mdns(void)
{
    if (s_mdns_started) {
        return;
    }
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(CONFIG_ESPNUTS_HOSTNAME);
        mdns_instance_name_set("esp32-nuts UPS monitor");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        mdns_service_add(NULL, "_nut", "_tcp", CONFIG_ESPNUTS_NUT_PORT, NULL, 0);
        s_mdns_started = true;
        ESP_LOGI(TAG, "mDNS: http://%s.local", CONFIG_ESPNUTS_HOSTNAME);
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_failures++;
        if (s_sta_failures == MAX_STA_FAILURES) {
            ESP_LOGW(TAG, "STA failed %d times, enabling setup AP", s_sta_failures);
            start_setup_ap();
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_sta_failures = 0;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        start_mdns();
    }
}

// Returns true if usable credentials were found.
static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t sl = ssid_len, pl = pass_len;
        nvs_get_str(nvs, "wifi_ssid", ssid, &sl);
        nvs_get_str(nvs, "wifi_pass", pass, &pl);
        nvs_close(nvs);
    }
    if (ssid[0] == '\0') {
        strlcpy(ssid, CONFIG_ESPNUTS_WIFI_SSID, ssid_len);
        strlcpy(pass, CONFIG_ESPNUTS_WIFI_PASSWORD, pass_len);
    }
    return ssid[0] != '\0';
}

esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, "wifi_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "wifi_pass", password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_LOGI(TAG, "saved credentials for SSID '%s'", ssid);
    return err;
}

bool wifi_mgr_is_provisioning(void)
{
    return s_provisioning;
}

void wifi_mgr_start(void)
{
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    char ssid[33], pass[65];
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t sta_cfg = { 0 };
        strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_LOGI(TAG, "connecting to '%s'", ssid);
    } else {
        s_provisioning = true;
        start_setup_ap();
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
}
