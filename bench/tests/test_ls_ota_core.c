/* LS_TEST_SOURCES: components/lakeshark/core/ls_ota_core.c */
/* The update manifest as the board reads it, and what it decides. */

#include "ls_test.h"

#include "ls_ota_core.h"

#include <stdio.h>
#include <string.h>

#define SHA "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define BASE "https://lakeshark-ota.pages.dev/tdp4/stable.txt"

static const char *const LINES[] = {
    "board=T-Display-P4",
    "channel=stable",
    "version=2.9.0",
    "build=2.9.0-g0123456789ab",
    "size=3538752",
    "sha256=" SHA,
    "url=2.9.0/lakeshark.bin",
    "notes=Updates over WiFi",
    "later=a field this board does not know yet",
};
#define NLINES ((int)(sizeof(LINES) / sizeof(LINES[0])))

static char s_text[2048];

/* The good manifest, less any line starting with `drop`, with `extra` added
   before the signature and `tail` after it. */
static const char *make(const char *drop, const char *extra, const char *tail)
{
    int n = snprintf(s_text, sizeof(s_text), "%s\n", LS_OTA_MAGIC);
    for (int i = 0; i < NLINES; i++)
        if (!drop || strncmp(LINES[i], drop, strlen(drop)))
            n += snprintf(s_text + n, sizeof(s_text) - n, "%s\n", LINES[i]);
    if (extra) n += snprintf(s_text + n, sizeof(s_text) - n, "%s\n", extra);
    n += snprintf(s_text + n, sizeof(s_text) - n, "sig=3006020101020101\n");
    if (tail) snprintf(s_text + n, sizeof(s_text) - n, "%s", tail);
    return s_text;
}

static ls_ota_err_t parse(const char *t, ls_ota_manifest_t *m)
{
    return ls_ota_parse(t, strlen(t), BASE, m);
}

LS_CASE(a_signed_manifest_gives_every_field)
{
    ls_ota_manifest_t m;
    LS_EQ_INT(parse(make(NULL, NULL, NULL), &m), LS_OTA_OK);
    LS_EQ_STR(m.board, "T-Display-P4");
    LS_EQ_STR(m.version, "2.9.0");
    LS_EQ_STR(m.build, "2.9.0-g0123456789ab");
    LS_EQ_INT(m.size, 3538752);
    LS_EQ_INT(m.sha256[0], 0x01);
    LS_EQ_INT(m.sha256[31], 0xef);
    LS_EQ_STR(m.url, "https://lakeshark-ota.pages.dev/tdp4/2.9.0/lakeshark.bin");
    LS_EQ_STR(m.notes, "Updates over WiFi");
    LS_EQ_STR(m.channel, "stable");
    LS_EQ_INT(m.sig_len, 8);
}

LS_CASE(the_signature_covers_every_byte_before_its_line)
{
    ls_ota_manifest_t m;
    const char *t = make(NULL, NULL, NULL);
    LS_EQ_INT(parse(t, &m), LS_OTA_OK);
    LS_EQ_INT(m.signed_len, (size_t)(strstr(t, "sig=") - t));
}

LS_CASE(nothing_may_follow_the_signature)
{
    ls_ota_manifest_t m;
    LS_EQ_INT(parse(make(NULL, NULL, "url=https://elsewhere/x.bin\n"), &m), LS_OTA_ERR_SIG);
    LS_EQ_INT(parse(make(NULL, NULL, "\n\n"), &m), LS_OTA_OK);
}

LS_CASE(an_unsigned_manifest_is_refused)
{
    ls_ota_manifest_t m;
    char t[2048];
    snprintf(t, sizeof(t), "%s", make(NULL, NULL, NULL));
    *strstr(t, "sig=") = 0;
    LS_EQ_INT(parse(t, &m), LS_OTA_ERR_SIG);
    LS_EQ_INT(parse(make(NULL, NULL, NULL), &m), LS_OTA_OK);
    snprintf(t, sizeof(t), "%s", make(NULL, NULL, NULL));
    strcpy(strstr(t, "sig=") + 4, "30xx\n");
    LS_EQ_INT(parse(t, &m), LS_OTA_ERR_SIG);
}

LS_CASE(every_field_but_the_notes_is_required)
{
    static const char *const NEED[] = { "board=", "version=", "build=", "size=", "sha256=", "url=" };
    ls_ota_manifest_t m;
    for (unsigned i = 0; i < sizeof(NEED) / sizeof(NEED[0]); i++)
        LS_CHECK_MSG(parse(make(NEED[i], NULL, NULL), &m) == LS_OTA_ERR_MISSING,
                     "a manifest without %s was accepted", NEED[i]);
    LS_EQ_INT(parse(make("notes=", NULL, NULL), &m), LS_OTA_OK);
    LS_EQ_STR(m.notes, "");
}

LS_CASE(a_repeated_field_is_refused)
{
    ls_ota_manifest_t m;
    LS_EQ_INT(parse(make(NULL, "version=9.9.9", NULL), &m), LS_OTA_ERR_VALUE);
    LS_EQ_INT(parse(make(NULL, "url=https://elsewhere/x.bin", NULL), &m), LS_OTA_ERR_VALUE);
}

LS_CASE(malformed_values_are_refused)
{
    static const char *const BAD[][2] = {
        { "sha256=", "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde" },
        { "sha256=", "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg" },
        { "size=",   "size=0" },
        { "size=",   "size=12a" },
        { "size=",   "size=16777217" },
        { "size=",   "size=" },
        { "version=", "version=two" },
        { "version=", "version=2.9" },
        { "board=",  "board=" },
    };
    ls_ota_manifest_t m;
    for (unsigned i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++)
        LS_CHECK_MSG(parse(make(BAD[i][0], BAD[i][1], NULL), &m) == LS_OTA_ERR_VALUE,
                     "[%s] was accepted", BAD[i][1]);
}

LS_CASE(anything_else_is_not_a_manifest)
{
    ls_ota_manifest_t m;
    LS_EQ_INT(parse("<!doctype html><html>404</html>", &m), LS_OTA_ERR_MAGIC);
    LS_EQ_INT(parse("LAKESHARK-OTA 2\nboard=x\n", &m), LS_OTA_ERR_MAGIC);
    LS_EQ_INT(parse("", &m), LS_OTA_ERR_MAGIC);
    /* The site's fallback page, whatever status it came with. */
    LS_EQ_INT(parse("<!DOCTYPE html>\n<html><head><title>LakeShark</title></head>\n"
                    "<body>sig=3006020101020101</body></html>\n", &m), LS_OTA_ERR_MAGIC);
    LS_EQ_INT(parse("{\"error\":\"not found\"}", &m), LS_OTA_ERR_MAGIC);
    LS_EQ_INT(parse(make(NULL, "no equals sign here", NULL), &m), LS_OTA_ERR_LINE);
}

LS_CASE(windows_line_ends_read_the_same)
{
    char crlf[2400];
    const char *t = make(NULL, NULL, NULL);
    size_t n = 0;
    for (; *t; t++) {
        if (*t == '\n') crlf[n++] = '\r';
        crlf[n++] = *t;
    }
    crlf[n] = 0;
    ls_ota_manifest_t m;
    LS_EQ_INT(ls_ota_parse(crlf, n, BASE, &m), LS_OTA_OK);
    LS_EQ_STR(m.build, "2.9.0-g0123456789ab");
    LS_EQ_STR(m.notes, "Updates over WiFi");
}

LS_CASE(the_image_url_resolves_and_must_be_https)
{
    char out[192];
    LS_CHECK(ls_ota_resolve(BASE, "2.9.0/lakeshark.bin", out, sizeof(out)));
    LS_EQ_STR(out, "https://lakeshark-ota.pages.dev/tdp4/2.9.0/lakeshark.bin");
    LS_CHECK(ls_ota_resolve(BASE, "/fw/lakeshark.bin", out, sizeof(out)));
    LS_EQ_STR(out, "https://lakeshark-ota.pages.dev/fw/lakeshark.bin");
    LS_CHECK(ls_ota_resolve(BASE, "https://cdn.example/x.bin", out, sizeof(out)));
    LS_EQ_STR(out, "https://cdn.example/x.bin");
    LS_CHECK(ls_ota_resolve("https://host", "x.bin", out, sizeof(out)));
    LS_EQ_STR(out, "https://host/x.bin");
    LS_CHECK(!ls_ota_resolve(BASE, "x.bin", out, 20));

    ls_ota_manifest_t m;
    LS_EQ_INT(parse(make("url=", "url=http://lakeshark-ota.pages.dev/x.bin", NULL), &m), LS_OTA_ERR_URL);
}

LS_CASE(versions_order_by_release_not_by_build)
{
    LS_CHECK(ls_ota_version_cmp("2.8.4", "2.8.3") > 0);
    LS_CHECK(ls_ota_version_cmp("2.10.0", "2.9.9") > 0);
    LS_CHECK(ls_ota_version_cmp("3.0.0", "2.99.99") > 0);
    LS_CHECK(ls_ota_version_cmp("2.9.0", "2.9.0-rc2") > 0);
    LS_CHECK(ls_ota_version_cmp("2.9.0-rc2", "2.9.0-rc1") > 0);
    LS_CHECK(ls_ota_version_cmp("2.9.0-rc1", "2.8.9") > 0);
    LS_EQ_INT(ls_ota_version_cmp("2.8.3-g6fb3ac015e1a", "2.8.3"), 0);
    LS_EQ_INT(ls_ota_version_cmp("2.8.3-g96094c505037-dirty", "2.8.3-g6fb3ac015e1a"), 0);
    LS_CHECK(ls_ota_version_cmp("garbage", "2.8.3") < 0);
    LS_CHECK(ls_ota_version_cmp("2.8.3", NULL) > 0);
}

LS_CASE(the_verdict_offers_only_a_later_release)
{
    ls_ota_manifest_t m;
    LS_EQ_INT(parse(make(NULL, NULL, NULL), &m), LS_OTA_OK);
    LS_EQ_INT(ls_ota_judge("2.8.3-g6fb3ac015e1a", &m), LS_OTA_NEWER);
    LS_EQ_INT(ls_ota_judge("2.9.1-g0000000000ab", &m), LS_OTA_OLDER);
    LS_EQ_INT(ls_ota_judge("2.9.0-g0123456789ab", &m), LS_OTA_CURRENT);
    LS_EQ_INT(ls_ota_judge("2.9.0-g0123456789ab-dirty", &m), LS_OTA_OTHER_BUILD);
    LS_EQ_INT(ls_ota_judge("2.9.0-rc3-g1111", &m), LS_OTA_NEWER);
}

LS_CASE(the_channel_is_the_manifest_file_name_less_txt)
{
    char c[24];
    LS_CHECK(ls_ota_channel_of(BASE, c, sizeof(c)));
    LS_EQ_STR(c, "stable");
    LS_CHECK(ls_ota_channel_of("https://h/a/b/beta.txt?x=1#y", c, sizeof(c)));
    LS_EQ_STR(c, "beta");
    LS_CHECK(ls_ota_channel_of("https://h/nightly", c, sizeof(c)));
    LS_EQ_STR(c, "nightly");
    LS_CHECK(!ls_ota_channel_of("https://h", c, sizeof(c)));
    LS_CHECK(!ls_ota_channel_of("https://h/dir/", c, sizeof(c)));
    LS_CHECK(!ls_ota_channel_of("https://h/.txt", c, sizeof(c)));
    LS_CHECK(!ls_ota_channel_of(BASE, c, 3));
}

LS_CASE(a_manifest_must_name_the_channel_it_was_asked_for)
{
    ls_ota_manifest_t m;
    LS_EQ_INT(parse(make(NULL, NULL, NULL), &m), LS_OTA_OK);
    LS_CHECK(ls_ota_channel_ok(&m, BASE));
    LS_CHECK(!ls_ota_channel_ok(&m, "https://lakeshark-ota.pages.dev/tdp4/beta.txt"));
    LS_CHECK(!ls_ota_channel_ok(&m, "https://lakeshark-ota.pages.dev/tdp4/Stable.txt"));
    LS_CHECK(!ls_ota_channel_ok(&m, "https://lakeshark-ota.pages.dev/tdp4/"));

    /* None at all: it parses, and is refused as another channel's. */
    LS_EQ_INT(parse(make("channel=", NULL, NULL), &m), LS_OTA_OK);
    LS_EQ_STR(m.channel, "");
    LS_CHECK(!ls_ota_channel_ok(&m, BASE));

    /* The fixtures that must never install carry their own names. */
    LS_EQ_INT(parse(make("channel=", "channel=badsig", NULL), &m), LS_OTA_OK);
    LS_CHECK(ls_ota_channel_ok(&m, "https://lakeshark-ota.pages.dev/tdp4/badsig.txt"));
    LS_CHECK(!ls_ota_channel_ok(&m, BASE));
    LS_EQ_INT(parse(make("channel=", "channel=badhash", NULL), &m), LS_OTA_OK);
    LS_CHECK(ls_ota_channel_ok(&m, "https://lakeshark-ota.pages.dev/tdp4/badhash.txt"));
}

LS_CASE(a_malformed_or_repeated_channel_is_refused)
{
    static const char *const BAD[] = {
        "channel=", "channel=sta ble", "channel=a/b", "channel=../x", "channel=0123456789012345678901234",
    };
    ls_ota_manifest_t m;
    for (unsigned i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++)
        LS_CHECK_MSG(parse(make("channel=", BAD[i], NULL), &m) == LS_OTA_ERR_VALUE,
                     "[%s] was accepted", BAD[i]);
    LS_EQ_INT(parse(make(NULL, "channel=beta", NULL), &m), LS_OTA_ERR_VALUE);
}

static bool only_key(size_t key, void *ctx) { return key == *(size_t *)ctx; }

LS_CASE(a_signature_is_good_when_any_trusted_key_accepts_it)
{
    for (size_t want = 0; want < 3; want++)
        LS_CHECK(ls_ota_verify_any(3, only_key, &want));
    size_t beyond = 3, zero = 0;
    LS_CHECK(!ls_ota_verify_any(3, only_key, &beyond));
    LS_CHECK(!ls_ota_verify_any(0, only_key, &zero));
    LS_CHECK(!ls_ota_verify_any(3, NULL, &zero));
}

LS_CASE(the_other_known_host_is_swapped_in_both_directions)
{
    char out[96];
    LS_CHECK(ls_ota_other_host("https://ota.terminalbay.com/tdp4/stable.txt", out, sizeof(out)));
    LS_EQ_STR(out, "https://lakeshark-ota.pages.dev/tdp4/stable.txt");
    LS_CHECK(ls_ota_other_host("https://lakeshark-ota.pages.dev/tdp4/stable.txt?x=1", out, sizeof(out)));
    LS_EQ_STR(out, "https://ota.terminalbay.com/tdp4/stable.txt?x=1");
    LS_CHECK(ls_ota_other_host("https://ota.terminalbay.com", out, sizeof(out)));
    LS_EQ_STR(out, "https://lakeshark-ota.pages.dev");
}

LS_CASE(an_unknown_host_or_http_or_a_small_buffer_gets_no_swap)
{
    char out[96] = "keep";
    LS_CHECK(!ls_ota_other_host("https://example.com/tdp4/stable.txt", out, sizeof(out)));
    LS_CHECK(!ls_ota_other_host("https://ota.terminalbay.com.evil.net/x.txt", out, sizeof(out)));
    LS_CHECK(!ls_ota_other_host("https://evil.ota.terminalbay.com/x.txt", out, sizeof(out)));
    LS_CHECK(!ls_ota_other_host("https://ota.terminalbay.com:8443/x.txt", out, sizeof(out)));
    LS_CHECK(!ls_ota_other_host("https://user@ota.terminalbay.com/x.txt", out, sizeof(out)));
    LS_CHECK(!ls_ota_other_host("http://ota.terminalbay.com/tdp4/stable.txt", out, sizeof(out)));
    LS_CHECK(!ls_ota_other_host("ota.terminalbay.com/tdp4/stable.txt", out, sizeof(out)));
    LS_CHECK(!ls_ota_other_host(NULL, out, sizeof(out)));
    LS_CHECK(!ls_ota_other_host("https://ota.terminalbay.com/x.txt", out, 0));
    LS_EQ_STR(out, "keep");
    /* "https://lakeshark-ota.pages.dev/tdp4/stable.txt" is 47 chars + NUL */
    LS_CHECK(ls_ota_other_host("https://ota.terminalbay.com/tdp4/stable.txt", out, 48));
    LS_CHECK(!ls_ota_other_host("https://ota.terminalbay.com/tdp4/stable.txt", out, 47));
}

LS_CASE(a_relative_image_follows_the_host_that_served_the_manifest)
{
    /* After a fallback the manifest was served by the other host, and the
       image is beside it there; the same image may then be retried on the first. */
    char fb[96], img[128], retry[128];
    LS_CHECK(ls_ota_other_host("https://ota.terminalbay.com/tdp4/stable.txt", fb, sizeof(fb)));
    LS_CHECK(ls_ota_resolve(fb, "2.8.4-rc5-gabc/lakeshark.bin", img, sizeof(img)));
    LS_EQ_STR(img, "https://lakeshark-ota.pages.dev/tdp4/2.8.4-rc5-gabc/lakeshark.bin");
    LS_CHECK(ls_ota_other_host(img, retry, sizeof(retry)));
    LS_EQ_STR(retry, "https://ota.terminalbay.com/tdp4/2.8.4-rc5-gabc/lakeshark.bin");
    /* An absolute image on a custom host is not retried anywhere. */
    LS_CHECK(!ls_ota_other_host("https://cdn.example/x.bin", retry, sizeof(retry)));
}

/* ------------------------------------------------- the background check */

static ls_ota_bg_t bg(bool enabled, int64_t wifi, int64_t last, int64_t defer, uint32_t seed)
{
    ls_ota_bg_t s = { enabled, wifi, last, defer, seed };
    return s;
}

#define OPEN 0u     /* nothing in the way */

LS_CASE(the_first_check_is_two_to_five_minutes_after_wifi_first_connects)
{
    for (uint32_t seed = 0; seed < 5000; seed++) {
        const int64_t j = ls_ota_bg_jitter_s(seed);
        LS_CHECK(j >= 120 && j <= 300);
    }
    LS_EQ_INT(ls_ota_bg_jitter_s(0), 120);
    LS_EQ_INT(ls_ota_bg_jitter_s(180), 300);
    LS_EQ_INT(ls_ota_bg_jitter_s(181), 120);
    /* The seed spreads boards: the whole span is used. */
    bool seen_low = false, seen_high = false;
    for (uint32_t seed = 0; seed < 181; seed++) {
        seen_low |= ls_ota_bg_jitter_s(seed) < 130;
        seen_high |= ls_ota_bg_jitter_s(seed) > 290;
    }
    LS_CHECK(seen_low && seen_high);
    /* WiFi joined at 40 s on a boot with seed 77: due at 40 + 120 + 77. */
    ls_ota_bg_t s = bg(true, 40, -1, -1, 77);
    LS_EQ_INT(ls_ota_bg_next_s(&s), 237);
}

LS_CASE(nothing_is_due_before_wifi_connects_or_when_the_setting_is_off)
{
    ls_ota_bg_t s = bg(true, -1, -1, -1, 5);
    LS_EQ_INT(ls_ota_bg_next_s(&s), -1);
    LS_EQ_INT(ls_ota_bg_act(&s, 100000, OPEN), LS_OTA_BG_WAIT);
    s = bg(false, 40, -1, -1, 5);
    LS_EQ_INT(ls_ota_bg_next_s(&s), -1);
    LS_EQ_INT(ls_ota_bg_act(&s, 100000, OPEN), LS_OTA_BG_WAIT);
    /* Off stays off however long the board has been up or what it has done. */
    s = bg(false, 40, 500, 900, 5);
    LS_EQ_INT(ls_ota_bg_act(&s, 10 * LS_OTA_BG_EVERY_S, OPEN), LS_OTA_BG_WAIT);
    LS_EQ_INT(ls_ota_bg_next_s(NULL), -1);
}

LS_CASE(it_waits_until_due_then_runs_when_clear)
{
    ls_ota_bg_t s = bg(true, 40, -1, -1, 77);           /* due at 237 */
    LS_EQ_INT(ls_ota_bg_act(&s, 0, OPEN), LS_OTA_BG_WAIT);
    LS_EQ_INT(ls_ota_bg_act(&s, 236, OPEN), LS_OTA_BG_WAIT);
    LS_EQ_INT(ls_ota_bg_act(&s, 237, OPEN), LS_OTA_BG_RUN);
    LS_EQ_INT(ls_ota_bg_act(&s, 90000, OPEN), LS_OTA_BG_RUN);
}

LS_CASE(after_a_check_the_next_is_a_day_on_from_it)
{
    ls_ota_bg_t s = bg(true, 40, 240, -1, 77);
    LS_EQ_INT(ls_ota_bg_next_s(&s), 240 + 86400);
    LS_EQ_INT(ls_ota_bg_act(&s, 240 + 86399, OPEN), LS_OTA_BG_WAIT);
    LS_EQ_INT(ls_ota_bg_act(&s, 240 + 86400, OPEN), LS_OTA_BG_RUN);
    /* Never more than once in 24 h: a later wifi time or seed changes nothing. */
    s.wifi_up_s = 9000;
    s.seed = 3;
    LS_EQ_INT(ls_ota_bg_next_s(&s), 240 + 86400);
}

LS_CASE(a_due_check_that_finds_the_board_busy_is_put_off_ten_minutes)
{
    ls_ota_bg_t s = bg(true, 40, -1, -1, 77);           /* due at 237 */
    LS_EQ_INT(ls_ota_bg_act(&s, 237, LS_OTA_BG_BUSY_RF), LS_OTA_BG_DEFER);
    LS_EQ_INT(ls_ota_bg_act(&s, 237, LS_OTA_BG_BUSY_JOB), LS_OTA_BG_DEFER);
    LS_EQ_INT(ls_ota_bg_act(&s, 237, LS_OTA_BG_LOW_HEAP), LS_OTA_BG_DEFER);
    LS_EQ_INT(ls_ota_bg_act(&s, 237, LS_OTA_BG_NO_WIFI), LS_OTA_BG_DEFER);
    LS_EQ_INT(ls_ota_bg_act(&s, 237, LS_OTA_BG_BUSY_RF | LS_OTA_BG_LOW_HEAP), LS_OTA_BG_DEFER);
    /* Being busy before it is due is nothing to note. */
    LS_EQ_INT(ls_ota_bg_act(&s, 100, LS_OTA_BG_BUSY_RF), LS_OTA_BG_WAIT);
    /* The caller records the deferral; the next look is ten minutes on. */
    s.last_defer_s = 237;
    LS_EQ_INT(ls_ota_bg_next_s(&s), 237 + 600);
    LS_EQ_INT(ls_ota_bg_act(&s, 237 + 599, OPEN), LS_OTA_BG_WAIT);
    LS_EQ_INT(ls_ota_bg_act(&s, 237 + 600, OPEN), LS_OTA_BG_RUN);
    /* Still busy then: put off again, ten minutes from that. */
    LS_EQ_INT(ls_ota_bg_act(&s, 237 + 600, LS_OTA_BG_BUSY_RF), LS_OTA_BG_DEFER);
    s.last_defer_s = 837;
    LS_EQ_INT(ls_ota_bg_next_s(&s), 1437);
}

LS_CASE(a_deferral_from_an_earlier_cycle_does_not_move_the_daily_check)
{
    /* Put off at 237, ran at 837: the next is a day after the run, whatever
       the stale deferral says. */
    ls_ota_bg_t s = bg(true, 40, 837, 237, 77);
    LS_EQ_INT(ls_ota_bg_next_s(&s), 837 + 86400);
    /* Due, put off at the due time itself: ten minutes on from it. */
    s = bg(true, 40, 837, 837 + 86400, 77);
    LS_EQ_INT(ls_ota_bg_next_s(&s), 837 + 86400 + 600);
}

LS_CASE(the_blockers_name_what_is_in_the_way)
{
    const size_t ok_int = LS_OTA_BG_MIN_INTERNAL, ok_dma = LS_OTA_BG_MIN_DMA;
    LS_EQ_INT(ls_ota_bg_blockers(false, false, true, ok_int, ok_dma), 0);
    LS_EQ_INT(ls_ota_bg_blockers(true, false, true, ok_int, ok_dma), LS_OTA_BG_BUSY_JOB);
    LS_EQ_INT(ls_ota_bg_blockers(false, true, true, ok_int, ok_dma), LS_OTA_BG_BUSY_RF);
    LS_EQ_INT(ls_ota_bg_blockers(false, false, false, ok_int, ok_dma), LS_OTA_BG_NO_WIFI);
    LS_EQ_INT(ls_ota_bg_blockers(false, false, true, ok_int - 1, ok_dma), LS_OTA_BG_LOW_HEAP);
    LS_EQ_INT(ls_ota_bg_blockers(false, false, true, ok_int, ok_dma - 1), LS_OTA_BG_LOW_HEAP);
    LS_EQ_INT(ls_ota_bg_blockers(true, true, false, 0, 0),
              LS_OTA_BG_BUSY_JOB | LS_OTA_BG_BUSY_RF | LS_OTA_BG_LOW_HEAP | LS_OTA_BG_NO_WIFI);
}

LS_CASE(a_build_is_announced_once)
{
    LS_CHECK(ls_ota_notice_new("2.9.0-g0123456789ab", ""));
    LS_CHECK(ls_ota_notice_new("2.9.0-g0123456789ab", NULL));
    LS_CHECK(!ls_ota_notice_new("2.9.0-g0123456789ab", "2.9.0-g0123456789ab"));
    /* A newer build after that is announced again. */
    LS_CHECK(ls_ota_notice_new("2.9.1-g1111111111aa", "2.9.0-g0123456789ab"));
    /* Another build of the same release is a new build. */
    LS_CHECK(ls_ota_notice_new("2.9.0-gffffffffffff", "2.9.0-g0123456789ab"));
    /* Nothing newer, nothing to say. */
    LS_CHECK(!ls_ota_notice_new("", ""));
    LS_CHECK(!ls_ota_notice_new("", "2.9.0-g0123456789ab"));
    LS_CHECK(!ls_ota_notice_new(NULL, "x"));
}

LS_CASE(the_toast_says_where_to_find_it)
{
    char t[64];
    LS_CHECK(ls_ota_toast_text("2.9.0", t, sizeof(t)));
    LS_EQ_STR(t, "2.9.0 available: SYSTEM > UPDATE");
    LS_CHECK(ls_ota_toast_text("2.8.4-rc1", t, sizeof(t)));
    LS_EQ_STR(t, "2.8.4-rc1 available: SYSTEM > UPDATE");
    LS_CHECK(!ls_ota_toast_text("", t, sizeof(t)));
    LS_CHECK(!ls_ota_toast_text(NULL, t, sizeof(t)));
    LS_CHECK(!ls_ota_toast_text("2.9.0", t, 10));
    LS_CHECK(!ls_ota_toast_text("2.9.0", NULL, 64));
}
