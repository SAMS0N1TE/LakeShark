/* CATCH channel windows: the burst filter, presets, strip columns and the
   saved blob, plus the filter in front of the real detector. */
#include "rec_catch_chan.h"
#include "rec_catch_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static catch_watch_t chosen(uint32_t centre, uint32_t width)
{
    catch_watch_t w = {.mode = CATCH_WATCH_CHOSEN};
    assert(catch_watch_add(&w, centre, width) == 0);
    return w;
}
static void window_edges(void)
{
    catch_watch_t w = chosen(433920000u, 200000u);
    assert(catch_watch_allows(&w, 433920000u));
    assert(catch_watch_allows(&w, 433820000u) && catch_watch_allows(&w, 434020000u)); /* edges are in */
    assert(!catch_watch_allows(&w, 433819999u) && !catch_watch_allows(&w, 434020001u));
    assert(!catch_watch_allows(&w, 915000000u));
    /* An odd width never widens the window past its half. */
    w = chosen(868350000u, 5001u);
    assert(catch_watch_allows(&w, 868352500u) && !catch_watch_allows(&w, 868352501u));
}
static void overlap_and_modes(void)
{
    catch_watch_t w = chosen(433920000u, 200000u);
    assert(catch_watch_add(&w, 434000000u, 200000u) == 1); /* overlaps the first */
    assert(catch_watch_allows(&w, 433950000u) && catch_watch_allows(&w, 434100000u));
    assert(catch_watch_allows(&w, 433820000u) && catch_watch_allows(&w, 434100000u));
    assert(!catch_watch_allows(&w, 434100001u) && !catch_watch_allows(&w, 433819999u));
    assert(catch_watch_add(&w, 915000000u, 20000u) == 2);
    assert(catch_watch_allows(&w, 915010000u) && !catch_watch_allows(&w, 915010001u));
    assert(!catch_watch_allows(&w, 600000000u)); /* a gap between windows */
    assert(catch_watch_filtering(&w) && !catch_watch_note(&w));
    /* ALL ignores the list entirely. */
    w.mode = CATCH_WATCH_ALL;
    assert(!catch_watch_filtering(&w) && catch_watch_allows(&w, 600000000u) && !catch_watch_note(&w));
    /* CHOSEN with nothing chosen is ALL, and says so. */
    catch_watch_t empty = {.mode = CATCH_WATCH_CHOSEN};
    assert(!catch_watch_filtering(&empty) && catch_watch_allows(&empty, 123456789u));
    assert(catch_watch_note(&empty) && strstr(catch_watch_note(&empty), "empty"));
    catch_watch_t none = {0};
    assert(catch_watch_allows(&none, 1u) && !catch_watch_note(&none));
}
static void editing(void)
{
    catch_watch_t w = {.mode = CATCH_WATCH_CHOSEN};
    assert(catch_watch_add(&w, 24000000u, 100000u) == -1);    /* below the tuner */
    assert(catch_watch_add(&w, 433920000u, 1000u) == -1);     /* too narrow */
    assert(catch_watch_add(&w, 433920000u, 3000000u) == -1);  /* wider than a slice */
    assert(catch_watch_add(&w, 0, 0) == -1);
    assert(catch_watch_add(&w, 433920000u, 200000u) == 0);
    assert(catch_watch_add(&w, 433920000u, 50000u) == 0 && w.chan[0].width == 50000u && catch_watch_count(&w) == 1);
    for (int i = 1; i < CATCH_CHAN_MAX; i++) assert(catch_watch_add(&w, 100000000u + i * 1000000u, 20000u) == i);
    assert(catch_watch_add(&w, 200000000u, 20000u) == -2);   /* full */
    assert(catch_watch_remove(&w, 3) && !catch_watch_remove(&w, 3) && !catch_watch_remove(&w, 8));
    assert(catch_watch_add(&w, 200000000u, 20000u) == 3);    /* the freed slot is reused */
    assert(catch_watch_valid(&w));
    w.chan[2].width = 1;
    assert(!catch_watch_valid(&w));
}
static void presets_and_strip(void)
{
    uint32_t p[CATCH_PRESETS];
    assert(catch_watch_presets(433920000u, p, CATCH_PRESETS) == 1 && p[0] == 433920000u);
    assert(catch_watch_presets(315000000u, p, CATCH_PRESETS) == 1 && p[0] == 315000000u);
    assert(catch_watch_presets(868300000u, p, CATCH_PRESETS) == 1 && p[0] == 868350000u);
    assert(catch_watch_presets(915000000u, p, CATCH_PRESETS) == 1 && p[0] == 915000000u);
    assert(catch_watch_presets(600000000u, p, CATCH_PRESETS) == 0);
    assert(catch_watch_presets(433920000u + 1200000u, p, CATCH_PRESETS) == 1);       /* edge in */
    assert(catch_watch_presets(433920000u + 1200001u, p, CATCH_PRESETS) == 0);       /* just out */
    assert(catch_chan_in_slice(433920000u, 432720000u) && !catch_chan_in_slice(433920000u, 432719999u));
    int l, r;
    catch_chan_t c = {433920000u, 200000u};
    assert(catch_chan_cols(433920000u, &c, 48, &l, &r) && l == 22 && r == 26);   /* around the middle */
    c = (catch_chan_t){432800000u, 400000u};                                      /* hangs off the left */
    assert(catch_chan_cols(433920000u, &c, 48, &l, &r) && l == 0 && r > 0);
    c = (catch_chan_t){915000000u, 20000u};
    assert(!catch_chan_cols(433920000u, &c, 48, &l, &r));
    c = (catch_chan_t){0, 0};
    assert(!catch_chan_cols(433920000u, &c, 48, &l, &r));
    assert(catch_slice_col(433920000u, 0, 48) == 0 && catch_slice_col(433920000u, 999999999u, 48) == 47);
}
static void blob_form(void)
{
    catch_watch_t w = chosen(433920000u, 200000u), back = {0};
    catch_watch_add(&w, 915600000u, 30000u);
    uint8_t b[CATCH_WATCH_BLOB];
    assert(catch_watch_encode(&w, b) == CATCH_WATCH_BLOB);
    assert(catch_watch_decode(b, sizeof(b), &back) && !memcmp(&w, &back, sizeof(w)));
    uint8_t t[CATCH_WATCH_BLOB];
    for (size_t i = 0; i < sizeof(b); i++) {          /* any single flipped byte is refused */
        memcpy(t, b, sizeof(t)); t[i] ^= 0x40;
        catch_watch_t x = {0};
        assert(!catch_watch_decode(t, sizeof(t), &x));
    }
    assert(!catch_watch_decode(b, sizeof(b) - 1, &back) && !catch_watch_decode(b, 0, &back) && !catch_watch_decode(NULL, CATCH_WATCH_BLOB, &back));
    /* A well-formed blob with an out-of-range window is refused too. */
    catch_watch_t bad = w; bad.chan[1].width = 7;
    catch_watch_encode(&bad, t);
    assert(!catch_watch_decode(t, sizeof(t), &back));
    bad = w; bad.mode = 2;
    catch_watch_encode(&bad, t);
    assert(!catch_watch_decode(t, sizeof(t), &back));
}
static void persistence(void)
{
    catch_watch_t got, w = chosen(433920000u, 200000u);
    catch_watch_host_reboot();
    catch_watch_load();                                   /* nothing saved: ALL, empty */
    catch_watch_get(&got);
    assert(got.mode == CATCH_WATCH_ALL && !catch_watch_count(&got) && catch_watch_passes(1u));
    catch_watch_add(&w, 868350000u, 100000u);
    assert(catch_watch_set(&w));
    assert(catch_watch_passes(433920000u) && !catch_watch_passes(915000000u));
    catch_watch_host_reboot();                            /* power cycle */
    catch_watch_get(&got);
    assert(got.mode == CATCH_WATCH_ALL);                  /* live copy gone until loaded */
    catch_watch_load();
    catch_watch_get(&got);
    assert(!memcmp(&got, &w, sizeof(w)));
    assert(catch_watch_passes(868350000u) && !catch_watch_passes(433000000u));
    /* An invalid edit is refused and the saved copy is untouched. */
    catch_watch_t bad = w; bad.chan[0].width = 1;
    assert(!catch_watch_set(&bad));
    catch_watch_host_reboot(); catch_watch_load(); catch_watch_get(&got);
    assert(!memcmp(&got, &w, sizeof(w)));
    /* A damaged saved blob starts from ALL rather than from garbage. */
    size_t n; uint8_t *blob = catch_watch_host_blob(&n);
    assert(n == CATCH_WATCH_BLOB);
    blob[10] ^= 0xff;
    catch_watch_host_reboot(); catch_watch_load(); catch_watch_get(&got);
    assert(got.mode == CATCH_WATCH_ALL && !catch_watch_count(&got));
}

/* The filter in front of the real detector: a 915 MHz slice with an
   interferer beside the channel that is wanted. */
static catch_detector_t detector;
static float bins[CATCH_BINS];
static unsigned passed, ignored;
static uint32_t passed_hz;
static void event(const catch_burst_t *b, bool start, void *ctx)
{
    (void)ctx;
    if (b->spur || !start) return;
    if (catch_watch_passes(b->frequency)) { passed++; passed_hz = b->frequency; } else ignored++;
}
static void detector_gate(void)
{
    catch_detector_init(&detector, 915000000u);
    for (int i = 0; i < CATCH_BINS; i++) bins[i] = -70;
    for (int i = 0; i < 300; i++) catch_detector_spectrum(&detector, bins, i * 1000, event, NULL);
    catch_watch_t w = chosen(915600000u, 100000u);
    assert(catch_watch_set(&w));
    passed = ignored = 0;
    bins[384] = -25; bins[300] = -25;                     /* 915.6 MHz and 915.206 MHz */
    for (int i = 300; i < 304; i++) catch_detector_spectrum(&detector, bins, i * 1000, event, NULL);
    for (int i = 0; i < CATCH_BINS; i++) bins[i] = -70;
    catch_detector_spectrum(&detector, bins, 314000, event, NULL);
    assert(passed == 1 && ignored == 1);
    assert(passed_hz > 915590000u && passed_hz < 915610000u);
    /* ALL puts the interferer back. */
    w.mode = CATCH_WATCH_ALL;
    assert(catch_watch_set(&w));
    catch_detector_init(&detector, 915000000u);
    for (int i = 0; i < CATCH_BINS; i++) bins[i] = -70;
    for (int i = 0; i < 300; i++) catch_detector_spectrum(&detector, bins, i * 1000, event, NULL);
    passed = ignored = 0;
    bins[384] = -25; bins[300] = -25;
    for (int i = 300; i < 304; i++) catch_detector_spectrum(&detector, bins, i * 1000, event, NULL);
    for (int i = 0; i < CATCH_BINS; i++) bins[i] = -70;
    catch_detector_spectrum(&detector, bins, 314000, event, NULL);
    assert(passed == 2 && ignored == 0);
}
int main(void)
{
    window_edges();
    overlap_and_modes();
    editing();
    presets_and_strip();
    blob_form();
    persistence();
    detector_gate();
    puts("PASS CATCH channel windows: edges, overlap, empty fallback, presets, strip, saved blob, detector gate");
    return 0;
}
