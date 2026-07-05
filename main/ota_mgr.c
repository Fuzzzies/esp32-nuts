#include "ota_mgr.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "ota_mgr";

static bool s_running;

static esp_err_t run_ota(const char *url)
{
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota = {
        .http_config = &http,
    };

    ESP_LOGI(TAG, "OTA from %s", url);
    esp_err_t err = esp_https_ota(&ota);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, rebooting");
        esp_restart();
    }
    ESP_LOGW(TAG, "OTA failed: %s", esp_err_to_name(err));
    return err;
}

static void ota_task(void *arg)
{
    const char *url = CONFIG_ESPNUTS_OTA_URL;
    s_running = true;
    vTaskDelay(pdMS_TO_TICKS(15000));  // let WiFi settle after boot
    run_ota(url);
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t ota_mgr_check_now(void)
{
    if (CONFIG_ESPNUTS_OTA_URL[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    return run_ota(CONFIG_ESPNUTS_OTA_URL);
}

void ota_mgr_start(void)
{
    if (CONFIG_ESPNUTS_OTA_URL[0] == '\0') {
        return;
    }
    xTaskCreate(ota_task, "ota_check", 8192, NULL, 5, NULL);
}
