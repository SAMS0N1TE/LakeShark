
#include "pocsag.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pocsag";

#define POCSAG_FSC      0x7CD215D8u
#define POCSAG_IDLE     0x7A89C197u
#define POCSAG_BCH_POLY 0x769u
#define SYNC_TOL        2
#define MSG_BITS_MAX    640

typedef enum { ST_HUNT = 0, ST_BATCH } pstate_t;

struct pocsag_ctx {
    fm_state_t *out;
    int   baud;

    float inc;
    float acc;
    float thr;
    int   prev_slice;
    float sum_a;
    float sum_b;

    uint32_t sr;
    uint32_t sr_b;
    pstate_t st;
    int      invert;
    uint32_t cw;
    int      cw_bits;
    int      cw_idx;
    int      idle_run;

    int      have_addr;
    uint32_t cur_addr;
    int      cur_func;
    uint8_t  bits[MSG_BITS_MAX];
    int      nbits;

    bool     synced;
    uint32_t n_frames;
    uint32_t n_pages;
    uint32_t n_cwerr;
    uint32_t n_addr;
    uint32_t n_msg;

    int      near_min;
    uint32_t n_near;
};

static uint32_t bch_syndrome(uint32_t cw)
{

    uint32_t reg = cw >> 1;
    for (int b = 30; b >= 10; b--) {
        if (reg & (1u << b)) reg ^= (POCSAG_BCH_POLY << (b - 10));
    }
    return reg & 0x3FFu;
}

static int even_parity_bad(uint32_t cw)
{
    uint32_t v = cw;
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (int)(v & 1u);
}

static int bch_fix(uint32_t *cwp)
{
    uint32_t cw = *cwp;
    if (bch_syndrome(cw) == 0 && !even_parity_bad(cw)) return 1;
    for (int i = 0; i < 32; i++) {
        uint32_t t = cw ^ (1u << i);
        if (bch_syndrome(t) == 0 && !even_parity_bad(t)) { *cwp = t; return 1; }
    }
    return 0;
}

static int popcount32(uint32_t v)
{
    int n = 0; while (v) { v &= v - 1; n++; } return n;
}

static const char NUM_MAP[16] = {
    '0','1','2','3','4','5','6','7','8','9','*','U',' ','-',')','('
};

static void flush_message(pocsag_ctx_t *c)
{
    if (!c->have_addr) { c->nbits = 0; return; }

    fm_page_t pg;
    memset(&pg, 0, sizeof(pg));
    pg.ts_us    = esp_timer_get_time();
    pg.address  = c->cur_addr;
    pg.function = (uint8_t)c->cur_func;
    pg.baud     = (uint16_t)c->baud;

    if (c->nbits == 0) {
        pg.type = 'T';
        snprintf(pg.text, sizeof(pg.text), "(tone)");
    } else {

        char alpha[FM_PAGE_TEXT_MAX]; int an = 0, printable = 0;
        for (int i = 0; i + 7 <= c->nbits && an < FM_PAGE_TEXT_MAX - 1; i += 7) {
            int ch = 0;
            for (int k = 0; k < 7; k++) ch |= (c->bits[i + k] & 1) << k;
            if (ch == 0) continue;
            alpha[an++] = (char)ch;
            if (ch >= 0x20 && ch <= 0x7e) printable++;
        }
        alpha[an] = 0;

        char num[FM_PAGE_TEXT_MAX]; int nn = 0;
        for (int i = 0; i + 4 <= c->nbits && nn < FM_PAGE_TEXT_MAX - 1; i += 4) {
            int v = 0;
            for (int k = 0; k < 4; k++) v |= (c->bits[i + k] & 1) << k;
            num[nn++] = NUM_MAP[v & 0xf];
        }
        num[nn] = 0;

        int alpha_ok = (an > 0) && (printable * 10 >= an * 6);
        if (c->cur_func == 3 || alpha_ok) {
            pg.type = 'A';
            snprintf(pg.text, sizeof(pg.text), "%s", alpha);
        } else {
            pg.type = 'N';
            snprintf(pg.text, sizeof(pg.text), "%s", num);
        }
    }

    fm_state_t *o = c->out;
    o->pages[o->page_head] = pg;
    o->page_head = (o->page_head + 1) % FM_PAGE_LOG_MAX;
    if (o->page_count < FM_PAGE_LOG_MAX) o->page_count++;
    c->n_pages++;
    ESP_LOGI(TAG, "page RIC=%lu F=%d %c '%s'",
             (unsigned long)pg.address, pg.function, pg.type, pg.text);

    c->nbits = 0;
    c->have_addr = 0;
}

static void process_codeword(pocsag_ctx_t *c, uint32_t cw, int idx)
{
    if (!bch_fix(&cw)) { c->n_cwerr++; return; }

    if (cw == POCSAG_IDLE) { flush_message(c); return; }

    if ((cw & 0x80000000u) == 0) {

        flush_message(c);
        uint32_t addr18 = (cw >> 13) & 0x3FFFFu;
        int func        = (int)((cw >> 11) & 0x3u);
        int frame       = idx / 2;
        c->cur_addr  = (addr18 << 3) | (uint32_t)frame;
        c->cur_func  = func;
        c->have_addr = 1;
        c->nbits     = 0;
        c->n_addr++;
    } else {

        if (!c->have_addr) return;
        for (int b = 30; b >= 11 && c->nbits < MSG_BITS_MAX; b--)
            c->bits[c->nbits++] = (uint8_t)((cw >> b) & 1u);
        c->n_msg++;
    }
}

pocsag_ctx_t *pocsag_create(fm_state_t *out, int baud)
{
    pocsag_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->out = out;
    pocsag_set_baud(c, baud);
    pocsag_reset(c);
    return c;
}

void pocsag_destroy(pocsag_ctx_t *c) { if (c) free(c); }

void pocsag_set_baud(pocsag_ctx_t *c, int baud)
{
    if (baud != 512 && baud != 1200 && baud != 2400) baud = 1200;
    c->baud = baud;
    c->inc  = (float)baud / (float)FM_DEMOD_RATE;
}

void pocsag_reset(pocsag_ctx_t *c)
{
    c->acc = 0.0f; c->thr = 0.0f; c->prev_slice = 0;
    c->sum_a = 0.0f; c->sum_b = 0.0f;
    c->sr = 0; c->sr_b = 0; c->st = ST_HUNT; c->invert = 0;
    c->cw = 0; c->cw_bits = 0; c->cw_idx = 0; c->idle_run = 0;
    c->have_addr = 0; c->nbits = 0;
    c->synced = false; c->n_frames = 0; c->n_pages = 0; c->n_cwerr = 0;
    c->n_addr = 0; c->n_msg = 0;
    c->near_min = 32; c->n_near = 0;
}

int      pocsag_near_min(const pocsag_ctx_t *c) { return c ? c->near_min : 32; }
uint32_t pocsag_n_near(const pocsag_ctx_t *c) { return c ? c->n_near : 0; }

bool     pocsag_synced(const pocsag_ctx_t *c) { return c && c->synced; }
int      pocsag_baud_of(const pocsag_ctx_t *c) { return c ? c->baud : 0; }
uint32_t pocsag_n_frames(const pocsag_ctx_t *c) { return c ? c->n_frames : 0; }
uint32_t pocsag_n_pages(const pocsag_ctx_t *c) { return c ? c->n_pages : 0; }
uint32_t pocsag_n_cwerr(const pocsag_ctx_t *c) { return c ? c->n_cwerr : 0; }
uint32_t pocsag_n_addr(const pocsag_ctx_t *c) { return c ? c->n_addr : 0; }
uint32_t pocsag_n_msg(const pocsag_ctx_t *c) { return c ? c->n_msg : 0; }

static void handle_bit(pocsag_ctx_t *c, int raw_bit)
{

    c->sr = (c->sr << 1) | (uint32_t)(raw_bit & 1);

    if (c->st == ST_HUNT) {
        uint32_t d0 = popcount32(c->sr ^ POCSAG_FSC);
        uint32_t d1 = popcount32((~c->sr) ^ POCSAG_FSC);
        uint32_t best = d0 < d1 ? d0 : d1;
        if ((int)best < c->near_min) c->near_min = (int)best;
        if (best <= 6) c->n_near++;
        if (d0 <= SYNC_TOL) {
            c->invert = 0; c->st = ST_BATCH;
        } else if (d1 <= SYNC_TOL) {
            c->invert = 1; c->st = ST_BATCH;
        } else {
            return;
        }
        c->cw = 0; c->cw_bits = 0; c->cw_idx = 0;
        c->synced = true;
        c->n_frames++;
        return;
    }

    int b = c->invert ? (raw_bit ^ 1) : raw_bit;
    c->cw = (c->cw << 1) | (uint32_t)(b & 1);
    if (++c->cw_bits < 32) return;
    c->cw_bits = 0;
    uint32_t cw = c->cw; c->cw = 0;

    if (c->cw_idx < 16) {
        process_codeword(c, cw, c->cw_idx);
        c->cw_idx++;
    } else {

        if (popcount32(cw ^ POCSAG_FSC) <= SYNC_TOL) {
            c->cw_idx = 0;
            c->n_frames++;
        } else {
            flush_message(c);
            c->st = ST_HUNT;
            c->synced = false;
        }
    }
}

void pocsag_process(pocsag_ctx_t *c, const float *demod, int n)
{
    for (int i = 0; i < n; i++) {
        float x = demod[i];
        c->thr += 0.0015f * (x - c->thr);
        int slice = (x > c->thr) ? 1 : 0;
        float dev = x - c->thr;

        c->sum_a += dev;
        c->sum_b += dev;

        if (slice == c->prev_slice) {
            if (++c->idle_run > FM_DEMOD_RATE / 4) {
                if (c->st == ST_BATCH) { flush_message(c); }
                c->st = ST_HUNT; c->synced = false; c->idle_run = 0;
            }
        } else {
            c->idle_run = 0;

            float err = c->acc - 0.5f;
            c->acc -= 0.10f * err;
        }
        c->prev_slice = slice;

        float old = c->acc;
        c->acc += c->inc;

        if (old < 0.5f && c->acc >= 0.5f) {
            int bit_b = (c->sum_b > 0.0f) ? 1 : 0;
            c->sum_b = 0.0f;
            if (c->st == ST_HUNT) {
                c->sr_b = (c->sr_b << 1) | (uint32_t)bit_b;
                uint32_t d0 = popcount32(c->sr_b ^ POCSAG_FSC);
                uint32_t d1 = popcount32((~c->sr_b) ^ POCSAG_FSC);
                uint32_t best = d0 < d1 ? d0 : d1;
                if ((int)best < c->near_min) c->near_min = (int)best;
                if (best <= 6) c->n_near++;
                if (d0 <= SYNC_TOL || d1 <= SYNC_TOL) {
                    c->invert = (d1 < d0) ? 1 : 0;
                    c->sr = c->sr_b;
                    c->st = ST_BATCH;
                    c->synced = true; c->n_frames++;
                    c->cw = 0; c->cw_bits = 0; c->cw_idx = 0;
                    c->acc = 0.0f; c->sum_a = 0.0f;
                    continue;
                }
            }
        }

        if (c->acc >= 1.0f) {
            c->acc -= 1.0f;
            int bit_a = (c->sum_a > 0.0f) ? 1 : 0;
            c->sum_a = 0.0f;
            handle_bit(c, bit_a);
        }
    }
}
