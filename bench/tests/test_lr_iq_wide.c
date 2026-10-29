#include "ls_lora_iq.h"
#include "ls_test.h"

LS_CASE(source0_has_the_exact_proven_576k_route)
{
    const uint8_t expected[] = {0x11, 0, 8, 0xCA, 0, 8, 0, 0, 0, 0, 1};
    uint8_t route[11]; uint32_t rate; lr20xx_iq_format_t format;
    LS_CHECK(lr20xx_iq_route_build(LR20XX_IQ_SOURCE0_576, 0, 0, route, &rate, &format));
    LS_CHECK(memcmp(route, expected, sizeof(route)) == 0);
    LS_EQ_UINT(rate, 576000); LS_EQ_INT(format, LR20XX_IQ8);
}

LS_CASE(source1_presets_have_exact_dividers_and_big_endian_routes)
{
    const uint8_t codes[] = {0x1A, 0x12, 0x21, 0x19};
    const uint32_t rates[] = {500000, 666667, 800000, 1000000};
    const uint8_t expected[][11] = {
        {0x1A, 0, 7, 0xA1, 0x20, 9, 0, 0, 0, 0, 1},
        {0x12, 0, 0x0A, 0x2C, 0x2B, 9, 0, 0, 0, 0, 1},
        {0x21, 0, 0x0C, 0x35, 0, 9, 0, 0, 0, 0, 1},
        {0x19, 0, 0x0F, 0x42, 0x40, 9, 0, 0, 0, 0, 1},
    };
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t route[11]; uint32_t rate; lr20xx_iq_format_t format;
        LS_EQ_UINT(lr20xx_iq_source1_rate(codes[i]), rates[i]);
        LS_CHECK(lr20xx_iq_route_build(LR20XX_IQ_SOURCE1, codes[i], 0, route, &rate, &format));
        LS_EQ_UINT(rate, rates[i]); LS_EQ_INT(format, LR20XX_IQ8);
        LS_CHECK(memcmp(route, expected[i], sizeof(route)) == 0);
    }
}

LS_CASE(rate_divider_includes_all_fractional_families_and_rounding)
{
    LS_EQ_UINT(lr20xx_iq_source1_rate(0), 8000000);
    LS_EQ_UINT(lr20xx_iq_source1_rate(0x3F), 7813);
    LS_EQ_UINT(lr20xx_iq_source1_rate(0x40), 7428571);
    LS_EQ_UINT(lr20xx_iq_source1_rate(0x80), 6933333);
    LS_EQ_UINT(lr20xx_iq_source1_rate(0xC0), 5777778);
    LS_EQ_UINT(lr20xx_iq_source1_rate(0xFF), 5642);
}

LS_CASE(source2_preserves_its_route_and_iq16_rates)
{
    const uint8_t expected[] = {0xFF, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 1};
    for (uint32_t bw = 31250; bw <= 250000; bw *= 2) {
        uint8_t route[11]; uint32_t rate; lr20xx_iq_format_t format;
        LS_CHECK(lr20xx_iq_route_build(LR20XX_IQ_SOURCE2, 0x12, 4*bw, route, &rate, &format));
        LS_CHECK(memcmp(route, expected, sizeof(route)) == 0);
        LS_EQ_UINT(rate, 4*bw); LS_EQ_INT(format, LR20XX_IQ16);
    }
}

LS_CASE(raw_route_serializes_all_four_rate_bytes)
{
    uint8_t route[11];
    const uint8_t expected[] = {0xC3, 0x12, 0x34, 0x56, 0x78, 0x0F, 0, 0, 0, 0, 1};
    LS_CHECK(lr20xx_iq_raw_route(0xC3, 0x12345678, 7, route));
    LS_CHECK(memcmp(route, expected, sizeof(route)) == 0);
}

LS_CASE(route_helpers_reject_invalid_arguments)
{
    uint8_t route[11]; uint32_t rate; lr20xx_iq_format_t format;
    LS_CHECK(!lr20xx_iq_raw_route(0, 1, 8, route));
    LS_CHECK(!lr20xx_iq_raw_route(0, 0, 0, route));
    LS_CHECK(!lr20xx_iq_raw_route(0, 1, 0, NULL));
    LS_CHECK(!lr20xx_iq_route_build((lr20xx_iq_route_t)3, 0, 125000, route, &rate, &format));
    LS_CHECK(!lr20xx_iq_route_build(LR20XX_IQ_SOURCE2, 0, 576000, route, &rate, &format));
    LS_CHECK(!lr20xx_iq_route_build(LR20XX_IQ_SOURCE2, 0, 125000, NULL, &rate, &format));
    LS_CHECK(!lr20xx_iq_route_build(LR20XX_IQ_SOURCE2, 0, 125000, route, NULL, &format));
    LS_CHECK(!lr20xx_iq_route_build(LR20XX_IQ_SOURCE2, 0, 125000, route, &rate, NULL));
}

LS_CASE(iq8_expansion_preserves_iq_order_signed_extremes_and_scale)
{
    const uint8_t in[] = {0x80, 0x7F, 0xE0, 0x1F, 0xFF, 1, 0, 0};
    const int16_t expected[] = {-32768, 32512, -8192, 7936, -256, 256, 0, 0};
    int16_t out[10];
    for (unsigned i = 0; i < 10; ++i) out[i] = 1234;
    lr20xx_iq_expand8(in, out + 1, 4);
    LS_CHECK(memcmp(out + 1, expected, sizeof(expected)) == 0);
    LS_EQ_INT(out[0], 1234); LS_EQ_INT(out[9], 1234);
    lr20xx_iq_expand8(NULL, NULL, 0);
}

LS_CASE(iq8_expands_every_possible_signed_sample)
{
    uint8_t in[256]; int16_t out[256];
    for (unsigned i = 0; i < 256; ++i) in[i] = (uint8_t)i;
    lr20xx_iq_expand8(in, out, 128);
    for (unsigned i = 0; i < 256; ++i)
        LS_EQ_INT(out[i], (i < 128 ? (int)i : (int)i - 256) * 256);
}
