// Minimal HID report descriptor parser, scoped to what a USB UPS needs:
// it indexes every Input/Feature data field on the Power Device (0x84) and
// Battery System (0x85) usage pages so values can be located and scaled.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define HID_MAX_FIELDS 96
#define HID_MAX_PATH   6

// (usage_page << 16) | usage_id
#define HID_USAGE(page, id) (((uint32_t)(page) << 16) | (uint32_t)(id))

#define HID_REPORT_TYPE_IN   0x01
#define HID_REPORT_TYPE_FEAT 0x03

typedef struct {
    uint32_t usage;                   // page<<16 | id
    uint32_t path[HID_MAX_PATH];      // enclosing collection usages, outermost first
    uint8_t path_len;
    uint8_t report_type;              // HID_REPORT_TYPE_IN / _FEAT
    uint8_t report_id;                // 0 if the device doesn't use report IDs
    uint16_t bit_offset;              // within the report payload (ID byte excluded)
    uint8_t bit_size;
    int32_t logical_min, logical_max;
    int32_t physical_min, physical_max;
    int8_t unit_exponent;
    uint32_t unit;
} hid_field_t;

typedef struct {
    hid_field_t fields[HID_MAX_FIELDS];
    size_t count;
    bool uses_report_ids;
} hid_report_map_t;

esp_err_t hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                      hid_report_map_t *map);

// Find a field by usage. `required_ancestor` (0 = any) must appear somewhere
// in the field's collection path; `report_type` 0 matches any type, with
// Feature preferred over Input when both exist.
const hid_field_t *hid_find_field(const hid_report_map_t *map, uint32_t usage,
                                  uint32_t required_ancestor, uint8_t report_type);

// Extract the field's value from a report payload (report ID byte already
// stripped) and convert it to physical units. Returns false if the report
// is too short.
bool hid_extract(const hid_field_t *f, const uint8_t *payload, size_t len,
                 double *out);
