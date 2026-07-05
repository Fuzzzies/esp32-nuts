// Shared UPS state — single snapshot updated by the active data source
// (USB host or emulator) and read by the web server and NUT server.
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Metrics are NAN until the data source has reported them.
typedef struct {
    bool connected;  // data source currently has a live UPS

    char mfr[32];
    char model[64];
    char serial[32];

    // PresentStatus flags
    bool ac_present;
    bool charging;
    bool discharging;
    bool low_battery;
    bool fully_charged;
    bool overload;
    bool replace_battery;
    bool shutdown_imminent;

    float battery_charge;     // %
    float battery_runtime;    // seconds remaining
    float battery_voltage;    // V
    float load_percent;       // %
    float realpower_nominal;  // W (rated active power)
    float input_voltage;      // V
    float output_voltage;     // V

    int64_t last_update_us;   // esp_timer time of last source update
} ups_state_t;

void ups_data_init(void);
void ups_data_get(ups_state_t *out);
void ups_data_set(const ups_state_t *in);

// Compose a NUT-style status string, e.g. "OL CHRG" / "OB LB". Returns buf.
const char *ups_status_string(const ups_state_t *s, char *buf, size_t len);

// Load in watts derived from load% and nominal power; NAN if unknown.
float ups_load_watts(const ups_state_t *s);
