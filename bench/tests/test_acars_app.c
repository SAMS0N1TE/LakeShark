/* LS_TEST_SOURCES: ${APP}/acars/acars_app.c ${APP}/acars/acars.c ${APP}/acars/acars_msk.c */

#include "ls_test.h"
#include "acars.h"
#include "acars_app.h"

#include <string.h>

static void expect_flight(const char *text, const char *want)
{
    char out[16];
    (void)acars_flight_from_text(text, out, sizeof(out));
    LS_CHECK_MSG(strcmp(out, want) == 0,
        "flight_from_text(\"%s\") = \"%s\", want \"%s\"", text, out, want);
}

LS_CASE(flight_from_text_extracts_common_airline_forms)
{
    /* Two-letter airline + three digits: the most common form on the wire. */
    expect_flight("BA123 POSITION REPORT",    "BA123");
    /* Two-letter + four digits. */
    expect_flight("DL4471 OFF",               "DL4471");
    /* Three-letter + digits. */
    expect_flight("SWA1234 OUT",              "SWA1234");
    /* Trailing letter suffix (leg identifier). */
    expect_flight("UA857A/CGN",               "UA857A");
    /* Leading whitespace is skipped; leading punctuation is not - showing
       "-" is safer than guessing which prefix bytes are noise. */
    expect_flight("   AA1234 DEP",            "AA1234");
    expect_flight("\tKL3021 GATE",            "KL3021");
}

LS_CASE(flight_from_text_rejects_non_flight_prefixes)
{
    /* Only-letter, only-digit, or too-short shapes are not flight IDs. */
    expect_flight("HELLO WORLD",              "");
    expect_flight("123456",                   "");
    expect_flight("A123",                     "");     /* one letter */
    expect_flight("AB1",                      "");     /* one digit  */
    /* Punctuation-prefixed forms are deliberately not decoded: without a
       sublabel table we cannot tell "#DFB" from a bogus three-letter
       airline, so both go to "". */
    expect_flight("#DFB123 DATA",             "");
    expect_flight("/AA1234 DEP",              "");
    /* Empty / null. */
    expect_flight("",                         "");
    char buf[8];
    LS_EQ_UINT(acars_flight_from_text(NULL, buf, sizeof(buf)), 0u);
}

LS_CASE(flight_from_text_stops_at_first_break)
{
    /* Once the shape breaks (space, punctuation, extra letter after the
       digits), the extractor must not fish further into the string. */
    expect_flight("AA123 BB456",              "AA123");
    expect_flight("AA123.OFF",                "AA123");
    /* A second letter after an already-taken suffix must not extend the
       token, and neither must a digit. */
    expect_flight("AA123AB",                  "AA123");
    expect_flight("AA123A5",                  "AA123");
}

LS_CASE(flight_from_text_bounds_check)
{
    /* Buffer must never be overrun and must always be NUL-terminated. */
    char tiny[4];
    memset(tiny, 0x55, sizeof(tiny));
    size_t n = acars_flight_from_text("SWA1234 X", tiny, sizeof(tiny));
    LS_CHECK_MSG(tiny[sizeof(tiny) - 1] == '\0',
        "output not NUL-terminated (got 0x%02x)", (unsigned)tiny[sizeof(tiny) - 1]);
    LS_CHECK_MSG(n < sizeof(tiny), "wrote %zu into %zu-byte buffer", n, sizeof(tiny));
}

LS_CASE(inject_and_state_visible_via_accessor)
{

    acars_app_clear();
    const acars_state_t *s = acars_app_state();
    LS_CHECK(s != NULL);
    LS_EQ_INT(s->msg_count, 0);

    acars_app_inject(".N12345", "H1", "UA857 POS REPORT");
    LS_EQ_INT(s->msg_count, 1);

    int head = (s->msg_head - 1 + ACARS_MSG_LOG_MAX) % ACARS_MSG_LOG_MAX;
    LS_EQ_STR(s->msgs[head].reg,   ".N12345");
    LS_EQ_STR(s->msgs[head].label, "H1");
    LS_EQ_STR(s->msgs[head].text,  "UA857 POS REPORT");
    LS_CHECK(s->msgs[head].crc_ok);

    /* Ring: rolling past the cap keeps count pinned at ACARS_MSG_LOG_MAX and
       advances head so the newest is still findable at head-1. */
    for (int i = 0; i < ACARS_MSG_LOG_MAX + 3; i++) acars_app_inject("R", "L1", "T");
    LS_EQ_INT(s->msg_count, ACARS_MSG_LOG_MAX);

    acars_app_clear();
    LS_EQ_INT(s->msg_count, 0);
    LS_EQ_INT(s->msg_head,  0);
}
