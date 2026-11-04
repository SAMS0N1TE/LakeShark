/* DOPPLER RADAR's DSP against a made-up room: the other board's carrier 3.5
   kHz from the LR's tune and drifting, a moving echo 40 dB under it, a
   slower one the other way, and noise. */
#include "ls_test.h"
#include "radar_dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static radar_dsp_t s_dsp;
static radar_row_t s_rows[64];

typedef struct { double t, ph0, ph1, ph2; uint32_t rng; } room_t;

static float noise(room_t *r)
{
    float a = 0;
    for (int i = 0; i < 4; i++) { r->rng = r->rng * 1664525u + 1013904223u; a += (float)(r->rng >> 8) / 16777216.0f - 0.5f; }
    return a;
}

static void put(uint8_t *p, float v)
{
    int x = (int)lrintf(v * 32767.0f);
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    p[0] = (uint8_t)x; p[1] = (uint8_t)((unsigned)x >> 8);
}

/* `pairs` of the room at 125 kHz. carrier: amplitude 0 switches the tone off. */
static void render(room_t *r, uint8_t *out, size_t pairs, double carrier_hz, float amp,
                   double echo1_hz, float e1, double echo2_hz, float e2, float nsd)
{
    for (size_t n = 0; n < pairs; n++) {
        const double dt = 1.0 / RADAR_IN_RATE;
        const double f = carrier_hz + 2.0 * r->t;          /* 2 Hz/s drift */
        r->ph0 += 2 * M_PI * f * dt;
        r->ph1 += 2 * M_PI * (f + echo1_hz) * dt;
        r->ph2 += 2 * M_PI * (f + echo2_hz) * dt;
        r->t += dt;
        const float i = amp * (float)cos(r->ph0) + e1 * (float)cos(r->ph1) + e2 * (float)cos(r->ph2) + nsd * noise(r);
        const float q = amp * (float)sin(r->ph0) + e1 * (float)sin(r->ph1) + e2 * (float)sin(r->ph2) + nsd * noise(r);
        put(out + 4 * n, i); put(out + 4 * n + 2, q);
    }
}

/* Rows past learning with an echo over the threshold: in a still room, noise
   (480 bins a row against an 11-12 dB threshold) puts one on a few in a hundred. */
static int s_echo_rows;

static int run(room_t *r, double seconds, double carrier, float amp, double e1hz, float e1,
               double e2hz, float e2, radar_row_t *last)
{
    static uint8_t buf[4 * 1020];
    int rows = 0;
    s_echo_rows = 0;
    const size_t total = (size_t)(seconds * RADAR_IN_RATE);
    for (size_t done = 0; done < total; done += 1020) {
        render(r, buf, 1020, carrier, amp, e1hz, e1, e2hz, e2, 0.002f);
        const int k = radar_dsp_feed(&s_dsp, buf, 1020, s_rows, 64);
        if (k) { *last = s_rows[k - 1]; rows += k; }
        for (int j = 0; j < k; j++) s_echo_rows += !s_rows[j].learning && s_rows[j].peak_hz != 0;
    }
    return rows;
}

LS_CASE(the_carrier_is_found_tracked_and_its_echoes_measured)
{
    radar_dsp_init(&s_dsp);
    room_t room = { .rng = 99 };
    radar_row_t last = {0};
    /* a still room for 6 s (the background is learned), then something moves:
       -6 dBFS carrier, echoes 40 and 50 dB under it */
    int rows = run(&room, 6.0, 3500.0, 0.5f, 0, 0, 0, 0, &last);
    /* nothing moves yet: no motion, and an echo on a noise bin only now and then */
    ls_note("still room: %d of %d rows with an echo, motion %.1f dBc", s_echo_rows, rows, (double)last.motion_db);
    LS_CHECK(!last.learning && last.motion_db < -80.0f);
    LS_CHECK(s_echo_rows * 5 < rows);
    rows += run(&room, 1.5, 3500.0, 0.5f, 30.0, 0.005f, -60.0, 0.0016f, &last);
    ls_note("%d rows; carrier %.1f dBFS at %+.1f Hz (true %+.1f), floor %.1f dBc, motion %.1f dBc, "
            "peak %+.2f Hz %.1f dB over floor", rows, (double)last.carrier_dbfs, (double)last.offset_hz,
            3500.0 + 2.0 * room.t, (double)last.floor_db, (double)last.motion_db,
            (double)last.peak_hz, (double)last.peak_db);
    LS_CHECK(s_dsp.locked);
    LS_CHECK(rows >= 24);                                  /* four a second once full */
    LS_CHECK(fabs(last.offset_hz - (3500.0 + 2.0 * room.t)) < 2.0);
    LS_CHECK(fabsf(last.carrier_dbfs - (-6.0f)) < 1.5f);
    LS_CHECK(fabsf(last.peak_hz - 30.0f) < 1.5f);          /* the stronger echo, its sign kept */
    LS_CHECK(last.peak_db > 25.0f);
    LS_CHECK(last.motion_db > -42.0f && last.motion_db < -37.0f);
    /* and the weaker one is there too, on the other side */
    float at60 = -1e9f;
    for (int k = 0; k < RADAR_N; k++)
        if (fabsf(radar_bin_hz(k) + 60.0f) < 1.5f && last.db[k] > at60) at60 = last.db[k];
    LS_CHECK(at60 > -53.0f && at60 < -47.0f);

    char text[50];
    LS_EQ_INT(radar_row_text(&last, 240.0f, 49, text), 49);
    ls_note("[%s]", text);
    LS_CHECK(text[24] == '|');
    LS_CHECK(text[24 + 3] != ' ');                          /* +30 Hz: column 3 right of centre */
    LS_CHECK(text[24 - 6] != ' ');                          /* -60 Hz */
    LS_CHECK(text[24 + 12] == ' ');                         /* +120 Hz: nothing there */
}

LS_CASE(what_never_changes_fades_into_the_room_unless_the_background_is_off)
{
    radar_dsp_init(&s_dsp);
    room_t room = { .rng = 3 };
    radar_row_t last = {0};
    /* a fan that was running before START: part of the room from the first row */
    (void)run(&room, 20.0, 2500.0, 0.4f, 45.0, 0.004f, 0, 0, &last);
    LS_CHECK(s_dsp.locked);
    LS_CHECK(last.peak_hz == 0);
    LS_CHECK(last.motion_db < -60.0f);
    /* the background off: the same fan is there to see */
    radar_dsp_set_background(&s_dsp, false);
    (void)run(&room, 2.0, 2500.0, 0.4f, 45.0, 0.004f, 0, 0, &last);
    LS_CHECK(fabsf(last.peak_hz - 45.0f) < 1.5f);
    LS_CHECK(last.motion_db > -45.0f);
}

LS_CASE(a_lost_carrier_is_searched_for_again)
{
    radar_dsp_init(&s_dsp);
    room_t room = { .rng = 5 };
    radar_row_t last = {0};
    (void)run(&room, 3.0, -2200.0, 0.3f, 0, 0, 0, 0, &last);
    LS_CHECK(s_dsp.locked);
    LS_CHECK(fabs(last.offset_hz - (-2200.0 + 2.0 * room.t)) < 2.0);
    (void)run(&room, 3.5, -2200.0, 0.0f, 0, 0, 0, 0, &last);   /* the tone stops */
    LS_CHECK(!s_dsp.locked);
    (void)run(&room, 4.0, 1800.0, 0.3f, 0, 0, 0, 0, &last);    /* another one starts */
    LS_CHECK(s_dsp.locked);
    LS_CHECK(fabs(last.offset_hz - (1800.0 + 2.0 * room.t)) < 2.0);
}

/* A crystal-only LR2021 module (PR #19: a Waveshare Core2021 runs about -37
   ppm) hears the tone 34 kHz from where it should be at 915 MHz: the search
   covers the whole 125 kHz, and the tracked offset is exact, either side. */
LS_CASE(a_carrier_far_off_tune_is_found_where_it_is)
{
    static const double AT[] = { 1500.0 + 33855.0, 1500.0 - 20000.0, 55000.0 };
    for (int i = 0; i < 3; i++) {
        radar_dsp_init(&s_dsp);
        room_t room = { .rng = 11u + (uint32_t)i };
        radar_row_t last = {0};
        (void)run(&room, 4.0, AT[i], 0.3f, 0, 0, 0, 0, &last);
        ls_note("true %+.1f Hz, tracked %+.2f Hz", AT[i] + 2.0 * room.t, (double)last.offset_hz);
        LS_CHECK(s_dsp.locked);
        LS_CHECK(fabs(last.offset_hz - (AT[i] + 2.0 * room.t)) < 1.0);
    }
}

/* On the board, with no tone running, DOPPLER RADAR locked on a line 8 kHz
   off tune that came and went between -103 and about -70 dBFS: something
   else in the band, far under the -13 dBFS the other board's tone lands at.
   A line that weak is not a tone, however clean; one at -40 dBFS is. */
LS_CASE(a_faint_line_is_not_the_other_board)
{
    static uint8_t buf[4 * 1020];
    static const float AMP[] = { 2.5e-5f, 3.2e-4f, 1e-2f };   /* -92, -70, -40 dBFS */
    for (int a = 0; a < 3; a++) {
        radar_dsp_init(&s_dsp);
        double ph = 0;
        for (int b = 0; b < 3 * RADAR_IN_RATE / 1020; b++) {
            for (int n = 0; n < 1020; n++) {
                ph += 2 * M_PI * -8337.0 / RADAR_IN_RATE;
                put(buf + 4 * n, AMP[a] * (float)cos(ph));
                put(buf + 4 * n + 2, AMP[a] * (float)sin(ph));
            }
            (void)radar_dsp_feed(&s_dsp, buf, 1020, s_rows, 64);
        }
        LS_CHECK(s_dsp.locked == (a == 2));
    }
}

/* Also on the board: a burst from some other transmitter, strong for the
   33 ms of one search and gone, read for a moment as a tone at -103 dBFS.
   No row of it is handed out, and the search starts again. */
LS_CASE(a_burst_is_not_a_lock)
{
    static uint8_t buf[4 * 1020];
    radar_dsp_init(&s_dsp);
    double ph = 0;
    int rows = 0;
    for (int b = 0; b < 3 * RADAR_IN_RATE / 1020; b++) {
        for (int n = 0; n < 1020; n++) {
            const float amp = b < 6 ? 0.1f : 0.0f;              /* -20 dBFS for 49 ms */
            ph += 2 * M_PI * 4081.0 / RADAR_IN_RATE;
            put(buf + 4 * n, amp * (float)cos(ph));
            put(buf + 4 * n + 2, amp * (float)sin(ph));
        }
        rows += radar_dsp_feed(&s_dsp, buf, 1020, s_rows, 64);
    }
    LS_EQ_INT(rows, 0);
    LS_CHECK(!s_dsp.locked);
}

LS_CASE(nothing_but_the_lr_s_own_dc_is_not_a_carrier)
{
    radar_dsp_init(&s_dsp);
    room_t room = { .rng = 7 };
    radar_row_t last = {0};
    (void)run(&room, 2.0, 40.0, 0.3f, 0, 0, 0, 0, &last);       /* inside the DC guard */
    LS_CHECK(!s_dsp.locked);
}

LS_CASE(speed_follows_doppler)
{
    LS_CHECK(fabsf(radar_speed_mps(6.1f, 915000000u) - 1.0f) < 0.01f);
    LS_CHECK(radar_speed_mps(-12.2f, 915000000u) < -1.99f);
}
