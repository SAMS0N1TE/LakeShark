/* LS_TEST_SOURCES: ${APP}/rec/rec_list_format.c */
/**/

#include "ls_test.h"
#include "rec_list_format.h"

#include <stdio.h>
#include <string.h>

#define APP_REC_FILES_MAX 24
#define APP_REC_LIST_BYTES 768

/* 27-char basename ("name.sub" with 23-char base) matches what
   rec_save produces after sanitize_name.  Formatted with a small size
   this is 37 chars per entry plus a 2-char separator - 39 chars once
   an entry has anything before it in the buffer.  The 768-byte buffer
   therefore holds ~19 max-length entries, well under FILES_MAX=24.  */
static const char MAX_NAME_TEMPLATE[] = "aaaaaaaaaaaaaaaaaaaaaaa.sub";

static void make_max_name(char *out, size_t out_len, int idx)
{
    /* Vary two chars in the middle of the base so a strstr(", ")
       walker sees distinct row names; keep the total length at 27
       chars either way, and keep the ".sub" tail intact so the FILES
       tab's dot-suffix filter recognises them. */
    (void)out_len;
    memcpy(out, MAX_NAME_TEMPLATE, sizeof(MAX_NAME_TEMPLATE));
    out[0] = '0' + ((idx / 10) % 10);
    out[1] = '0' + (idx % 10);
}

LS_CASE(single_entry_fits_and_reports_no_truncation)
{
    /* Sanity: one short capture goes in cleanly, the write cursor
       advances, and rec_files_append returns success. */
    char buf[64];
    memset(buf, 0x5A, sizeof(buf));
    buf[0] = '\0';
    size_t used = 0;
    LS_CHECK(rec_files_append(buf, sizeof(buf), &used, "rec000", 42));
    LS_EQ_STR(buf, "rec000 (42 B)");
    LS_EQ_INT((int)used, (int)strlen("rec000 (42 B)"));
}

LS_CASE(second_entry_uses_a_comma_separator)
{
    /* The FILES tab walks the buffer with strstr(", "); the primitive
       must emit that separator only from the second entry onward so a
       lone entry does not start with a stray ", ". */
    char buf[64];
    buf[0] = '\0';
    size_t used = 0;
    LS_CHECK(rec_files_append(buf, sizeof(buf), &used, "a", 1));
    LS_CHECK(rec_files_append(buf, sizeof(buf), &used, "b", 2));
    LS_EQ_STR(buf, "a (1 B), b (2 B)");
}

LS_CASE(entry_that_would_overflow_is_refused_atomically)
{
    /* This is the whole invariant that keeps refreshFiles' walker from
       seeing "name (size" as a half-formed row: on a refused append the
       buffer must be nul-terminated at the previous *used and the
       cursor must not move.  Was the bug in rec_list() - snprintf's
       truncated prefix was left in place until the next successful
       write overwrote it, and if none followed the caller inherited it. */
    char buf[24];
    memset(buf, 0x5A, sizeof(buf));
    buf[0] = '\0';
    size_t used = 0;
    LS_CHECK(rec_files_append(buf, sizeof(buf), &used, "rec000", 42));
    size_t used_before = used;

    /* This second entry would need 25+ bytes and we have <11 left. */
    LS_CHECK(!rec_files_append(buf, sizeof(buf), &used,
                               "aaaaaaaaaaaaaaaaaaaaaaa.sub", 999999));
    LS_EQ_INT((int)used, (int)used_before);
    LS_EQ_STR(buf, "rec000 (42 B)");
    /* Buffer must still be nul-terminated at *used - a walker reading
       past the count is otherwise reading whatever snprintf tried. */
    LS_EQ_INT(buf[used], '\0');
}

LS_CASE(format_wrapper_reports_all_fit)
{
    /* rec_files_format is the shape rec_list uses internally: hand it
       a name/size pair list, get back how many landed and a truncation
       flag.  Short list, plenty of buffer - everyone fits and the flag
       is clear. */
    const char *names[] = { "a", "b", "c" };
    const long   sizes[] = { 1, 2, 3 };
    char buf[128];
    bool trunc = true;   /* seed to opposite of expected */
    int wrote = rec_files_format(buf, sizeof(buf), names, sizes, 3, &trunc);
    LS_EQ_INT(wrote, 3);
    LS_CHECK(!trunc);
    LS_EQ_STR(buf, "a (1 B), b (2 B), c (3 B)");
}

LS_CASE(max_length_names_fill_byte_buffer_before_files_max)
{

    enum { N_ENTRIES = 22 };
    char names[N_ENTRIES][32];
    long sizes[N_ENTRIES];
    const char *name_ptrs[N_ENTRIES];
    for (int i = 0; i < N_ENTRIES; i++) {
        make_max_name(names[i], sizeof(names[i]), i);
        sizes[i] = 12345;                       /* 5-digit body, worst realistic */
        name_ptrs[i] = names[i];
    }

    /* Precondition on the scenario itself: names must be at their real
       maximum, or the test is proving something weaker than the tab
       actually has to handle. */
    LS_EQ_INT((int)strlen(names[0]), 27);

    char buf[APP_REC_LIST_BYTES];
    bool trunc = false;
    int wrote = rec_files_format(buf, sizeof(buf),
                                 (const char *const *)name_ptrs, sizes,
                                 N_ENTRIES, &trunc);

    LS_CHECK(trunc);

    /* Fewer rows landed than the row cap; a total-vs-fit check that
       compared wrote against FILES_MAX would report "no truncation"
       and hide the missing captures.  This is exactly the case
       AppREC::refreshFiles was blind to. */
    LS_CHECK(wrote < APP_REC_FILES_MAX);
    LS_CHECK(wrote < N_ENTRIES);

    /* Buffer must be nul-terminated and end at the boundary of a
       complete entry, not a half-written name.  Peek at the tail: the
       last three characters of a well-formed entry are " B)". */
    size_t used = strlen(buf);
    LS_CHECK(used > 0);
    LS_CHECK(used < sizeof(buf));
    LS_CHECK(strcmp(buf + used - 3, " B)") == 0);
}

LS_CASE(zero_entries_yields_empty_buffer)
{
    /* Empty directory case.  refreshFiles calls this on first paint and
       after a DELETE that removes the last capture.  Must return 0,
       leave the buffer empty and not raise the truncation flag. */
    char buf[16] = "leftover";
    bool trunc = true;
    int wrote = rec_files_format(buf, sizeof(buf), NULL, NULL, 0, &trunc);
    LS_EQ_INT(wrote, 0);
    LS_CHECK(!trunc);
    LS_EQ_STR(buf, "");
}

LS_CASE(null_output_buffer_is_rejected)
{
    /* Defensive: a mis-wired caller must not crash the recorder task.
       Returning 0 and refusing to touch a null buffer keeps the
       failure loud but survivable. */
    LS_CHECK(!rec_files_append(NULL, 32, NULL, "a", 1));
    char buf[16];
    size_t used = 0;
    LS_CHECK(!rec_files_append(buf, 0, &used, "a", 1));

    int wrote = rec_files_format(NULL, 32, NULL, NULL, 0, NULL);
    LS_EQ_INT(wrote, 0);
}

/**/
LS_CASE(row_helper_emits_name_freq_time_columns)
{
    /* The FILES tab has to let a user pick between "was this the good
       capture or the noisy one" without opening either.  A row now
       carries name, frequency in MHz to four decimals, and either the
       sidecar time string or "-" when the sidecar is missing.
       Two-space field separator so the AppREC walker's
       `strchr(p, ' ')` still isolates the name at the first single
       space, and the ", " entry separator is untouched. */
    char buf[128];
    buf[0] = '\0';
    size_t used = 0;

    LS_CHECK(rec_files_append_row(buf, sizeof(buf), &used,
                                  "rec000", 433920000UL,
                                  "2026-03-05T14:22:07Z"));
    LS_EQ_STR(buf, "rec000  433.9200 MHz  2026-03-05T14:22:07Z");

    /* Name is still the first space-delimited token so the DELETE flow
       can extract it with strchr(p, ' '). */
    const char *sp = strchr(buf, ' ');
    LS_CHECK(sp != NULL);
    LS_EQ_INT((int)(sp - buf), 6);        /* "rec000" is six chars */
}

/**/
LS_CASE(row_helper_falls_back_to_dash_when_no_sidecar_time)
{

    char buf[128];
    buf[0] = '\0';
    size_t used = 0;

    LS_CHECK(rec_files_append_row(buf, sizeof(buf), &used,
                                  "rec000", 433920000UL, NULL));
    LS_EQ_STR(buf, "rec000  433.9200 MHz  -");

    buf[0] = '\0'; used = 0;
    LS_CHECK(rec_files_append_row(buf, sizeof(buf), &used,
                                  "rec001", 315000000UL, ""));
    LS_EQ_STR(buf, "rec001  315.0000 MHz  -");
}

/**/
LS_CASE(row_helper_uses_comma_separator_from_second_entry)
{
    /* Same invariant the size-based rec_files_append pinned - a single
       row must not lead with a stray ", " because the AppREC walker
       treats that as a preceding empty entry. */
    char buf[256];
    buf[0] = '\0';
    size_t used = 0;

    LS_CHECK(rec_files_append_row(buf, sizeof(buf), &used,
                                  "rec000", 433920000UL, "T1"));
    LS_CHECK(rec_files_append_row(buf, sizeof(buf), &used,
                                  "rec001", 315000000UL, "T2"));
    LS_EQ_STR(buf,
        "rec000  433.9200 MHz  T1, rec001  315.0000 MHz  T2");
}

/**/
LS_CASE(row_helper_refuses_overflow_atomically)
{
    /* On a refused append the buffer must be left nul-terminated at
       the previous *used and the cursor unchanged, so the walker never
       observes a half-written "rec001  433.92" fragment.  Same
       invariant that keeps rec_files_append safe. */
    char buf[64];
    buf[0] = '\0';
    size_t used = 0;

    LS_CHECK(rec_files_append_row(buf, sizeof(buf), &used,
                                  "rec000", 433920000UL, "T1"));
    size_t used_before = used;

    /* The next entry needs more room than the buffer has left. */
    LS_CHECK(!rec_files_append_row(buf, sizeof(buf), &used,
                                   "rec001", 433920000UL,
                                   "2026-03-05T14:22:07Z"));
    LS_EQ_INT((int)used, (int)used_before);
    LS_EQ_INT(buf[used], '\0');
}

LS_CASE(name_that_alone_would_overflow_scratch_is_refused_without_writing)
{

    char buf[8];
    buf[0] = '\0';
    size_t used = 0;
    LS_CHECK(!rec_files_append(buf, sizeof(buf), &used,
                               "aaaaaaaaaaaaaaaaaaaaaaa.sub", 100));
    LS_EQ_INT((int)used, 0);
    LS_EQ_STR(buf, "");
}
