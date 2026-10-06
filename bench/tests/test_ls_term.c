/* TERMINAL's scrollback and history: what a command printed comes back as
   text the grid can draw, the oldest goes first and whole, and the history
   behaves as UP and DOWN expect. */

#include "ls_test.h"
#include "ls_term.h"

#include <stdio.h>
#include <string.h>

static ls_term_scroll_t s_sc;
static char s_out[LS_TERM_SCROLL_BYTES + 1];

LS_CASE(output_comes_back_as_printed_less_what_the_grid_cannot_draw)
{
    ls_term_scroll_reset(&s_sc);
    const char text[] = "heap\r\n\tfree=1\x1b[0m\n\xb0ok\n";
    ls_term_scroll_put(&s_sc, text, sizeof(text) - 1);
    ls_term_scroll_copy(&s_sc, s_out, sizeof(s_out));
    LS_EQ_STR(s_out, "heap\n free=1[0m\n.ok\n");
}

LS_CASE(a_write_is_seen_by_the_change_counter)
{
    ls_term_scroll_reset(&s_sc);
    const uint32_t g = s_sc.gen;
    ls_term_scroll_put(&s_sc, "x", 1);
    LS_CHECK(s_sc.gen != g);
}

LS_CASE(a_full_scrollback_drops_the_oldest_lines_whole)
{
    ls_term_scroll_reset(&s_sc);
    char line[48];
    for (int i = 0; i < 1000; i++) {
        snprintf(line, sizeof(line), "line %04d of the output\n", i);
        ls_term_scroll_put(&s_sc, line, strlen(line));
    }
    const size_t n = ls_term_scroll_copy(&s_sc, s_out, sizeof(s_out));
    LS_CHECK(n > 0 && n < sizeof(s_out));
    LS_CHECK(!strncmp(s_out, "line ", 5));
    LS_CHECK(strstr(s_out, "line 0999 of the output\n") != NULL);
    LS_CHECK(strstr(s_out, "line 0000 ") == NULL);
    /* a smaller copy keeps the newest whole lines */
    char small[64];
    ls_term_scroll_copy(&s_sc, small, sizeof(small));
    LS_EQ_STR(small, "line 0998 of the output\nline 0999 of the output\n");
}

LS_CASE(history_skips_blanks_and_repeats_and_reads_newest_first)
{
    ls_term_history_t h;
    memset(&h, 0, sizeof(h));
    ls_term_history_add(&h, "heap");
    ls_term_history_add(&h, "   ");
    ls_term_history_add(&h, "crumb log");
    ls_term_history_add(&h, "crumb log");
    LS_EQ_INT(h.count, 2);
    LS_EQ_STR(ls_term_history_get(&h, 1), "crumb log");
    LS_EQ_STR(ls_term_history_get(&h, 2), "heap");
    LS_CHECK(ls_term_history_get(&h, 3) == NULL);
    LS_CHECK(ls_term_history_get(&h, 0) == NULL);
}

LS_CASE(a_full_history_forgets_the_oldest)
{
    ls_term_history_t h;
    memset(&h, 0, sizeof(h));
    char cmd[16];
    for (int i = 0; i < LS_TERM_HISTORY + 3; i++) {
        snprintf(cmd, sizeof(cmd), "cmd %d", i);
        ls_term_history_add(&h, cmd);
    }
    LS_EQ_INT(h.count, LS_TERM_HISTORY);
    snprintf(cmd, sizeof(cmd), "cmd %d", LS_TERM_HISTORY + 2);
    LS_EQ_STR(ls_term_history_get(&h, 1), cmd);
    LS_EQ_STR(ls_term_history_get(&h, LS_TERM_HISTORY), "cmd 3");
}
