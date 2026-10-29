/* What the board hands a detector app (tui/ls_seen.c, SDK 2.2 lap_seen_*):
   nothing is kept while no app watches, addresses become pseudonyms, a full
   queue counts its drops, and drone watch raises its own alert when no app
   takes it. */
#include "ls_test.h"
#include "ls_seen.h"
#include <stdio.h>
#include <string.h>

extern int64_t (*ls_seen_host_clock)(void);
static int64_t clock_us;
static int64_t host_clock(void) { return clock_us; }

LS_CASE(seen_keeps_nothing_until_watched) {
    ls_seen_want(0);
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    ls_seen_observe(mac, 0, LS_SEEN_BLE, 3, -60, false, false, 0, 1000);
    ls_det_signal_t p25 = {.source = DET_P25, .a = 4512, .b = 1704, .flag = 1};
    ls_seen_signal(&p25);
    ls_seen_t s;
    LS_CHECK(!ls_seen_take(&s));
    LS_CHECK(!ls_seen_pending());
}

LS_CASE(seen_pseudonyms_and_fields) {
    ls_seen_want(0);
    ls_seen_host_clock = host_clock;
    clock_us = 5000000;
    ls_seen_want(LS_SEEN_BLE | LS_SEEN_RID | DET_P25 | DET_REC);
    LS_CHECK(ls_seen_pending());
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6}, other[6] = {1, 2, 3, 4, 5, 7};
    ls_seen_observe(mac, 0, LS_SEEN_BLE, 3, -200, false, true, 0, 1000);
    ls_seen_observe(mac, 0, LS_SEEN_BLE, 3, -200, false, true, 0, 1000);   /* the same pass twice: one */
    ls_seen_observe(other, 0, LS_SEEN_BLE, 3, -50, false, false, 2, 1000);
    ls_seen_observe(mac, 0, LS_SEEN_WIFI, 3, -50, false, false, 2, 1000);  /* not watched */
    ls_seen_t a, b;
    LS_CHECK(ls_seen_take(&a) && ls_seen_take(&b));
    LS_EQ_INT(a.source, LS_SEEN_BLE);
    LS_EQ_INT(a.rssi, -127);
    LS_EQ_INT(a.flags, LS_SEEN_F_FLOOD);
    LS_EQ_INT(a.category, 3);
    LS_CHECK(a.entity && b.entity && a.entity != b.entity);
    /* The address itself never shows: no byte run of it in the pseudonym. */
    LS_CHECK(memcmp(&a.entity, mac, 6) != 0);
    ls_det_signal_t p25 = {.source = DET_P25, .a = 4512, .b = 1704, .flag = 1};
    ls_seen_signal(&p25);
    ls_det_signal_t rec = {.source = DET_REC, .a = 433920000, .b = 9, .flag = 1};
    snprintf(rec.text, sizeof(rec.text), "OOK24");
    ls_seen_signal(&rec);
    ls_seen_t s;
    LS_CHECK(ls_seen_take(&s));
    LS_EQ_INT(s.source, DET_P25);
    LS_EQ_INT(s.flags, LS_SEEN_F_EMERGENCY);
    LS_EQ_INT(s.a, 4512);
    LS_EQ_INT(s.b, 1704);
    LS_CHECK(s.at_us == 5000000);
    LS_CHECK(ls_seen_take(&s));
    LS_EQ_INT(s.flags, LS_SEEN_F_NEW);
    LS_EQ_STR(s.text, "OOK24");
    LS_CHECK(!ls_seen_take(&s));
    ls_seen_want(0);
}

LS_CASE(seen_full_queue_counts_drops_and_resets) {
    ls_seen_want(0);
    ls_seen_want(DET_POCSAG);
    ls_det_signal_t page = {.source = DET_POCSAG, .a = 1234};
    for (int i = 0; i < LS_SEEN_QUEUE + 5; i++) ls_seen_signal(&page);
    LS_EQ_INT(ls_seen_dropped(), 5);
    ls_seen_want(DET_POCSAG | DET_P25);       /* new sources: what waited is dropped */
    ls_seen_t s;
    LS_CHECK(!ls_seen_take(&s));
    ls_seen_want(0);
    LS_EQ_INT(ls_seen_dropped(), 0);
}

LS_CASE(seen_drone_watch_app_or_board) {
    ls_seen_want(0);
    ls_seen_want(LS_SEEN_DRONE);
    LS_CHECK(ls_seen_drone_watch(7, -60, true, 915, true, 10000000));
    ls_seen_t s;
    LS_CHECK(ls_seen_take(&s));
    LS_EQ_INT(s.source, LS_SEEN_DRONE);
    LS_EQ_INT(s.flags, LS_SEEN_F_VALIDATED | LS_SEEN_F_APPROACHING);
    LS_EQ_INT(s.band, 915);
    LS_CHECK(s.entity == 7);
    /* No app: the board's own banner, at most one every 2 s. */
    ls_seen_want(0);
    LS_CHECK(ls_seen_drone_watch(7, -60, true, 915, false, 20000000));
    LS_CHECK(!ls_seen_drone_watch(8, -60, true, 915, false, 21000000));
    LS_CHECK(ls_seen_drone_watch(8, -60, true, 915, false, 22000000));
    ls_seen_banner_poll();
    LS_CHECK(!ls_seen_take(&s));
    LS_CHECK(!ls_seen_drone_watch(0, -60, true, 915, false, 30000000));
}

LS_CASE(seen_lora_only_when_watched) {
    ls_seen_want(0);
    ls_seen_drone(915, 3, 250, -70, 12, 1000);
    ls_seen_t s;
    LS_CHECK(!ls_seen_take(&s));
    ls_seen_want(LS_SEEN_LORA);
    ls_seen_drone(433, 3, 250, -70, 12, 1000);   /* not a drone-link band */
    ls_seen_drone(915, 3, 250, -70, 12, 1000);
    LS_CHECK(ls_seen_take(&s));
    LS_EQ_INT(s.band, 915);
    LS_EQ_INT(s.a, 3);
    LS_EQ_INT(s.b, 250);
    LS_EQ_INT(s.hits_per_min, 12);
    LS_CHECK(!ls_seen_take(&s));
    ls_seen_want(0);
}
