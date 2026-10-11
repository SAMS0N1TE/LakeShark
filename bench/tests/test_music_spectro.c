/* The waterfall's arithmetic: bins per pixel row, the axis both ways, the
   label ticks and the power code. */
#include "ls_test.h"
#include "ls_spectro.h"
#include "ls_music_backend.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { N = LS_MUSIC_FFT_SIZE };

/* Linear, 0 to Nyquist: every bin belongs to exactly one row, rows run top
   to bottom from high to low, and a tone lands in the row the axis says. */
LS_CASE(linear_rows_cover_every_bin_once){
    enum { ROWS = 510 };
    static int16_t first[ROWS], last[ROWS];
    ls_spectro_rows(ROWS, N, 44100, false, 22050.0f, first, last);
    LS_EQ_INT(first[ROWS - 1], 0);
    LS_EQ_INT(last[0], N / 2 - 1);
    int expect = 0;
    for (int y = ROWS - 1; y >= 0; y--) {
        LS_CHECK(first[y] <= last[y]);
        /* With more bins than rows nothing repeats and nothing is skipped. */
        LS_EQ_INT(first[y], expect);
        expect = last[y] + 1;
    }
    LS_EQ_INT(expect, N / 2);
    /* The row that shows bin 46's centre, 990.5 Hz, reads bin 46. */
    int row = ls_spectro_hz_row(46 * 44100.0f / N, ROWS, false, 22050.0f);
    LS_CHECK(first[row] <= 46 && last[row] >= 46);
    LS_EQ_INT(ls_spectro_hz_row(0.0f, ROWS, false, 22050.0f), ROWS - 1);
    LS_EQ_INT(ls_spectro_hz_row(22050.0f, ROWS, false, 22050.0f), 0);
    LS_EQ_INT(ls_spectro_hz_row(30000.0f, ROWS, false, 22050.0f), -1);
}

/* Zoomed past the FFT's resolution, rows repeat the nearest bin rather than
   invent detail, and stay in order. */
LS_CASE(zoomed_rows_repeat_the_nearest_bin){
    enum { ROWS = 510 };
    static int16_t first[ROWS], last[ROWS];
    /* 0 .. 2756 Hz on 510 rows: 5.4 Hz a row against 21.5 Hz a bin. */
    ls_spectro_rows(ROWS, N, 44100, false, 22050.0f / 8, first, last);
    int repeats = 0;
    for (int y = ROWS - 1; y > 0; y--) {
        LS_EQ_INT(first[y], last[y]);
        LS_CHECK(first[y - 1] >= first[y]);
        LS_CHECK(first[y - 1] - first[y] <= 1);
        repeats += first[y - 1] == first[y];
    }
    LS_CHECK(repeats > ROWS / 2);
    LS_CHECK(last[0] >= 127 && last[0] <= 128);
}

/* Log from 40 Hz: octaves get equal height, the low rows repeat bins and the
   high rows gather many. */
LS_CASE(log_axis_gives_octaves_equal_height){
    enum { ROWS = 400 };
    static int16_t first[ROWS], last[ROWS];
    ls_spectro_rows(ROWS, N, 48000, true, 24000.0f, first, last);
    int r1 = ls_spectro_hz_row(1000.0f, ROWS, true, 24000.0f);
    int r2 = ls_spectro_hz_row(2000.0f, ROWS, true, 24000.0f);
    int r4 = ls_spectro_hz_row(4000.0f, ROWS, true, 24000.0f);
    LS_CHECK(abs((r1 - r2) - (r2 - r4)) <= 1);
    LS_CHECK(r1 > r2 && r2 > r4);
    LS_CHECK(last[0] - first[0] > 10);
    LS_EQ_INT(first[ROWS - 1], last[ROWS - 1]);
    LS_EQ_INT(ls_spectro_hz_row(20.0f, ROWS, true, 24000.0f), -1);
    float back = ls_spectro_axis_hz(ls_spectro_axis_pos(3000.0f, true, 24000.0f), true, 24000.0f);
    LS_CHECK(fabsf(back - 3000.0f) < 0.5f);
}

LS_CASE(ticks_are_round_and_far_enough_apart){
    /* 510 px for 22 kHz, labels 34 px apart: 2 kHz. */
    LS_CHECK(ls_spectro_tick_hz(22050.0f, 510, 34) == 2000.0f);
    /* A short axis in CALLS: 119 px for 4 kHz. */
    LS_CHECK(ls_spectro_tick_hz(4000.0f, 119, 34) == 2000.0f);
    LS_CHECK(ls_spectro_tick_hz(2756.0f, 510, 34) == 200.0f);
    char t[8];
    ls_spectro_khz(t, sizeof(t), 0); LS_CHECK(!strcmp(t, "0"));
    ls_spectro_khz(t, sizeof(t), 500); LS_CHECK(!strcmp(t, "500"));
    ls_spectro_khz(t, sizeof(t), 2000); LS_CHECK(!strcmp(t, "2k"));
    ls_spectro_khz(t, sizeof(t), 2500); LS_CHECK(!strcmp(t, "2.5k"));
    ls_spectro_khz(t, sizeof(t), 22050); LS_CHECK(!strcmp(t, "22k"));
}

LS_CASE(fast_db_matches_libm_and_codes_round_trip){
    for (float p = 1e-13f; p < 10.0f; p *= 1.37f) {
        float want = 10.0f * log10f(p);
        LS_CHECK_MSG(fabsf(ls_spectro_db(p) - want) < 0.01f, "p=%g got %f want %f",
                     (double)p, (double)ls_spectro_db(p), (double)want);
    }
    LS_CHECK(ls_spectro_db(0.0f) <= -140.0f);
    LS_EQ_INT(ls_spectro_code(-200.0f), 0);
    LS_EQ_INT(ls_spectro_code(0.0f), 255);
    LS_EQ_INT(ls_spectro_code(10.0f), 255);
    for (int c = 0; c < 256; c++) LS_EQ_INT(ls_spectro_code(ls_spectro_code_db(c)), c);
    LS_CHECK(fabsf(ls_spectro_code_db(ls_spectro_code(-37.3f)) + 37.3f) <= 0.25f);
}
