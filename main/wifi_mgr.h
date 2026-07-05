#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Connect using credentials from NVS (falling back to Kconfig defaults).
// If none exist, or connecting keeps failing, an open provisioning AP
// "esp32-nuts-setup" is started so the web UI at 192.168.4.1 can save creds.
void wifi_mgr_start(void);

// True while the provisioning AP is up (no usable STA credentials).
bool wifi_mgr_is_provisioning(void);

// Persist new credentials to NVS; the caller is expected to reboot after.
esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *password);
