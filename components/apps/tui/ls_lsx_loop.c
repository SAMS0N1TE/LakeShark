/* Two LSX phys joined by a simulated channel, for the bench and lssim
   (ls_lsx.h). Host builds only: the board's phy is ls_lsx_svc.c. */
#ifndef ESP_PLATFORM
#include "ls_lsx.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    struct ls_lsx_loop *l;
    int end;
} loop_ctx_t;

typedef struct {
    bool held, blocked, txing;
    int mode;
    uint32_t hz, tx_end;
    uint8_t txf[LSX_FRAME_MAX];
    int txlen;
    struct { uint8_t f[LSX_FRAME_MAX]; int len; } q[16];
    int qh, qn;
    uint32_t delivered;
} loop_end_t;

struct ls_lsx_loop {
    loop_end_t e[2];
    loop_ctx_t ctx[2];
    ls_lsx_phy_t phy[2];
    uint32_t now, rng;
    uint8_t loss;
    int8_t rssi;
};

static uint32_t lrand(struct ls_lsx_loop *l)
{
    l->rng ^= l->rng << 13;
    l->rng ^= l->rng >> 17;
    l->rng ^= l->rng << 5;
    return l->rng;
}

static bool l_claim(void *c, bool on)
{
    loop_ctx_t *k = c;
    loop_end_t *e = &k->l->e[k->end];
    if (!on) { e->held = false; e->txing = false; e->mode = -1; e->qn = 0; return false; }
    if (e->blocked) return false;
    e->held = true;
    return true;
}

static bool l_yield(void *c)
{
    loop_ctx_t *k = c;
    return k->l->e[k->end].blocked;
}

static int l_tune(void *c, int mode, uint32_t hz, int8_t dbm)
{
    (void)dbm;
    loop_ctx_t *k = c;
    loop_end_t *e = &k->l->e[k->end];
    if (!e->held) return -1;
    const ls_lsx_mode_info_t *m = ls_lsx_mode_info(mode);
    if ((m->kind == LSX_K_FLRC && !(k->l->phy[k->end].caps & LSX_CAP_FLRC)) ||
        (m->kind == LSX_K_GFSK && !(k->l->phy[k->end].caps & LSX_CAP_GFSK)))
        return -1;
    e->mode = mode;
    e->hz = hz;
    e->qn = 0;        /* a retune loses what was arriving */
    return 0;
}

static int l_send(void *c, const uint8_t *f, int len)
{
    loop_ctx_t *k = c;
    loop_end_t *e = &k->l->e[k->end];
    if (!e->held || e->txing || len <= 0 || len > LSX_FRAME_MAX) return -1;
    memcpy(e->txf, f, (size_t)len);
    e->txlen = len;
    e->txing = true;
    const uint32_t ms = (ls_lsx_airtime_us(e->mode, len) + 999) / 1000;
    e->tx_end = k->l->now + (ms ? ms : 1);
    return 0;
}

static int l_tx_done(void *c)
{
    loop_ctx_t *k = c;
    struct ls_lsx_loop *l = k->l;
    loop_end_t *e = &l->e[k->end], *o = &l->e[!k->end];
    if (!e->txing) return 1;
    if ((int32_t)(l->now - e->tx_end) < 0) return 0;
    e->txing = false;
    /* Heard only by an end that holds its radio on the same mode and
       channel and is not sending itself. */
    if (o->held && !o->txing && o->mode == e->mode && o->hz == e->hz && (lrand(l) & 0xFF) >= l->loss &&
        o->qn < 16) {
        const int at = (o->qh + o->qn) % 16;
        memcpy(o->q[at].f, e->txf, (size_t)e->txlen);
        o->q[at].len = e->txlen;
        o->qn++;
    }
    return 1;
}

static int l_recv(void *c, uint8_t *buf, int cap, ls_lsx_rxmeta_t *m)
{
    loop_ctx_t *k = c;
    loop_end_t *e = &k->l->e[k->end];
    if (!e->held || !e->qn) return 0;
    const int len = e->q[e->qh].len;
    if (len > cap) return -1;
    memcpy(buf, e->q[e->qh].f, (size_t)len);
    e->qh = (e->qh + 1) % 16;
    e->qn--;
    e->delivered++;
    if (m) {
        m->rssi = (int8_t)(k->l->rssi + (int)(lrand(k->l) % 5) - 2);
        m->snr = ls_lsx_mode_info(e->mode)->kind == LSX_K_LORA ? 8 : 127;
    }
    return len;
}

ls_lsx_loop_t *ls_lsx_loop_create(uint32_t caps_a, uint32_t caps_b, uint32_t seed)
{
    struct ls_lsx_loop *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->rng = seed ? seed : 0x1234567u;
    l->rssi = -60;
    for (int i = 0; i < 2; i++) {
        l->ctx[i].l = l;
        l->ctx[i].end = i;
        l->e[i].mode = -1;
        l->phy[i] = (ls_lsx_phy_t){ &l->ctx[i], i ? caps_b : caps_a, l_claim, l_yield, l_tune, l_send, l_tx_done, l_recv };
    }
    return l;
}

void ls_lsx_loop_destroy(ls_lsx_loop_t *l) { free(l); }
const ls_lsx_phy_t *ls_lsx_loop_phy(ls_lsx_loop_t *l, int end) { return &l->phy[end ? 1 : 0]; }
void ls_lsx_loop_set(ls_lsx_loop_t *l, uint8_t loss, int8_t rssi) { l->loss = loss; l->rssi = rssi; }
void ls_lsx_loop_time(ls_lsx_loop_t *l, uint32_t now_ms) { l->now = now_ms; }
void ls_lsx_loop_block(ls_lsx_loop_t *l, int end, bool blocked) { l->e[end ? 1 : 0].blocked = blocked; }
uint32_t ls_lsx_loop_frames(ls_lsx_loop_t *l, int end) { return l->e[end ? 1 : 0].delivered; }
#endif
