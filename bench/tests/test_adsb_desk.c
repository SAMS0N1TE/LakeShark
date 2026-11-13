/* ADS-B on the desk, the part with no screen in it: the aircraft table in
   any order with emergencies pinned, what each cell reads, Find, and the
   CSV a row goes out as. */
#include "ls_test.h"
#include "ls_adsb_desk.h"

#include <string.h>

static ls_adsbd_row_t ac(uint32_t icao, const char *call, int alt, int spd, float nm)
{
    ls_adsbd_row_t r;
    memset(&r, 0, sizeof(r));
    r.icao = r.key = icao;
    strcpy(r.call, call);
    strcpy(r.src, "1090");
    r.nm = nm;
    r.brg = nm >= 0 ? 90 : -1;
    if (alt) { r.alt = alt; r.have |= LS_AF_ALT; }
    if (spd) { r.spd = (int16_t)spd; r.have |= LS_AF_SPD; }
    if (nm >= 0) { r.have |= LS_AF_POS; r.lat = 43.5f; r.lon = -71.5f; }
    return r;
}

static void rows(ls_adsbd_row_t *r)
{
    r[0] = ac(0xA1B2C3, "UAL1175", 36000, 462, 41.5f);
    r[1] = ac(0xA77000, "N911MD", 2400, 118, 6.2f);
    r[2] = ac(0x4CA2D1, "", 0, 0, -1);                      /* heard, nothing known yet */
    r[3] = ac(0xC01234, "ACA842", 11500, 301, 18.0f);
    r[1].squawk = 7700;
    r[1].alarm = "EMERGENCY";
    r[0].squawk = 1200;
    r[0].msgs = 900; r[1].msgs = 40; r[2].msgs = 2; r[3].msgs = 300;
    r[0].age_s = 1; r[1].age_s = 3; r[2].age_s = 40; r[3].age_s = 0;
}

#define ORDER(a, b, c, d) (o[0] == (a) && o[1] == (b) && o[2] == (c) && o[3] == (d))

LS_CASE(table_sorts_by_a_column_with_unknowns_last)
{
    ls_adsbd_row_t r[4];
    uint8_t o[4];
    rows(r);
    ls_adsbd_sort(o, r, 4, LS_AC__COUNT, false, false);
    LS_CHECK(ORDER(0, 1, 2, 3));                            /* as tracked */
    ls_adsbd_sort(o, r, 4, LS_AC_DIST, false, false);
    LS_CHECK(ORDER(1, 3, 0, 2));                            /* nearest first, no position last */
    ls_adsbd_sort(o, r, 4, LS_AC_DIST, true, false);
    LS_CHECK(ORDER(0, 3, 1, 2));                            /* and last the other way round too */
    ls_adsbd_sort(o, r, 4, LS_AC_ALT, false, false);
    LS_CHECK(ORDER(1, 3, 0, 2));
    ls_adsbd_sort(o, r, 4, LS_AC_ALT, true, false);
    LS_CHECK(ORDER(0, 3, 1, 2));
    ls_adsbd_sort(o, r, 4, LS_AC_CALL, false, false);
    LS_CHECK(ORDER(3, 1, 0, 2));                            /* ACA, N911, UAL, then the one with no callsign */
    ls_adsbd_sort(o, r, 4, LS_AC_ICAO, false, false);
    LS_CHECK(ORDER(2, 0, 1, 3));
    ls_adsbd_sort(o, r, 4, LS_AC_SQUAWK, false, false);
    LS_CHECK(o[0] == 0 && o[1] == 1);                       /* 1200, 7700, then none heard */
    ls_adsbd_sort(o, r, 4, LS_AC_MSGS, false, false);
    LS_CHECK(ORDER(0, 3, 1, 2));                            /* the busiest first */
    ls_adsbd_sort(o, r, 4, LS_AC_AGE, false, false);
    LS_CHECK(ORDER(3, 0, 1, 2));                            /* the freshest first */
    ls_adsbd_sort(o, r, 0, LS_AC_ALT, false, true);         /* an empty table is no trouble */
}

LS_CASE(an_emergency_is_at_the_top_whatever_the_order)
{
    ls_adsbd_row_t r[4];
    uint8_t o[4];
    rows(r);
    ls_adsbd_sort(o, r, 4, LS_AC_ALT, true, true);
    LS_CHECK(ORDER(1, 0, 3, 2));                            /* highest first, but the emergency above them */
    ls_adsbd_sort(o, r, 4, LS_AC__COUNT, false, true);
    LS_CHECK(ORDER(1, 0, 2, 3));
    ls_adsbd_sort(o, r, 4, LS_AC_ALARM, false, false);
    LS_CHECK(ORDER(1, 0, 2, 3));                            /* the ! column means the same */
    r[2].alarm = "NO COMMS";
    ls_adsbd_sort(o, r, 4, LS_AC_DIST, false, true);
    LS_CHECK(ORDER(1, 2, 3, 0));                            /* two of them: by the column among themselves */
}

LS_CASE(cells_read_as_the_table_prints_them)
{
    ls_adsbd_row_t r[4];
    char t[24];
    rows(r);
    r[0].vs = -1216; r[0].have |= LS_AF_VS;
    r[0].hdg = 7; r[0].have |= LS_AF_HDG;
    r[0].stale = LS_AF_ALT;
#define CELL(i, col, want) do { ls_adsbd_cell(&r[i], (col), t, sizeof(t)); LS_EQ_STR(t, want); } while (0)
    CELL(0, LS_AC_CALL, "UAL1175");
    CELL(2, LS_AC_CALL, "-");
    CELL(2, LS_AC_ICAO, "4CA2D1");
    CELL(0, LS_AC_ALT, "36000*");                           /* not heard for a while */
    CELL(2, LS_AC_ALT, "");
    CELL(0, LS_AC_VS, "-1216");
    CELL(1, LS_AC_VS, "");
    CELL(0, LS_AC_SPD, "462");
    CELL(0, LS_AC_HDG, "007");
    CELL(0, LS_AC_DIST, "41.5");
    CELL(2, LS_AC_DIST, "");
    CELL(0, LS_AC_BRG, "090");
    CELL(1, LS_AC_SQUAWK, "7700");
    CELL(3, LS_AC_SQUAWK, "");
    CELL(1, LS_AC_ALARM, "!");
    CELL(0, LS_AC_ALARM, "");
    CELL(2, LS_AC_AGE, "40s");
    CELL(0, LS_AC_MSGS, "900");
    r[0].nm = 240.4f; CELL(0, LS_AC_DIST, "240");
    r[0].age_s = 400; CELL(0, LS_AC_AGE, "6m");
    r[0].squawk = 21; CELL(0, LS_AC_SQUAWK, "0021");
#undef CELL
    LS_CHECK(ls_adsbd_trend(&r[0]) == 'v');
    r[0].vs = 900; LS_CHECK(ls_adsbd_trend(&r[0]) == '^');
    r[0].vs = 60; LS_CHECK(ls_adsbd_trend(&r[0]) == '=');
    LS_CHECK(ls_adsbd_trend(&r[1]) == ' ');
    /* A buffer too small is cut, never overrun. */
    char tiny[4];
    ls_adsbd_cell(&r[0], LS_AC_CALL, tiny, sizeof(tiny));
    LS_EQ_STR(tiny, "UAL");
}

LS_CASE(find_matches_callsign_address_squawk_and_alarm)
{
    ls_adsbd_row_t r[4];
    rows(r);
    LS_CHECK(ls_adsbd_match(&r[0], ""));
    LS_CHECK(ls_adsbd_match(&r[0], NULL));
    LS_CHECK(ls_adsbd_match(&r[0], "ual"));
    LS_CHECK(ls_adsbd_match(&r[0], "b2c"));
    LS_CHECK(ls_adsbd_match(&r[1], "7700"));
    LS_CHECK(ls_adsbd_match(&r[1], "emerg"));
    LS_CHECK(!ls_adsbd_match(&r[0], "emerg"));
    LS_CHECK(!ls_adsbd_match(&r[2], "ual"));
    LS_CHECK(ls_adsbd_match(&r[2], "4ca"));
}

LS_CASE(a_row_goes_out_as_one_csv_line)
{
    ls_adsbd_row_t r[4];
    char line[200];
    rows(r);
    LS_CHECK(strstr(LS_ADSBD_CSV_HEAD, "icao,callsign,altitude_ft") == LS_ADSBD_CSV_HEAD);
    ls_adsbd_csv(&r[1], line, sizeof(line));
    LS_EQ_STR(line, "A77000,N911MD,2400,,118,,43.50000,-71.50000,6.2,90,7700,EMERGENCY,1090,3,40");
    ls_adsbd_csv(&r[2], line, sizeof(line));
    LS_EQ_STR(line, "4CA2D1,,,,,,,,,,,,1090,40,2");
    /* As many commas as the header, whatever is known. */
    int head = 0, row = 0;
    for (const char *c = LS_ADSBD_CSV_HEAD; *c; c++) head += *c == ',';
    for (const char *c = line; *c; c++) row += *c == ',';
    LS_EQ_INT(row, head);
}
