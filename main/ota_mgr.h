#pragma once

#include "esp_err.h"

// Start background OTA check when CONFIG_ESPNUTS_OTA_URL is set.
void ota_mgr_start(void);

// Trigger an immediate OTA check (e.g. from web UI).
esp_err_t ota_mgr_check_now(void);
