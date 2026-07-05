#include "hid_parser.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "hid_parser";

#define MAX_LOCAL_USAGES 24
#define MAX_REPORT_SLOTS 32
#define MAX_GLOBAL_STACK 4

typedef struct {
    uint16_t usage_page;
    int32_t logical_min, logical_max;
    int32_t physical_min, physical_max;
    int8_t unit_exponent;
    uint32_t unit;
    uint16_t report_size;
    uint16_t report_count;
    uint8_t report_id;
} globals_t;

// Running bit position per (report_id, type) so fields land at the right offset.
typedef struct {
    uint8_t id;
    uint8_t type;
    uint16_t bits;
} bitpos_t;

typedef struct {
    uint32_t usages[MAX_LOCAL_USAGES];
    size_t n_usages;
    uint32_t usage_min, usage_max;
    bool has_range;
} locals_t;

static uint16_t *bitpos_get(bitpos_t *slots, size_t *n, uint8_t id, uint8_t type)
{
    for (size_t i = 0; i < *n; i++) {
        if (slots[i].id == id && slots[i].type == type) {
            return &slots[i].bits;
        }
    }
    if (*n >= MAX_REPORT_SLOTS) {
        return NULL;
    }
    slots[*n].id = id;
    slots[*n].type = type;
    slots[*n].bits = 0;
    return &slots[(*n)++].bits;
}

static int32_t sign_extend(uint32_t v, size_t bytes)
{
    switch (bytes) {
        case 1: return (int8_t)v;
        case 2: return (int16_t)v;
        default: return (int32_t)v;
    }
}

// Combine a local usage item with the current page when it isn't extended.
static uint32_t make_usage(uint32_t data, size_t size, uint16_t page)
{
    return (size == 4) ? data : HID_USAGE(page, data);
}

static void emit_fields(hid_report_map_t *map, const globals_t *g,
                        const locals_t *loc, uint8_t report_type,
                        const uint32_t *path, uint8_t path_len,
                        uint16_t start_bit, bool is_constant)
{
    if (is_constant) {
        return;  // padding
    }
    for (uint32_t i = 0; i < g->report_count; i++) {
        uint32_t usage = 0;
        if (loc->has_range) {
            usage = loc->usage_min + i;
            if (usage > loc->usage_max) {
                usage = loc->usage_max;
            }
        } else if (loc->n_usages > 0) {
            size_t idx = (i < loc->n_usages) ? i : loc->n_usages - 1;
            usage = loc->usages[idx];
        } else {
            continue;
        }

        uint16_t page = usage >> 16;
        if (page != 0x84 && page != 0x85) {
            continue;  // only power/battery usages are interesting
        }
        if (map->count >= HID_MAX_FIELDS) {
            return;
        }

        hid_field_t *f = &map->fields[map->count++];
        memset(f, 0, sizeof(*f));
        f->usage = usage;
        f->path_len = path_len > HID_MAX_PATH ? HID_MAX_PATH : path_len;
        memcpy(f->path, path, f->path_len * sizeof(uint32_t));
        f->report_type = report_type;
        f->report_id = g->report_id;
        f->bit_offset = start_bit + i * g->report_size;
        f->bit_size = g->report_size > 255 ? 255 : g->report_size;
        f->logical_min = g->logical_min;
        f->logical_max = g->logical_max;
        f->physical_min = g->physical_min;
        f->physical_max = g->physical_max;
        f->unit_exponent = g->unit_exponent;
        f->unit = g->unit;
    }
}

esp_err_t hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                      hid_report_map_t *map)
{
    memset(map, 0, sizeof(*map));

    globals_t g = { 0 };
    globals_t gstack[MAX_GLOBAL_STACK];
    size_t gdepth = 0;
    locals_t loc = { 0 };
    uint32_t path[HID_MAX_PATH];
    uint8_t path_len = 0;
    bitpos_t slots[MAX_REPORT_SLOTS];
    size_t n_slots = 0;

    size_t pos = 0;
    while (pos < len) {
        uint8_t prefix = desc[pos++];
        if (prefix == 0xFE) {  // long item: skip
            if (pos >= len) break;
            uint8_t dsize = desc[pos];
            pos += 2 + dsize;
            continue;
        }
        size_t dsize = prefix & 0x03;
        if (dsize == 3) {
            dsize = 4;
        }
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = prefix >> 4;
        if (pos + dsize > len) {
            break;
        }
        uint32_t data = 0;
        for (size_t i = 0; i < dsize; i++) {
            data |= (uint32_t)desc[pos + i] << (8 * i);
        }
        pos += dsize;

        if (type == 0) {  // Main
            switch (tag) {
                case 0x8:    // Input
                case 0xB: {  // Feature
                    uint8_t rtype = (tag == 0x8) ? HID_REPORT_TYPE_IN : HID_REPORT_TYPE_FEAT;
                    uint16_t *bits = bitpos_get(slots, &n_slots, g.report_id, rtype);
                    if (bits) {
                        emit_fields(map, &g, &loc, rtype, path, path_len, *bits,
                                    (data & 0x01) != 0);
                        *bits += g.report_count * g.report_size;
                    }
                    break;
                }
                case 0x9: {  // Output — track bits only, we don't read them
                    uint16_t *bits = bitpos_get(slots, &n_slots, g.report_id, 0x02);
                    if (bits) {
                        *bits += g.report_count * g.report_size;
                    }
                    break;
                }
                case 0xA:  // Collection
                    if (path_len < HID_MAX_PATH) {
                        path[path_len] = loc.n_usages ? loc.usages[0] : 0;
                    }
                    path_len++;
                    break;
                case 0xC:  // End Collection
                    if (path_len > 0) {
                        path_len--;
                    }
                    break;
                default:
                    break;
            }
            memset(&loc, 0, sizeof(loc));
        } else if (type == 1) {  // Global
            switch (tag) {
                case 0x0: g.usage_page = (uint16_t)data; break;
                case 0x1: g.logical_min = sign_extend(data, dsize); break;
                case 0x2:
                    // If the minimum is non-negative, a max with the top bit
                    // set is almost certainly meant unsigned (e.g. 0xFF=255).
                    g.logical_max = (g.logical_min >= 0)
                                        ? (int32_t)data
                                        : sign_extend(data, dsize);
                    break;
                case 0x3: g.physical_min = sign_extend(data, dsize); break;
                case 0x4:
                    g.physical_max = (g.physical_min >= 0)
                                         ? (int32_t)data
                                         : sign_extend(data, dsize);
                    break;
                case 0x5:  // Unit exponent: 4-bit signed nibble
                    g.unit_exponent = (data <= 15)
                                          ? ((data > 7) ? (int8_t)(data - 16) : (int8_t)data)
                                          : (int8_t)data;
                    break;
                case 0x6: g.unit = data; break;
                case 0x7: g.report_size = (uint16_t)data; break;
                case 0x8: g.report_id = (uint8_t)data; map->uses_report_ids = true; break;
                case 0x9: g.report_count = (uint16_t)data; break;
                case 0xA:  // Push
                    if (gdepth < MAX_GLOBAL_STACK) {
                        gstack[gdepth++] = g;
                    }
                    break;
                case 0xB:  // Pop
                    if (gdepth > 0) {
                        g = gstack[--gdepth];
                    }
                    break;
                default:
                    break;
            }
        } else if (type == 2) {  // Local
            switch (tag) {
                case 0x0:  // Usage
                    if (loc.n_usages < MAX_LOCAL_USAGES) {
                        loc.usages[loc.n_usages++] = make_usage(data, dsize, g.usage_page);
                    }
                    break;
                case 0x1:  // Usage Minimum
                    loc.usage_min = make_usage(data, dsize, g.usage_page);
                    loc.has_range = true;
                    break;
                case 0x2:  // Usage Maximum
                    loc.usage_max = make_usage(data, dsize, g.usage_page);
                    loc.has_range = true;
                    break;
                default:
                    break;
            }
        }
    }

    ESP_LOGI(TAG, "indexed %u power/battery fields (report IDs: %s)",
             (unsigned)map->count, map->uses_report_ids ? "yes" : "no");
    return map->count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool path_contains(const hid_field_t *f, uint32_t ancestor)
{
    for (uint8_t i = 0; i < f->path_len; i++) {
        if (f->path[i] == ancestor) {
            return true;
        }
    }
    return false;
}

const hid_field_t *hid_find_field(const hid_report_map_t *map, uint32_t usage,
                                  uint32_t required_ancestor, uint8_t report_type)
{
    const hid_field_t *input_match = NULL;
    for (size_t i = 0; i < map->count; i++) {
        const hid_field_t *f = &map->fields[i];
        if (f->usage != usage) {
            continue;
        }
        if (required_ancestor && !path_contains(f, required_ancestor)) {
            continue;
        }
        if (report_type && f->report_type != report_type) {
            continue;
        }
        if (f->report_type == HID_REPORT_TYPE_FEAT) {
            return f;  // Feature preferred: it can be polled on demand
        }
        if (!input_match) {
            input_match = f;
        }
    }
    return input_match;
}

// HID units expressed in cm/gram carry an implicit 10^-7 against SI volts
// and watts (cm^2*g => 10^-4 * 10^-3). Volt = 0x00F0D121, Watt = 0x0000D121.
static int unit_correction(uint32_t unit)
{
    if (unit == 0x00F0D121 || unit == 0x0000D121) {
        return -7;
    }
    return 0;
}

bool hid_extract(const hid_field_t *f, const uint8_t *payload, size_t len,
                 double *out)
{
    uint32_t end_bit = f->bit_offset + f->bit_size;
    if (end_bit > len * 8) {
        return false;
    }

    uint32_t raw = 0;
    for (uint8_t b = 0; b < f->bit_size && b < 32; b++) {
        uint32_t bit = f->bit_offset + b;
        if ((payload[bit >> 3] >> (bit & 7)) & 1) {
            raw |= 1u << b;
        }
    }

    double logical;
    if (f->logical_min < 0 && f->bit_size > 1 && f->bit_size < 32 &&
        (raw & (1u << (f->bit_size - 1)))) {
        logical = (double)(int32_t)(raw | ~((1u << f->bit_size) - 1));
    } else {
        logical = (double)raw;
    }

    // Map logical range onto physical range when one is declared.
    double value = logical;
    if ((f->physical_min != 0 || f->physical_max != 0) &&
        f->logical_max != f->logical_min) {
        value = (logical - f->logical_min) *
                    ((double)f->physical_max - f->physical_min) /
                    ((double)f->logical_max - f->logical_min) +
                f->physical_min;
    }

    int exp = f->unit_exponent + unit_correction(f->unit);
    if (exp != 0) {
        value *= pow(10.0, exp);
    }
    *out = value;
    return true;
}
