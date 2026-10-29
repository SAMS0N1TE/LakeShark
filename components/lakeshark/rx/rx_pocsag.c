/* See rx_pocsag.h. The decoding is apps/fm/pocsag.c.

   A stream with the discriminator runs three loops (512, 1200, 2400). A chip
   stream (BITS/BATCH only) runs one, made on first use or by
   rx_pocsag_prepare at the chip's bit rate, since the chip hears one rate. */
#include "rx_pocsag.h"
#include "pocsag.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

static const int BAUDS[3] = { 512, 1200, 2400 };

typedef struct {
    pocsag_ctx_t *poc[3];    /* by BAUDS; a chip stream uses poc[0] only */
    fm_state_t   *out;
    uint32_t      rate;      /* DISC sample rate the loops are set for */
    int           baud;      /* the chip's loop */
    bool          chip;
} rx_pocsag_t;

static int baud_index(int baud)
{
    for (int i = 0; i < 3; i++) if (BAUDS[i] == baud) return i;
    return 1;
}

static int baud_at(const rx_pocsag_t *p, int i) { return p->chip ? p->baud : BAUDS[i]; }

static void drop(rx_pocsag_t *p)
{
    for (int i = 0; i < 3; i++) if (p->poc[i]) { pocsag_destroy(p->poc[i]); p->poc[i] = NULL; }
}

static void detach(void *st)
{
    drop(st);
    heap_caps_free(st);
}

static bool make(rx_pocsag_t *p)
{
    drop(p);
    if (p->chip) return true;
    for (int i = 0; i < 3; i++)
        if (!(p->poc[i] = pocsag_create(p->out, BAUDS[i]))) return false;
    return true;
}

static void *attach(struct ls_rx_head *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)h; (void)why; (void)n;
    rx_pocsag_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    p->rate = FM_DEMOD_RATE;
    p->chip = !(s->inputs & LS_RX_DISC);
    if (!make(p)) { detach(p); return NULL; }
    return p;
}

void rx_pocsag_bind(void *st, fm_state_t *out)
{
    rx_pocsag_t *p = st;
    if (!p || p->out == out) return;
    p->out = out;
    make(p);
}

bool rx_pocsag_prepare(void *st, int baud)
{
    rx_pocsag_t *p = st;
    if (!p || !p->chip) return p != NULL;
    p->baud = baud;
    if (p->poc[0]) { pocsag_set_baud(p->poc[0], baud); return true; }
    return (p->poc[0] = pocsag_create(p->out, baud)) != NULL;
}

static void reset(void *st, ls_rx_reset_t why)
{
    rx_pocsag_t *p = st;
    for (int i = 0; i < 3; i++) {
        if (!p->poc[i]) continue;
        if (why >= LS_RX_RESET_RATE) pocsag_reset(p->poc[i]);
        else pocsag_seam(p->poc[i]);
    }
    if (p->out && why == LS_RX_RESET_USER) p->out->pocsag_lock_baud = 0;
}

/* The FM state's counters, and auto baud following the busiest synced loop. */
static void publish(rx_pocsag_t *p)
{
    fm_state_t *o = p->out;
    if (!o) return;
    uint32_t fr = 0, pg = 0, ce = 0, ad = 0, ms = 0, best = 0;
    bool sync = false; int lock = 0;
    for (int i = 0; i < 3; i++) {
        const pocsag_ctx_t *c = p->poc[i];
        if (!c) continue;
        fr += pocsag_n_frames(c); pg += pocsag_n_pages(c); ce += pocsag_n_cwerr(c);
        ad += pocsag_n_addr(c); ms += pocsag_n_msg(c);
        if (pocsag_synced(c) && pocsag_n_frames(c) >= best) {
            sync = true; best = pocsag_n_frames(c); lock = baud_at(p, i);
        }
    }
    o->pocsag_sync = sync; o->pocsag_frames = fr; o->pocsag_pages = pg;
    o->pocsag_cw_errs = ce; o->pocsag_addr = ad; o->pocsag_msg = ms;
    if (lock) {
        o->pocsag_lock_baud = lock;
        if (o->pocsag_auto) o->pocsag_baud = lock;
    }
}

static void process(void *st, const ls_rx_block_t *b)
{
    rx_pocsag_t *p = st;
    if (b->input == LS_RX_DISC) {
        if (b->rate_hz && b->rate_hz != p->rate) {
            p->rate = b->rate_hz;
            for (int i = 0; i < 3; i++) if (p->poc[i]) pocsag_sample_rate(p->poc[i], (int)p->rate);
        }
        const int one = p->out && !p->out->pocsag_auto ? baud_index(p->out->pocsag_baud) : -1;
        for (int i = 0; i < 3; i++)
            if (p->poc[i] && (one < 0 || one == i)) pocsag_process(p->poc[i], b->data, (int)b->n);
    } else {
        /* The chip's rate picks the loop. */
        pocsag_ctx_t *c = p->chip ? p->poc[0] : p->poc[baud_index((int)b->rate_hz)];
        if (p->chip && (!c || p->baud != (int)b->rate_hz)) {
            if (!rx_pocsag_prepare(p, (int)b->rate_hz)) return;
            c = p->poc[0];
        }
        if (b->input == LS_RX_BITS) pocsag_process_bits(c, b->data, b->n);
        else if (b->input == LS_RX_BATCH)
            pocsag_process_batch(c, b->data, (int)b->n, b->flags & LS_RX_B_INVERTED,
                                 b->flags & LS_RX_B_CONTIGUOUS);
    }
    publish(p);
}

static void status(const void *st, ls_rx_status_t *out)
{
    const rx_pocsag_t *p = st;
    uint32_t bf = 0;
    for (int i = 0; i < 3; i++) {
        const pocsag_ctx_t *c = p->poc[i];
        if (!c) continue;
        out->frames += pocsag_n_frames(c);
        out->decoded += pocsag_n_pages(c);
        out->errors += pocsag_n_cwerr(c);
        if (pocsag_synced(c) && pocsag_n_frames(c) >= bf) {
            bf = pocsag_n_frames(c); out->synced = true; out->detail = baud_at(p, i);
        }
    }
}

void rx_pocsag_near(const void *st, int *near_min, uint32_t *near)
{
    const rx_pocsag_t *p = st;
    *near_min = 32; *near = 0;
    for (int i = 0; p && i < 3; i++) {
        if (!p->poc[i]) continue;
        const int m = pocsag_near_min(p->poc[i]);
        if (m < *near_min) *near_min = m;
        *near += pocsag_n_near(p->poc[i]);
    }
}

uint32_t rx_pocsag_frames(const void *st)
{
    ls_rx_status_t s = {0};
    if (st) status(st, &s);
    return s.frames;
}

uint32_t rx_pocsag_pages(const void *st)
{
    ls_rx_status_t s = {0};
    if (st) status(st, &s);
    return s.decoded;
}

const ls_rx_plugin_t rx_pocsag = {
    .abi = LS_RX_ABI, .id = "pocsag", .name = "POCSAG",
    .sub = "pagers 512/1200/2400", .maturity = LS_RX_WORKS,
    .needs = {
        .inputs = LS_RX_DISC | LS_RX_BITS | LS_RX_BATCH, .front = LS_RX_FRONT_NFM,
        .channel_bw_hz = 25000, .min_rate_hz = 32000, .rsel_job = -1,
        .psram_bytes = 3 * 1100, .cpu_permille = 60,
    },
    .doc = "POCSAG pagers from FM (3 rates at once) or the chip's FSK. "
           "Test: MODE POCSAG on a paging channel.",
    .attach = attach, .detach = detach, .reset = reset,
    .process = process, .status = status,
};
