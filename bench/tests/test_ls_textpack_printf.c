/* LS_SNPRINTF / LS_FPRINTF / LS_PRINTF in a packed build go through
   ls_text_snprintf and friends, which look the format up by id and hand the
   arguments on as a va_list. Every byte and every return value must be what
   the plain call with the literal gives. Its ids are packed by the bench's
   generator run (bench/CMakeLists.txt, LS_TEXT_SCAN). */
#include "ls_test.h"
#include "ls_textpack.h"

#include <stdio.h>
#include <string.h>

LS_CASE(snprintf_with_packed_format_matches_the_literal)
{
    char a[96], b[96];
    const int ra = LS_SNPRINTF(tp_test_mixed, a, sizeof(a),
                               "%-8s|%5d|%+.3f|%lu|%c|%%|%08x|%s", "name", -42, 3.14159,
                               (unsigned long)4000000000u, 'Z', 0xBEEFu, "tail");
    const int rb = snprintf(b, sizeof(b), "%-8s|%5d|%+.3f|%lu|%c|%%|%08x|%s", "name", -42, 3.14159,
                            (unsigned long)4000000000u, 'Z', 0xBEEFu, "tail");
    LS_EQ_INT(ra, rb);
    LS_EQ_STR(a, b);
}

LS_CASE(snprintf_truncates_and_counts_like_snprintf)
{
    char a[12], b[12];
    memset(a, 'x', sizeof(a));
    memset(b, 'x', sizeof(b));
    const int ra = LS_SNPRINTF(tp_test_long, a, sizeof(a), "a long line %d that will not fit", 12345);
    const int rb = snprintf(b, sizeof(b), "a long line %d that will not fit", 12345);
    LS_EQ_INT(ra, rb);
    LS_CHECK(memcmp(a, b, sizeof(a)) == 0);
}

LS_CASE(fprintf_with_packed_format_writes_the_same_bytes)
{
    FILE *f = fopen("ls_textpack_printf.tmp", "w+");
    LS_CHECK(f != NULL);
    if (!f) return;
    const int ra = LS_FPRINTF(tp_test_stream, f, "stream %s %d %.1f\n", "ok", 7, 2.5);
    const int rb = fprintf(f, "stream %s %d %.1f\n", "ok", 7, 2.5);
    LS_EQ_INT(ra, rb);
    rewind(f);
    char line1[64] = {0}, line2[64] = {0};
    LS_CHECK(fgets(line1, sizeof(line1), f) != NULL);
    LS_CHECK(fgets(line2, sizeof(line2), f) != NULL);
    LS_EQ_STR(line1, line2);
    fclose(f);
    remove("ls_textpack_printf.tmp");
}

LS_CASE(printf_with_packed_format_returns_the_same_count)
{
    const int ra = LS_PRINTF(tp_test_console, "packed printf check %d %s\n", 3, "ok");
    const int rb = printf("packed printf check %d %s\n", 3, "ok");
    LS_EQ_INT(ra, rb);
}
