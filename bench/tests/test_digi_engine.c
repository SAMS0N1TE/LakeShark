/* DIGI runtime pieces that run on the host: grid squares, bearings, the band
   table, and the decode list the screen reads. */
#include "ls_test.h"
#include "digi_engine.h"
#include "digi_ftx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

LS_CASE(grid_squares_map_to_their_centres)
{
    double la, lo;
    LS_CHECK(digi_grid_to_ll("FN42", &la, &lo));
    LS_NEAR(la, 42.5, 1e-9);
    LS_NEAR(lo, -71.0, 1e-9);
    LS_CHECK(digi_grid_to_ll("JO62qm", &la, &lo));
    LS_NEAR(la, 52.52, 0.03);
    LS_NEAR(lo, 13.37, 0.05);
    LS_CHECK(!digi_grid_to_ll("ZZ99", &la, &lo));
    LS_CHECK(!digi_grid_to_ll("FN4", &la, &lo));
    char g[7];
    digi_ll_to_grid(38.627, -90.199, g);     /* St. Louis */
    LS_EQ_STR(g, "EM48vp");
    digi_ll_to_grid(-33.87, 151.21, g);      /* Sydney */
    LS_CHECK(!strncmp(g, "QF56", 4));
}

LS_CASE(bearing_and_distance_are_great_circle)
{
    float b, km;
    digi_bearing_range(38.6, -90.2, 42.5, -71.0, &b, &km);   /* EM48 -> FN42 */
    LS_NEAR(km, 1690.0, 30.0);
    LS_NEAR(b, 70.0, 4.0);
    digi_bearing_range(51.5, 0.0, -33.9, 151.2, &b, &km);    /* London -> Sydney */
    LS_NEAR(km, 17000.0, 100.0);
}

LS_CASE(bands_have_standard_dials)
{
    int n = digi_band_count(), b20 = -1;
    for (int i = 0; i < n; ++i) if (!strcmp(digi_band(i)->band, "20m")) b20 = i;
    LS_CHECK(b20 >= 0);
    LS_EQ_UINT(digi_band_dial(b20, DIGI_MODE_FT8), 14074000u);
    LS_EQ_UINT(digi_band_dial(b20, DIGI_MODE_FT4), 14080000u);
    LS_EQ_UINT(digi_band_dial(b20, DIGI_MODE_WSPR), 14095600u);
    LS_CHECK(digi_band(n) == NULL);
    for (int i = 0; i < n; ++i) LS_EQ_STR(digi_band_names[i], digi_band(i)->band);
}

LS_CASE(the_list_is_newest_first_with_rising_sequence)
{
    digi_clear();
    digi_row_t r;
    memset(&r, 0, sizeof(r));
    strcpy(r.text, "CQ K1ABC FN42");
    digi_host_add(&r);
    strcpy(r.text, "K1ABC W9XYZ -11");
    digi_host_add(&r);
    static digi_view_t v;
    digi_view(&v);
    LS_EQ_INT(v.n, 2);
    LS_EQ_STR(v.row[0].text, "K1ABC W9XYZ -11");
    LS_CHECK(v.row[0].seq > v.row[1].seq);
    LS_EQ_UINT(v.total, 2u);
    for (int i = 0; i < DIGI_VIEW_ROWS + 5; ++i) digi_host_add(&r);
    digi_view(&v);
    LS_EQ_INT(v.n, DIGI_VIEW_ROWS);
    digi_clear();
    digi_view(&v);
    LS_EQ_INT(v.n, 0);
}

LS_CASE(streamed_signal_matches_the_whole_one)
{
    uint8_t p[10], tones[105];
    digi_msg_pack("CQ K1ABC FN42", p, NULL);
    digi_ftx_tones(DIGI_FT8, p, tones);
    const int n = 15 * 12000;
    float *a = calloc((size_t)n, sizeof(float)), *b = calloc((size_t)n, sizeof(float));
    digi_ftx_synth(DIGI_FT8, tones, 1000.0f, 0.5f, a, n, 6000);
    digi_ftx_tx_t tx;
    LS_CHECK(digi_ftx_tx_begin(&tx, DIGI_FT8, p, 1000.0f, 0.5f, 6000));
    for (int at = 0; at < n; at += 240) digi_ftx_tx_add(&tx, b + at, n - at < 240 ? n - at : 240, at);
    double err = 0;
    for (int i = 0; i < n; ++i) err = fmax(err, fabs(a[i] - b[i]));
    LS_CHECK(err < 1e-4);
    free(a);
    free(b);
}
