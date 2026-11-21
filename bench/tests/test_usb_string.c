/* USB string descriptors to C strings: the length comes from bLength, which
   counts the two header bytes, so nothing after the descriptor leaks into
   the name the RTL-SDR model checks compare. */
#include "ls_test.h"
#include "ls_usb_string.h"

#include <string.h>

/* A descriptor for `s`, then `trail` bytes that are not part of it. */
static size_t make_desc(uint8_t *buf, const char *s, const uint8_t *trail, size_t trail_n)
{
    const size_t n = strlen(s);
    buf[0] = (uint8_t)(2 + 2 * n);
    buf[1] = 0x03;                                  /* STRING */
    for (size_t i = 0; i < n; i++) {
        buf[2 + 2 * i] = (uint8_t)s[i];
        buf[3 + 2 * i] = 0;
    }
    memcpy(buf + 2 + 2 * n, trail, trail_n);
    return 2 + 2 * n + trail_n;
}

LS_CASE(a_blog_v4_reads_as_its_exact_name_whatever_follows_it)
{
    /* What follows a descriptor in memory is not zero in general. The old
       copy took bLength / 2 code units, one too many, and read "Blog V4X". */
    static const uint8_t junk[] = { 'X', 0, 'Y', 0 };
    uint8_t d[64];
    char s[256];
    memset(s, 0x5A, sizeof(s));
    make_desc(d, "Blog V4", junk, sizeof(junk));
    LS_EQ_UINT(7, ls_usb_string_ascii(d, s, sizeof(s)));
    LS_EQ_STR("Blog V4", s);
    make_desc(d, "RTLSDRBlog", junk, sizeof(junk));
    LS_EQ_UINT(10, ls_usb_string_ascii(d, s, sizeof(s)));
    LS_EQ_STR("RTLSDRBlog", s);
}

LS_CASE(empty_and_short_descriptors_give_an_empty_string)
{
    char s[8] = "stale";
    const uint8_t header_only[] = { 2, 0x03, 'Z', 0 };
    LS_EQ_UINT(0, ls_usb_string_ascii(header_only, s, sizeof(s)));
    LS_EQ_STR("", s);
    const uint8_t too_short[] = { 1, 0x03, 'Z', 0 };
    memcpy(s, "stale", 6);
    LS_EQ_UINT(0, ls_usb_string_ascii(too_short, s, sizeof(s)));
    LS_EQ_STR("", s);
    memcpy(s, "stale", 6);
    LS_EQ_UINT(0, ls_usb_string_ascii(NULL, s, sizeof(s)));
    LS_EQ_STR("", s);
}

LS_CASE(an_odd_length_drops_the_half_code_unit)
{
    /* bLength 7: header plus two whole code units and one stray byte. */
    const uint8_t d[] = { 7, 0x03, 'A', 0, 'B', 0, 'C', 0 };
    char s[8];
    LS_EQ_UINT(2, ls_usb_string_ascii(d, s, sizeof(s)));
    LS_EQ_STR("AB", s);
}

LS_CASE(the_output_buffer_bounds_the_copy)
{
    uint8_t d[64];
    char s[5];
    make_desc(d, "RTLSDRBlog", (const uint8_t *)"", 0);
    LS_EQ_UINT(4, ls_usb_string_ascii(d, s, sizeof(s)));
    LS_EQ_STR("RTLS", s);
    LS_EQ_UINT(0, ls_usb_string_ascii(d, s, 0));
}
