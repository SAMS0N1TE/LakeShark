/* The POCSAG plugin through an RX HEAD: the same page reaches the same page
   ring whether it arrives as the FM discriminator (FM, three baud rates on
   auto), as the chip's clock-recovered FIFO bytes (LABS stream), or as the
   chip's sync-stripped 64-byte batches (LABS / PAGER RECON). */
#include "ls_test.h"
#include "pocsag_gen.h"
#include "rx_pocsag.h"
#include "ls_rx_head.h"

#include <stdlib.h>
#include <string.h>

fm_state_t FM;

static const fm_page_t *last_page(const fm_state_t *o)
{
    if (o->page_count <= 0) return NULL;
    return &o->pages[(o->page_head - 1 + FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX];
}

static void *head_with_pocsag(ls_rx_head_t *h, uint32_t inputs, uint8_t front, fm_state_t *out)
{
    char why[64];
    const ls_rx_stream_t s = { .center_hz = 152600000, .front = front, .inputs = inputs };
    ls_rx_head_init(h, &s);
    LS_EQ_INT(ls_rx_head_attach(h, &rx_pocsag, why, sizeof(why)), 0);
    void *st = ls_rx_head_state(h, &rx_pocsag);
    rx_pocsag_bind(st, out);
    return st;
}

static void via_disc(fm_state_t *out, int baud, uint32_t ric, const char *text)
{
    memset(out, 0, sizeof(*out));
    out->pocsag_auto = true;
    out->pocsag_baud = 1200;
    ls_rx_head_t h;
    head_with_pocsag(&h, LS_RX_DISC | LS_RX_PCM16, LS_RX_FRONT_NFM, out);
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);
    cfg.baud = baud;
    const size_t cap = pocsag_tx_page(&cfg, ric, 3, text, NULL, 0);
    float *buf = malloc(cap * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return;
    const size_t n = pocsag_tx_page(&cfg, ric, 3, text, buf, cap);
    for (size_t i = 0; i < n; i += 84) {   /* the LR path's block size */
        const ls_rx_block_t b = { .input = LS_RX_DISC, .data = buf + i,
            .n = (uint32_t)(n - i < 84 ? n - i : 84), .rate_hz = 32000 };
        ls_rx_head_feed(&h, &b);
    }
    free(buf);
    ls_rx_head_detach_all(&h);
}

static size_t page_bits(uint32_t ric, const char *text, uint8_t *bits, size_t cap)
{
    return pocsag_build_bits(ric, 3, text, 0, bits, cap);
}

static void via_bits(fm_state_t *out, int baud, uint32_t ric, const char *text)
{
    memset(out, 0, sizeof(*out));
    ls_rx_head_t h;
    head_with_pocsag(&h, LS_RX_BITS, LS_RX_FRONT_NONE, out);
    static uint8_t bits[BITS_GEN_MAX], bytes[BITS_GEN_MAX / 8 + 8];
    const size_t nb = page_bits(ric, text, bits, sizeof(bits));
    memset(bytes, 0, sizeof(bytes));
    for (size_t i = 0; i < nb; i++) bytes[i / 8] |= (uint8_t)(bits[i] << (7 - i % 8));
    const size_t nbytes = (nb + 7) / 8;
    /* FIFO reads of 7 bytes cross codeword boundaries. */
    for (size_t i = 0; i < nbytes; i += 7) {
        const ls_rx_block_t b = { .input = LS_RX_BITS, .data = bytes + i,
            .n = (uint32_t)(nbytes - i < 7 ? nbytes - i : 7), .rate_hz = (uint32_t)baud };
        ls_rx_head_feed(&h, &b);
    }
    ls_rx_head_detach_all(&h);
}

static void via_batch(fm_state_t *out, int baud, uint32_t ric, const char *text, bool inverted)
{
    memset(out, 0, sizeof(*out));
    ls_rx_head_t h;
    void *st = head_with_pocsag(&h, LS_RX_BATCH | LS_RX_BITS, LS_RX_FRONT_NONE, out);
    static uint8_t bits[BITS_GEN_MAX];
    page_bits(ric, text, bits, sizeof(bits));
    uint8_t packet[64];
    for (int i = 0; i < 64; i++) {
        packet[i] = 0;
        for (int j = 0; j < 8; j++)
            packet[i] = (uint8_t)((packet[i] << 1) | (bits[32 + i * 8 + j] ^ (inverted ? 1 : 0)));
    }
    const ls_rx_block_t b = { .input = LS_RX_BATCH, .data = packet, .n = 64, .rate_hz = (uint32_t)baud,
        .flags = inverted ? LS_RX_B_INVERTED : 0 };
    ls_rx_head_feed(&h, &b);
    LS_EQ_UINT(rx_pocsag_pages(st), 1);
    ls_rx_head_detach_all(&h);
}

LS_CASE(one_page_is_the_same_page_through_all_three_inputs)
{
    static const int bauds[3] = { 512, 1200, 2400 };
    for (int k = 0; k < 3; k++) {
        static fm_state_t a, b, c, d;
        via_disc(&a, bauds[k], 1234560, "UNIT 4 RESPOND TO STATION");
        via_bits(&b, bauds[k], 1234560, "UNIT 4 RESPOND TO STATION");
        via_batch(&c, bauds[k], 1234560, "UNIT 4 RESPOND TO STATION", false);
        via_batch(&d, bauds[k], 1234560, "UNIT 4 RESPOND TO STATION", true);
        const fm_state_t *all[4] = { &a, &b, &c, &d };
        for (int i = 0; i < 4; i++) {
            const fm_page_t *p = last_page(all[i]);
            LS_CHECK_MSG(p != NULL, "baud %d input %d: no page", bauds[k], i);
            if (!p) continue;
            LS_EQ_INT(all[i]->page_count, 1);
            LS_EQ_UINT(p->address, 1234560);
            LS_EQ_INT(p->type, 'A');
            LS_EQ_STR(p->text, "UNIT 4 RESPOND TO STATION");
            LS_EQ_INT(p->baud, bauds[k]);
            LS_EQ_UINT(all[i]->pocsag_pages, 1);
        }
        /* Auto baud locked on the rate that was sent. */
        LS_EQ_INT(a.pocsag_lock_baud, bauds[k]);
        LS_EQ_INT(a.pocsag_baud, bauds[k]);
    }
}

LS_CASE(a_fixed_baud_runs_only_that_loop)
{
    static fm_state_t o;
    memset(&o, 0, sizeof(o));
    o.pocsag_auto = false;
    o.pocsag_baud = 512;
    ls_rx_head_t h;
    head_with_pocsag(&h, LS_RX_DISC, LS_RX_FRONT_NFM, &o);
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);
    cfg.baud = 1200;
    const size_t cap = pocsag_tx_page(&cfg, 777, 3, "WRONG RATE", NULL, 0);
    float *buf = malloc(cap * sizeof(float));
    const size_t n = pocsag_tx_page(&cfg, 777, 3, "WRONG RATE", buf, cap);
    const ls_rx_block_t b = { .input = LS_RX_DISC, .data = buf, .n = (uint32_t)n, .rate_hz = 32000 };
    ls_rx_head_feed(&h, &b);
    free(buf);
    LS_EQ_INT(o.page_count, 0);
    ls_rx_head_detach_all(&h);
}

LS_CASE(a_user_reset_clears_counts_and_the_baud_lock)
{
    static fm_state_t o;
    via_disc(&o, 1200, 42, "HELLO WORLD");
    LS_EQ_INT(o.pocsag_lock_baud, 1200);
    ls_rx_head_t h;
    void *st = head_with_pocsag(&h, LS_RX_DISC, LS_RX_FRONT_NFM, &o);
    o.pocsag_lock_baud = 1200;
    ls_rx_head_reset(&h, LS_RX_RESET_USER);
    LS_EQ_INT(o.pocsag_lock_baud, 0);
    LS_EQ_UINT(rx_pocsag_frames(st), 0);
    ls_rx_status_t s;
    LS_CHECK(ls_rx_head_status(&h, 0, &s));
    LS_EQ_STR(s.chip, "POCSAG");
    LS_CHECK(!s.synced);
    ls_rx_head_detach_all(&h);
}

/* Unbound, it still counts what it hears (PAGER RECON's quiet mode). */
LS_CASE(unbound_it_counts_without_storing)
{
    ls_rx_head_t h;
    char why[64];
    const ls_rx_stream_t s = { .front = LS_RX_FRONT_NONE, .inputs = LS_RX_BATCH };
    ls_rx_head_init(&h, &s);
    LS_EQ_INT(ls_rx_head_attach(&h, &rx_pocsag, why, sizeof(why)), 0);
    void *st = ls_rx_head_state(&h, &rx_pocsag);
    static uint8_t bits[BITS_GEN_MAX];
    page_bits(99, "QUIET", bits, sizeof(bits));
    uint8_t packet[64];
    for (int i = 0; i < 64; i++) {
        packet[i] = 0;
        for (int j = 0; j < 8; j++) packet[i] = (uint8_t)((packet[i] << 1) | bits[32 + i * 8 + j]);
    }
    const ls_rx_block_t b = { .input = LS_RX_BATCH, .data = packet, .n = 64, .rate_hz = 1200 };
    ls_rx_head_feed(&h, &b);
    LS_EQ_UINT(rx_pocsag_pages(st), 1);
    ls_rx_head_detach_all(&h);
}
