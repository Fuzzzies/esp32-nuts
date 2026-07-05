#include "ups_data.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static ups_state_t s_state;

void ups_data_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
    s_state.battery_charge = NAN;
    s_state.battery_runtime = NAN;
    s_state.battery_voltage = NAN;
    s_state.load_percent = NAN;
    s_state.realpower_nominal = NAN;
    s_state.input_voltage = NAN;
    s_state.output_voltage = NAN;
}

void ups_data_get(ups_state_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

void ups_data_set(const ups_state_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = *in;
    xSemaphoreGive(s_lock);
}

const char *ups_status_string(const ups_state_t *s, char *buf, size_t len)
{
    buf[0] = '\0';
    if (!s->connected) {
        strlcpy(buf, "OFF", len);
        return buf;
    }
    strlcpy(buf, s->ac_present ? "OL" : "OB", len);
    if (s->low_battery)       strlcat(buf, " LB", len);
    if (s->charging)          strlcat(buf, " CHRG", len);
    if (s->discharging && s->ac_present) strlcat(buf, " DISCHRG", len);
    if (s->overload)          strlcat(buf, " OVER", len);
    if (s->replace_battery)   strlcat(buf, " RB", len);
    if (s->shutdown_imminent) strlcat(buf, " FSD", len);
    return buf;
}

float ups_load_watts(const ups_state_t *s)
{
    if (isnan(s->load_percent) || isnan(s->realpower_nominal)) {
        return NAN;
    }
    return s->load_percent * s->realpower_nominal / 100.0f;
}
