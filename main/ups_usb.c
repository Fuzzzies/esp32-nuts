// Real UPS data source: USB OTG host speaking the HID Power Device class,
// the same interface PowerPanel / NUT's usbhid-ups use. The report
// descriptor is parsed once at connect to locate the interesting usages,
// then Feature reports are polled every couple of seconds and async Input
// reports are folded in as they arrive.
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_parser.h"
#include "ups_data.h"
#include "ups_source.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "ups_usb";

#define POLL_INTERVAL_MS 2000
#define MAX_REPORT_LEN   64

// --- usage -> ups_state mapping ---------------------------------------------

// Power Device (0x84) / Battery System (0x85) usages, as used by NUT.
#define U_INPUT_COLLECTION   HID_USAGE(0x84, 0x1A)
#define U_OUTPUT_COLLECTION  HID_USAGE(0x84, 0x1C)
#define U_POWER_SUMMARY      HID_USAGE(0x84, 0x24)
#define U_BATTERY_SYSTEM     HID_USAGE(0x84, 0x10)
#define U_VOLTAGE            HID_USAGE(0x84, 0x30)
#define U_PERCENT_LOAD       HID_USAGE(0x84, 0x35)
#define U_CONFIG_ACTIVE_PWR  HID_USAGE(0x84, 0x43)
#define U_OVERLOAD           HID_USAGE(0x84, 0x65)
#define U_SHUTDOWN_IMMINENT  HID_USAGE(0x84, 0x69)
#define U_BELOW_CAP_LIMIT    HID_USAGE(0x85, 0x42)
#define U_CHARGING           HID_USAGE(0x85, 0x44)
#define U_DISCHARGING        HID_USAGE(0x85, 0x45)
#define U_FULLY_CHARGED      HID_USAGE(0x85, 0x46)
#define U_NEED_REPLACEMENT   HID_USAGE(0x85, 0x4B)
#define U_REMAINING_CAPACITY HID_USAGE(0x85, 0x66)
#define U_RUNTIME_TO_EMPTY   HID_USAGE(0x85, 0x68)
#define U_AC_PRESENT         HID_USAGE(0x85, 0xD0)

typedef void (*apply_fn_t)(ups_state_t *s, double v);

static void ap_input_v(ups_state_t *s, double v)   { s->input_voltage = (float)v; }
static void ap_output_v(ups_state_t *s, double v)  { s->output_voltage = (float)v; }
static void ap_batt_v(ups_state_t *s, double v)    { s->battery_voltage = (float)v; }
static void ap_load(ups_state_t *s, double v)      { s->load_percent = (float)v; }
static void ap_nominal(ups_state_t *s, double v)   { s->realpower_nominal = (float)v; }
static void ap_charge(ups_state_t *s, double v)    { s->battery_charge = (float)v; }
static void ap_runtime(ups_state_t *s, double v)   { s->battery_runtime = (float)v; }
static void ap_ac(ups_state_t *s, double v)        { s->ac_present = v > 0.5; }
static void ap_chrg(ups_state_t *s, double v)      { s->charging = v > 0.5; }
static void ap_dischrg(ups_state_t *s, double v)   { s->discharging = v > 0.5; }
static void ap_lb(ups_state_t *s, double v)        { s->low_battery = v > 0.5; }
static void ap_full(ups_state_t *s, double v)      { s->fully_charged = v > 0.5; }
static void ap_over(ups_state_t *s, double v)      { s->overload = v > 0.5; }
static void ap_rb(ups_state_t *s, double v)        { s->replace_battery = v > 0.5; }
static void ap_fsd(ups_state_t *s, double v)       { s->shutdown_imminent = v > 0.5; }

typedef struct {
    uint32_t usage;
    uint32_t ancestor;  // 0 = anywhere
    apply_fn_t apply;
} usage_map_t;

static const usage_map_t USAGE_MAP[] = {
    { U_VOLTAGE,            U_INPUT_COLLECTION,  ap_input_v },
    { U_VOLTAGE,            U_OUTPUT_COLLECTION, ap_output_v },
    { U_VOLTAGE,            U_POWER_SUMMARY,     ap_batt_v },
    { U_VOLTAGE,            U_BATTERY_SYSTEM,    ap_batt_v },
    { U_PERCENT_LOAD,       0,                   ap_load },
    { U_CONFIG_ACTIVE_PWR,  0,                   ap_nominal },
    { U_REMAINING_CAPACITY, 0,                   ap_charge },
    { U_RUNTIME_TO_EMPTY,   0,                   ap_runtime },
    { U_AC_PRESENT,         0,                   ap_ac },
    { U_CHARGING,           0,                   ap_chrg },
    { U_DISCHARGING,        0,                   ap_dischrg },
    { U_BELOW_CAP_LIMIT,    0,                   ap_lb },
    { U_FULLY_CHARGED,      0,                   ap_full },
    { U_OVERLOAD,           0,                   ap_over },
    { U_NEED_REPLACEMENT,   0,                   ap_rb },
    { U_SHUTDOWN_IMMINENT,  0,                   ap_fsd },
};
#define USAGE_MAP_LEN (sizeof(USAGE_MAP) / sizeof(USAGE_MAP[0]))

// --- device state ------------------------------------------------------------

typedef struct {
    const usage_map_t *entry;
    const hid_field_t *field;
} resolved_t;

static hid_host_device_handle_t s_dev;
static hid_report_map_t s_map;
static resolved_t s_resolved[USAGE_MAP_LEN];
static size_t s_n_resolved;
static uint8_t s_feature_ids[16];
static size_t s_n_feature_ids;
static ups_state_t s_ups;          // accumulated readings (poll task only mutates)
static volatile bool s_connected;

typedef enum {
    EVT_DEVICE_CONNECTED,
    EVT_DEVICE_DISCONNECTED,
    EVT_INPUT_REPORT,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    hid_host_device_handle_t handle;
    uint8_t data[MAX_REPORT_LEN];
    size_t data_len;
} app_event_t;

static QueueHandle_t s_events;

// --- report processing ---------------------------------------------------

static void process_report(const uint8_t *buf, size_t len, uint8_t report_type)
{
    if (len == 0) {
        return;
    }
    const uint8_t *payload = buf;
    size_t plen = len;
    uint8_t id = 0;
    if (s_map.uses_report_ids) {
        id = buf[0];
        payload = buf + 1;
        plen = len - 1;
    }

    bool any = false;
    for (size_t i = 0; i < s_n_resolved; i++) {
        const hid_field_t *f = s_resolved[i].field;
        if (f->report_type != report_type || f->report_id != id) {
            continue;
        }
        double v;
        if (hid_extract(f, payload, plen, &v)) {
            s_resolved[i].entry->apply(&s_ups, v);
            any = true;
        }
    }
    if (any) {
        s_ups.connected = true;
        s_ups.last_update_us = esp_timer_get_time();
        ups_data_set(&s_ups);
    }
}

static void resolve_fields(void)
{
    s_n_resolved = 0;
    s_n_feature_ids = 0;

    for (size_t i = 0; i < USAGE_MAP_LEN; i++) {
        const hid_field_t *f = hid_find_field(&s_map, USAGE_MAP[i].usage,
                                              USAGE_MAP[i].ancestor, 0);
        if (!f) {
            continue;
        }
        // Don't let the "anywhere" voltage rules shadow input/output matches.
        bool dup = false;
        for (size_t j = 0; j < s_n_resolved; j++) {
            if (s_resolved[j].field == f && s_resolved[j].entry->usage == USAGE_MAP[i].usage) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        s_resolved[s_n_resolved].entry = &USAGE_MAP[i];
        s_resolved[s_n_resolved].field = f;
        s_n_resolved++;

        if (f->report_type == HID_REPORT_TYPE_FEAT) {
            bool known = false;
            for (size_t j = 0; j < s_n_feature_ids; j++) {
                if (s_feature_ids[j] == f->report_id) {
                    known = true;
                    break;
                }
            }
            if (!known && s_n_feature_ids < sizeof(s_feature_ids)) {
                s_feature_ids[s_n_feature_ids++] = f->report_id;
            }
        }
        ESP_LOGI(TAG, "usage %04x:%04x -> %s report %u @ bit %u (%u bits)",
                 (unsigned)(f->usage >> 16), (unsigned)(f->usage & 0xFFFF),
                 f->report_type == HID_REPORT_TYPE_FEAT ? "feature" : "input",
                 f->report_id, f->bit_offset, f->bit_size);
    }
    ESP_LOGI(TAG, "resolved %u of %u usages; polling %u feature reports",
             (unsigned)s_n_resolved, (unsigned)USAGE_MAP_LEN,
             (unsigned)s_n_feature_ids);
}

static void poll_feature_reports(void)
{
    for (size_t i = 0; i < s_n_feature_ids; i++) {
        uint8_t buf[MAX_REPORT_LEN];
        size_t len = sizeof(buf);
        esp_err_t err = hid_class_request_get_report(
            s_dev, HID_REPORT_TYPE_FEAT, s_feature_ids[i], buf, &len);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "GET_REPORT id %u failed: %s",
                     s_feature_ids[i], esp_err_to_name(err));
            continue;
        }
        // Devices typically echo the report ID as the first byte; if this one
        // doesn't, splice it in so process_report sees a consistent layout.
        if (s_map.uses_report_ids && (len == 0 || buf[0] != s_feature_ids[i])) {
            memmove(buf + 1, buf, len < MAX_REPORT_LEN - 1 ? len : MAX_REPORT_LEN - 1);
            buf[0] = s_feature_ids[i];
            len++;
        }
        process_report(buf, len, HID_REPORT_TYPE_FEAT);
    }
}

// --- hid_host callbacks (driver context: forward to app queue) ------------

static void interface_callback(hid_host_device_handle_t handle,
                               const hid_host_interface_event_t event, void *arg)
{
    app_event_t evt = { .handle = handle };
    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            evt.type = EVT_INPUT_REPORT;
            size_t len = 0;
            if (hid_host_device_get_raw_input_report_data(
                    handle, evt.data, sizeof(evt.data), &len) == ESP_OK) {
                evt.data_len = len;
                xQueueSend(s_events, &evt, 0);
            }
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            evt.type = EVT_DEVICE_DISCONNECTED;
            xQueueSend(s_events, &evt, 0);
            break;
        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        default:
            break;
    }
}

static void device_callback(hid_host_device_handle_t handle,
                            const hid_host_driver_event_t event, void *arg)
{
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        app_event_t evt = { .type = EVT_DEVICE_CONNECTED, .handle = handle };
        xQueueSend(s_events, &evt, 0);
    }
}

// --- connect / disconnect --------------------------------------------------

static void handle_connect(hid_host_device_handle_t handle)
{
    const hid_host_device_config_t cfg = {
        .callback = interface_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_device_open(handle, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device open failed: %s", esp_err_to_name(err));
        return;
    }

    size_t desc_len = 0;
    const uint8_t *desc = hid_host_get_report_descriptor(handle, &desc_len);
    if (!desc || hid_parse_report_descriptor(desc, desc_len, &s_map) != ESP_OK) {
        ESP_LOGW(TAG, "no power-device usages in report descriptor — not a UPS?");
        hid_host_device_close(handle);
        return;
    }

    s_dev = handle;
    resolve_fields();

    memset(&s_ups, 0, sizeof(s_ups));
    s_ups.battery_charge = NAN;
    s_ups.battery_runtime = NAN;
    s_ups.battery_voltage = NAN;
    s_ups.load_percent = NAN;
    s_ups.realpower_nominal = NAN;
    s_ups.input_voltage = NAN;
    s_ups.output_voltage = NAN;
    s_ups.connected = true;
    s_ups.ac_present = true;  // assume mains until the device says otherwise
    strlcpy(s_ups.model, "USB HID UPS", sizeof(s_ups.model));

    err = hid_host_device_start(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device start failed: %s", esp_err_to_name(err));
    }
    s_connected = true;
    ups_data_set(&s_ups);
    ESP_LOGI(TAG, "UPS connected");
    poll_feature_reports();
}

static void handle_disconnect(hid_host_device_handle_t handle)
{
    ESP_LOGW(TAG, "UPS disconnected");
    s_connected = false;
    s_dev = NULL;
    hid_host_device_close(handle);

    ups_state_t s;
    ups_data_get(&s);
    s.connected = false;
    ups_data_set(&s);
}

// --- tasks -------------------------------------------------------------------

static void app_task(void *arg)
{
    int64_t last_poll = 0;
    while (true) {
        app_event_t evt;
        if (xQueueReceive(s_events, &evt, pdMS_TO_TICKS(250))) {
            switch (evt.type) {
                case EVT_DEVICE_CONNECTED:
                    handle_connect(evt.handle);
                    break;
                case EVT_DEVICE_DISCONNECTED:
                    handle_disconnect(evt.handle);
                    break;
                case EVT_INPUT_REPORT:
                    if (s_connected) {
                        process_report(evt.data, evt.data_len, HID_REPORT_TYPE_IN);
                    }
                    break;
            }
        }
        if (s_connected &&
            esp_timer_get_time() - last_poll > POLL_INTERVAL_MS * 1000LL) {
            last_poll = esp_timer_get_time();
            poll_feature_reports();
        }
    }
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

void ups_source_start(void)
{
    ESP_LOGI(TAG, "starting USB OTG host UPS data source");
    s_events = xQueueCreate(8, sizeof(app_event_t));

    xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                            xTaskGetCurrentTaskHandle(), 10, NULL, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // wait for usb_host_install

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 9,
        .stack_size = 4096,
        .core_id = 0,
        .callback = device_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_cfg));

    xTaskCreate(app_task, "ups_usb", 5120, NULL, 6, NULL);
}
