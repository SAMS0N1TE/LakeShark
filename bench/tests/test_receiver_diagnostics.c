/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/receiver_diagnostics.c */
#include "ls_test.h"
#include "receiver_diagnostics.h"

#include <limits.h>
#include <string.h>

static void check_has(const char *text, const char *part)
{
    LS_CHECK_MSG(strstr(text, part) != NULL, "missing '%s' in '%s'", part, text);
}

static ls_receiver_diag_t trial_snapshot(void)
{
    ls_receiver_diag_t s={0};
    s.app_known=s.parked_known=s.endpoint_known=s.source_known=true;
    s.rx_state=LS_RECEIVER_RX_ACTIVE;
    strcpy(s.app,"FM"); strcpy(s.demod,"listen");
    strcpy(s.source,"external-iq"); strcpy(s.endpoint_id,"rtl-sdr.usb");
    strcpy(s.owner,"fm");
    s.requested_frequency_known=s.effective_frequency_known=true;
    s.requested_frequency_hz=s.effective_frequency_hz=152600000;
    s.sample_rate_known=s.config_known=true;
    s.sample_rate_hz=s.bandwidth_hz=256000;
    s.requested_gain_known=s.effective_gain_known=true;
    s.requested_gain_tenths_db=s.effective_gain_tenths_db=200;
    s.gain_mode=LS_RADIO_GAIN_MANUAL;
    s.context_epoch_known=s.consumer_epoch_known=s.iq_epoch_known=true;
    s.context_epoch=10; s.consumer_epoch=4; s.iq_epoch=2;
    s.captured_known=true; s.captured_us=1000000;
    s.iq_total_known=s.frame_count_known=s.valid_count_known=true;
    s.iq_bytes_total=512; s.frame_count=9; s.valid_count=4;
    return s;
}

LS_CASE(trial_known_zero_and_deltas_require_same_context_and_counter_epoch)
{
    ls_receiver_diag_t a=trial_snapshot(),b=a;
    ls_receiver_trial_t t={0};
    ls_receiver_trial_compare(&t,&a,a.captured_us);
    LS_CHECK(!t.baseline_set && !t.compared);
    ls_receiver_trial_baseline(&t,&a);
    LS_EQ_STR(ls_receiver_trial_state(&t),"BASELINE SAVED");
    b.captured_us+=1000000; b.iq_bytes_total+=256; b.valid_count+=2;
    ls_receiver_trial_compare(&t,&b,b.captured_us);
    LS_EQ_UINT(t.delta_known,LS_TRIAL_IQ);
    LS_EQ_UINT(t.iq_delta,256);
    LS_CHECK(t.unknown&LS_TRIAL_DECODER_EPOCH);
    LS_EQ_UINT(t.before.captured_us,a.captured_us);
    a.decoder_epoch_known=b.decoder_epoch_known=true;
    a.decoder_epoch=b.decoder_epoch=7;
    ls_receiver_trial_baseline(&t,&a);
    b.iq_bytes_total=a.iq_bytes_total; b.valid_count=a.valid_count;
    ls_receiver_trial_compare(&t,&b,b.captured_us);
    LS_CHECK(t.delta_known&LS_TRIAL_VALID);
    LS_EQ_UINT(t.valid_delta,0);
    LS_EQ_UINT(t.iq_delta,0);
    char out[768]; ls_receiver_trial_format(&t,out,sizeof(out));
    check_has(out,"valid_delta=0");
    ls_receiver_trial_baseline(&t,NULL);
    LS_EQ_STR(ls_receiver_trial_state(&t),"NO BASELINE");
}

LS_CASE(trial_changed_settings_and_unknown_context_never_publish_counter_deltas)
{
    ls_receiver_diag_t a=trial_snapshot();
    for (int i=0;i<12;i++) {
        ls_receiver_diag_t b=a; b.captured_us+=1000000; b.iq_bytes_total+=500;
        ls_receiver_trial_t t; ls_receiver_trial_baseline(&t,&a);
        switch(i) {
        case 0: strcpy(b.app,"P25"); break;
        case 1: strcpy(b.endpoint_id,"hackrf.usb"); break;
        case 2: strcpy(b.demod,"pocsag"); break;
        case 3: ++b.requested_frequency_hz; break;
        case 4: ++b.effective_frequency_hz; break;
        case 5: ++b.sample_rate_hz; break;
        case 6: ++b.bandwidth_hz; break;
        case 7: ++b.effective_gain_tenths_db; break;
        case 8: ++b.context_epoch; break; /* tune away and back */
        case 9: ++b.consumer_epoch; break; /* mode away and back */
        case 10: b.source_known=false; break;
        case 11: b.context_epoch_known=false; break;
        }
        ls_receiver_trial_compare(&t,&b,b.captured_us);
        LS_CHECK(t.changed || t.unknown);
        LS_EQ_UINT(t.delta_known,0);
        LS_EQ_UINT(t.iq_delta,0);
    }
}

LS_CASE(trial_reset_wrap_saturation_reversed_time_and_staleness_are_explicit)
{
    ls_receiver_diag_t a=trial_snapshot(),b=a;
    ls_receiver_trial_t t; ls_receiver_trial_baseline(&t,&a);
    b.captured_us+=1000000; b.iq_bytes_total=1;
    ls_receiver_trial_compare(&t,&b,b.captured_us);
    LS_CHECK(t.counter_reset&LS_TRIAL_IQ); LS_EQ_UINT(t.delta_known,0);
    b.iq_bytes_total=a.iq_bytes_total+20;
    b.context_epoch=UINT64_MAX;
    ls_receiver_trial_compare(&t,&b,b.captured_us);
    LS_CHECK(t.unknown&LS_TRIAL_CONTEXT); LS_EQ_UINT(t.delta_known,0);
    b=a; b.captured_us-=1;
    ls_receiver_trial_compare(&t,&b,a.captured_us);
    LS_CHECK(t.time_invalid); LS_EQ_UINT(t.delta_known,0);
    b=a; b.captured_us+=100;
    ls_receiver_trial_compare(&t,&b,b.captured_us+LS_RECEIVER_TRIAL_FRESH_US+1);
    LS_CHECK(t.stale); LS_EQ_UINT(t.delta_known,0);
    b.captured_us=a.captured_us+LS_RECEIVER_TRIAL_MAX_US+1;
    ls_receiver_trial_compare(&t,&b,b.captured_us);
    LS_CHECK(t.stale);
    b=a; b.captured_us+=100; b.parked=true;
    ls_receiver_trial_compare(&t,&b,b.captured_us);
    LS_CHECK(t.inactive); LS_EQ_UINT(t.delta_known,0);
    ls_receiver_trial_compare(&t,NULL,b.captured_us);
    LS_CHECK(t.time_invalid); LS_EQ_UINT(t.delta_known,0);
}

LS_CASE(trial_receipt_preserves_null_unknown_and_escapes_tokens_without_truncation)
{
    ls_receiver_diag_t a=trial_snapshot(),b=a;
    strcpy(a.owner,"a\"b\\c"); b=a; b.captured_us+=1000000;
    ls_receiver_trial_t t; ls_receiver_trial_baseline(&t,&a);
    ls_receiver_trial_compare(&t,&b,b.captured_us);
    char full[4096];
    int n=ls_receiver_trial_json(&t,full,sizeof(full));
    LS_CHECK(n>0);
    check_has(full,"\"type\":\"receiver_trial\"");
    check_has(full,"\"owner\":\"a\\\"b\\\\c\"");
    check_has(full,"\"iq_delta\":0");
    check_has(full,"\"valid_delta\":null");
    check_has(full,"\"rf_coverage\":null,\"calibration\":null,\"direction\":null");
    for (size_t cap=1;cap<=(size_t)n;cap++) {
        char out[4096]; memset(out,'X',sizeof(out));
        LS_EQ_INT(ls_receiver_trial_json(&t,out,cap),-1);
        LS_EQ_INT(out[0],0); LS_EQ_INT(out[cap],'X');
    }
}

LS_CASE(guided_ui_does_not_promote_pager_sync_or_saved_capture_to_valid_frames)
{
    static const struct { const char *app, *mode; } native[] = {
        { "FM", "pocsag" }, { "FM", "flex" }, { "REC", "raw" }
    };
    for (unsigned i = 0; i < sizeof(native) / sizeof(native[0]); ++i) {
        ls_receiver_diag_t s = {0};
        s.app_known = true;
        strcpy(s.app, native[i].app);
        strcpy(s.demod, native[i].mode);
        s.rx_state = LS_RECEIVER_RX_ACTIVE;
        s.frame_count_known = s.valid_count_known = true;
        s.frame_count = s.valid_count = 42;
        ls_receiver_diag_ui_counters(&s);
        LS_CHECK(s.frame_count_known);
        LS_EQ_INT(42, s.frame_count);
        LS_CHECK(!s.valid_count_known);
        LS_CHECK(strcmp(ls_receiver_diag_assess(&s).title, "VALID FRAMES SEEN") != 0);
    }
    ls_receiver_diag_t p25 = {0};
    p25.app_known = true;
    strcpy(p25.app, "P25");
    p25.valid_count_known = true;
    p25.valid_count = 7;
    ls_receiver_diag_ui_counters(&p25);
    LS_CHECK(p25.valid_count_known);
    LS_EQ_INT(7, p25.valid_count);
}

LS_CASE(reception_check_keeps_missing_evidence_distinct_from_zero)
{
    ls_receiver_diag_t s = {0};
    ls_receiver_check_t a = ls_receiver_diag_assess(&s);
    LS_CHECK(a.level == LS_RECEIVER_CHECK_UNKNOWN);
    LS_CHECK(strcmp(a.title, "STATE UNAVAILABLE") == 0);
    s.rx_state = LS_RECEIVER_RX_ACTIVE;
    a = ls_receiver_diag_assess(&s);
    LS_CHECK(a.level == LS_RECEIVER_CHECK_UNKNOWN);
    s.iq_rate_known = true;
    a = ls_receiver_diag_assess(&s);
    LS_CHECK(strcmp(a.title, "NO CURRENT IQ") == 0);
    LS_CHECK(strstr(a.next, "Wait") != NULL);
}

LS_CASE(reception_check_does_not_diagnose_expected_parking_as_missing_radio)
{
    ls_receiver_diag_t s = {0};
    s.parked_known = s.parked = true;
    s.endpoint_known = true;
    s.rx_state = LS_RECEIVER_RX_DISCONNECTED;
    ls_receiver_check_t a = ls_receiver_diag_assess(&s);
    LS_CHECK(a.level == LS_RECEIVER_CHECK_INFO);
    LS_CHECK(strcmp(a.title, "RECEIVER PARKED") == 0);
    s.parked = false;
    a = ls_receiver_diag_assess(&s);
    LS_CHECK(strcmp(a.title, "RECEIVER MISSING") == 0);
    LS_CHECK(strstr(a.next, "connection") != NULL);
}

LS_CASE(reception_check_samples_are_transport_evidence_not_decoded_rf)
{
    ls_receiver_diag_t s = {0};
    s.rx_state = LS_RECEIVER_RX_ACTIVE;
    s.iq_rate_known = true;
    s.iq_bytes_per_second = 491520;
    ls_receiver_check_t a = ls_receiver_diag_assess(&s);
    LS_CHECK(strcmp(a.title, "SAMPLES FLOWING") == 0);
    LS_CHECK(strstr(a.reason, "unavailable") != NULL);
    s.valid_count_known = true;
    a = ls_receiver_diag_assess(&s);
    LS_CHECK(strstr(a.reason, "no valid frames") != NULL);
    LS_CHECK(strstr(a.next, "frequency, mode and antenna") != NULL);
    s.valid_count = 4;
    a = ls_receiver_diag_assess(&s);
    LS_CHECK(strstr(a.reason, "cumulative") != NULL);
    LS_CHECK(strstr(a.next, "not live proof") != NULL);
    s.iq_bytes_per_second = 0;
    a = ls_receiver_diag_assess(&s);
    LS_CHECK(strcmp(a.title, "NO CURRENT IQ") == 0);
}

LS_CASE(reception_check_optional_usb_absence_does_not_fail_active_chip_receiver)
{
    ls_receiver_diag_t s = {0};
    s.endpoint_known = true;
    s.endpoint_present = false;
    s.rx_state = LS_RECEIVER_RX_ACTIVE;
    s.valid_count_known = true;
    s.valid_count = 12;
    ls_receiver_check_t a = ls_receiver_diag_assess(&s);
    LS_CHECK(strcmp(a.title, "VALID FRAMES SEEN") == 0);
}

LS_CASE(reception_check_errors_offer_specific_actions_and_do_not_hide_behind_history)
{
    ls_receiver_diag_t s = {0};
    s.receiver_error_known = true;
    s.rx_state = LS_RECEIVER_RX_ACTIVE;
    s.valid_count_known = true;
    s.valid_count = 42;
    static const struct { const char *error, *title, *action; } cases[] = {
        {"unsupported", "MODE NOT READY", "supported receiver"},
        {"busy", "RECEIVER BUSY", "owner"},
        {"no-memory", "MEMORY SHORTAGE", "MEMORY"},
        {"io", "RECEIVER ERROR", "health"},
        {"timeout", "RECEIVER ERROR", "health"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        strcpy(s.receiver_error, cases[i].error);
        ls_receiver_check_t a = ls_receiver_diag_assess(&s);
        LS_CHECK(a.level == LS_RECEIVER_CHECK_ATTENTION);
        LS_CHECK(strcmp(a.title, cases[i].title) == 0);
        LS_CHECK(strstr(a.next, cases[i].action) != NULL);
    }
    s.receiver_error_known = false;
    s.rx_state = LS_RECEIVER_RX_IDLE;
    LS_CHECK(strcmp(ls_receiver_diag_assess(&s).title, "RECEIVER IDLE") == 0);
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
