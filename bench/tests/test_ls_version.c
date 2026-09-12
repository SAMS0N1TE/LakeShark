/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/ls_version.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/core */
/* Version string assembly, on the host. */

#include "ls_test.h"
#include "ls_version.h"

#include <string.h>

static bool contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* --- the format string is well-behaved even when nothing is filled in ---- */

LS_CASE(null_info_yields_empty_string_not_a_crash)
{
    /* Boot before esp_app_get_description() has resolved: never happens on
       a healthy device but the formatter must not fault if it does.  A
       zero-length buffer must also be safe - the settings screen builds
       into a stack buffer whose size a future edit might set to 0. */
    char buf[64] = "sentinel";
    int n = ls_version_format(buf, sizeof(buf), NULL);
    LS_EQ_INT(n, 0);
    LS_EQ_STR(buf, "");

    n = ls_version_format(NULL, 0, NULL);
    LS_EQ_INT(n, 0);
}

/* --- a clean build looks like a clean build ------------------------------ */

LS_CASE(clean_build_prints_every_field_and_no_dirty_marker)
{
    ls_version_info_t v = {
        .version = "v0.3.1-4-g1a2b3c4",
        .board   = "ESP32-P4-NANO",
        .idf     = "v5.5.4",
        .date    = "Sep  6 2026",
        .time    = "12:34:56",
    };
    char buf[LS_VERSION_LINE_MAX];
    int n = ls_version_format(buf, sizeof(buf), &v);

    LS_CHECK(n > 0);
    LS_CHECK(contains(buf, "LakeShark"));
    LS_CHECK(contains(buf, "v0.3.1-4-g1a2b3c4"));
    LS_CHECK(contains(buf, "ESP32-P4-NANO"));
    LS_CHECK(contains(buf, "v5.5.4"));
    LS_CHECK(contains(buf, "Sep  6 2026"));
    LS_CHECK(contains(buf, "12:34:56"));
    /* A tagged clean build must not carry the dirty marker - otherwise a
       release looks like a work-in-progress every time it is inspected. */
    LS_CHECK_MSG(!contains(buf, "DIRTY"),
                 "clean build wrongly flagged as dirty: [%s]", buf);
}

/* --- a build from an unclean tree is visibly marked as such -------------- */

LS_CASE(dirty_tree_is_visibly_flagged)
{
    /* This is the case the task file called out: a build from uncommitted
       changes must not be mistaken for a tagged one.  IDF's
       `git describe --dirty` appends "-dirty" to PROJECT_VER, and the
       formatter turns that into a plain-text [DIRTY] marker. */
    ls_version_info_t v = {
        .version = "v0.3.1-4-g1a2b3c4-dirty",
        .board   = "ESP32-P4-NANO",
        .idf     = "v5.5.4",
        .date    = "Sep  6 2026",
        .time    = "12:34:56",
    };
    char buf[LS_VERSION_LINE_MAX];
    ls_version_format(buf, sizeof(buf), &v);

    LS_CHECK_MSG(contains(buf, "[DIRTY]"),
                 "dirty build not marked as such: [%s]", buf);
    /* The raw suffix must still be there - `git describe` output is what a
       remote observer knows about, the marker just makes it obvious. */
    LS_CHECK(contains(buf, "-dirty"));
}

/* --- the dirty predicate is what the settings row and SYS reply use ----- */

LS_CASE(is_dirty_recognises_git_describe_output)
{
    LS_CHECK(!ls_version_is_dirty(NULL));
    LS_CHECK(!ls_version_is_dirty(""));
    LS_CHECK(!ls_version_is_dirty("v0.3.1"));
    LS_CHECK(!ls_version_is_dirty("v0.3.1-4-g1a2b3c4"));

    /* The exact form IDF's git_describe writes: "<tag>-dirty". */
    LS_CHECK(ls_version_is_dirty("v0.3.1-dirty"));
    LS_CHECK(ls_version_is_dirty("v0.3.1-4-g1a2b3c4-dirty"));
    /* Uppercased or mid-string is still a dirty tree; the predicate is
       forgiving so a future git config tweak (dirty.suffix) does not
       silently break the marker. */
    LS_CHECK(ls_version_is_dirty("DIRTY-1234"));
    LS_CHECK(ls_version_is_dirty("v1.0-Dirty"));
}

LS_CASE(bounded_build_identity_marks_dirty_without_mislabeling_unknown_sources)
{
    LS_CHECK(ls_version_is_dirty("1.0.1-gfc9fb8101234-dirty"));
    LS_CHECK(!ls_version_is_dirty("1.0.1-gfc9fb8101234"));
    LS_CHECK(!ls_version_is_dirty("1.0.1-archive"));
    LS_CHECK(!ls_version_is_dirty("1.0.1-unknown"));
    ls_version_info_t v = {.version="1.0.1-gfc9fb8101234-dirty"};
    char line[LS_VERSION_LINE_MAX];
    ls_version_format(line, sizeof(line), &v);
    LS_CHECK(contains(line, "gfc9fb8101234-dirty"));
    LS_CHECK(contains(line, "[DIRTY]"));
}

/* --- truncation is not a crash ----------------------------------------- */

LS_CASE(truncated_buffer_stays_nul_terminated)
{
    ls_version_info_t v = {
        .version = "v0.3.1-4-g1a2b3c4-dirty",
        .board   = "ESP32-P4-NANO",
        .idf     = "v5.5.4",
        .date    = "Sep  6 2026",
        .time    = "12:34:56",
    };
    char tiny[16];
    memset(tiny, 0xAA, sizeof(tiny));
    int n = ls_version_format(tiny, sizeof(tiny), &v);
    LS_CHECK(n > 0);
    LS_CHECK((size_t)n < sizeof(tiny));
    /* Never writes past the last byte and always terminates. */
    LS_EQ_INT((int)tiny[sizeof(tiny) - 1], 0);
}

LS_CASE(missing_fields_render_as_question_marks)
{

    ls_version_info_t v = {
        .version = NULL,
        .board   = "ESP32-P4-NANO",
        .idf     = "",
        .date    = NULL,
        .time    = "",
    };
    char buf[LS_VERSION_LINE_MAX];
    ls_version_format(buf, sizeof(buf), &v);
    LS_CHECK(contains(buf, "ESP32-P4-NANO"));
    LS_CHECK(contains(buf, "?"));
    LS_CHECK(!contains(buf, "DIRTY"));
}
