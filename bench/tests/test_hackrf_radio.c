/* LS_TEST_SOURCES: */
#include "ls_test.h"
#include "hackrf_radio.h"

#include <string.h>

LS_CASE(m0_state_wire_layout_uses_40_bytes_and_preserves_failure_evidence)
{
    const uint8_t wire[40] = {
        2,0,1,0, 2,0,0,0, 0x78,0x56,0x34,0x12, 0xf0,0xde,0xbc,0x9a,
        7,0,0,0, 0,0x20,0,0, 0,0,0x10,0, 0xff,0xff,0xff,0xff,
        1,0,0,0, 3,0,0,0
    };
    ls_radio_iq_health_t h;
    LS_CHECK(ls_hackrf_decode_m0_state(wire,sizeof(wire),&h));
    LS_EQ_UINT(h.requested_mode,2);LS_EQ_UINT(h.request_flag,1);
    LS_EQ_UINT(h.active_mode,2);LS_EQ_UINT(h.m0_count,0x12345678);
    LS_EQ_UINT(h.m4_count,0x9abcdef0);LS_EQ_UINT(h.num_shortfalls,7);
    LS_EQ_UINT(h.longest_shortfall,8192);LS_EQ_UINT(h.shortfall_limit,1048576);
    LS_EQ_UINT(h.threshold,UINT32_MAX);LS_EQ_UINT(h.next_mode,1);LS_EQ_UINT(h.error,3);
    LS_CHECK(!ls_hackrf_decode_m0_state(wire,39,&h));
    LS_EQ_UINT(h.num_shortfalls,0);LS_EQ_UINT(h.m0_count,0);
    LS_CHECK(!ls_hackrf_decode_m0_state(NULL,40,&h));
}

LS_CASE(signed_iq_is_converted_to_rtl_style_offset_binary)
{
    uint8_t samples[] = {
        UINT8_C(0x80), /* -128 */
        UINT8_C(0xff), /*   -1 */
        UINT8_C(0x00), /*    0 */
        UINT8_C(0x01), /*    1 */
        UINT8_C(0x7f), /*  127 */
    };
    const uint8_t expected[] = {0, 127, 128, 129, 255};
    ls_hackrf_iq_s8_to_u8(samples, sizeof(samples));
    LS_CHECK(memcmp(samples, expected, sizeof(samples)) == 0);
}

LS_CASE(endpoint_profile_is_receive_only_half_duplex_and_honest)
{
    LS_EQ_UINT(LS_HACKRF_CAPABILITIES, LS_RADIO_RX_IQ_U8);
    LS_CHECK((LS_HACKRF_CAPABILITIES & LS_RADIO_TX_PACKET) == 0);
    LS_EQ_INT(LS_HACKRF_DUPLEX, LS_RADIO_DUPLEX_HALF);
    LS_CHECK(ls_hackrf_frequency_ranges[0].min_hz == UINT64_C(1000000));
    LS_CHECK(ls_hackrf_frequency_ranges[0].max_hz == UINT64_C(6000000000));
    LS_EQ_UINT(ls_hackrf_sample_rate_ranges[0].min_hz, UINT64_C(2000000));
    LS_EQ_UINT(ls_hackrf_sample_rate_ranges[0].max_hz, UINT64_C(20000000));
    LS_EQ_UINT(LS_HACKRF_MIN_STREAM_BYTES_PER_SEC, UINT32_C(4000000));
}

LS_CASE(vendor_payloads_are_little_endian)
{
    uint8_t payload[8];
    const uint8_t frequency_expected[8] = {
        0x70, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    ls_hackrf_encode_frequency(UINT64_C(6000000000), payload);
    LS_CHECK(memcmp(payload, frequency_expected, sizeof(payload)) == 0);

    const uint8_t rate_expected[8] = {
        0x80, 0x84, 0x1e, 0x00, 0x01, 0x00, 0x00, 0x00,
    };
    ls_hackrf_encode_sample_rate(UINT32_C(2000000), payload);
    LS_CHECK(memcmp(payload, rate_expected, sizeof(payload)) == 0);
    LS_EQ_UINT(ls_hackrf_filter_bandwidth(0, 2000000), 1750000);
}
