#include "ls_test.h"
#include "ls_note_blocks.h"
#include "ls_notes.h"
#include "ls_value.h"
#include <math.h>
#include <string.h>

static char out[512];
void ls_gps_get(ls_gps_state_t *g) { memset(g, 0, sizeof(*g)); }

LS_CASE(gps_line_carries_position_quality_and_time_and_parses_back)
{
    ls_gps_state_t g = { .alive = true, .fix = true, .sats_used = 9,
        .lat_deg = 43.2, .lon_deg = -71.65, .alt_m = 120.4f, .hdop = 1.2f,
        .hour = 14, .minute = 21, .second = 3, .year = 2026 };
    LS_CHECK(ls_note_fmt_gps(out, sizeof(out), &g, true) > 0);
    LS_EQ_STR(out, "> GPS 43.200000, -71.650000  alt 120 m  hdop 1.2  9 sats  14:21:03Z\n");
    double lat, lon;
    LS_CHECK(ls_notes_parse_place(out, &lat, &lon));
    LS_NEAR(lat, 43.2, 1e-9);
}

LS_CASE(a_stale_or_absent_fix_says_so_rather_than_printing_old_numbers)
{
    ls_gps_state_t g = { .alive = true, .fix = true, .lat_deg = 1, .lon_deg = 2 };
    ls_note_fmt_gps(out, sizeof(out), &g, false);
    LS_EQ_STR(out, "> GPS no fix (receiver running)\n");
    ls_note_fmt_gps(out, sizeof(out), NULL, false);
    LS_EQ_STR(out, "> GPS no fix\n");
    double lat, lon;
    LS_CHECK(!ls_notes_parse_place(out, &lat, &lon));
}

LS_CASE(heading_bearing_and_map_lines)
{
    ls_note_fmt_heading(out, sizeof(out), 211.6f, 197.2f, 3.4f, true);
    LS_EQ_STR(out, "> HEADING 212 M  197 T  tilt 3  calibrated\n");
    ls_note_fmt_heading(out, sizeof(out), NAN, NAN, 0, false);
    LS_EQ_STR(out, "> HEADING unavailable\n");
    ls_note_fmt_bearing(out, sizeof(out), -5, 12, "MESH", -88, "dBm", 43.1, -71.2, true);
    LS_EQ_STR(out, "> BEARING 355 T +/-12  MESH  -88 dBm  from 43.100000, -71.200000\n");
    double lat, lon;
    LS_CHECK(ls_notes_parse_place(out, &lat, &lon));
    LS_NEAR(lon, -71.2, 1e-9);
    LS_EQ_UINT(ls_note_fmt_bearing(out, sizeof(out), NAN, 0, "X", 0, "", 0, 0, false), 0);
    ls_note_fmt_map(out, sizeof(out), 43.21, -71.66, 15, "0007_x-map1.png");
    LS_EQ_STR(out, "> MAP 43.210000, -71.660000  z15  0007_x-map1.png\n");
    int zoom; char file[40];
    LS_CHECK(ls_notes_parse_map(out, &lat, &lon, &zoom, file, sizeof(file)));
    LS_EQ_STR(file, "0007_x-map1.png");
}

LS_CASE(a_line_that_would_not_fit_is_dropped_whole)
{
    char tiny[12];
    LS_EQ_UINT(ls_note_fmt_map(tiny, sizeof(tiny), 43.21, -71.66, 15, "x.png"), 0);
    LS_EQ_STR(tiny, "");
}

static bool v_freq(ls_val_t *v) { v->kind = LS_VAL_FLOAT; v->f = 154.785f; return true; }
static bool v_nac(ls_val_t *v)  { v->kind = LS_VAL_TEXT; v->s = "527"; return true; }
static bool v_tg(ls_val_t *v)   { v->kind = LS_VAL_INT; v->i = 0; return true; }
static bool v_lvl(ls_val_t *v)  { v->kind = LS_VAL_FLOAT; v->f = 0.42f; return true; }

LS_CASE(radio_line_lists_what_the_group_publishes_and_skips_empty_values)
{
    ls_value_publish("p25.freq", "MHz", v_freq);
    ls_value_publish("p25.nac", NULL, v_nac);
    ls_value_publish("p25.tg", NULL, v_tg);
    ls_value_publish("p25.level", NULL, v_lvl);
    LS_CHECK(ls_note_live_radio(out, sizeof(out), "p25") > 0);
    LS_EQ_STR(out, "> RADIO P25  freq 154.7850 MHz  nac 527  level 0.42\n");
    LS_EQ_UINT(ls_note_live_radio(out, sizeof(out), "fm"), 0);
    const char *groups[8], *labels[8];
    int n = ls_note_radio_groups(groups, labels, 8);
    LS_EQ_INT(n, 1);
    LS_EQ_STR(labels[0], "P25");
}
