#pragma once

// HTTP server on port 80:
//   GET  /            status dashboard
//   GET  /api/status  JSON snapshot of UPS + device state
//   GET  /setup       WiFi credentials form
//   POST /setup/save  store credentials and reboot
//   GET  /api/sim     (emulator builds only) ?ac=0|1 toggles utility power
void web_server_start(void);
