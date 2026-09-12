/* LS_TEST_SOURCES: ${APP}/rec/rec_sidecar.c */
/**/
/* Provenance sidecar for captures. */

#include "ls_test.h"
#include "rec_sidecar.h"

#include <direct.h>
#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

static char fixture[96];

static void fx_init(void)
{
    if (fixture[0] == '\0') {
        snprintf(fixture, sizeof(fixture), ".ls_rec_sidecar_fixture_%lu",
                 (unsigned long)_getpid());
    }
}

static void fx_path(char *out, size_t len, const char *name)
{
    snprintf(out, len, "%s/%s.json", fixture, name);
}

static void fx_write_raw(const char *name, const char *body)
{
    char path[160];
    fx_path(path, sizeof(path), name);
    FILE *f = fopen(path, "wb");
    LS_CHECK_MSG(f != NULL, "could not create fixture %s", path);
    if (f) { fputs(body, f); fclose(f); }
}

static bool fx_path_is_gone(const char *path)
{
    struct _stat st;
    errno = 0;
    return _stat(path, &st) != 0 && errno == ENOENT;
}

static bool fx_remove_file(const char *path)
{
    int last_error = 0;

    for (int attempt = 0; attempt < 20; ++attempt) {
        errno = 0;
        if (remove(path) == 0 || errno == ENOENT) {
            if (fx_path_is_gone(path)) return true;
        }
        last_error = errno;
        Sleep(10);
    }

    LS_CHECK_MSG(false, "could not clear fixture file %s after retries: %s",
                 path, strerror(last_error));
    return false;
}

static bool fx_remove_dir(const char *path)
{
    int last_error = 0;

    for (int attempt = 0; attempt < 20; ++attempt) {
        errno = 0;
        if (_rmdir(path) == 0 || errno == ENOENT) {
            if (fx_path_is_gone(path)) return true;
        }
        last_error = errno;
        Sleep(10);
    }

    LS_CHECK_MSG(false, "could not clear fixture directory %s after retries: %s",
                 path, strerror(last_error));
    return false;
}

static bool fx_rm_all(void)
{
    char path[160];
    static const char *bases[] = {
        "rec000", "rec001", "rec042", "capture", "collide"
    };
    bool cleared = true;

    fx_init();
    for (size_t i = 0; i < sizeof(bases)/sizeof(bases[0]); i++) {
        fx_path(path, sizeof(path), bases[i]);
        if (!fx_remove_file(path)) cleared = false;
    }
    if (!fx_remove_dir(fixture)) cleared = false;
    return cleared;
}

static bool fx_setup(void)
{
    /* fixed fixture names let a transient Windows sharing violation
       leave state for the next run.  A locked collide.json reproduced five
       unrelated failures beginning at _mkdir.  Use a per-process path, retry
       Windows deletion, and stop a case at the path-specific cleanup error. */
    if (!fx_rm_all()) return false;
    errno = 0;
    if (_mkdir(fixture) != 0) {
        LS_CHECK_MSG(false, "could not create fixture directory %s: %s",
                     fixture, strerror(errno));
        return false;
    }
    return true;
}

static void fill_reference(rec_sidecar_t *s)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->time,     sizeof(s->time),     "2026-03-05T14:22:07Z");
    snprintf(s->board,    sizeof(s->board),    "T-Nano");
    snprintf(s->firmware, sizeof(s->firmware), "v0.1-3-g1a2b3c-dirty");
    s->freq_hz     = 433920000UL;
    s->gain_tenths = 400;
    s->bw_hz       = 250000UL;
    s->sample_rate = 256000UL;
    s->edges       = 350;
    s->span_us     = 12345UL;
    s->mag_peak    = 128;
    s->mag_floor   = 8;
}

LS_CASE(format_then_parse_is_a_lossless_roundtrip)
{
    /* Every field the writer emits must come back through the parser
       with the same value.  This is the whole contract - if any single
       field drops, a capture written today reloads with wrong metadata
       tomorrow and there is no way to notice from the file itself. */
    rec_sidecar_t in;
    fill_reference(&in);

    char json[REC_SIDECAR_JSON_MAX];
    int n = rec_sidecar_format(json, sizeof(json), &in);
    LS_CHECK(n > 0);
    LS_CHECK((size_t)n < sizeof(json));

    rec_sidecar_t out;
    LS_CHECK(rec_sidecar_parse(json, &out));

    LS_EQ_STR (out.time,     in.time);
    LS_EQ_STR (out.board,    in.board);
    LS_EQ_STR (out.firmware, in.firmware);
    LS_EQ_UINT(out.freq_hz,     in.freq_hz);
    LS_EQ_INT (out.gain_tenths, in.gain_tenths);
    LS_EQ_UINT(out.bw_hz,       in.bw_hz);
    LS_EQ_UINT(out.sample_rate, in.sample_rate);
    LS_EQ_INT (out.edges,       in.edges);
    LS_EQ_UINT(out.span_us,     in.span_us);
    LS_EQ_INT (out.mag_peak,    in.mag_peak);
    LS_EQ_INT (out.mag_floor,   in.mag_floor);
}

LS_CASE(write_then_read_is_a_lossless_roundtrip)
{
    /* Same as above but through the filesystem: a capture written to
       disk and immediately reloaded must observe identical metadata.
       Catches a truncation-on-write or a fread short-count that a
       string-only round-trip would miss. */
    if (!fx_setup()) return;
    rec_sidecar_t in;
    fill_reference(&in);

    LS_EQ_INT(rec_sidecar_write(fixture, "rec042", &in), 0);

    rec_sidecar_t out;
    LS_CHECK(rec_sidecar_read(fixture, "rec042", &out));

    LS_EQ_STR (out.time,        in.time);
    LS_EQ_STR (out.board,       in.board);
    LS_EQ_STR (out.firmware,    in.firmware);
    LS_EQ_UINT(out.freq_hz,     in.freq_hz);
    LS_EQ_INT (out.gain_tenths, in.gain_tenths);
    LS_EQ_UINT(out.bw_hz,       in.bw_hz);
    LS_EQ_UINT(out.sample_rate, in.sample_rate);
    LS_EQ_INT (out.edges,       in.edges);
    LS_EQ_UINT(out.span_us,     in.span_us);
    LS_EQ_INT (out.mag_peak,    in.mag_peak);
    LS_EQ_INT (out.mag_floor,   in.mag_floor);

    (void)fx_rm_all();
}

LS_CASE(missing_sidecar_read_returns_false_and_zeroed_struct)
{
    /* The done-when case: a capture without a sidecar must still list
       and replay - so reading its missing sidecar returns false with an
       all-zero struct, NOT an error the caller has to disambiguate from
       a corrupt read.  Every caller in the tree treats a zero struct as
       "no provenance recorded". */
    if (!fx_setup()) return;

    rec_sidecar_t out;
    memset(&out, 0xAA, sizeof(out));                 /* seed with garbage */
    LS_CHECK(!rec_sidecar_read(fixture, "rec000", &out));

    /* Every field is zero-initialised so a downstream printf will not
       reach through uninitialised memory.  Check a representative set. */
    LS_EQ_INT((int)out.time[0],  0);
    LS_EQ_INT((int)out.board[0], 0);
    LS_EQ_UINT(out.freq_hz,      0);
    LS_EQ_INT (out.gain_tenths,  0);
    LS_EQ_INT (out.edges,        0);

    (void)fx_rm_all();
}

LS_CASE(unknown_keys_are_ignored)
{
    /* Older or newer sidecars might carry fields this parser does not
       know about.  It must still parse the ones it does know and skip
       the rest - otherwise adding a field in a future revision breaks
       every reader that has not been rebuilt. */
    if (!fx_setup()) return;
    fx_write_raw("rec001",
        "{\n"
        "  \"time\": \"2026-03-05T14:22:07Z\",\n"
        "  \"future_field\": 12345,\n"
        "  \"freq_hz\": 433920000,\n"
        "  \"another_new_string\": \"whatever\",\n"
        "  \"gain_tenths\": 400,\n"
        "  \"edges\": 350\n"
        "}\n");

    rec_sidecar_t out;
    LS_CHECK(rec_sidecar_read(fixture, "rec001", &out));
    LS_EQ_STR (out.time, "2026-03-05T14:22:07Z");
    LS_EQ_UINT(out.freq_hz,     433920000UL);
    LS_EQ_INT (out.gain_tenths, 400);
    LS_EQ_INT (out.edges,       350);

    (void)fx_rm_all();
}

LS_CASE(malformed_body_parse_returns_false)
{
    /* A file that is not JSON at all must be refused cleanly without
       filling in random garbage from a strtol on a non-number.  The
       struct is zero-initialised as a defensive baseline so a caller
       that ignores the return value still gets a safe empty struct. */
    rec_sidecar_t out;
    memset(&out, 0xAA, sizeof(out));
    LS_CHECK(!rec_sidecar_parse("this is not JSON", &out));
    /* On failure the struct is left zero, not garbage. */
    LS_EQ_UINT(out.freq_hz, 0);
    LS_EQ_INT ((int)out.time[0], 0);
}

LS_CASE(uptime_time_marker_survives_the_roundtrip)
{
    /* says a capture written before SNTP has landed carries the
       "up <seconds>s" marker instead of a fake date.  The sidecar must
       preserve that string verbatim, or the file goes from "honestly
       unknown when" to "we lost even that". */
    rec_sidecar_t in;
    memset(&in, 0, sizeof(in));
    snprintf(in.time, sizeof(in.time), "up 12345s");
    in.freq_hz = 315000000UL;

    char json[REC_SIDECAR_JSON_MAX];
    LS_CHECK(rec_sidecar_format(json, sizeof(json), &in) > 0);

    rec_sidecar_t out;
    LS_CHECK(rec_sidecar_parse(json, &out));
    LS_EQ_STR(out.time, "up 12345s");
    LS_EQ_UINT(out.freq_hz, 315000000UL);
}

LS_CASE(write_refuses_to_overwrite_an_existing_sidecar)
{
    /* Same rule the .sub uses (): a stale sidecar from an earlier
       capture must not be silently paved over.  If rec_save picked a
       fresh basename its sidecar cannot exist yet; if it did not, the
       sidecar has bytes that should not disappear without a rename. */
    if (!fx_setup()) return;
    fx_write_raw("collide", "{ \"time\": \"stale\", \"freq_hz\": 1 }\n");

    rec_sidecar_t in;
    fill_reference(&in);
    LS_EQ_INT(rec_sidecar_write(fixture, "collide", &in), -1);

    /* The stale bytes are still there. */
    rec_sidecar_t out;
    LS_CHECK(rec_sidecar_read(fixture, "collide", &out));
    LS_EQ_STR (out.time,    "stale");
    LS_EQ_UINT(out.freq_hz, 1);

    (void)fx_rm_all();
}

LS_CASE(format_refuses_a_buffer_that_is_too_small)
{
    /* Better a clean zero than a JSON body missing the closing brace -
       a downstream parser cannot recover from that. */
    rec_sidecar_t in;
    fill_reference(&in);

    char tiny[32];
    LS_EQ_INT(rec_sidecar_format(tiny, sizeof(tiny), &in), 0);
    LS_EQ_INT((int)tiny[0], 0);
}

LS_CASE(null_arguments_are_rejected_cleanly)
{
    /* Defensive: a mis-wired caller must not crash.  Every entry point
       returns a safe empty/failure result. */
    char buf[REC_SIDECAR_JSON_MAX];

    LS_EQ_INT(rec_sidecar_format(NULL, sizeof(buf), NULL), 0);
    LS_EQ_INT(rec_sidecar_format(buf,  0,           NULL), 0);

    rec_sidecar_t s;
    memset(&s, 0, sizeof(s));
    LS_EQ_INT(rec_sidecar_format(buf, sizeof(buf), NULL), 0);
    /* format on empty struct still emits a valid (if empty-looking) JSON body. */
    LS_CHECK(rec_sidecar_format(buf, sizeof(buf), &s) > 0);

    LS_CHECK(!rec_sidecar_parse(NULL, &s));
    LS_CHECK(!rec_sidecar_parse("{}", NULL));

    LS_EQ_INT(rec_sidecar_write(NULL, "x", &s), -1);
    LS_EQ_INT(rec_sidecar_write(".",  NULL, &s), -1);
    LS_EQ_INT(rec_sidecar_write(".",  "x",  NULL), -1);

    LS_CHECK(!rec_sidecar_read(NULL, "x", &s));
    LS_CHECK(!rec_sidecar_read(".",  NULL, &s));
    LS_CHECK(!rec_sidecar_read(".",  "x",  NULL));
}
