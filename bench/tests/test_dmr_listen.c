#include "ls_test.h"
#include "dmr_listen.h"
#include "dmr_gen.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

static const uint8_t data_sync[6] = {0xDF,0xF5,0x7D,0x75,0xDF,0x5D};
static const uint8_t voice_sync[6] = {0x75,0x5F,0xD7,0xDF,0x75,0xF7};
/* Systematic TACT 1000101 / 1100010, AT=1, TC=0/1, LCSS=00. */
static const uint8_t cach[2][3] = {{0x80,0x02,0x02},{0x88,0x00,0x20}};
static void feed(const uint8_t *bytes, unsigned n)
{
    for (unsigned i = 0; i < n * 8; i += 2) {
        unsigned hi = (bytes[i / 8] >> (7 - i % 8)) & 1;
        unsigned lo = (bytes[i / 8] >> (6 - i % 8)) & 1;
        int symbol = hi ? (lo ? -3600 : -1200) : (lo ? 3600 : 1200);
        dmr_watch_symbol(symbol, 0, 2400, -2400);
    }
}
static void burst(unsigned slot, uint8_t dt, uint32_t tg, bool encrypted)
{
    dmr_lc_t lc = {.destination = tg, .source = 1234567,
                   .service_options = encrypted ? 0x40 : 0};
    uint8_t b[33];
    dmr_gen_burst(b, data_sync, 7, dt, &lc);
    feed(cach[slot - 1], 3); feed(b, 33);
}
LS_CASE(repeater_slots_keep_separate_identity_privacy_and_termination)
{
    dmr_watch_reset();
    burst(1, 1, 2051, false);
    burst(2, 1, 16777215, true);
    dmr_watch_t w; LS_CHECK(dmr_watch_get(&w));
    LS_EQ_UINT(w.call[0].lc.destination, 2051);
    LS_EQ_UINT(w.call[1].lc.destination, 16777215);
    LS_CHECK(w.call[0].active && w.call[1].active);
    LS_CHECK(!w.call[0].encrypted && w.call[1].encrypted);
    int64_t start = w.call[0].start_us;
    burst(1, 2, 2051, false);
    LS_CHECK(dmr_watch_get(&w));
    LS_CHECK(!w.call[0].active && w.call[1].active);
    LS_CHECK(w.call[0].start_us == start);
    LS_CHECK(w.call[0].end_us >= start);
}
LS_CASE(voice_payload_never_becomes_slot_type_or_link_control)
{
    dmr_lc_t lc = {.destination = 2051, .source = 1234567};
    uint8_t b[33]; dmr_gen_burst(b, voice_sync, 7, 1, &lc);
    dmr_watch_reset(); feed(cach[1], 3); feed(b, 33);
    dmr_watch_t w; LS_CHECK(dmr_watch_get(&w));
    LS_EQ_UINT(w.bursts, 1); LS_EQ_UINT(w.slot_type_ok, 0);
    LS_EQ_UINT(w.lc_ok, 0); LS_CHECK(!w.have_lc);
    LS_EQ_UINT(w.tracker.slot[1].bursts, 1);
}
LS_CASE(cach_corrects_each_single_bit_including_slot_bit)
{
    const unsigned pos[] = {0,4,8,12,14,18,22};
    for (unsigned s = 0; s < 2; ++s) {
        LS_EQ_UINT(dmr_cach_slot(cach[s]), s + 1);
        for (unsigned i = 0; i < 7; ++i) {
            uint8_t c[3]; memcpy(c, cach[s], 3);
            c[pos[i]/8] ^= 1u << (7 - pos[i]%8);
            LS_EQ_UINT(dmr_cach_slot(c), s + 1);
        }
    }
}
LS_CASE(filters_hold_and_encryption_bound_selection_to_one_clear_slot)
{
    dmr_listen_t p; dmr_listen_init(&p);
    dmr_watch_t w = {0};
    w.call[0] = (dmr_call_t){.have_lc=true,.active=true,.colour_code=7,
        .last_us=1000000,.start_us=1000,.lc={.destination=2051}};
    w.call[1] = w.call[0]; w.call[1].lc.destination = 16777215;
    LS_EQ_UINT(dmr_listen_select(&p, &w, 1100000), 1);
    p.slot = 2; LS_EQ_UINT(dmr_listen_select(&p, &w, 1100000), 2);
    w.call[1].encrypted = true; LS_EQ_UINT(dmr_listen_select(&p, &w, 1100000), 0);
    p.slot = 0; p.colour_code = 3;
    LS_EQ_UINT(dmr_listen_select(&p, &w, 1100000), 0);
    p.colour_code = -1; p.hold = 16777215;
    LS_EQ_UINT(dmr_listen_select(&p, &w, 1100000), 0);
    p.hold = 0; LS_CHECK(dmr_listen_allow(&p, "16777215, 2051"));
    LS_CHECK(!dmr_listen_allow(&p, "16777216"));
    LS_CHECK(!dmr_listen_allow(&p, "-1"));
    LS_CHECK(!dmr_listen_allow(&p, "12x"));
    LS_EQ_UINT(p.count, 2);
    LS_EQ_UINT(dmr_listen_select(&p, &w, 2600000), 0);
    LS_EQ_UINT(dmr_call_duration_ms(&w.call[0], 2600000), 999);
    LS_CHECK(dmr_listen_allow(&p, "")); LS_EQ_UINT(p.count, 0);
}
LS_CASE(metadata_frame_cost_is_measured_without_audio)
{
    dmr_watch_reset();
    dmr_lc_t lc = {.destination = 2051, .source = 1234567};
    uint8_t b[33]; dmr_gen_burst(b, data_sync, 7, 1, &lc);
    clock_t start = clock();
    for (unsigned i = 0; i < 10000; ++i) { feed(cach[i % 2], 3); feed(b, 33); }
    double us = (double)(clock() - start) * 1000000 / CLOCKS_PER_SEC / 10000;
    printf("DMR metadata: %.2f us per 288-bit frame (symbol slicing, framing, CACH and LC)\n", us);
    dmr_watch_t w; LS_CHECK(dmr_watch_get(&w)); LS_EQ_UINT(w.lc_ok, 10000);
}
