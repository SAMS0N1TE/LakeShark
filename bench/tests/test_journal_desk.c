/* JOURNAL on the desk, the part with no screen in it: the entry table in
   any order, what a cell reads, Find, and a note wrapped for its pane. */
#include "ls_test.h"
#include "ls_journal_desk.h"

#include <string.h>

static ls_jrnd_row_t entry(int index, const char *title, const char *when, const char *source, uint32_t freq, float rssi)
{
    ls_jrnd_row_t r;
    memset(&r, 0, sizeof(r));
    r.index = (uint16_t)index;
    r.id = 100u + (uint32_t)index;
    strcpy(r.title, title);
    strcpy(r.when, when);
    r.clock = when[0] == '2';
    r.time_us = 1000000LL * (50 - index);
    strcpy(r.source, source);
    r.freq = freq;
    if (rssi != 0) { r.signal = true; r.rssi = rssi; }
    return r;
}

static void rows(ls_jrnd_row_t *r)
{
    r[0] = entry(0, "bridge survey", "2026-11-12 14:02:11Z", "RTL-SDR", 162550000, -71);
    r[1] = entry(1, "Antenna test", "2026-11-12 09:30:00Z", "MESH", 910525000, -98);
    r[2] = entry(2, "", "0:04:10", "GPS", 0, 0);                    /* before the clock was set, no title */
    r[3] = entry(3, "Camp", "2026-11-10 20:15:42Z", "GPS", 0, 0);
    r[0].gps = true; r[0].lat = 43.44; r[0].lon = -71.64;
    r[3].gps = true; r[3].lat = 44.10; r[3].lon = -71.20;
    r[0].chars = 120; r[1].chars = 40; r[2].chars = 0; r[3].chars = 700;
    r[0].saved = r[3].saved = true;
}

#define ORDER(a, b, c, d) (o[0] == (a) && o[1] == (b) && o[2] == (c) && o[3] == (d))

LS_CASE(entries_sort_by_any_column)
{
    ls_jrnd_row_t r[4];
    uint8_t o[4];
    rows(r);
    ls_jrnd_sort(o, r, 4, LS_JC_WHEN, false);
    LS_CHECK(ORDER(0, 1, 3, 2));                    /* newest first; the one with no clock after the stamped ones */
    ls_jrnd_sort(o, r, 4, LS_JC_WHEN, true);
    LS_CHECK(ORDER(2, 3, 1, 0));
    ls_jrnd_sort(o, r, 4, LS_JC_TITLE, false);
    LS_CHECK(ORDER(1, 0, 3, 2));                    /* case-blind; the untitled one last */
    ls_jrnd_sort(o, r, 4, LS_JC_TITLE, true);
    LS_CHECK(ORDER(3, 0, 1, 2));                    /* and last the other way round too */
    ls_jrnd_sort(o, r, 4, LS_JC_FREQ, false);
    LS_CHECK(o[0] == 0 && o[1] == 1);               /* 162.55, 910.525, then none */
    ls_jrnd_sort(o, r, 4, LS_JC_RSSI, false);
    LS_CHECK(o[0] == 0 && o[1] == 1);               /* the strongest first */
    ls_jrnd_sort(o, r, 4, LS_JC_PLACE, false);
    LS_CHECK(o[0] == 3 && o[1] == 0);               /* north to south, then the unplaced */
    ls_jrnd_sort(o, r, 4, LS_JC_SIZE, false);
    LS_CHECK(ORDER(3, 0, 1, 2));
    ls_jrnd_sort(o, r, 4, LS_JC_KEPT, false);
    LS_CHECK(ORDER(0, 3, 1, 2));                    /* on the card first, each group as it was */
    ls_jrnd_sort(o, r, 4, LS_JC_SOURCE, false);
    LS_CHECK(ORDER(2, 3, 1, 0));
    ls_jrnd_sort(o, r, 0, LS_JC_WHEN, false);
}

LS_CASE(cells_read_as_the_table_prints_them)
{
    ls_jrnd_row_t r[4];
    char t[32];
    rows(r);
#define CELL(i, col, want) do { ls_jrnd_cell(&r[i], (col), t, sizeof(t)); LS_EQ_STR(t, want); } while (0)
    CELL(0, LS_JC_TITLE, "bridge survey");
    CELL(2, LS_JC_TITLE, "(no title)");
    CELL(0, LS_JC_WHEN, "2026-11-12 14:02:11Z");
    CELL(0, LS_JC_FREQ, "162.5500");
    CELL(2, LS_JC_FREQ, "");
    CELL(1, LS_JC_RSSI, "-98");
    CELL(3, LS_JC_RSSI, "");
    CELL(0, LS_JC_PLACE, "43.4400 -71.6400");
    CELL(1, LS_JC_PLACE, "");
    CELL(3, LS_JC_SIZE, "700");
    CELL(0, LS_JC_KEPT, "SD");
    CELL(1, LS_JC_KEPT, "RAM");
#undef CELL
    LS_CHECK(ls_jrnd_holds("Bridge Survey", "SURV"));
    LS_CHECK(ls_jrnd_holds("anything", ""));
    LS_CHECK(ls_jrnd_holds("anything", NULL));
    LS_CHECK(!ls_jrnd_holds("bridge", "camp"));
    LS_CHECK(!ls_jrnd_holds(NULL, "camp"));
}

LS_CASE(a_note_wraps_at_spaces_and_keeps_its_own_line_breaks)
{
    const char *text = "First line of the note.\n\nA second paragraph that is long enough to need wrapping at the pane's width.";
    char line[64];
    int off = 0;
    off = ls_jrnd_wrap(text, off, 30, line, sizeof(line)); LS_EQ_STR(line, "First line of the note.");
    off = ls_jrnd_wrap(text, off, 30, line, sizeof(line)); LS_EQ_STR(line, "");                /* the blank line is kept */
    off = ls_jrnd_wrap(text, off, 30, line, sizeof(line)); LS_EQ_STR(line, "A second paragraph that is");
    off = ls_jrnd_wrap(text, off, 30, line, sizeof(line)); LS_EQ_STR(line, "long enough to need wrapping");
    off = ls_jrnd_wrap(text, off, 30, line, sizeof(line)); LS_EQ_STR(line, "at the pane's width.");
    LS_CHECK(text[off] == 0);
    LS_EQ_INT(ls_jrnd_lines(text, 30), 5);
    LS_EQ_INT(ls_jrnd_lines("", 30), 0);
    LS_EQ_INT(ls_jrnd_lines(NULL, 30), 0);
    LS_EQ_INT(ls_jrnd_lines("one", 30), 1);
    /* A word longer than the pane is cut, never dropped, and nothing loops. */
    LS_EQ_INT(ls_jrnd_lines("ABCDEFGHIJ", 4), 3);
    LS_EQ_INT(ls_jrnd_lines("ABCDEFGHIJ", 0), 10);
}
