/* LS_TEST_SOURCES: ${FW}/main/ls_wifi_sta_core.c */
/* Station-mode helpers, on the host.

   Covers the two bits of ls_wifi.c that are pure logic and worth pinning:

     - credential validation.  A blank SSID or a 7-char passphrase is not
       something the driver will accept, and letting it reach esp_wifi_connect()
       just costs a round-trip and a confusing error.
     - reconnect backoff.  The doubling and the max clamp is the whole reason
       we do not spin against a router that stopped answering. */

#include "ls_test.h"
#include "ls_wifi_sta_core.h"

#include <string.h>

LS_CASE(full_length_ssid_reaches_driver_without_truncation)
{
    uint8_t out[32];
    const char *ssid = "12345678901234567890123456789012";
    ls_wifi_copy_ssid(out, ssid);
    LS_CHECK(memcmp(out, ssid, 32) == 0);
    ls_wifi_copy_ssid(out, "home");
    LS_CHECK(memcmp(out, "home", 4) == 0);
    for (int i = 4; i < 32; ++i) LS_CHECK(out[i] == 0);
}

LS_CASE(connection_failures_are_visible_and_bounded)
{
    char out[128];
    ls_wifi_connecting_status(out, sizeof(out), "home", 202, 4000);
    LS_CHECK(strstr(out, "home") != NULL);
    LS_CHECK(strstr(out, "authentication failed (202)") != NULL);
    LS_CHECK(strstr(out, "4s") != NULL);
    ls_wifi_connecting_status(out, sizeof(out), "home", 0, 1000);
    LS_CHECK(strstr(out, "Connecting") != NULL);
    LS_CHECK(strstr(out, "reason") == NULL);
    char small[4] = { 1, 1, 1, 1 };
    ls_wifi_connecting_status(small, sizeof(small), "home", 202, 1000);
    LS_CHECK(small[3] == 0);
    ls_wifi_connecting_status(NULL, 0, NULL, 0, 0);
}

LS_CASE(reported_disconnect_codes_have_actionable_names)
{
    char out[128];
    ls_wifi_connecting_status(out, sizeof(out), "home", 15, 1000);
    LS_CHECK(strstr(out, "4-way handshake timeout (15)") != NULL);
    ls_wifi_connecting_status(out, sizeof(out), "home", 205, 2000);
    LS_CHECK(strstr(out, "connection failed (205)") != NULL);
    ls_wifi_connecting_status(out, sizeof(out), "home", 8, 4000);
    LS_CHECK(strstr(out, "association left (8)") != NULL);
    ls_wifi_connecting_status(out, sizeof(out), "home", 77, 8000);
    LS_CHECK(strstr(out, "disconnect reason 77") != NULL);
}

LS_CASE(ssid_length_bounds)
{
    LS_CHECK(!ls_wifi_ssid_valid(NULL));
    LS_CHECK(!ls_wifi_ssid_valid(""));

    char one[2] = { 'a', 0 };
    LS_CHECK(ls_wifi_ssid_valid(one));

    char maxlen[LS_WIFI_SSID_MAX_LEN + 1];
    memset(maxlen, 'a', LS_WIFI_SSID_MAX_LEN);
    maxlen[LS_WIFI_SSID_MAX_LEN] = 0;
    LS_CHECK(ls_wifi_ssid_valid(maxlen));

    char toolong[LS_WIFI_SSID_MAX_LEN + 2];
    memset(toolong, 'a', LS_WIFI_SSID_MAX_LEN + 1);
    toolong[LS_WIFI_SSID_MAX_LEN + 1] = 0;
    LS_CHECK(!ls_wifi_ssid_valid(toolong));
}

LS_CASE(ssid_rejects_control_characters)
{

    LS_CHECK(!ls_wifi_ssid_valid("home\r"));
    LS_CHECK(!ls_wifi_ssid_valid("\nhome"));
    LS_CHECK(!ls_wifi_ssid_valid("home\x01net"));
    LS_CHECK(!ls_wifi_ssid_valid("home\x7f"));

    /* Space and printable punctuation are fine - real networks have them. */
    LS_CHECK(ls_wifi_ssid_valid("home 5G"));
    LS_CHECK(ls_wifi_ssid_valid("A-B_C.42"));
}

LS_CASE(pass_open_is_empty)
{
    LS_CHECK(ls_wifi_pass_valid(""));
    LS_CHECK(!ls_wifi_pass_valid(NULL));
}

LS_CASE(pass_wpa2_length_bounds)
{
    char seven[8]; memset(seven, 'a', 7); seven[7] = 0;
    LS_CHECK(!ls_wifi_pass_valid(seven));

    char eight[9]; memset(eight, 'a', 8); eight[8] = 0;
    LS_CHECK(ls_wifi_pass_valid(eight));

    char max[LS_WIFI_PASS_MAX_LEN + 1];
    memset(max, 'a', LS_WIFI_PASS_MAX_LEN);
    max[LS_WIFI_PASS_MAX_LEN] = 0;
    LS_CHECK(ls_wifi_pass_valid(max));

    /* 64 chars is the raw-PMK slot and not something we accept from the
       console. */
    char toolong[LS_WIFI_PASS_MAX_LEN + 2];
    memset(toolong, 'a', LS_WIFI_PASS_MAX_LEN + 1);
    toolong[LS_WIFI_PASS_MAX_LEN + 1] = 0;
    LS_CHECK(!ls_wifi_pass_valid(toolong));
}

LS_CASE(pass_rejects_control_characters)
{
    LS_CHECK(!ls_wifi_pass_valid("abcdefg\n"));
    LS_CHECK(!ls_wifi_pass_valid("abc\rdefg"));
}

LS_CASE(backoff_starts_at_min)
{
    /* The very first failure should ask for the minimum wait, not zero. */
    LS_EQ_UINT(ls_wifi_backoff_next(0, LS_WIFI_BACKOFF_MIN_MS,
                                    LS_WIFI_BACKOFF_MAX_MS),
               LS_WIFI_BACKOFF_MIN_MS);
    LS_EQ_UINT(ls_wifi_backoff_reset(LS_WIFI_BACKOFF_MIN_MS),
               LS_WIFI_BACKOFF_MIN_MS);
}

LS_CASE(backoff_doubles_until_ceiling)
{
    uint32_t v = ls_wifi_backoff_reset(LS_WIFI_BACKOFF_MIN_MS);
    uint32_t prev = 0;
    int at_ceiling = 0;
    for (int i = 0; i < 40; i++) {
        uint32_t next = ls_wifi_backoff_next(v, LS_WIFI_BACKOFF_MIN_MS,
                                             LS_WIFI_BACKOFF_MAX_MS);
        LS_CHECK_MSG(next >= v, "backoff went backwards: %u -> %u",
                     (unsigned)v, (unsigned)next);
        if (next == LS_WIFI_BACKOFF_MAX_MS) at_ceiling = 1;
        LS_CHECK_MSG(next <= LS_WIFI_BACKOFF_MAX_MS,
                     "backoff exceeded ceiling: %u", (unsigned)next);
        prev = v;
        v    = next;
    }
    (void)prev;
    LS_CHECK_MSG(at_ceiling, "backoff never reached the ceiling in 40 tries");
    LS_EQ_UINT(v, LS_WIFI_BACKOFF_MAX_MS);
}

LS_CASE(backoff_overflow_is_clamped)
{
    /* An earlier version of the doubling code overflowed a uint32_t on the
       second-to-last step and returned a value BELOW the ceiling. Pin the
       corner: any value near UINT32_MAX must clamp to max, not wrap. */
    uint32_t v = ls_wifi_backoff_next((uint32_t)0xFFFFFFF0u,
                                      LS_WIFI_BACKOFF_MIN_MS,
                                      LS_WIFI_BACKOFF_MAX_MS);
    LS_EQ_UINT(v, LS_WIFI_BACKOFF_MAX_MS);
}

LS_CASE(backoff_max_below_min_is_tolerated)
{
    /* Callers should not pass max < min, but if they do we cannot loop
       forever. Behave as if max == min. */
    uint32_t v = ls_wifi_backoff_next(500u, 2000u, 1000u);
    LS_EQ_UINT(v, 2000u);
}

LS_CASE(a_network_that_is_not_there_is_not_retried_forever)
{
    /* The backoff clamped the interval but nothing clamped the count, so a
       stored network out of range was retried every sixty seconds for as
       long as the board was on - each attempt waking the co-processor link
       to ask a question already answered. An idle device should settle. */
    LS_CHECK(ls_wifi_should_retry(0, 201));
    LS_CHECK(ls_wifi_should_retry(LS_WIFI_TRIES_NOT_FOUND - 1, 201));
    LS_CHECK_MSG(!ls_wifi_should_retry(LS_WIFI_TRIES_NOT_FOUND, 201),
                 "still retrying after %d attempts at a network that is not "
                 "in range", LS_WIFI_TRIES_NOT_FOUND);

    /* A handshake or auth failure might be a busy AP or a moment of
       interference, so it gets the longer count rather than the short one. */
    LS_CHECK(ls_wifi_should_retry(LS_WIFI_TRIES_NOT_FOUND, 15));
    LS_CHECK(ls_wifi_should_retry(LS_WIFI_TRIES_OTHER - 1, 15));
    LS_CHECK(!ls_wifi_should_retry(LS_WIFI_TRIES_OTHER, 15));
    LS_CHECK_MSG(LS_WIFI_TRIES_NOT_FOUND < LS_WIFI_TRIES_OTHER,
                 "not-found should give up sooner than a transient failure");

    /* Whatever the reason, it stops eventually - a code this build has
       never seen must not mean "retry without end". */
    LS_CHECK_MSG(!ls_wifi_should_retry(LS_WIFI_TRIES_OTHER, 9999),
                 "an unknown reason retried forever");
}
