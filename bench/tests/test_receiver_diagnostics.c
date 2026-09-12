/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/receiver_diagnostics.c */
#include "ls_test.h"
#include "receiver_diagnostics.h"

#include <limits.h>
#include <string.h>

static void check_has(const char *text, const char *part)
{
    LS_CHECK_MSG(strstr(text, part) != NULL, "missing '%s' in '%s'", part, text);
}

LS_CASE(unknown_snapshot_does_not_invent_zero_values)
{
    ls_receiver_diag_t s = {0};
    char out[384];
    int n = ls_receiver_diag_format(&s, out, sizeof(out));

    LS_CHECK(n > 0 && n < (int)sizeof(out));
    check_has(out, "app=? park=? rx=?");
    check_has(out, "present=? stream=? rhs=? hbps=?");
    check_has(out, "freq_req=? freq_eff=? freq_valid=?");
    check_has(out, "gain_req=? gain_eff=? gain_valid=?");
    check_has(out, "iq=? iqps=? frame=? valid=? fail=? ctrl=? voice=? adrop=? err=?");
    check_has(out, "mem=?/?,?/?");
}

LS_CASE(disconnected_snapshot_preserves_known_zero_counters)
{
    ls_receiver_diag_t s = {0};
    s.app_known = true;
    strcpy(s.app, "P25");
    s.parked_known = true;
    s.rx_state = LS_RECEIVER_RX_DISCONNECTED;
    s.endpoint_known = true;
    strcpy(s.endpoint_id, "rtl-sdr.usb");
    s.requested_frequency_known = true;
    s.requested_frequency_hz = 851012500;
    s.requested_gain_known = true;
    s.requested_gain_tenths_db = 230;
    s.iq_total_known = true;
    s.frame_count_known = true;
    s.audio_drop_count_known = true;
    s.receiver_error_known = true;
    strcpy(s.receiver_error, "disconnected");

    char out[384];
    ls_receiver_diag_format(&s, out, sizeof(out));
    check_has(out, "app=P25 park=0 rx=disconnected");
    check_has(out, "present=0 stream=0 rhs=? hbps=?");
    check_has(out, "freq_req=851012500 freq_eff=? freq_valid=?");
    check_has(out, "iq=0 iqps=? frame=0 valid=? fail=? ctrl=? voice=? adrop=0");
    check_has(out, "err=disconnected");
}

LS_CASE(parked_and_active_states_are_explicit_and_bounded)
{
    ls_receiver_diag_t s = {0};
    s.app_known = true;
    strcpy(s.app, "P25");
    s.parked_known = true;
    s.parked = true;
    s.rx_state = LS_RECEIVER_RX_PARKED;
    char out[48];
    memset(out, 'X', sizeof(out));
    int n = ls_receiver_diag_format(&s, out, sizeof(out));
    LS_EQ_INT(n, (int)sizeof(out) - 1);
    LS_EQ_INT(out[sizeof(out) - 1], '\0');
    check_has(out, "app=P25 park=1 rx=parked");

    memset(&s, 0, sizeof(s));
    s.app_known = true;
    strcpy(s.app, "P25");
    s.parked_known = true;
    s.rx_state = LS_RECEIVER_RX_ACTIVE;
    strcpy(s.decoder, "sync");
    strcpy(s.demod, "C4FM");
    s.endpoint_known = true;
    strcpy(s.endpoint_id, "rtl-sdr.usb");
    strcpy(s.owner, "p25");
    s.endpoint_present = true;
    s.endpoint_streaming = true;
    s.health_known = true;
    strcpy(s.health, "ok");
    s.health_rate_known = true;
    s.health_bytes_per_second = 491520;
    s.effective_frequency_known = true;
    s.effective_frequency_hz = 851012500;
    s.effective_gain_known = true;
    s.effective_gain_tenths_db = 230;
    char full[384];
    ls_receiver_diag_format(&s, full, sizeof(full));
    check_has(full, "rx=active dec=sync demod=C4FM");
    check_has(full, "present=1 stream=1 rhs=ok hbps=491520");
    check_has(full, "freq_eff=851012500 freq_valid=1");
    check_has(full, "gain_eff=230 gain_valid=1");
}

LS_CASE(integer_edges_and_every_output_size_stay_bounded)
{
    ls_receiver_diag_t s = {0};
    s.requested_frequency_known = true;
    s.requested_frequency_hz = UINT64_MAX;
    s.requested_gain_known = true;
    s.requested_gain_tenths_db = INT_MIN;
    s.memory_known = true;
    s.internal_free = UINT32_MAX;
    s.internal_largest = 0;
    s.dma_free = 19;
    s.dma_largest = UINT32_MAX;

    char full[384];
    int n = ls_receiver_diag_format(&s, full, sizeof(full));
    LS_CHECK(n > 0 && n < (int)sizeof(full));
    check_has(full, "freq_req=18446744073709551615");
    check_has(full, "gain_req=-2147483648");
    check_has(full, "mem=4294967295/0,19/4294967295");

    for (size_t cap = 1; cap < sizeof(full); ++cap) {
        char out[384];
        memset(out, 'X', sizeof(out));
        n = ls_receiver_diag_format(&s, out, cap);
        LS_CHECK(n >= 0 && n < (int)cap);
        LS_EQ_INT(out[n], '\0');
        LS_EQ_INT(out[n + 1], 'X');
        LS_EQ_INT(out[cap], 'X');
    }
}
