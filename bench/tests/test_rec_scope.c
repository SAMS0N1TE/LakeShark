/* SUB-GHZ's live trace (rec_scope.c): what READ and RECORD RAW draw while
   listening. A quiet band must sit on the floor, a burst must stand out and
   be marked as signal, and the newest column is the one on the right. */
#include "ls_test.h"
#include "rec_scope.h"
#include <string.h>

static void quiet(int columns)
{
    for (int i = 0; i < columns; i++) rec_scope_level(-86.0f + (float)(i % 3), REC_SCOPE_BUCKET_US);
}

LS_CASE(a_quiet_band_sits_on_the_floor)
{
    rec_scope_reset();
    quiet(200);
    uint8_t col[REC_SCOPE_N];
    const int n = rec_scope_read(col, 200);
    LS_EQ_INT(n, 200);
    for (int i = 50; i < n; i++) {
        LS_CHECK(!(col[i] & REC_SCOPE_HOT));
        LS_CHECK((col[i] & REC_SCOPE_DB) <= 4);
    }
}

LS_CASE(a_burst_stands_out_and_is_newest_on_the_right)
{
    rec_scope_reset();
    quiet(200);
    for (int i = 0; i < 10; i++) rec_scope_level(-40.0f, REC_SCOPE_BUCKET_US);
    uint8_t col[REC_SCOPE_N];
    const int n = rec_scope_read(col, 20);
    LS_EQ_INT(n, 20);
    for (int i = 0; i < 10; i++) LS_CHECK(!(col[i] & REC_SCOPE_HOT));
    for (int i = 10; i < 20; i++) {
        LS_CHECK(col[i] & REC_SCOPE_HOT);
        LS_CHECK((col[i] & REC_SCOPE_DB) >= 40);
    }
    /* The burst did not drag the floor up with it. */
    LS_CHECK(rec_scope_floor_db() < -80.0f);
    /* And the floor comes back down to it once the burst ends. */
    quiet(100);
    LS_CHECK(rec_scope_now_db() < 6.0f);
}

LS_CASE(iq_magnitude_feeds_the_same_scale)
{
    rec_scope_reset();
    /* 125 kS/s, 8 us a sample: a 4 ms column is 500 samples. */
    static uint8_t iq[4 * 500 * 40];
    for (int i = 0; i < 500 * 40; i++) {
        const int16_t v = (int16_t)(i < 500 * 30 ? 40 + (i % 7) : 12000);
        iq[4 * i] = (uint8_t)v; iq[4 * i + 1] = (uint8_t)(v >> 8);
        iq[4 * i + 2] = 0; iq[4 * i + 3] = 0;
    }
    rec_scope_iq16(iq, sizeof(iq), 8);
    uint8_t col[REC_SCOPE_N];
    const int n = rec_scope_read(col, 40);
    LS_EQ_INT(n, 40);
    LS_CHECK(!(col[25] & REC_SCOPE_HOT));
    LS_CHECK(col[35] & REC_SCOPE_HOT);
    LS_CHECK((col[35] & REC_SCOPE_DB) >= 40);   /* 12000 over ~45: 48 dB */
}

LS_CASE(a_long_reading_fills_every_column_it_covers)
{
    rec_scope_reset();
    quiet(50);
    uint8_t before[REC_SCOPE_N], after[REC_SCOPE_N];
    (void)before;
    rec_scope_level(-40.0f, REC_SCOPE_BUCKET_US * 3);
    const int n = rec_scope_read(after, 3);
    LS_EQ_INT(n, 3);
    for (int i = 0; i < 3; i++) LS_CHECK(after[i] & REC_SCOPE_HOT);
}
