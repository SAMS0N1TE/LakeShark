/* P25 on the desk, the part with no screen in it: the receiver's per-channel
   record merged into one row a talkgroup, the rows only a list knows, the
   table in every order, what a cell reads and the CSV a row goes out as. */
#include "ls_test.h"
#include "ls_p25_desk.h"

#include <string.h>

#define NOW (20u * 60u * 1000u + 30000u)

static void heard(uint64_t hz, uint16_t tg, uint8_t via, uint32_t ago_ms, int times)
{
    for (int i = 0; i < times; i++)
        LS_CHECK(p25_tg_observed_record(hz, 0x293, tg, via, NOW - ago_ms - 1000u + (uint32_t)i * 10u));
}

static int rows(ls_p25d_row_t *r)
{
    static p25_tg_observed_snapshot_t snap;
    p25_tg_observed_clear();
    heard(851012500ULL, 1041, P25_TG_SEEN_LCW, 5 * 60000u, 12);     /* five minutes ago on one channel */
    heard(851262500ULL, 1041, P25_TG_SEEN_GRANT, 2000u, 30);        /* and just now on another */
    heard(851512500ULL, 2104, P25_TG_SEEN_HDU, 9 * 60000u, 50);
    heard(851762500ULL, 3301, P25_TG_SEEN_LCW, 40000u, 3);
    LS_CHECK(p25_tg_observed_read(&snap));
    return ls_p25d_merge(r, LS_P25D_ROWS, &snap, NOW);
}

static const ls_p25d_row_t *find(const ls_p25d_row_t *r, int n, uint16_t tg)
{
    for (int i = 0; i < n; i++)
        if (r[i].tg == tg) return &r[i];
    return NULL;
}

LS_CASE(channels_merge_into_one_row_a_talkgroup)
{
    static ls_p25d_row_t r[LS_P25D_ROWS];
    const int n = rows(r);
    LS_EQ_INT(n, 3);
    const ls_p25d_row_t *a = find(r, n, 1041);
    LS_CHECK(a != NULL);
    LS_EQ_UINT(a->hits, 42);
    LS_EQ_UINT(a->recent, 42);
    LS_CHECK(a->heard);
    LS_CHECK(a->hz == 851262500ULL);                        /* where it spoke last */
    LS_EQ_INT(a->sources, P25_TG_SEEN_LCW | P25_TG_SEEN_GRANT);
    LS_EQ_INT(a->nac, 0x293);
    /* Its sixteen minutes, oldest first: twelve five minutes back, thirty in the newest. */
    LS_EQ_INT(a->minute[P25_TG_OBSERVED_MINUTES - 1], 30);
    LS_EQ_INT(a->minute[P25_TG_OBSERVED_MINUTES - 1 - 5], 12);
    int sum = 0;
    for (int m = 0; m < P25_TG_OBSERVED_MINUTES; m++) sum += a->minute[m];
    LS_EQ_INT(sum, 42);
    LS_EQ_UINT(find(r, n, 2104)->hits, 50);
}

LS_CASE(a_talkgroup_only_a_list_knows_gets_a_row)
{
    static ls_p25d_row_t r[LS_P25D_ROWS];
    int n = rows(r);
    LS_EQ_INT(ls_p25d_row(r, &n, LS_P25D_ROWS, 1041), (int)(find(r, n, 1041) - r));   /* already there */
    LS_EQ_INT(n, 3);
    const int k = ls_p25d_row(r, &n, LS_P25D_ROWS, 9999);
    LS_EQ_INT(k, 3);
    LS_EQ_INT(n, 4);
    LS_CHECK(!r[k].heard && r[k].hits == 0);
    LS_EQ_INT(ls_p25d_row(r, &n, LS_P25D_ROWS, 0), -1);     /* no talkgroup is no row */
    int full = 2;
    LS_EQ_INT(ls_p25d_row(r, &full, 2, 7777), -1);          /* no room is no row, and no overrun */
    LS_EQ_INT(full, 2);
}

LS_CASE(table_sorts_by_any_column_and_unheard_goes_last)
{
    static ls_p25d_row_t r[LS_P25D_ROWS];
    uint8_t o[LS_P25D_ROWS];
    int n = rows(r);
    ls_p25d_row(r, &n, LS_P25D_ROWS, 500);                  /* locked out, never heard */
    strcpy(r[find(r, n, 1041) - r].name, "FIRE DISPATCH");
    strcpy(r[find(r, n, 3301) - r].name, "ambulance");
    r[find(r, n, 500) - r].flags = LS_PF_LOCKED;
    r[find(r, n, 2104) - r].flags = LS_PF_HELD;
#define TG(i) r[o[i]].tg
    ls_p25d_sort(o, r, n, LS_PT_LAST, false);
    LS_CHECK(TG(0) == 1041 && TG(1) == 3301 && TG(2) == 2104 && TG(3) == 500);    /* most recent first */
    ls_p25d_sort(o, r, n, LS_PT_LAST, true);
    LS_CHECK(TG(0) == 2104 && TG(2) == 1041 && TG(3) == 500);                    /* never heard still last */
    ls_p25d_sort(o, r, n, LS_PT_HITS, false);
    LS_CHECK(TG(0) == 2104 && TG(1) == 1041 && TG(2) == 3301 && TG(3) == 500);    /* busiest first */
    ls_p25d_sort(o, r, n, LS_PT_BUSY, false);
    LS_CHECK(TG(0) == 2104 && TG(1) == 1041);
    ls_p25d_sort(o, r, n, LS_PT_TG, false);
    LS_CHECK(TG(0) == 500 && TG(1) == 1041 && TG(2) == 2104 && TG(3) == 3301);
    ls_p25d_sort(o, r, n, LS_PT_TG, true);
    LS_CHECK(TG(0) == 3301 && TG(3) == 500);
    ls_p25d_sort(o, r, n, LS_PT_NAME, false);
    LS_CHECK(TG(0) == 3301 && TG(1) == 1041);                                    /* case-blind; the unnamed after */
    ls_p25d_sort(o, r, n, LS_PT_CHAN, false);
    LS_CHECK(TG(0) == 1041 && TG(1) == 2104 && TG(2) == 3301 && TG(3) == 500);
    ls_p25d_sort(o, r, n, LS_PT_FLAGS, false);
    LS_CHECK(TG(0) == 500 && TG(1) == 2104);                                     /* locked and held above the plain ones */
    ls_p25d_sort(o, r, 0, LS_PT_TG, false);                                      /* an empty table is no trouble */
#undef TG
}

LS_CASE(cells_and_csv_read_as_the_table_prints_them)
{
    static ls_p25d_row_t r[LS_P25D_ROWS];
    char t[160];
    int n = rows(r);
    ls_p25d_row_t *a = &r[find(r, n, 1041) - r];
    strcpy(a->name, "FIRE DISPATCH");
    a->flags = LS_PF_HELD | LS_PF_ENC;
#define CELL(row, col, want) do { ls_p25d_cell((row), (col), NOW, t, sizeof(t)); LS_EQ_STR(t, want); } while (0)
    CELL(a, LS_PT_TG, "1041");
    CELL(a, LS_PT_NAME, "FIRE DISPATCH");
    CELL(a, LS_PT_HITS, "42");
    CELL(a, LS_PT_LAST, "2s");
    CELL(a, LS_PT_CHAN, "851.2625");
    CELL(a, LS_PT_NAC, "293");
    CELL(a, LS_PT_VIA, "grant lcw");
    CELL(a, LS_PT_FLAGS, "HE");
    CELL(find(r, n, 2104), LS_PT_LAST, "9m");
    CELL(find(r, n, 2104), LS_PT_VIA, "hdu");
    const int k = ls_p25d_row(r, &n, LS_P25D_ROWS, 500);
    r[k].flags = LS_PF_LOCKED | LS_PF_ALLOW;
    CELL(&r[k], LS_PT_LAST, "-");
    CELL(&r[k], LS_PT_HITS, "");
    CELL(&r[k], LS_PT_CHAN, "");
    CELL(&r[k], LS_PT_FLAGS, "LA");
#undef CELL
    LS_CHECK(ls_p25d_match(a, ""));
    LS_CHECK(ls_p25d_match(a, "104"));
    LS_CHECK(ls_p25d_match(a, "dispatch"));
    LS_CHECK(!ls_p25d_match(a, "2104"));
    LS_CHECK(!ls_p25d_match(&r[k], "fire"));
    ls_p25d_csv(a, NOW, t, sizeof(t));
    LS_EQ_STR(t, "1041,FIRE DISPATCH,42,42,2,851.2625,293,grant lcw,1,0,0,1");
    ls_p25d_csv(&r[k], NOW, t, sizeof(t));
    LS_EQ_STR(t, "500,,0,0,,,,,0,1,1,0");
    int head = 0, row = 0;
    for (const char *c = LS_P25D_CSV_HEAD; *c; c++) head += *c == ',';
    for (const char *c = t; *c; c++) row += *c == ',';
    LS_EQ_INT(row, head);
}
