/* RF SURVEY core: plans, the survey file, the floor, the heat grid, export. */
#include "ls_test.h"
#include "ls_rfs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ls_rfs_grid_t g;

static void header(ls_rfs_header_t *h, int nch)
{
    memset(h, 0, sizeof(*h));
    h->version = 1; h->nch = (uint16_t)nch; h->start_utc = 1790000000u; h->cell_m = 25; h->threshold_cdb = 1000;
    snprintf(h->source, sizeof(h->source), "RTL-SDR");
    snprintf(h->plan, sizeof(h->plan), "PAGERS");
    for (int i = 0; i < nch; i++) {
        h->ch[i].hz = 152000000u + 25000u * i; h->ch[i].bw_hz = 12500;
        snprintf(h->ch[i].label, sizeof(h->ch[i].label), "C%d", i % 100);
    }
}

LS_CASE(every_preset_is_a_usable_plan)
{
    LS_CHECK(ls_rfs_preset_count() >= 10);
    for (int i = 0; i < ls_rfs_preset_count(); i++) {
        ls_rfs_plan_t p;
        LS_CHECK(ls_rfs_preset(i, &p));
        LS_CHECK(p.n >= 1 && p.n <= LS_RFS_CH_MAX);
        LS_CHECK(p.name[0]);
        for (int k = 0; k < p.n; k++) { LS_CHECK(p.ch[k].hz > 1000000u); LS_CHECK(p.ch[k].bw_hz >= 1000u); LS_CHECK(p.ch[k].label[0]); }
    }
    ls_rfs_plan_t p;
    LS_CHECK(!ls_rfs_preset(-1, &p));
    LS_CHECK(!ls_rfs_preset(ls_rfs_preset_count(), &p));
}

LS_CASE(a_range_is_cut_into_equal_cells)
{
    ls_rfs_plan_t p;
    LS_CHECK(ls_rfs_plan_range(&p, "ISM", 902000000u, 928000000u, 13));
    LS_EQ_INT(p.n, 13);
    LS_EQ_UINT(p.ch[0].bw_hz, 2000000u);
    LS_EQ_UINT(p.ch[0].hz, 903000000u);
    LS_EQ_UINT(p.ch[12].hz, 927000000u);
    LS_EQ_STR(p.ch[0].label, "903.0000");
    LS_CHECK(!ls_rfs_plan_range(&p, "X", 900000000u, 900000000u, 4));
    LS_CHECK(!ls_rfs_plan_range(&p, "X", 900000000u, 901000000u, 33));
}

LS_CASE(the_plan_file_reads_and_refuses_strictly)
{
    static ls_rfs_plan_t out[4];
    char why[80];
    const char *ok = "{ \"schema\": \"lakeshark.rfsurvey.plans\", \"version\": 1, \"note\": [1, {\"x\": true}],"
                     " \"plans\": [ { \"name\": \"MY PAGERS\", \"channels\": [ { \"hz\": 152007500, \"bw\": 12500, \"label\": \"P1\" },"
                     " { \"hz\": 157740000 } ] }, { \"name\": \"MY ISM\", \"range\": { \"lo\": 902000000, \"hi\": 928000000, \"cells\": 16 } } ] }";
    LS_EQ_INT(ls_rfs_plans_parse(ok, strlen(ok), out, 4, why, sizeof(why)), 2);
    LS_EQ_STR(out[0].name, "MY PAGERS");
    LS_EQ_INT(out[0].n, 2);
    LS_EQ_STR(out[0].ch[0].label, "P1");
    LS_EQ_UINT(out[0].ch[1].bw_hz, 12500u);
    LS_EQ_STR(out[0].ch[1].label, "157.7400");
    LS_EQ_INT(out[1].n, 16);
    LS_CHECK(out[1].range);
    static const char *const bad[] = {
        "{ \"schema\": \"other\", \"version\": 1, \"plans\": [] }",
        "{ \"schema\": \"lakeshark.rfsurvey.plans\", \"version\": 2, \"plans\": [] }",
        "{ \"schema\": \"lakeshark.rfsurvey.plans\", \"version\": 1, \"plans\": [ { \"name\": \"X\" } ] }",
        "{ \"schema\": \"lakeshark.rfsurvey.plans\", \"version\": 1, \"plans\": [ { \"name\": \"X\", \"channels\": [ { \"hz\": 5 } ] } ] }",
        "{ \"schema\": \"lakeshark.rfsurvey.plans\", \"version\": 1, \"plans\": [ { \"name\": \"X\", \"range\": { \"lo\": 9e8 } } ] }",
        "{ \"schema\": \"lakeshark.rfsurvey.plans\", \"version\": 1, \"plans\": [ { \"name\": \"X\", \"channels\": [ ",
        "[]",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        why[0] = 0;
        LS_CHECK_MSG(ls_rfs_plans_parse(bad[i], strlen(bad[i]), out, 4, why, sizeof(why)) == -1, "case %u", i);
        LS_CHECK_MSG(why[0], "case %u has no reason", i);
    }
}

LS_CASE(the_default_plan_file_on_data_is_valid)
{
    FILE *f = fopen(LS_RFS_PLANS_FILE, "rb");
    LS_CHECK(f != NULL);
    if (!f) return;
    static char text[16384];
    const size_t n = fread(text, 1, sizeof(text), f);
    fclose(f);
    static ls_rfs_plan_t out[16];
    char why[80] = "";
    const int got = ls_rfs_plans_parse(text, n, out, 16, why, sizeof(why));
    LS_CHECK_MSG(got >= 1, "%s", why);
}

LS_CASE(a_survey_file_round_trips)
{
    ls_rfs_header_t h, back;
    header(&h, 7);
    uint8_t buf[1024];
    const size_t hn = ls_rfs_header_pack(&h, buf, sizeof(buf));
    LS_EQ_INT((int)hn, 64 + 16 * 7);
    ls_rfs_row_t rows[3];
    for (int r = 0; r < 3; r++) {
        memset(&rows[r], 0, sizeof(rows[r]));
        rows[r].t_ms = 1000u * r; rows[r].utc_s = h.start_utc + r;
        rows[r].lat = 43.2081234 + r * 1e-4; rows[r].lon = -71.5376543 - r * 1e-4;
        rows[r].flags = LS_RFS_ROW_FIX; rows[r].pass = (uint16_t)(65534 + r); rows[r].occ = 0x15u >> r;
        for (int c = 0; c < 7; c++) { rows[r].level[c] = (int16_t)(-9000 + 37 * c - r); rows[r].floor[c] = (int16_t)(-10500 + c); }
        rows[r].level[3] = LS_RFS_NO_LEVEL;
    }
    size_t at = hn;
    for (int r = 0; r < 3; r++) at += ls_rfs_row_pack(&h, &rows[r], buf + at, sizeof(buf) - at);
    LS_EQ_INT((int)at, (int)(hn + 3 * (24 + 4 * 7)));
    /* A torn last row is ignored by length. */
    const size_t len = at - 5;

    const size_t got = ls_rfs_header_unpack(buf, len, &back);
    LS_EQ_INT((int)got, (int)hn);
    LS_EQ_INT(back.nch, 7);
    LS_EQ_STR(back.source, "RTL-SDR");
    LS_EQ_STR(back.plan, "PAGERS");
    LS_EQ_INT(back.threshold_cdb, 1000);
    LS_EQ_INT(back.cell_m, 25);
    LS_EQ_UINT(back.start_utc, h.start_utc);
    for (int c = 0; c < 7; c++) { LS_EQ_UINT(back.ch[c].hz, h.ch[c].hz); LS_EQ_STR(back.ch[c].label, h.ch[c].label); }

    int read = 0;
    for (size_t p = got; p + back.row_bytes <= len; p += back.row_bytes) {
        ls_rfs_row_t r;
        LS_CHECK(ls_rfs_row_unpack(&back, buf + p, len - p, &r));
        LS_NEAR(r.lat, rows[read].lat, 1e-7);
        LS_NEAR(r.lon, rows[read].lon, 1e-7);
        LS_EQ_UINT(r.pass, rows[read].pass);
        LS_EQ_UINT(r.occ, rows[read].occ);
        LS_EQ_UINT(r.flags, LS_RFS_ROW_FIX);
        for (int c = 0; c < 7; c++) { LS_EQ_INT(r.level[c], rows[read].level[c]); LS_EQ_INT(r.floor[c], rows[read].floor[c]); }
        LS_EQ_INT(r.level[7], LS_RFS_NO_LEVEL);
        read++;
    }
    LS_EQ_INT(read, 2);

    /* Not ours, another version, or a header that lies about its size. */
    uint8_t bad[1024];
    memcpy(bad, buf, hn); bad[0] = 'X';
    LS_EQ_INT((int)ls_rfs_header_unpack(bad, hn, &back), 0);
    memcpy(bad, buf, hn); bad[8] = 2;
    LS_EQ_INT((int)ls_rfs_header_unpack(bad, hn, &back), 0);
    memcpy(bad, buf, hn); bad[12] = 40;
    LS_EQ_INT((int)ls_rfs_header_unpack(bad, hn, &back), 0);
    LS_EQ_INT((int)ls_rfs_header_unpack(buf, hn - 1, &back), 0);
}

LS_CASE(the_floor_drops_at_once_and_climbs_slowly)
{
    ls_rfs_floor_t f;
    ls_rfs_floor_reset(&f);
    LS_NEAR(ls_rfs_floor_update(&f, 0, -100), -100, 1e-6);
    LS_NEAR(ls_rfs_floor_update(&f, 0, -60), -99.95, 1e-4);    /* a carrier keys up */
    for (int i = 0; i < 99; i++) ls_rfs_floor_update(&f, 0, -60);
    LS_NEAR(f.floor[0], -95.0, 1e-3);                          /* a hundred passes on, 35 dB still over */
    LS_NEAR(ls_rfs_floor_update(&f, 0, -110), -110, 1e-6);
    LS_CHECK(isnan(ls_rfs_floor_update(&f, LS_RFS_CH_MAX, -50)));
}

static ls_rfs_row_t row_at(double lat, double lon, int16_t level0, uint32_t occ)
{
    ls_rfs_row_t r;
    memset(&r, 0, sizeof(r));
    r.lat = lat; r.lon = lon; r.occ = occ;
    for (int i = 0; i < LS_RFS_CH_MAX; i++) { r.level[i] = LS_RFS_NO_LEVEL; r.floor[i] = LS_RFS_NO_LEVEL; }
    r.level[0] = level0; r.floor[0] = -10000;
    r.level[1] = -9500; r.floor[1] = -10000;
    return r;
}

LS_CASE(heat_bins_rows_into_square_cells)
{
    ls_rfs_grid_reset(&g, 25, 2);
    const double lat0 = 43.2, lon0 = -71.5;
    const double dlat = 25.0 / 111132.0, dlon = 25.0 / (111132.0 * cos(lat0 * M_PI / 180));
    ls_rfs_row_t r = row_at(lat0 + 0.2 * dlat, lon0 + 0.2 * dlon, -6000, 1);
    const int a = ls_rfs_grid_add(&g, &r);
    LS_EQ_INT(a, 0);
    /* Same cell. */
    r = row_at(lat0 + 0.7 * dlat, lon0 + 0.6 * dlon, -8000, 0);
    LS_EQ_INT(ls_rfs_grid_add(&g, &r), 0);
    /* One cell east, one cell south-west. */
    r = row_at(lat0 + 0.5 * dlat, lon0 + 1.5 * dlon, -7000, 1);
    LS_EQ_INT(ls_rfs_grid_add(&g, &r), 1);
    r = row_at(lat0 - 0.5 * dlat, lon0 - 0.5 * dlon, -9000, 0);
    LS_EQ_INT(ls_rfs_grid_add(&g, &r), 2);
    LS_EQ_INT(g.count, 3);
    LS_EQ_INT(g.cell[1].ix, 1); LS_EQ_INT(g.cell[1].iy, 0);
    LS_EQ_INT(g.cell[2].ix, -1); LS_EQ_INT(g.cell[2].iy, -1);
    LS_EQ_INT(ls_rfs_grid_find(&g, 1, 0), 1);
    LS_EQ_INT(ls_rfs_grid_find(&g, 5, 5), -1);

    float v;
    LS_CHECK(ls_rfs_grid_value(&g, 0, 0, LS_RFS_MAX, &v));  LS_NEAR(v, -60, 1e-4);
    LS_CHECK(ls_rfs_grid_value(&g, 0, 0, LS_RFS_MEAN, &v)); LS_NEAR(v, -70, 1e-4);
    LS_CHECK(ls_rfs_grid_value(&g, 0, 0, LS_RFS_OCC, &v));  LS_NEAR(v, 50, 1e-4);
    LS_CHECK(ls_rfs_grid_value(&g, 0, 0, LS_RFS_OVER, &v)); LS_NEAR(v, 30, 1e-4);
    /* The loudest channel, for "every channel". */
    LS_CHECK(ls_rfs_grid_value(&g, 2, -1, LS_RFS_MAX, &v)); LS_NEAR(v, -90, 1e-4);
    LS_CHECK(ls_rfs_grid_value(&g, 0, -1, LS_RFS_MAX, &v)); LS_NEAR(v, -60, 1e-4);
    LS_CHECK(!ls_rfs_grid_value(&g, 0, 5, LS_RFS_MAX, &v));
    float lo, hi;
    LS_CHECK(ls_rfs_grid_range(&g, 0, LS_RFS_MAX, &lo, &hi));
    LS_NEAR(lo, -90, 1e-4); LS_NEAR(hi, -60, 1e-4);

    /* A cell's centre is the middle of the cell it names, counted from the
       first place the grid was given. */
    double clat, clon;
    ls_rfs_grid_centre(&g, 1, &clat, &clon);
    LS_NEAR(clat, g.lat0 + 0.5 * dlat, 1e-7);
    LS_NEAR(clon, g.lon0 + 1.5 * 25.0 / g.m_per_deg_lon, 1e-7);
    LS_NEAR(g.lat0, lat0 + 0.2 * dlat, 1e-9);
}

LS_CASE(a_full_grid_drops_new_places_and_says_so)
{
    ls_rfs_grid_reset(&g, 10, 1);
    int kept = 0;
    for (int i = 0; i < LS_RFS_CELLS + 20; i++) {
        ls_rfs_row_t r = row_at(43.0 + (i / 40) * 15.0 / 111132.0, -71.0 + (i % 40) * 15.0 / 81000.0, -5000, 0);
        if (ls_rfs_grid_add(&g, &r) >= 0) kept++;
    }
    LS_EQ_INT(g.count, LS_RFS_CELLS);
    LS_CHECK(g.dropped >= 20);
    /* Every cell is still found by its own index. */
    for (int i = 0; i < g.count; i += 37) LS_EQ_INT(ls_rfs_grid_find(&g, g.cell[i].ix, g.cell[i].iy), i);
}

LS_CASE(rows_are_kept_every_n_metres)
{
    LS_CHECK(ls_rfs_moved_enough(false, 0, 0, 43, -71, 50));
    LS_CHECK(ls_rfs_moved_enough(true, 43, -71, 43, -71, 0));
    LS_CHECK(!ls_rfs_moved_enough(true, 43, -71, 43 + 40 / 111195.0, -71, 50));
    LS_CHECK(ls_rfs_moved_enough(true, 43, -71, 43 + 60 / 111195.0, -71, 50));
    LS_NEAR(ls_rfs_distance_m(0, 0, 0, 1), 111195, 5);
}

LS_CASE(export_writes_csv_and_geojson)
{
    ls_rfs_header_t h;
    header(&h, 3);
    ls_rfs_row_t r = row_at(43.25, -71.5, -6512, 1);
    r.utc_s = 1790000001u; r.t_ms = 1500; r.flags = LS_RFS_ROW_FIX;
    char line[512];
    ls_rfs_csv_header(line, sizeof(line));
    LS_CHECK(!strncmp(line, "utc_s,t_ms,lat,lon,fix,channel", 30));
    ls_rfs_csv_line(&h, &r, 0, true, line, sizeof(line));
    LS_EQ_STR(line, "1790000001,1500,43.2500000,-71.5000000,gps,0,C0,152000000,12500,-65.12,-100.00,1\n");
    ls_rfs_csv_line(&h, &r, 2, false, line, sizeof(line));
    LS_EQ_STR(line, "1790000001,1500,43.2500000,-71.5000000,gps,2,C2,152050000,12500,,,0\n");

    static char doc[4096];
    int n = ls_rfs_geojson_begin(&h, doc, sizeof(doc));
    n += ls_rfs_geojson_feature(&h, &r, true, doc + n, sizeof(doc) - n);
    r.flags = LS_RFS_ROW_SIM;
    n += ls_rfs_geojson_feature(&h, &r, false, doc + n, sizeof(doc) - n);
    n += ls_rfs_geojson_end(doc + n, sizeof(doc) - n);
    LS_CHECK(strstr(doc, "\"type\":\"FeatureCollection\"") != NULL);
    LS_CHECK(strstr(doc, "\"coordinates\":[-71.5000000,43.2500000]") != NULL);
    LS_CHECK(strstr(doc, "\"level_db\":[-65.12,-95.00,null]") != NULL);
    LS_CHECK(strstr(doc, "\"occupied\":[1,0,0]") != NULL);
    LS_CHECK(strstr(doc, "\"fix\":\"sim\"") != NULL);
    /* Balanced brackets: it is one JSON document. */
    int depth = 0, low = 0;
    for (const char *p = doc; *p; p++) { depth += (*p == '{' || *p == '[') - (*p == '}' || *p == ']'); if (depth < low) low = depth; }
    LS_EQ_INT(depth, 0);
    LS_EQ_INT(low, 0);
}
