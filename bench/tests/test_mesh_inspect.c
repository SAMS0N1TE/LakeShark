#include "ls_test.h"
#include "ls_mesh_inspect.h"
#include <string.h>
#include <stdio.h>

static ls_inspect_tracker_t t;
static const char *peer = "0123456789ABCDEF";
static void trace(uint32_t now)
{
    ls_inspect_init(&t);
    LS_EQ_INT(ls_inspect_begin(&t, now, 42, peer, LS_INSPECT_TRACE, true), LS_INSPECT_WAITING);
    t.result.hash_size = 2; t.result.hop_count = 2;
    memcpy(t.result.hashes, "abcd", 4);
}
LS_CASE(trace_matches_tag_route_and_kind)
{
    const int8_t snrs[] = {-20, 32};
    trace(100);
    LS_CHECK(!ls_inspect_trace(&t, 200, 41, 1, (const uint8_t *)"abcd", 4, snrs, 2, -90, 6));
    LS_CHECK(!ls_inspect_trace(&t, 200, 42, 1, (const uint8_t *)"abce", 4, snrs, 2, -90, 6));
    LS_EQ_INT(t.result.state, LS_INSPECT_WAITING);
    LS_CHECK(ls_inspect_trace(&t, 250, 42, 1, (const uint8_t *)"abcd", 4, snrs, 2, -90, 6));
    LS_EQ_INT(t.result.rtt_ms, 150);
    LS_EQ_INT(t.result.snr_q4[0], -20);
    LS_EQ_INT(t.result.snr_q4[1], 32);
    LS_EQ_INT(t.history_count, 1);
    LS_CHECK(!ls_inspect_trace(&t, 300, 42, 1, (const uint8_t *)"abcd", 4, snrs, 2, -90, 6));
}
LS_CASE(malformed_hop_lists_do_not_complete)
{
    const int8_t snrs[] = {1, 2};
    trace(0);
    LS_CHECK(!ls_inspect_trace(&t, 1, 42, 1, (const uint8_t *)"abc", 3, snrs, 2, -90, 6));
    LS_CHECK(!ls_inspect_trace(&t, 1, 42, 1, (const uint8_t *)"abcd", 4, snrs, 1, -90, 6));
    LS_CHECK(!ls_inspect_trace(&t, 1, 42, 1, NULL, 4, snrs, 2, -90, 6));
    LS_EQ_INT(t.result.state, LS_INSPECT_WAITING);
}
LS_CASE(one_outstanding_and_cooldown)
{
    trace(0);
    LS_EQ_INT(ls_inspect_begin(&t, 1, 43, peer, LS_INSPECT_TELEMETRY, true), LS_INSPECT_REFUSED);
    LS_EQ_INT(t.result.tag, 42);
    ls_inspect_finish(&t, 10, LS_INSPECT_OK);
    LS_EQ_INT(ls_inspect_begin(&t, 59999, 43, peer, LS_INSPECT_TELEMETRY, true), LS_INSPECT_REFUSED);
    LS_EQ_INT(ls_inspect_begin(&t, 60000, 43, peer, LS_INSPECT_TELEMETRY, true), LS_INSPECT_WAITING);
}
LS_CASE(transmit_gate_cannot_reserve_a_probe)
{
    ls_inspect_init(&t);
    LS_EQ_INT(ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_TRACE, false), LS_INSPECT_NOT_PERMITTED);
    LS_EQ_INT(t.result.state, LS_INSPECT_IDLE);
    LS_CHECK(!t.sent);
    LS_EQ_INT(ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_TRACE, true), LS_INSPECT_WAITING);
    ls_inspect_finish(&t, 1, LS_INSPECT_NOT_PERMITTED);
    LS_EQ_INT(ls_inspect_begin(&t, 2, 43, peer, LS_INSPECT_TRACE, true), LS_INSPECT_REFUSED);
}
LS_CASE(timeout_and_clock_wrap_reject_late_replies)
{
    const int8_t snrs[] = {1, 2};
    trace(UINT32_MAX - 1000);
    ls_inspect_tick(&t, 28998);
    LS_EQ_INT(t.result.state, LS_INSPECT_WAITING);
    ls_inspect_tick(&t, 28999);
    LS_EQ_INT(t.result.state, LS_INSPECT_TIMEOUT);
    LS_EQ_INT(t.result.rtt_ms, 30000);
    LS_CHECK(!ls_inspect_trace(&t, 29000, 42, 1, (const uint8_t *)"abcd", 4, snrs, 2, -90, 6));
}
LS_CASE(telemetry_matches_sender_and_decodes_signed_and_zero_channel_fields)
{
    uint8_t data[] = {42,0,0,0, 0,116,1,144, 1,103,255,156, 2,104,80, 0,0,0};
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_TELEMETRY, true);
    LS_CHECK(!ls_inspect_telemetry(&t, 10, "FEDCBA9876543210", data, sizeof(data), -90, 4));
    data[0] = 41;
    LS_CHECK(!ls_inspect_telemetry(&t, 10, peer, data, sizeof(data), -90, 4));
    data[0] = 42;
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, data, sizeof(data), -90, 4));
    LS_EQ_INT(t.result.field_count, 3);
    LS_CHECK(t.result.fields[0].value == 4.0f);
    LS_CHECK(t.result.fields[1].value == -10.0f);
    LS_CHECK(t.result.fields[2].value == 40.0f);
}
LS_CASE(truncated_telemetry_leaves_request_pending)
{
    uint8_t data[] = {42,0,0,0, 1,116,1};
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_TELEMETRY, true);
    LS_CHECK(!ls_inspect_telemetry(&t, 10, peer, data, sizeof(data), -90, 4));
    LS_EQ_INT(t.result.field_count, 0);
    LS_EQ_INT(t.result.state, LS_INSPECT_WAITING);
}
LS_CASE(options_and_history_are_bounded)
{
    ls_inspect_init(&t);
    ls_inspect_options(&t, (ls_inspect_options_t){0,0,65535});
    LS_EQ_INT(t.options.timeout_s, 5);
    LS_EQ_INT(t.options.interval_s, 30);
    LS_EQ_INT(t.options.repeat_minutes, 60);
    for (int i = 0; i < 10; i++) {
        LS_EQ_INT(ls_inspect_begin(&t, (uint32_t)i * 30000, i + 1, peer, LS_INSPECT_TRACE, true), LS_INSPECT_WAITING);
        ls_inspect_finish(&t, (uint32_t)i * 30000 + 1, LS_INSPECT_OK);
    }
    LS_EQ_INT(t.history_count, LS_INSPECT_HISTORY);
    LS_EQ_INT(t.history[0].tag, 10);
    LS_EQ_INT(t.history[3].tag, 7);
}
LS_CASE(auto_repeat_defaults_off_and_respects_both_intervals)
{
    trace(0);
    ls_inspect_finish(&t, 1, LS_INSPECT_OK);
    LS_CHECK(!ls_inspect_repeat_due(&t, 3600000));
    ls_inspect_options(&t, (ls_inspect_options_t){30, 120, 1});
    LS_CHECK(!ls_inspect_repeat_due(&t, 60000));
    LS_CHECK(!ls_inspect_repeat_due(&t, 119999));
    LS_CHECK(ls_inspect_repeat_due(&t, 120000));
    ls_inspect_begin(&t, 120000, 43, peer, LS_INSPECT_TRACE, true);
    LS_CHECK(!ls_inspect_repeat_due(&t, 240000));
}

LS_CASE(round_trip_route_keeps_hash_width_and_reverse_leg)
{
    const uint8_t relays[] = {1,2,3,4}, target[] = {5,6};
    const uint8_t expected[] = {1,2,3,4,5,6,3,4,1,2};
    ls_inspect_init(&t);
    LS_CHECK(ls_inspect_route(&t, relays, 2, 2, target));
    LS_EQ_INT(t.result.hop_count, 5);
    LS_EQ_INT(t.result.hash_size, 2);
    LS_CHECK(!memcmp(t.result.hashes, expected, sizeof(expected)));
    LS_CHECK(!ls_inspect_route(&t, relays, 25, 2, target));
    LS_CHECK(!ls_inspect_route(&t, relays, 2, 3, target));
    LS_CHECK(!ls_inspect_route(&t, NULL, 2, 2, target));
    LS_CHECK(ls_inspect_route(&t, NULL, 0, 1, target));
    LS_EQ_INT(t.result.hop_count, 1);
    LS_EQ_INT(t.result.hashes[0], 5);
}
LS_CASE(telemetry_keeps_vectors_and_exact_integer_fields)
{
    uint8_t data[] = {42,0,0,0, 1,133,255,255,255,255,
        2,113,0,100,255,156,0,0};
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_TELEMETRY, true);
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, data, sizeof(data), -90, 4));
    LS_EQ_UINT(t.result.fields[0].raw, UINT32_MAX);
    LS_NEAR(t.result.fields[1].value, 0.1f, 0.0001f);
    LS_NEAR(t.result.fields[1].value2, -0.1f, 0.0001f);
    LS_NEAR(t.result.fields[1].value3, 0, 0.0001f);
    LS_EQ_INT(t.latest[LS_INSPECT_TELEMETRY].field_count, 2);
}
LS_CASE(dispatch_delay_cannot_shorten_rf_cooldown_or_inflate_rtt)
{
    trace(0);
    ls_inspect_transmitted(&t, 29000);
    ls_inspect_finish(&t, 29200, LS_INSPECT_OK);
    LS_EQ_INT(t.result.rtt_ms, 200);
    LS_EQ_INT(ls_inspect_begin(&t, 60000, 43, peer, LS_INSPECT_TRACE, true), LS_INSPECT_REFUSED);
    LS_EQ_INT(ls_inspect_begin(&t, 89000, 43, peer, LS_INSPECT_TRACE, true), LS_INSPECT_WAITING);
}

LS_CASE(unknown_telemetry_does_not_hide_the_decoded_prefix)
{
    uint8_t data[] = {42,0,0,0, 1,116,1,144, 2,240,4,5};
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_TELEMETRY, true);
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, data, sizeof(data), -90, 4));
    LS_EQ_INT(t.result.field_count, 1);
    LS_CHECK(t.result.telemetry_truncated);
    LS_CHECK(t.result.fields[0].value == 4.0f);
}

LS_CASE(repeater_status_is_tagged_bounded_and_little_endian)
{
    uint8_t data[28] = {42,0,0,0, 0xA0,0x0F,0,0, 0,0,0,0,
        12,0,0,0, 5,0,0,0, 3,0,0,0, 0x78,0x56,0x34,0x12};
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_STATUS, true);
    LS_CHECK(!ls_inspect_telemetry(&t, 10, peer, data, 27, -90, 4));
    LS_EQ_INT(t.result.state, LS_INSPECT_WAITING);
    data[0] = 41;
    LS_CHECK(!ls_inspect_telemetry(&t, 10, peer, data, sizeof(data), -90, 4));
    data[0] = 42;
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, data, sizeof(data), -90, 4));
    LS_CHECK(t.result.has_status);
    LS_EQ_UINT(t.result.uptime_s, 0x12345678);
    LS_EQ_INT(t.result.rx_packets, 12);
    LS_EQ_INT(t.result.tx_packets, 5);
    LS_EQ_INT(t.result.airtime_s, 3);
    LS_CHECK(t.result.fields[0].value == 4.0f);
}
/* ---- repeater login and request encoding (MeshCore wire formats) -------- */
LS_CASE(login_payload_is_timestamp_then_password_without_terminator)
{
    uint8_t out[24];
    const uint8_t blank[] = {0x44,0x33,0x22,0x11};
    const uint8_t hello[] = {0x44,0x33,0x22,0x11, 'h','e','l','l','o'};
    LS_EQ_INT(ls_inspect_login_build(out, sizeof(out), 0x11223344, ""), 4);
    LS_CHECK(!memcmp(out, blank, 4));
    LS_EQ_INT(ls_inspect_login_build(out, sizeof(out), 0x11223344, NULL), 4);
    LS_EQ_INT(ls_inspect_login_build(out, sizeof(out), 0x11223344, "hello"), 9);
    LS_CHECK(!memcmp(out, hello, 9));
    LS_EQ_INT(ls_inspect_login_build(out, sizeof(out), 1, "123456789012345"), 19);
    LS_EQ_INT(ls_inspect_login_build(out, sizeof(out), 1, "1234567890123456"), 0);
    LS_EQ_INT(ls_inspect_login_build(out, sizeof(out), 1, "a\x01" "b"), 0);
    LS_EQ_INT(ls_inspect_login_build(out, 8, 1, "hello"), 0);
}
LS_CASE(request_payload_matches_get_status_and_get_telemetry)
{
    uint8_t out[13];
    const uint8_t nonce[4] = {0xAA,0xBB,0xCC,0xDD};
    const uint8_t telem[13] = {0x04,0x03,0x02,0x01, 0x03, 0,0,0,0, 0xAA,0xBB,0xCC,0xDD};
    LS_EQ_INT(ls_inspect_request_build(out, 0x01020304, LS_INSPECT_TELEMETRY, nonce), 13);
    LS_CHECK(!memcmp(out, telem, 13));
    LS_EQ_INT(ls_inspect_request_build(out, 0x01020304, LS_INSPECT_STATUS, nonce), 13);
    LS_EQ_INT(out[4], 1);
    LS_EQ_INT(ls_inspect_request_build(out, 1, LS_INSPECT_TRACE, nonce), 0);
    LS_EQ_INT(ls_inspect_request_build(out, 1, LS_INSPECT_LOGIN, nonce), 0);
}
LS_CASE(guest_login_reply_opens_a_per_node_session)
{
    /* Server clock, RESP_SERVER_LOGIN_OK, keep-alive, is-admin, permissions,
       random blob, firmware level, then cipher padding. */
    uint8_t reply[16] = {0x10,0x20,0x30,0x40, 0, 0, 0, 1, 9,9,9,9, 2, 0,0,0};
    ls_inspect_init(&t);
    LS_CHECK(ls_inspect_needs_login(&t, peer, LS_INSPECT_TELEMETRY, true));
    LS_CHECK(!ls_inspect_needs_login(&t, peer, LS_INSPECT_TELEMETRY, false));
    LS_CHECK(!ls_inspect_needs_login(&t, peer, LS_INSPECT_LOGIN, true));
    LS_CHECK(!ls_inspect_needs_login(&t, peer, LS_INSPECT_TRACE, true));
    LS_EQ_INT(ls_inspect_begin(&t, 100, 77, peer, LS_INSPECT_LOGIN, true), LS_INSPECT_WAITING);
    /* A reply from another node, a failure code and a short reply do not log in. */
    LS_CHECK(!ls_inspect_telemetry(&t, 150, "FEDCBA9876543210", reply, sizeof(reply), -90, 4));
    reply[4] = 1;
    LS_CHECK(!ls_inspect_telemetry(&t, 150, peer, reply, sizeof(reply), -90, 4));
    reply[4] = 0;
    LS_CHECK(!ls_inspect_telemetry(&t, 150, peer, reply, 12, -90, 4));
    LS_EQ_INT(t.result.state, LS_INSPECT_WAITING);
    LS_CHECK(ls_inspect_telemetry(&t, 400, peer, reply, sizeof(reply), -90, 4));
    LS_EQ_INT(t.result.state, LS_INSPECT_OK);
    LS_EQ_INT(t.result.rtt_ms, 300);
    const ls_inspect_session_t *s = ls_inspect_session(&t, peer);
    LS_CHECK(s != NULL);
    LS_CHECK(!s->admin);
    LS_EQ_INT(s->perms, 1);
    LS_EQ_INT(s->fw_level, 2);
    LS_CHECK(!ls_inspect_needs_login(&t, peer, LS_INSPECT_TELEMETRY, true));
    LS_CHECK(ls_inspect_needs_login(&t, "FEDCBA9876543210", LS_INSPECT_STATUS, true));
    /* The login is not kept as a result of its own, and never repeats. */
    LS_EQ_INT(t.latest[LS_INSPECT_TELEMETRY].state, LS_INSPECT_IDLE);
    ls_inspect_options(&t, (ls_inspect_options_t){30, 30, 1});
    LS_CHECK(!ls_inspect_repeat_due(&t, 3600000));
}
LS_CASE(admin_login_and_relogin_replace_the_same_slot)
{
    uint8_t reply[16] = {1,2,3,4, 0, 0, 1, 3, 0,0,0,0, 2, 0,0,0};
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 5, peer, LS_INSPECT_LOGIN, true);
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, reply, sizeof(reply), -90, 4));
    LS_CHECK(ls_inspect_session(&t, peer)->admin);
    reply[6] = 0; reply[7] = 1;
    ls_inspect_begin(&t, 60000, 6, peer, LS_INSPECT_LOGIN, true);
    LS_CHECK(ls_inspect_telemetry(&t, 60010, peer, reply, sizeof(reply), -90, 4));
    LS_CHECK(!ls_inspect_session(&t, peer)->admin);
    int used = 0;
    for (int i = 0; i < LS_INSPECT_SESSIONS; i++) used += t.sessions[i].peer[0] != 0;
    LS_EQ_INT(used, 1);
}
LS_CASE(sessions_are_bounded_and_can_be_dropped)
{
    uint8_t reply[16] = {1,2,3,4, 0, 0, 0, 1, 0,0,0,0, 2, 0,0,0};
    char id[17];
    ls_inspect_init(&t);
    for (int i = 0; i < LS_INSPECT_SESSIONS + 2; i++) {
        snprintf(id, sizeof(id), "00000000000000%02X", i);
        ls_inspect_begin(&t, (uint32_t)i * 60000 + 1, i + 1, id, LS_INSPECT_LOGIN, true);
        LS_CHECK(ls_inspect_telemetry(&t, (uint32_t)i * 60000 + 5, id, reply, sizeof(reply), -90, 4));
    }
    LS_CHECK(ls_inspect_session(&t, "0000000000000005") != NULL);
    LS_CHECK(ls_inspect_session(&t, "0000000000000000") == NULL); /* oldest replaced */
    ls_inspect_session_drop(&t, "0000000000000005");
    LS_CHECK(ls_inspect_session(&t, "0000000000000005") == NULL);
    LS_CHECK(ls_inspect_session(&t, "0000000000000004") != NULL);
    ls_inspect_session_drop(&t, "all");
    LS_CHECK(ls_inspect_session(&t, "0000000000000004") == NULL);
}
LS_CASE(an_unanswered_telemetry_request_ends_the_session)
{
    uint8_t reply[16] = {1,2,3,4, 0, 0, 0, 1, 0,0,0,0, 2, 0,0,0};
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 5, peer, LS_INSPECT_LOGIN, true);
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, reply, sizeof(reply), -90, 4));
    ls_inspect_begin(&t, 60000, 6, peer, LS_INSPECT_TELEMETRY, true);
    ls_inspect_tick(&t, 100000);
    LS_EQ_INT(t.result.state, LS_INSPECT_TIMEOUT);
    LS_CHECK(ls_inspect_session(&t, peer) == NULL);
    /* A login that times out leaves no session either. */
    ls_inspect_begin(&t, 200000, 7, peer, LS_INSPECT_LOGIN, true);
    ls_inspect_tick(&t, 240000);
    LS_EQ_INT(t.result.state, LS_INSPECT_TIMEOUT);
    LS_CHECK(ls_inspect_session(&t, peer) == NULL);
}
LS_CASE(login_reply_is_not_taken_as_telemetry_or_the_reverse)
{
    uint8_t telem[] = {42,0,0,0, 1,116,1,144, 0,0,0,0, 2,0,0,0};
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_TELEMETRY, true);
    telem[4] = 1;
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, telem, 8, -90, 4));
    LS_CHECK(ls_inspect_session(&t, peer) == NULL);
}

/* ---- answering for allowed peers ---------------------------------------- */
static const ls_inspect_self_t me = { .millivolts = 4000, .percent = 87, .uptime_s = 7200,
    .rx_packets = 12, .tx_packets = 5, .airtime_s = 3, .noise_dbm = -110 };
static ls_inspect_allow_t allow;
static const uint8_t req_telem[13] = {42,0,0,0, 3, 0,0,0,0, 1,2,3,4};
LS_CASE(nobody_is_answered_until_allowed)
{
    uint8_t out[96];
    memset(&allow, 0, sizeof(allow));
    LS_EQ_INT(ls_inspect_allow_count(&allow), 0);
    LS_EQ_INT(ls_inspect_answer(&allow, 0, peer, req_telem, 13, &me, -92, 6.5f, out, sizeof(out)), 0);
    LS_CHECK(ls_inspect_allow_set(&allow, peer, true));
    LS_CHECK(ls_inspect_allowed(&allow, peer));
    LS_CHECK(!ls_inspect_allowed(&allow, "FEDCBA9876543210"));
    LS_CHECK(ls_inspect_answer(&allow, 0, peer, req_telem, 13, &me, -92, 6.5f, out, sizeof(out)) > 0);
    LS_CHECK(ls_inspect_allow_set(&allow, peer, false));
    LS_EQ_INT(ls_inspect_answer(&allow, 99999, peer, req_telem, 13, &me, -92, 6.5f, out, sizeof(out)), 0);
    LS_CHECK(!ls_inspect_allow_set(&allow, "not-a-key", true));
    LS_CHECK(!ls_inspect_allow_set(&allow, "0123456789ABCDEG", true));
}
LS_CASE(allow_list_folds_case_and_is_bounded)
{
    char id[17];
    memset(&allow, 0, sizeof(allow));
    LS_CHECK(ls_inspect_allow_set(&allow, "abcdef0123456789", true));
    LS_CHECK(ls_inspect_allowed(&allow, "ABCDEF0123456789"));
    LS_CHECK(ls_inspect_allow_set(&allow, "ABCDEF0123456789", true));
    LS_EQ_INT(ls_inspect_allow_count(&allow), 1);
    for (int i = 1; i < LS_INSPECT_ALLOW_MAX; i++) {
        snprintf(id, sizeof(id), "00000000000000%02X", i);
        LS_CHECK(ls_inspect_allow_set(&allow, id, true));
    }
    LS_CHECK(!ls_inspect_allow_set(&allow, "1111111111111111", true));
    LS_EQ_INT(ls_inspect_allow_count(&allow), LS_INSPECT_ALLOW_MAX);
}
LS_CASE(telemetry_answer_has_the_tag_and_cayenne_fields_without_location)
{
    uint8_t out[96];
    const uint8_t want[] = {42,0,0,0, 1,116,0x01,0x90, 1,120,87, 2,2,0xDC,0x10, 3,2,0x02,0x8A, 4,2,0x00,0xC8};
    memset(&allow, 0, sizeof(allow));
    ls_inspect_allow_set(&allow, peer, true);
    size_t n = ls_inspect_answer(&allow, 0, peer, req_telem, 13, &me, -92, 6.5f, out, sizeof(out));
    LS_EQ_INT(n, sizeof(want));
    LS_CHECK(!memcmp(out, want, sizeof(want)));
    /* The requesting side decodes it. */
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_TELEMETRY, true);
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, out, n, -90, 4));
    LS_EQ_INT(t.result.field_count, 5);
    LS_NEAR(t.result.fields[0].value, 4.0f, 0.001f);
    LS_NEAR(t.result.fields[1].value, 87.0f, 0.001f);
    LS_NEAR(t.result.fields[2].value, -92.0f, 0.001f);
    LS_NEAR(t.result.fields[3].value, 6.5f, 0.001f);
    LS_NEAR(t.result.fields[4].value, 2.0f, 0.001f);
    LS_CHECK(!t.result.telemetry_truncated);
}
LS_CASE(location_is_sent_only_when_the_caller_says_it_is_shared)
{
    uint8_t out[96];
    ls_inspect_self_t loc = me;
    memset(&allow, 0, sizeof(allow));
    ls_inspect_allow_set(&allow, peer, true);
    size_t plain = ls_inspect_answer(&allow, 0, peer, req_telem, 13, &me, -92, 6.5f, out, sizeof(out));
    LS_EQ_INT(plain, 23);
    loc.has_loc = true; loc.lat_e6 = 43458200; loc.lon_e6 = -71651100;
    uint8_t req2[13]; memcpy(req2, req_telem, 13); req2[0] = 43;
    size_t n = ls_inspect_answer(&allow, 60000, peer, req2, 13, &loc, -92, 6.5f, out, sizeof(out));
    LS_EQ_INT(n, plain + 11);
    const uint8_t gps[] = {1,136, 0x06,0xA1,0x96, 0xF5,0x11,0x21, 0,0,0};
    LS_CHECK(!memcmp(out + plain, gps, sizeof(gps)));
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 43, peer, LS_INSPECT_TELEMETRY, true);
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, out, n, -90, 4));
    LS_EQ_INT(t.result.field_count, 6);
    LS_NEAR(t.result.fields[5].value, 43.4582f, 0.0001f);
    LS_NEAR(t.result.fields[5].value2, -71.6511f, 0.0001f);
}
LS_CASE(answers_are_rate_limited_and_repeat_tags_are_ignored)
{
    uint8_t out[96], req[13];
    memcpy(req, req_telem, 13);
    memset(&allow, 0, sizeof(allow));
    ls_inspect_allow_set(&allow, peer, true);
    LS_CHECK(ls_inspect_answer(&allow, 1000, peer, req, 13, &me, -92, 6.5f, out, sizeof(out)) > 0);
    req[0] = 43;
    LS_EQ_INT(ls_inspect_answer(&allow, 1000 + LS_INSPECT_ANSWER_GAP_MS - 1, peer, req, 13, &me, -92, 6.5f, out, sizeof(out)), 0);
    LS_CHECK(ls_inspect_answer(&allow, 1000 + LS_INSPECT_ANSWER_GAP_MS, peer, req, 13, &me, -92, 6.5f, out, sizeof(out)) > 0);
    LS_EQ_INT(ls_inspect_answer(&allow, 100000, peer, req, 13, &me, -92, 6.5f, out, sizeof(out)), 0); /* same tag */
    req[0] = 44; req[4] = 2; /* get access list: not ours */
    LS_EQ_INT(ls_inspect_answer(&allow, 200000, peer, req, 13, &me, -92, 6.5f, out, sizeof(out)), 0);
    LS_EQ_INT(ls_inspect_answer(&allow, 200000, peer, req, 4, &me, -92, 6.5f, out, sizeof(out)), 0);
    req[4] = 3;
    LS_EQ_INT(ls_inspect_answer(&allow, 200000, peer, req, 13, &me, -92, 6.5f, out, 32), 0);
}
LS_CASE(status_answer_is_a_repeater_stats_block_the_inspector_reads)
{
    uint8_t out[96], req[13] = {42,0,0,0, 1, 0,0,0,0, 1,2,3,4};
    memset(&allow, 0, sizeof(allow));
    ls_inspect_allow_set(&allow, peer, true);
    size_t n = ls_inspect_answer(&allow, 0, peer, req, 13, &me, -92, 6.5f, out, sizeof(out));
    LS_EQ_INT(n, 60);
    LS_EQ_INT(out[4], 0xA0); LS_EQ_INT(out[5], 0x0F);        /* millivolts */
    LS_EQ_INT(out[8], 0x92); LS_EQ_INT(out[9], 0xFF);        /* noise -110 */
    LS_EQ_INT(out[10], 0xA4); LS_EQ_INT(out[11], 0xFF);      /* rssi -92 */
    LS_EQ_INT(out[46], 26); LS_EQ_INT(out[47], 0);           /* snr x4 */
    ls_inspect_init(&t);
    ls_inspect_begin(&t, 0, 42, peer, LS_INSPECT_STATUS, true);
    LS_CHECK(ls_inspect_telemetry(&t, 10, peer, out, n, -90, 4));
    LS_EQ_UINT(t.result.uptime_s, 7200);
    LS_EQ_INT(t.result.rx_packets, 12);
    LS_EQ_INT(t.result.tx_packets, 5);
    LS_EQ_INT(t.result.airtime_s, 3);
    LS_NEAR(t.result.fields[0].value, 4.0f, 0.001f);
}

_Static_assert(sizeof(ls_inspect_tracker_t) < 2048, "Inspector snapshots stay bounded");
