/* TIME MACHINE timeline: one list in time order from the ls_decoded bus,
   repeats merged, late arrivals placed where they happened, frequencies
   found from what was tuned, clips linked both ways round. */
#include "ls_test.h"
#include "ls_tmach_timeline.h"
#include "ls_tmach_ring.h"

#include <stdlib.h>
#include <string.h>

static ls_decoded_t ev(uint16_t kind, int64_t at_s, uint32_t address, const char *from, const char *text)
{
    ls_decoded_t d;
    memset(&d, 0, sizeof(d));
    d.kind = kind;
    d.at_us = at_s * 1000000;
    d.address = address;
    snprintf(d.from, sizeof(d.from), "%s", from ? from : "");
    snprintf(d.text, sizeof(d.text), "%s", text ? text : "");
    return d;
}

static bool tuned(void *ctx, int64_t at_us, uint32_t *hz, uint8_t *radio)
{
    (void)ctx;
    if (at_us < 100000000) { *hz = 152600000; *radio = LS_TM_RADIO_RTL; }
    else { *hz = 851012500; *radio = LS_TM_RADIO_HACKRF; }
    return true;
}

LS_CASE(rows_come_out_newest_first_and_late_ones_land_in_place)
{
    ls_tm_timeline_t *tl = malloc(sizeof(*tl));
    ls_tm_tl_init(tl);
    ls_decoded_t a = ev(LS_DECODED_PAGE, 10, 1001, "POCSAG", "first");
    ls_decoded_t b = ev(LS_DECODED_APRS, 30, 0, "N0CALL", "wx");
    ls_decoded_t c = ev(LS_DECODED_P25, 20, 4512, "TG 4512", "unit 7");   /* arrives last, happened between */
    ls_tm_tl_add(tl, &a, tuned, NULL);
    ls_tm_tl_add(tl, &b, tuned, NULL);
    ls_tm_tl_add(tl, &c, tuned, NULL);
    LS_EQ_UINT(tl->n, 3);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 0)->kind, LS_DECODED_APRS);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 1)->kind, LS_DECODED_P25);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 2)->kind, LS_DECODED_PAGE);
    LS_CHECK(ls_tm_tl_newest(tl, 3) == NULL);
    /* Unsaid frequency and radio come from what was tuned then. */
    LS_EQ_UINT(ls_tm_tl_newest(tl, 2)->freq_hz, 152600000);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 2)->radio, LS_TM_RADIO_RTL);
    /* A said one is kept. */
    ls_decoded_t d = ev(LS_DECODED_REC, 200, 0, "LR2021", "OOK24 A1B2C3");
    d.freq_hz = 433920000; d.radio = LS_TM_RADIO_LR2021;
    ls_tm_tl_add(tl, &d, tuned, NULL);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 0)->freq_hz, 433920000);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 0)->radio, LS_TM_RADIO_LR2021);
    LS_EQ_STR(ls_tm_kind_name(LS_DECODED_REC), "SUB-GHZ");
    LS_EQ_INT(ls_tm_kind_letter(LS_DECODED_P25), 'T');
    free(tl);
}

LS_CASE(repeats_merge_inside_their_window_only)
{
    ls_tm_timeline_t *tl = malloc(sizeof(*tl));
    ls_tm_tl_init(tl);
    /* One aircraft heard for ten minutes in 30 s steps: one row. */
    for (int s = 0; s < 600; s += 30) {
        ls_decoded_t d = ev(LS_DECODED_ADSB, 1000 + s, 0xA1B2C3, s < 60 ? "" : "UAL442", "");
        if (s == 300) { d.has_pos = true; d.lat = 43.2; d.lon = -71.5; }
        ls_tm_tl_add(tl, &d, NULL, NULL);
    }
    LS_EQ_UINT(tl->n, 1);
    const ls_tm_event_t *e = ls_tm_tl_newest(tl, 0);
    LS_EQ_UINT(e->count, 20);
    LS_EQ_STR(e->from, "UAL442");                 /* the callsign, once learnt */
    LS_CHECK(e->has_pos);
    LS_CHECK(e->at_us == 1000000000 && e->last_us == 1570000000);
    /* A page repeated by the paging system 1 s later merges; 5 s later is a new page. */
    ls_decoded_t p = ev(LS_DECODED_PAGE, 2000, 77, "POCSAG", "CALL ME");
    ls_tm_tl_add(tl, &p, NULL, NULL);
    p.at_us += 1000000;
    ls_tm_tl_add(tl, &p, NULL, NULL);
    p.at_us += 5000000;
    ls_tm_tl_add(tl, &p, NULL, NULL);
    LS_EQ_UINT(tl->n, 3);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 1)->count, 2);
    /* Mesh never merges; different addresses never merge. */
    ls_decoded_t m = ev(LS_DECODED_MESH, 3000, 0, "node", "hi");
    ls_tm_tl_add(tl, &m, NULL, NULL);
    ls_tm_tl_add(tl, &m, NULL, NULL);
    ls_decoded_t q = ev(LS_DECODED_PAGE, 3000, 78, "POCSAG", "CALL ME");
    ls_tm_tl_add(tl, &q, NULL, NULL);
    LS_EQ_UINT(tl->n, 6);
    LS_EQ_UINT(tl->merged, 20);
    free(tl);
}

LS_CASE(full_timeline_drops_the_oldest_and_counts_it)
{
    ls_tm_timeline_t *tl = malloc(sizeof(*tl));
    ls_tm_tl_init(tl);
    for (int k = 0; k < LS_TM_TL_MAX + 10; k++) {
        ls_decoded_t d = ev(LS_DECODED_MESH, 100 + k, 0, "n", "m");
        ls_tm_tl_add(tl, &d, NULL, NULL);
    }
    LS_EQ_UINT(tl->n, LS_TM_TL_MAX);
    LS_EQ_UINT(tl->dropped, 10);
    LS_CHECK(ls_tm_tl_newest(tl, LS_TM_TL_MAX - 1)->at_us == 110 * 1000000LL);
    /* Older than everything kept: refused, not inserted at the front. */
    ls_decoded_t old = ev(LS_DECODED_MESH, 5, 0, "n", "old");
    LS_EQ_UINT(ls_tm_tl_add(tl, &old, NULL, NULL), 0);
    LS_EQ_UINT(tl->dropped, 11);
    free(tl);
}

LS_CASE(clips_link_rows_already_there_and_rows_that_arrive_later)
{
    ls_tm_timeline_t *tl = malloc(sizeof(*tl));
    ls_tm_tl_init(tl);
    ls_decoded_t a = ev(LS_DECODED_PAGE, 50, 1, "POCSAG", "in");
    ls_decoded_t b = ev(LS_DECODED_PAGE, 10, 2, "POCSAG", "before");
    ls_tm_tl_add(tl, &a, NULL, NULL);
    ls_tm_tl_add(tl, &b, NULL, NULL);
    ls_tm_tl_clip(tl, 7, 30000000, 60000000);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 0)->clip, 7);
    LS_EQ_INT(ls_tm_tl_newest(tl, 0)->clip_ms, 20000);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 1)->clip, 0);
    /* A decoder that reports late still finds its clip. */
    ls_decoded_t late = ev(LS_DECODED_P25, 55, 9, "TG 9", "");
    ls_tm_tl_add(tl, &late, NULL, NULL);
    LS_EQ_UINT(ls_tm_tl_newest(tl, 0)->clip, 7);
    LS_EQ_INT(ls_tm_tl_newest(tl, 0)->clip_ms, 25000);
    free(tl);
}

/* The real bus: a reader on ls_decoded drained into the timeline. */
LS_CASE(the_timeline_reads_the_decoded_bus)
{
    const int reader = ls_decoded_open(0xFFFF);
    LS_CHECK(reader >= 0);
    ls_decoded_t d = ev(LS_DECODED_AIS, 70, 366123456, "EVER GIVEN", "");
    ls_decoded_publish(&d);
    d = ev(LS_DECODED_DETECT, 71, 0, "BLE", "tracker following");
    ls_decoded_publish(&d);
    ls_tm_timeline_t *tl = malloc(sizeof(*tl));
    ls_tm_tl_init(tl);
    ls_decoded_t got;
    uint32_t lost = 0;
    while (ls_decoded_next(reader, &got, &lost)) ls_tm_tl_add(tl, &got, NULL, NULL);
    LS_EQ_UINT(tl->n, 2);
    LS_EQ_UINT(lost, 0);
    LS_EQ_STR(ls_tm_tl_newest(tl, 0)->text, "tracker following");
    LS_EQ_STR(ls_tm_tl_newest(tl, 1)->from, "EVER GIVEN");
    ls_decoded_close(reader);
    free(tl);
}
