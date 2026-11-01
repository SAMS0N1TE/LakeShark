#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { HID_MAP_MAX = 2048, HID_FIELDS_MAX = 64, HID_REPORTS_MAX = 16, HID_PAYLOAD_MAX = 64 };
typedef enum {
    HID_PARSE_OK,
    HID_PARSE_MALFORMED,
    HID_PARSE_LIMIT,
    HID_PARSE_UNSUPPORTED
} hid_parse_result_t;
typedef struct {
    uint16_t page, usage, usage_max, bit, count;
    int32_t minimum, maximum;
    uint8_t size, id, type, flags;
} hid_field_t;
typedef struct {
    uint8_t id, type;
    uint16_t bits;
} hid_report_size_t;
typedef struct {
    hid_field_t fields[HID_FIELDS_MAX];
    hid_report_size_t reports[HID_REPORTS_MAX];
    uint16_t nfields, nreports, unsupported;
    uint32_t fingerprint;
} hid_map_t;
typedef struct {
    uint8_t keys[32]; /* keyboard usages 0..255, including modifiers */
    uint16_t consumer[8];
    uint8_t nconsumer, buttons;
    int32_t x, y, wheel, pan;
    bool keyboard, pointer, rollover;
} hid_sample_t;
hid_parse_result_t hid_map_compile(hid_map_t *out, const uint8_t *bytes, size_t len);
bool hid_map_decode(const hid_map_t *map, uint8_t id, const uint8_t *bytes, size_t len,
                    hid_sample_t *out);
bool hid_boot_decode(bool mouse, const uint8_t *bytes, size_t len, hid_sample_t *out);
