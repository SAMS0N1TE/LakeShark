#include "ble_hid_report.h"
#include "hid_fixtures.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static hid_map_t m;
static bool key(hid_sample_t *s, unsigned u)
{
    return (s->keys[u / 8] >> (u % 8)) & 1;
}
int main(void)
{
    hid_sample_t s;
    unsigned char b[64] = {2, 0, 4, 5};
    assert(hid_map_compile(&m, boot_map, sizeof(boot_map)) == HID_PARSE_OK);
    assert(hid_map_decode(&m, 0, b, 8, &s));
    assert(key(&s, 4) && key(&s, 5) && key(&s, 0xe1));
    assert(!hid_map_decode(&m, 0, b, 7, &s));
    assert(!hid_map_decode(&m, 1, b, 8, &s));
    b[2] = 1;
    assert(!hid_map_decode(&m, 0, b, 8, &s));
    assert(!hid_boot_decode(false, b, 8, &s));
    b[2] = 4;
    assert(hid_boot_decode(false, b, 8, &s));
    assert(key(&s, 0xe1));
    assert(hid_map_compile(&m, nkro_map, sizeof(nkro_map)) == HID_PARSE_OK);
    memset(b, 0, sizeof(b));
    b[4 / 8] |= 1 << (4 % 8);
    b[200 / 8] |= 1 << (200 % 8);
    b[0xe7 / 8] |= 1 << (0xe7 % 8);
    assert(hid_map_decode(&m, 7, b, 29, &s));
    assert(key(&s, 4) && key(&s, 200) && key(&s, 0xe7));
    assert(hid_map_compile(&m, mouse_map, sizeof(mouse_map)) == HID_PARSE_OK);
    unsigned char mouse[] = {5, 0xfe, 3, 0xff};
    assert(hid_map_decode(&m, 0, mouse, 4, &s));
    assert(s.buttons == 5 && s.x == -2 && s.y == 3 && s.wheel == -1);
    assert(hid_map_compile(&m, rii_map, sizeof(rii_map)) == HID_PARSE_OK);
    unsigned char kbd[] = {0, 0, 4, 0, 0, 0, 0, 0};
    assert(hid_map_decode(&m, 1, kbd, 8, &s));
    assert(key(&s, 4));
    /* Signed 12-bit axes: x=-3 (ffd), y=17 (011), unaligned byte boundary. */
    unsigned char pad[] = {2, 0xfd, 0x1f, 0x01, 1};
    assert(hid_map_decode(&m, 2, pad, 5, &s));
    assert(s.x == -3 && s.y == 17 && s.buttons == 2 && s.wheel == 1);
    unsigned char media[] = {0xcd, 0};
    assert(hid_map_decode(&m, 3, media, 2, &s));
    assert(s.nconsumer == 1 && s.consumer[0] == 0xcd);
    for (unsigned i = 0; i < m.nreports; i++)
        if (m.reports[i].id == 1)
            assert(m.reports[i].bits == (m.reports[i].type == 1 ? 64 : 8));
    /* Prefixes must never read beyond their supplied length. A collection-free
     * prefix can be valid, but a prefix inside an item/collection cannot. */
    for (size_t n = 0; n < sizeof(rii_map); n++)
        (void)hid_map_compile(&m, rii_map, n);
    const unsigned char truncated[] = {0x05};
    assert(hid_map_compile(&m, truncated, sizeof(truncated)) == HID_PARSE_MALFORMED);
    const unsigned char pop[] = {0xb4};
    assert(hid_map_compile(&m, pop, sizeof(pop)) == HID_PARSE_MALFORMED);
    const unsigned char range[] = {0x19, 9, 0x29, 2};
    assert(hid_map_compile(&m, range, sizeof(range)) == HID_PARSE_MALFORMED);
    const unsigned char longitem[] = {0xfe, 0xff, 0};
    assert(hid_map_compile(&m, longitem, sizeof(longitem)) == HID_PARSE_UNSUPPORTED);
    const unsigned char overflow[] = {0x75, 32, 0x97, 0xff, 0xff, 0xff, 0xff, 0x81, 2};
    assert(hid_map_compile(&m, overflow, sizeof(overflow)) == HID_PARSE_LIMIT);
    unsigned char huge[2049] = {0};
    assert(hid_map_compile(&m, huge, sizeof(huge)) == HID_PARSE_LIMIT);
    const unsigned char pushes[] = {0xa4, 0xa4, 0xa4, 0xa4, 0xa4};
    assert(hid_map_compile(&m, pushes, sizeof(pushes)) == HID_PARSE_LIMIT);
    unsigned char fields[512];
    unsigned n = 0;
    fields[n++] = 0x75;
    fields[n++] = 1;
    fields[n++] = 0x95;
    fields[n++] = 1;
    for (unsigned i = 0; i < 65; i++) {
        fields[n++] = 0x09;
        fields[n++] = 4;
        fields[n++] = 0x81;
        fields[n++] = 2;
    }
    assert(hid_map_compile(&m, fields, n) == HID_PARSE_LIMIT);
    n = 0;
    fields[n++] = 0x75;
    fields[n++] = 1;
    fields[n++] = 0x95;
    fields[n++] = 1;
    for (unsigned i = 1; i <= 17; i++) {
        fields[n++] = 0x85;
        fields[n++] = i;
        fields[n++] = 0x81;
        fields[n++] = 1;
    }
    assert(hid_map_compile(&m, fields, n) == HID_PARSE_LIMIT);
    /* Deterministic malformed corpus also exercises arithmetic and item sizes. */
    unsigned seed = 12345;
    for (unsigned z = 0; z < 10000; z++) {
        unsigned char fuzz[80];
        for (unsigned j = 0; j < 80; j++) {
            seed = seed * 1664525u + 1013904223u;
            fuzz[j] = seed >> 24;
        }
        hid_map_compile(&m, fuzz, z % 80);
    }
    puts("BLE HID report fixtures and bounds passed");
    return 0;
}
