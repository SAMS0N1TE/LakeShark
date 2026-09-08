
#include "pocsag.h"
#include "esp_timer.h"
#include "esp_log.h"
/*LS-200*/
#include "ls_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/*LS-826  Does this look like a message a person would read?

   The old test was `printable * 10 >= an * 6` - any 60% printable ASCII - and
   it let two kinds of rubbish onto the screen: symbol soup (a run of
   {} ^ ~ $ < > % and stray letters) and letter soup ("oiawoiahdoihaw").
   Both are ~100% "printable", so both were shown as ALPHA messages.

   Character classes alone cannot separate those - letter soup is all
   letters. What separates real traffic is STRUCTURE: real pages carry spaces
   at sane intervals and have no improbably long unbroken run. Checked
   against this site's own traffic:
     "CODE STROKE B HALLWAY 08"                 -> scores high
     "From: <clinician> Subject: Order for ..." -> scores high
     symbol soup                                -> symbol penalty
     "oiawoiahdoihaw"                           -> no-space penalty

   Returns 0..100. Deliberately cheap: runs once per page, not per sample.

   Extended for the short-string blind spot: the structural signals below
   ("expects a space", "long runs are suspicious") cannot fire until there
   are enough characters to see them, so anything under 12 chars scored 100
   by default. A 7-digit numeric callback packs into 28 message bits, and
   28 / 7 = 4 - so it renders as 4 printable ASCII bytes ("U*H!" for
   RIC 1234568 carrying "5551234") and out-voted the correct numeric decode
   every time. Measured on the host bench: numeric_page_decodes_as_numeric
   in test_pocsag.c reproduced it in isolation. Fix: cap the score below the
   accept threshold when n is short. Short means "no evidence", not
   "confident yes" - if a plausible numeric reading exists, let it win. Real
   short alpha pages ("OK", "CALL ME") still ride the cur_func == 3 branch
   in the classifier below, which is what pagers actually send them on. */
static int pocsag_text_score(const char *s, int n)
{
    if (n <= 0) return 0;

    int letters = 0, digits = 0, spaces = 0, punct = 0, weird = 0;
    int run = 0, maxrun = 0;

    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7e) return 0;      /* control byte: not text at all */

        if (ch == ' ')                                      { spaces++;  run = 0; }
        else if ((ch >= 'A' && ch <= 'Z') ||
                 (ch >= 'a' && ch <= 'z'))                  { letters++; run++;   }
        else if (ch >= '0' && ch <= '9')                    { digits++;  run++;   }
        else if (strchr(".,:;'\"-/()!?@#&%+=*", (char)ch))  { punct++;   run++;   }
        else                                                { weird++;   run++;   }

        if (run > maxrun) maxrun = run;
    }

    int score = (letters + digits + spaces + punct) * 100 / n;

    /* Brace/caret/tilde/dollar barely appear in real pager traffic. */
    if (weird * 10 > n)         score -= 40;
    /* Anything long enough to be worth reading contains a space. */
    if (n >= 12 && spaces == 0) score -= 40;
    /* 24 characters with no break is not a word. */
    if (maxrun > 24)            score -= 25;

    /* Short strings offer no structure either way, so a "clean" 100 here is
       not evidence of text - it is absence of evidence. Cap below the 70
       accept threshold so the classifier falls through to the numeric branch
       when there is a plausible numeric reading. */
    if (n < 12 && score > 60)   score = 60;

    return score < 0 ? 0 : score;
}

static void flush_message(pocsag_ctx_t *c)
{
    if (!c->have_addr) { c->nbits = 0; return; }

    fm_page_t pg;
    memset(&pg, 0, sizeof(pg));
    pg.ts_us    = esp_timer_get_time();
    /*LS-200  Real epoch only when the wall clock is known - otherwise 0,
       which the page log formatter reads as "uptime only". A page written
       to the log before SNTP replied stays uptime-stamped forever; the
       renderer decides what to show, not this. */
    pg.ts_epoch = ls_time_is_synced() ? (int64_t)time(NULL) : 0;
    pg.address  = c->cur_addr;
    pg.function = (uint8_t)c->cur_func;
    pg.protocol = FM_PAGE_PROTOCOL_POCSAG;
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

        /*LS-826  Function code alone does not decide this. On the site used
           for testing, F=0 carried "CODE STROKE B HALLWAY 08" and F=2 carried
           a full text page, so anything keyed on "3 means alphanumeric" is
           wrong here. Score the characters instead, and when neither reading
           holds up say so ('?') rather than dressing mush up as a message. */
        int alpha_score = pocsag_text_score(alpha, an);
        int num_digits  = 0;
        for (int i = 0; i < nn; i++) if (num[i] >= '0' && num[i] <= '9') num_digits++;

        (void)printable;
        if (c->cur_func == 3 || alpha_score >= 70) {
            pg.type = 'A';
            snprintf(pg.text, sizeof(pg.text), "%s", alpha);
        } else if (nn > 0 && num_digits * 2 >= nn) {
            pg.type = 'N';
            snprintf(pg.text, sizeof(pg.text), "%s", num);
        } else {
            /* A RIC that keyed up is still worth seeing, so keep the page -
               just do not claim it is readable text. */
            pg.type = '?';
            snprintf(pg.text, sizeof(pg.text), "%s", nn > 0 ? num : alpha);
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

        /*LS-818  ONE sampling phase, and it must be the aligned one.

           This used to run two integrate-and-dump samplers half a symbol
           apart: sum_b dumped at the acc 0.5 crossing and hunted the frame
           sync, sum_a dumped at the acc 1.0 crossing and fed handle_bit with
           the codeword bits. They cannot both be right, and it was the DATA
           one that was wrong.

           The timing loop above pulls acc toward 0.5 on every transition, so
           once locked a transition sits at acc = 0.5. sum_b therefore
           integrates transition-to-transition - exactly one symbol, correctly
           aligned. sum_a integrates from half a symbol AFTER a transition to
           half a symbol after the next, so its window STRADDLES a transition
           every time and its output is close to a coin toss.

           Measured on 152.6000 before this change: frame sync matched
           EXACTLY (near_min=0, the b phase is clean), then 147 of ~160
           codewords failed BCH - about 92%. Every so often an address
           codeword survived on its own, which produced a page with no message
           bits behind it, which flush_message() correctly renders as
           "(tone)". That is the long-standing "POCSAG only ever shows tones":
           not a tone-only pager, a data sampler reading across transitions.

           So: sample once, at the phase the timing loop aligns, and hand it to
           handle_bit for both sync hunting and codeword assembly. handle_bit
           already carries its own sync detector and the near_min/n_near
           instrumentation, so nothing is lost by dropping the duplicate. */
        if (old < 0.5f && c->acc >= 0.5f) {
            int bit = (c->sum_b > 0.0f) ? 1 : 0;
            c->sum_b = 0.0f;
            handle_bit(c, bit);
        }

        if (c->acc >= 1.0f) {
            c->acc -= 1.0f;
            c->sum_a = 0.0f;   /* kept dumped so it cannot drift into a stale sum */
        }
    }
}
