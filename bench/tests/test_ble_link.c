/* LS_TEST_SOURCES: ${FW}/main/ble_link_core.c */
/* LS_TEST_INCLUDE: ${FW}/main */
/* BLE control-head PROTOCOL, on the host. */

#include "ls_test.h"
#include "ble_link_core.h"

#include <stdint.h>
#include <string.h>

/* --------------------------------------------------------------- helpers */

typedef struct {
    char lines[8][BLE_LINK_LINE_MAX];
    int  count;
} captured_t;

static void capture_cb(const char *line, void *user)
{
    captured_t *c = (captured_t *)user;
    if (c->count < (int)(sizeof(c->lines) / sizeof(c->lines[0]))) {
        strncpy(c->lines[c->count], line, sizeof(c->lines[0]) - 1);
        c->lines[c->count][sizeof(c->lines[0]) - 1] = '\0';
        c->count++;
    }
}

static void feed_str(ble_link_rx_t *rx, const char *s, captured_t *cap)
{
    ble_link_rx_feed(rx, (const uint8_t *)s, (int)strlen(s), capture_cb, cap);
}

/* --------------------------------------------------------------- framing */

LS_CASE(rx_whole_frame_in_one_call)
{
    ble_link_rx_t rx;
    ble_link_rx_reset(&rx);
    captured_t cap = { 0 };

    feed_str(&rx, "PING\n", &cap);
    LS_EQ_INT(cap.count, 1);
    LS_EQ_STR(cap.lines[0], "PING");
    LS_EQ_UINT(rx.lines, 1u);
    LS_EQ_UINT(rx.drops, 0u);
}

LS_CASE(rx_split_across_two_notifications)
{
    /* counts BOTH the notify and the bytes it carries, and the head
       coalesces small replies into short GATT notifications. A payload
       split across two of them ("PIN" then "G\n") must reassemble to one
       line. That is the whole point of a byte-stream reader on top of a
       notify-oriented transport. */
    ble_link_rx_t rx;
    ble_link_rx_reset(&rx);
    captured_t cap = { 0 };

    feed_str(&rx, "PIN", &cap);
    LS_EQ_INT(cap.count, 0);
    LS_EQ_UINT(rx.lines, 0u);

    feed_str(&rx, "G\n", &cap);
    LS_EQ_INT(cap.count, 1);
    LS_EQ_STR(cap.lines[0], "PING");
    LS_EQ_UINT(rx.lines, 1u);
    LS_EQ_UINT(rx.drops, 0u);
}

LS_CASE(rx_byte_at_a_time)
{
    /* An extreme fragmentation - one byte per feed - must still deliver the
       full line exactly once, when (and only when) the delimiter arrives. */
    ble_link_rx_t rx;
    ble_link_rx_reset(&rx);
    captured_t cap = { 0 };

    const char *s = "STAT\n";
    for (int i = 0; s[i]; i++) {
        uint8_t b = (uint8_t)s[i];
        ble_link_rx_feed(&rx, &b, 1, capture_cb, &cap);
    }
    LS_EQ_INT(cap.count, 1);
    LS_EQ_STR(cap.lines[0], "STAT");
    LS_EQ_UINT(rx.lines, 1u);
}

LS_CASE(rx_truncated_frame_is_not_emitted)
{
    /* A frame without a delimiter is held in the buffer, not delivered.
       Anything the transport does about "wait for more or time out" is its
       own concern - the reader must not invent a line. */
    ble_link_rx_t rx;
    ble_link_rx_reset(&rx);
    captured_t cap = { 0 };

    feed_str(&rx, "PARTIAL", &cap);
    LS_EQ_INT(cap.count, 0);
    LS_EQ_UINT(rx.lines, 0u);
    LS_EQ_UINT(rx.drops, 0u);
    LS_EQ_INT(rx.pos, 7);
}

LS_CASE(rx_bad_length_command_is_dropped)
{
    /* Bad length = a command line longer than the receiver's buffer.
       The overlong bytes drop per-byte-over-limit (matching the current
       on-device feed_rx exactly), a delimiter resynchronises the reader,
       and the NEXT well-formed line arrives cleanly. */
    ble_link_rx_t rx;
    ble_link_rx_reset(&rx);
    captured_t cap = { 0 };

    char big[BLE_LINK_LINE_MAX * 2];
    for (size_t i = 0; i < sizeof(big) - 2; i++) big[i] = 'X';
    big[sizeof(big) - 2] = '\n';
    big[sizeof(big) - 1] = '\0';

    feed_str(&rx, big, &cap);
    LS_CHECK_MSG(rx.drops > 0, "overlong line produced no drop");

    /* After the delimiter the reader is resynchronised. */
    feed_str(&rx, "OK\n", &cap);
    LS_CHECK_MSG(cap.count >= 1, "reader did not resynchronise after overlong");
    LS_EQ_STR(cap.lines[cap.count - 1], "OK");
}

LS_CASE(rx_empty_lines_are_skipped)
{
    /* CR/LF, "\n\n" and other empty-line sequences must not emit spurious
       empty commands - the head sends CRLF at times and flipper_link would
       not know what to do with a zero-arg command. */
    ble_link_rx_t rx;
    ble_link_rx_reset(&rx);
    captured_t cap = { 0 };

    feed_str(&rx, "\r\n\r\nA\r\n\n", &cap);
    LS_EQ_INT(cap.count, 1);
    LS_EQ_STR(cap.lines[0], "A");
}

/* --------------------------------------------- telemetry encode/decode round-trip */

LS_CASE(hello_line_round_trips_through_feeder)
{
    /* The one line ble_link.c itself formats is the HELLO greeting after
       CCCD subscribe. Encode it, then feed the bytes back through the RX
       reassembler - the byte pipeline must deliver the same line intact.
       That is the closest thing to an encode/decode round-trip this link
       protocol has, and it pins the framing contract in both directions. */
    char line[128];
    int n = ble_link_format_hello(line, sizeof(line),
                                  BLE_LINK_HELLO_PROTO_VERSION, "v1.2.3");
    LS_CHECK(n > 0);
    LS_CHECK(line[n - 1] == '\n');

    ble_link_rx_t rx;
    ble_link_rx_reset(&rx);
    captured_t cap = { 0 };
    ble_link_rx_feed(&rx, (const uint8_t *)line, n, capture_cb, &cap);

    LS_EQ_INT(cap.count, 1);
    /* The reassembler strips the delimiter, so the recovered line matches
       the formatted one minus its trailing '\n'. */
    char expected[128];
    strncpy(expected, line, sizeof(expected));
    expected[n - 1] = '\0';
    LS_EQ_STR(cap.lines[0], expected);
}

LS_CASE(hello_line_sanitises_version_field)
{
    /* HELLO's version field is space-separated from the rest of the frame,
       so a raw ls_version_line() that contains a space would corrupt the
       greeting. ble_link.c calls ble_link_sanitize_field() first; the pin
       here is that sanitize actually replaces the offending bytes. */
    char v[] = "v1.2.3 dirty\ttag=x";
    ble_link_sanitize_field(v);
    LS_EQ_STR(v, "v1.2.3_dirty_tag_x");

    char line[128];
    int n = ble_link_format_hello(line, sizeof(line), 3, v);
    LS_CHECK(n > 0);
    LS_CHECK_MSG(strstr(line, "v1.2.3_dirty_tag_x") != NULL,
                 "sanitized version missing from HELLO: [%s]", line);
    LS_CHECK_MSG(line[n - 1] == '\n', "HELLO did not end in newline: [%s]", line);
}

/* --------------------------------------------------------------- payload */

LS_CASE(payload_cap_uses_min_when_mtu_unknown)
{

    LS_EQ_INT(ble_link_payload_cap(0), BLE_LINK_MIN_PAYLOAD);
    LS_EQ_INT(ble_link_payload_cap(20), BLE_LINK_MIN_PAYLOAD);
    LS_EQ_INT(ble_link_payload_cap(23), BLE_LINK_MIN_PAYLOAD);
}

LS_CASE(payload_cap_pays_att_header)
{
    /* MTU-3 is the transport's limit; the head's RX characteristic is
       244 bytes wide and is the tighter of the two, so it wins.  A write past
       it is discarded by the head's GATT server without a reply - and these
       are writes WITHOUT response, so nothing comes back and this end counts
       a success.  A telemetry frame then arrived at the head as a 253-byte
       chunk that never landed plus a short remainder that did, which the app
       logged as "rx junk" and rendered as NO SDR. */
    LS_EQ_INT(ble_link_payload_cap(256), BLE_LINK_HEAD_ATT_MAX);
    LS_EQ_INT(ble_link_payload_cap(517), BLE_LINK_HEAD_ATT_MAX);
    LS_EQ_INT(BLE_LINK_HEAD_ATT_MAX, 244);

    /* Below the clamp the ATT header is still paid, so a small MTU is not
       silently rounded up to something the link cannot carry. */
    LS_EQ_INT(ble_link_payload_cap(100), 97);
    LS_EQ_INT(ble_link_payload_cap(247), BLE_LINK_HEAD_ATT_MAX);
    LS_EQ_INT(ble_link_payload_cap(246), 243);
}

/* --------------------------------------------------------------- backoff */

LS_CASE(backoff_starts_at_min)
{
    LS_EQ_UINT(ble_link_backoff_next(0), BLE_LINK_RECONNECT_BACKOFF_MS);
}

LS_CASE(backoff_doubles_to_ceiling)
{
    uint32_t v = BLE_LINK_RECONNECT_BACKOFF_MS;
    int at_ceiling = 0;
    for (int i = 0; i < 20; i++) {
        uint32_t next = ble_link_backoff_next(v);
        LS_CHECK_MSG(next >= v, "backoff went backwards: %u -> %u",
                     (unsigned)v, (unsigned)next);
        LS_CHECK_MSG(next <= BLE_LINK_RECONNECT_BACKOFF_MAX_MS,
                     "backoff exceeded ceiling: %u", (unsigned)next);
        if (next == BLE_LINK_RECONNECT_BACKOFF_MAX_MS) at_ceiling = 1;
        v = next;
    }
    LS_CHECK_MSG(at_ceiling, "backoff never reached the ceiling in 20 tries");
}

LS_CASE(backoff_overflow_is_clamped)
{
    LS_EQ_UINT(ble_link_backoff_next(0xFFFFFFF0u),
               BLE_LINK_RECONNECT_BACKOFF_MAX_MS);
}

/* ---------------------------------------------- disconnect class */

LS_CASE(disc_ls714_authreq_refusal_keeps_bond)
{
    /* AUTH_FAIL with a prior AUTHREQ enc_change means the head just
       does not want to pair right now - keep the stored bond so the next
       real attempt does not have to re-pass the passkey. Deleting the bond
       on this path (as the pre-code did) forced a fresh passkey
       every retry. */
    ble_link_disc_class_t c =
        ble_link_classify_disc(true, BLE_LINK_ENC_AUTHREQ_REFUSED);
    LS_EQ_INT(c, BLE_LINK_DISC_AUTHREQ_REFUSAL);
}

LS_CASE(disc_ls714_key_reject_drops_bond)
{
    /* AUTH_FAIL with NO enc_change context - or with the more
       specific KEY_REJECTED kind - is the "stored keys are bad" case,
       which is the one that legitimately wants the bond forgotten. */
    LS_EQ_INT(ble_link_classify_disc(true, BLE_LINK_ENC_NONE),
              BLE_LINK_DISC_KEY_REJECT);
    LS_EQ_INT(ble_link_classify_disc(true, BLE_LINK_ENC_KEY_REJECTED),
              BLE_LINK_DISC_KEY_REJECT);
    LS_EQ_INT(ble_link_classify_disc(true, BLE_LINK_ENC_OTHER_FAIL),
              BLE_LINK_DISC_KEY_REJECT);
}

LS_CASE(disc_normal_reason_is_unrelated)
{
    /* A plain disconnect (peer went away, we terminated, etc.) must not
       be classified as auth-related regardless of the last enc kind. */
    LS_EQ_INT(ble_link_classify_disc(false, BLE_LINK_ENC_NONE),
              BLE_LINK_DISC_UNRELATED);
    LS_EQ_INT(ble_link_classify_disc(false, BLE_LINK_ENC_OK),
              BLE_LINK_DISC_UNRELATED);
    LS_EQ_INT(ble_link_classify_disc(false, BLE_LINK_ENC_TIMEOUT),
              BLE_LINK_DISC_UNRELATED);
}

LS_CASE(enc_kind_key_rejected_wipes_bond)
{
    /* At ENC_CHANGE time - before any disconnect - KEY_REJECTED means the
       stored keys are the problem.

       LS-980  AUTHREQ_REFUSED now wipes too, and this case used to assert the
       opposite. That was not a weakened test, it was a changed contract, so the
       reason is here rather than in a commit nobody will read:

       The head sets pairing_method = GapPairingNone and both characteristics
       are ATTR_PERMISSION_NONE. It will never complete pairing. Any security
       state we hold for it - written by older firmware that asked for bonding -
       is therefore unusable by construction, and keeping it only guarantees the
       same rejection on the next connection. Observed as an endless reconnect
       loop on two boards. */
    LS_CHECK(ble_link_enc_kind_wipes_bond(BLE_LINK_ENC_KEY_REJECTED));
    LS_CHECK(ble_link_enc_kind_wipes_bond(BLE_LINK_ENC_AUTHREQ_REFUSED));
    LS_CHECK(!ble_link_enc_kind_wipes_bond(BLE_LINK_ENC_TIMEOUT));
    LS_CHECK(!ble_link_enc_kind_wipes_bond(BLE_LINK_ENC_OK));
    LS_CHECK(!ble_link_enc_kind_wipes_bond(BLE_LINK_ENC_NONE));
}

LS_CASE(only_a_passkey_timeout_justifies_dropping_the_link)
{
    /* Discovery finishes before any of this () and the
       characteristics need no encryption, so a link that failed to encrypt is
       still a working link. Tearing it down on AUTHREQ is what turned "we asked
       for something this head does not offer" into a loop that never ended.

       A timeout is different: it means the head is waiting for a human to enter
       a passkey, and nobody is coming. */
    LS_CHECK(ble_link_enc_kind_needs_teardown(BLE_LINK_ENC_TIMEOUT));

    LS_CHECK(!ble_link_enc_kind_needs_teardown(BLE_LINK_ENC_AUTHREQ_REFUSED));
    LS_CHECK(!ble_link_enc_kind_needs_teardown(BLE_LINK_ENC_KEY_REJECTED));
    LS_CHECK(!ble_link_enc_kind_needs_teardown(BLE_LINK_ENC_OTHER_FAIL));
    LS_CHECK(!ble_link_enc_kind_needs_teardown(BLE_LINK_ENC_OK));
    LS_CHECK(!ble_link_enc_kind_needs_teardown(BLE_LINK_ENC_NONE));
}

/* ---------------------------------------------- state machine walk */

LS_CASE(state_step_cold_start_reaches_ready)
{
    /* Cold start: OFF -> SYNCING -> SCANNING -> CONNECTING -> DISCOVERING
       -> READY. Every state in the graph must be visited on the way. */
    ble_link_state_t s = BLE_LINK_OFF;
    int seen[6] = { 0 };
    seen[s]++;

    s = ble_link_state_step(s, BLE_LINK_EV_START_COLD);
    LS_EQ_INT(s, BLE_LINK_SYNCING); seen[s]++;

    s = ble_link_state_step(s, BLE_LINK_EV_SYNC);
    LS_EQ_INT(s, BLE_LINK_SCANNING); seen[s]++;

    s = ble_link_state_step(s, BLE_LINK_EV_SCAN_MATCH);
    LS_EQ_INT(s, BLE_LINK_CONNECTING); seen[s]++;

    s = ble_link_state_step(s, BLE_LINK_EV_CONNECT_OK);
    LS_EQ_INT(s, BLE_LINK_DISCOVERING); seen[s]++;

    s = ble_link_state_step(s, BLE_LINK_EV_SUBSCRIBED);
    LS_EQ_INT(s, BLE_LINK_READY); seen[s]++;

    /* All six states reached at least once. */
    for (int i = 0; i < 6; i++) {
        LS_CHECK_MSG(seen[i] > 0, "state %d never visited on the cold path", i);
    }
}

LS_CASE(state_step_warm_start_skips_syncing)
{
    /* Warm start (stack was already up): OFF -> SCANNING. */
    ble_link_state_t s = ble_link_state_step(BLE_LINK_OFF, BLE_LINK_EV_START_UP);
    LS_EQ_INT(s, BLE_LINK_SCANNING);
}

LS_CASE(state_step_connect_fail_returns_to_scan)
{
    /* a failed connect drops back to SCANNING (the transport also
       schedules a backoff via ble_link_backoff_next). */
    ble_link_state_t s = BLE_LINK_CONNECTING;
    s = ble_link_state_step(s, BLE_LINK_EV_CONNECT_FAIL);
    LS_EQ_INT(s, BLE_LINK_SCANNING);
}

LS_CASE(state_step_disconnect_running_scans_again)
{
    /* /both hit here: a disconnect at any post-connect state,
       while s_run is true, MUST return to SCANNING so the link reconnects
       automatically. Before those tickets, some paths sat at READY forever
       or fell into a passkey wait that never fired again. */
    ble_link_state_t states[] = {
        BLE_LINK_CONNECTING, BLE_LINK_DISCOVERING, BLE_LINK_READY,
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        ble_link_state_t s = ble_link_state_step(states[i],
                                                 BLE_LINK_EV_DISCONNECT_RUNNING);
        LS_CHECK_MSG(s == BLE_LINK_SCANNING,
                     "disconnect from state %d landed at %d, not SCANNING",
                     (int)states[i], (int)s);
    }
}

LS_CASE(state_step_disconnect_stopping_is_off)
{
    /* If ble_link_stop was requested during the connection, the disconnect
       arrives while s_run is false and the link must fall to OFF, not
       reschedule a scan. */
    ble_link_state_t s = ble_link_state_step(BLE_LINK_READY,
                                             BLE_LINK_EV_DISCONNECT_STOPPING);
    LS_EQ_INT(s, BLE_LINK_OFF);
}

LS_CASE(state_step_explicit_stop_is_off)
{
    /* ble_link_stop() from any state ends up OFF. */
    ble_link_state_t states[] = {
        BLE_LINK_SYNCING, BLE_LINK_SCANNING, BLE_LINK_CONNECTING,
        BLE_LINK_DISCOVERING, BLE_LINK_READY,
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        ble_link_state_t s = ble_link_state_step(states[i], BLE_LINK_EV_STOP);
        LS_CHECK_MSG(s == BLE_LINK_OFF,
                     "stop from state %d landed at %d, not OFF",
                     (int)states[i], (int)s);
    }
}

/* ---------------------------------------------- / / */

LS_CASE(ls822_default_conn_params_carry_our_supervision_timeout)
{
    /* connect must supply supervision_timeout=400 (4 s) - NimBLE's
       default is 256 (2.56 s), which killed every pairing attempt against
       the Flipper on the timeout, presenting as "pairing failed status=7 /
       disconnected reason=520". If a future edit passes NULL to
       ble_gap_connect or changes this number, this test fails and the same
       149 pairing failures come back. */
    ble_link_conn_params_t cp;
    ble_link_default_conn_params(&cp);
    LS_EQ_UINT(cp.supervision_timeout, 400u);
    LS_EQ_UINT(cp.itvl_min, 24u);
    LS_EQ_UINT(cp.itvl_max, 48u);
    LS_EQ_UINT(cp.latency, 0u);
}

LS_CASE(ls823_never_initiate_security_on_connect)
{
    /* the head does not pair - it is a Flipper. If ble_link.c ever
       calls ble_gap_security_initiate() from BLE_GAP_EVENT_CONNECT again,
       the link will sit in "connected, pairing..." until the supervision
       timer drops it. This function must stay `return false;`. */
    LS_CHECK(!ble_link_ls823_should_pair_on_connect());
}

LS_CASE(ls824_never_request_conn_update_after_subscribe)
{
    /* the head does not answer a CONN_UPDATE request and the LL
       response timer drops the link at exactly 40 s. already set
       the correct parameters at connect time; there is nothing to
       renegotiate. This function must stay `return false;`. */
    LS_CHECK(!ble_link_ls824_should_request_conn_update());
}

/* ---------------------------------------------- passkey early hold */

LS_CASE(pk_hold_take_once)
{
    /* A `ble pin <code>` typed before the PASSKEY_ACTION event arrives is
       stashed. When the event does arrive the transport takes the code and
       the hold is cleared - a second take without a fresh set fails. */
    ble_link_pk_hold_t h;
    ble_link_pk_hold_reset(&h);

    uint32_t out = 0;
    LS_CHECK(!ble_link_pk_hold_take(&h, &out));

    ble_link_pk_hold_set(&h, 123456u);
    LS_CHECK(ble_link_pk_hold_take(&h, &out));
    LS_EQ_UINT(out, 123456u);

    /* Second take without a fresh set returns false; the code is consumed. */
    LS_CHECK(!ble_link_pk_hold_take(&h, &out));
}

/* ---------------------------------------------- pinned peer */

static ble_link_peer_addr_t make_addr(uint8_t b0, uint8_t b1, uint8_t b2,
                                      uint8_t b3, uint8_t b4, uint8_t b5,
                                      uint8_t type)
{
    ble_link_peer_addr_t a = { .type = type, .valid = 1 };
    a.val[0] = b0; a.val[1] = b1; a.val[2] = b2;
    a.val[3] = b3; a.val[4] = b4; a.val[5] = b5;
    return a;
}

LS_CASE(peer_addr_equal_byte_by_byte)
{
    ble_link_peer_addr_t a = make_addr(1, 2, 3, 4, 5, 6, 0);
    ble_link_peer_addr_t b = make_addr(1, 2, 3, 4, 5, 6, 0);
    LS_CHECK(ble_link_peer_addr_equal(&a, &b));

    /* One byte off is not equal. */
    b.val[3] = 0x99;
    LS_CHECK(!ble_link_peer_addr_equal(&a, &b));

    /* Same bytes, different type is not equal - a public and a random
       address that happen to share bytes are different peers. */
    ble_link_peer_addr_t c = make_addr(1, 2, 3, 4, 5, 6, 1);
    LS_CHECK(!ble_link_peer_addr_equal(&a, &c));

    /* NULL handling: two NULLs match, one NULL does not. */
    LS_CHECK(ble_link_peer_addr_equal(NULL, NULL));
    LS_CHECK(!ble_link_peer_addr_equal(&a, NULL));
    LS_CHECK(!ble_link_peer_addr_equal(NULL, &a));
}

LS_CASE(peer_addr_format_big_endian_matches_addr_str)
{
    /* addr_str() in ble_link.c prints val[] in big-endian
       ("val[5]:val[4]:...:val[0]"), so ble_link_peer_addr_format has to
       match or `ble show` and the transport's log line disagree on the same
       peer.  A subtle mismatch would look like the pin never applied. */
    ble_link_peer_addr_t a = make_addr(0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0);
    char s[24];
    ble_link_peer_addr_format(&a, s, sizeof(s));
    LS_EQ_STR(s, "45:67:89:ab:cd:ef");

    /* NULL / !valid renders "-", never an empty string - the console shows
       the field even when nothing is pinned yet. */
    ble_link_peer_addr_t none = { .valid = 0 };
    ble_link_peer_addr_format(&none, s, sizeof(s));
    LS_EQ_STR(s, "-");
    ble_link_peer_addr_format(NULL, s, sizeof(s));
    LS_EQ_STR(s, "-");
}

LS_CASE(scan_decide_no_pin_needs_the_service)
{

    ble_link_peer_addr_t adv = make_addr(1, 2, 3, 4, 5, 6, 0);

    LS_EQ_INT(ble_link_scan_decide(true,  true,  &adv, NULL),
              BLE_LINK_SCAN_CONNECT);
    LS_EQ_INT(ble_link_scan_decide(true,  false, &adv, NULL),
              BLE_LINK_SCAN_SKIP);
    LS_EQ_INT(ble_link_scan_decide(false, false, &adv, NULL),
              BLE_LINK_SCAN_SKIP);

    /* An invalid pin (valid=0) reads the same as no pin. */
    ble_link_peer_addr_t empty = { .valid = 0 };
    LS_EQ_INT(ble_link_scan_decide(true, true, &adv, &empty),
              BLE_LINK_SCAN_CONNECT);
}

LS_CASE(scan_decide_name_alone_is_never_enough)
{
    /* The regression this exists to stop. */

    ble_link_peer_addr_t stock = make_addr(0x80, 0xe1, 0x26, 0x1b, 0x5e, 0x47, 0);

    LS_EQ_INT(ble_link_scan_decide(false, true, &stock, NULL),
              BLE_LINK_SCAN_SKIP);

    ble_link_peer_addr_t none = { .valid = 0 };
    LS_EQ_INT(ble_link_scan_decide(false, true, &stock, &none),
              BLE_LINK_SCAN_SKIP);
}

LS_CASE(scan_decide_pin_wins_over_unknown_same_service)
{
    /* The whole point.  Both a pinned peer and an unknown peer are
       advertising our service - only the pinned one gets a connect.

       Before this, adv_name_matches() returned true for the first candidate
       carrying the service UUID, so with a nano and an LCD board both
       powered up they raced to ble_gap_connect() and one always lost.
       The pin is what breaks the tie by address. */
    ble_link_peer_addr_t pinned  = make_addr(0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01, 0);
    ble_link_peer_addr_t unknown = make_addr(0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x02, 0);

    LS_EQ_INT(ble_link_scan_decide(true, false, &pinned,  &pinned),
              BLE_LINK_SCAN_CONNECT);
    LS_EQ_INT(ble_link_scan_decide(true, false, &unknown, &pinned),
              BLE_LINK_SCAN_SKIP);

    /* Name-only match against something advertising nothing we recognise -
       still SKIP, because with a pin we require the service too.  A Flipper
       with a stale app closed can still advertise "Lr1cher"; if it is not
       the pinned peer we do not touch it. */
    ble_link_peer_addr_t unknown_name = make_addr(0xde, 0xad, 0xbe, 0xef, 0, 0, 0);
    LS_EQ_INT(ble_link_scan_decide(false, true, &unknown_name, &pinned),
              BLE_LINK_SCAN_SKIP);
}

LS_CASE(scan_decide_pin_requires_the_service_too)
{
    /* An advertiser whose address matches the pin but does NOT carry our
       service is skipped.  Some other device happening to sit on the same
       MAC (or a stale scan-response from the head with the service UUID
       trimmed for space) must not trigger a connect just because the address
       matches; a connect that immediately failed discovery would loop us
       through the backoff for no gain. */
    ble_link_peer_addr_t pinned = make_addr(0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01, 0);
    LS_EQ_INT(ble_link_scan_decide(false, true, &pinned, &pinned),
              BLE_LINK_SCAN_SKIP);
}

LS_CASE(identity_field_is_present_in_telemetry_suffix)
{
    /* Two boards indistinguishable on the head is exactly the failure
       this ticket is about.  flipper_link_telemetry.c splices this helper
       into the tail of every telemetry frame, so a field asserted here means
       the field is asserted for FM, ADS-B, P25 and REC without four almost
       identical struct-passing tests.

       If a future edit drops this call, the bd= assertion fails and the
       app is back to seeing two identical "LakeShark" boards. */
    char out[32];
    int n = ble_link_format_identity(out, sizeof(out), "ESP32-P4-NANO");
    LS_CHECK_MSG(n > 0, "identity helper produced nothing");
    LS_CHECK_MSG(strstr(out, " bd=ESP32-P4-NANO") == out,
                 "identity did not lead with ' bd=<board>': [%s]", out);

    n = ble_link_format_identity(out, sizeof(out), NULL);
    LS_CHECK_MSG(n > 0, "identity refused a NULL board name");
    LS_CHECK_MSG(strstr(out, " bd=?") == out,
                 "identity did not fall back to '?': [%s]", out);
}

LS_CASE(scan_decide_pin_absent_peer_keeps_scanning_forever)
{

    ble_link_peer_addr_t pinned = make_addr(0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01, 0);
    ble_link_peer_addr_t other  = make_addr(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0);

    LS_EQ_INT(ble_link_scan_decide(true,  false, &other,      &pinned),
              BLE_LINK_SCAN_SKIP);
    LS_EQ_INT(ble_link_scan_decide(true,  true,  &other,      &pinned),
              BLE_LINK_SCAN_SKIP);
    LS_EQ_INT(ble_link_scan_decide(false, false, NULL,        &pinned),
              BLE_LINK_SCAN_SKIP);
}

/* a tight DMA heap must shorten the write, not kill the frame.
 *
 * Measured on the LCD with P25, the panel and USB all running: the largest
 * free DMA-capable block sat at 768 B while one full-size write asked for
 * 845 B (253 payload + 16 header + 64 alignment slack + 512 margin). Short by
 * 77 B, nearly all of it margin - and the old guard threw the whole frame
 * away, so the head sat there connected and showing nothing. */
LS_CASE(tx_chunk_shortens_the_write_before_it_drops_the_frame)
{
    const size_t overhead = 16 + 64 + 512;   /* header + slack + margin */

    /* The exact numbers off the device: 253 wanted, 768 available. */
    LS_EQ_INT(768 - (int)overhead, ble_link_tx_chunk(768, overhead, 253));
    LS_CHECK(ble_link_tx_chunk(768, overhead, 253) > 0);

    /* Plenty of room: the full write is used, never more than asked. */
    LS_EQ_INT(253, ble_link_tx_chunk(64 * 1024, overhead, 253));
    LS_EQ_INT(20, ble_link_tx_chunk(64 * 1024, overhead, 20));

    /* Exactly enough for the minimum payload is still worth sending. */
    LS_EQ_INT(BLE_LINK_MIN_PAYLOAD,
              ble_link_tx_chunk(overhead + BLE_LINK_MIN_PAYLOAD, overhead, 253));

    /* One byte below the minimum, and there is nothing useful to send. */
    LS_EQ_INT(0, ble_link_tx_chunk(overhead + BLE_LINK_MIN_PAYLOAD - 1,
                                   overhead, 253));

    /* Degenerate inputs must not underflow the unsigned subtraction. */
    LS_EQ_INT(0, ble_link_tx_chunk(overhead, overhead, 253));
    LS_EQ_INT(0, ble_link_tx_chunk(0, overhead, 253));
    LS_EQ_INT(0, ble_link_tx_chunk(64 * 1024, overhead, 0));
    LS_EQ_INT(0, ble_link_tx_chunk(64 * 1024, overhead, -1));
}
