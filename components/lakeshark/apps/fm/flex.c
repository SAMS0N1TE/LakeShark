
#include "flex.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ls_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "flex";

/* Wire constants, mirrored from bench/fixtures/flex_gen.c.  Duplicated on
   purpose - the decoder is what runs on device, the fixture is host-only, and
   the contract between them is these constants. */
#define FLEX_SYNC_A     0xA6C65939u
#define FLEX_BCH_POLY   0x769u
#define SYNC_TOL        3      /* accept up to 3 bit errors in the 32-bit sync */
#define MODE_TOL        4      /* accept up to 4 bit errors in the mode word */
#define BLOCK_BITS      256    /* 8 codewords x 32 bits, interleaved */

static const uint32_t FLEX_MODE_WORDS[4] = {
    0x870CF8F3u, 0xB0684F97u, 0x3D9CC263u, 0x14D2EB2Du,
};
static const int MODE_BIT_RATE[4] = { 1600, 3200, 3200, 6400 };
static const int MODE_SYM_RATE[4] = { 1600, 3200, 1600, 3200 };
static const int MODE_LEVELS  [4] = { 2, 2, 4, 4 };

typedef enum {
    ST_HUNT = 0,   /* shifting bits, correlating against FLEX_SYNC_A */
    ST_MODE,       /* just synced, collecting the 32-bit mode word */
    ST_FIW,        /* mode word validated, collecting the 32-bit FIW */
    ST_BLOCK,      /* FIW BCH-decoded, collecting the 256-bit payload */
} fstate_t;

struct flex_ctx {
    fm_state_t    *out;
    ls_flex_mode_t mode;

    /* Bit slicer / timing loop */
    float inc, acc;
    float sum;
    float thr;

    float lvl_est;
    int   prev_slice;
    int   idle_run;
    int   bits_per_sym;   /* 1 for 2-FSK, 2 for 4-FSK */

    /* Sync hunt state */
    uint32_t sr;
    int      invert;
    int      near_min;
    uint32_t n_near;

    /* Post-sync bit collector */
    fstate_t st;
    int      bits_left;
    uint32_t reg;
    uint8_t  block_bits[BLOCK_BITS];
    int      block_idx;
    uint32_t mode_word;
    uint32_t fiw;

    /* Frame state */
    bool           synced;
    ls_flex_mode_t detected_mode;
    uint32_t       n_frames;
    uint32_t       n_pages;
    uint32_t       n_cwerr;
};

static int popcount32(uint32_t v)
{
    int n = 0; while (v) { v &= v - 1; n++; } return n;
}

static uint32_t bch_syndrome(uint32_t cw)
{
    uint32_t reg = cw >> 1;
    for (int b = 30; b >= 10; b--)
        if (reg & (1u << b)) reg ^= (FLEX_BCH_POLY << (b - 10));
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

static void deinterleave(const uint8_t *bits256, uint32_t *cws)
{
    for (int r = 0; r < 8; r++) cws[r] = 0;
    for (int c = 0; c < 32; c++)
        for (int r = 0; r < 8; r++)
            if (bits256[c * 8 + r]) cws[r] |= (1u << (31 - c));
}

static int mode_of_word(uint32_t word)
{
    int best = -1, best_d = 33;
    for (int m = 0; m < 4; m++) {
        int d = (int)popcount32(word ^ FLEX_MODE_WORDS[m]);
        if (d < best_d) { best_d = d; best = m; }
    }
    return (best_d <= MODE_TOL) ? best : -1;
}

static void reset_hunt(flex_ctx_t *c)
{
    c->st = ST_HUNT;
    c->sr = 0;
    c->invert = 0;
    c->reg = 0;
    c->bits_left = 0;
    c->block_idx = 0;
    c->synced = false;
    c->idle_run = 0;
}

/* Text quality score.  Same shape as pocsag.c::pocsag_text_score - see for why character-class ratios alone are not enough.  A block whose
   codewords BCH-fixed to random-looking payloads must not render as alpha. */
static int flex_text_score(const char *s, int n)
{
    if (n <= 0) return 0;
    int letters = 0, digits = 0, spaces = 0, punct = 0, weird = 0;
    int run = 0, maxrun = 0;
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7e) return 0;
        if (ch == ' ') { spaces++; run = 0; }
        else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) { letters++; run++; }
        else if (ch >= '0' && ch <= '9') { digits++; run++; }
        else if (strchr(".,:;'\"-/()!?@#&%+=*", (char)ch)) { punct++; run++; }
        else { weird++; run++; }
        if (run > maxrun) maxrun = run;
    }
    int score = (letters + digits + spaces + punct) * 100 / n;
    if (weird * 10 > n)         score -= 40;
    if (n >= 12 && spaces == 0) score -= 40;
    if (maxrun > 24)            score -= 25;
    if (n < 12 && score > 60)   score = 60;
    return score < 0 ? 0 : score;
}

static const char NUM_MAP[16] = {
    '0','1','2','3','4','5','6','7','8','9','*','U',' ','-',')','('
};

/* One full 256-bit block collected - deinterleave, BCH-fix, parse, and either
   emit a page or return to hunt. */
static void process_block(flex_ctx_t *c)
{
    uint32_t cws[8];
    deinterleave(c->block_bits, cws);

    int cw_ok[8];
    int ok_count = 0;
    for (int i = 0; i < 8; i++) {
        cw_ok[i] = bch_fix(&cws[i]);
        if (cw_ok[i]) ok_count++;
        else c->n_cwerr++;
    }

    (void)ok_count;    /* counted into c->n_cwerr for diagnostics */
    if (!cw_ok[0] || !cw_ok[1]) {
        reset_hunt(c);
        return;
    }

    uint32_t biw_data21  = (cws[0] >> 11) & 0x1FFFFFu;
    int msg_type    = (int)((biw_data21 >> 20) & 1);
    int msg_len_cws = (int)((biw_data21 >> 13) & 0x7F);
    if (msg_len_cws > 6) msg_len_cws = 6;

    /* All message CWs the BIW claims must decode - a hole in the message
       is worse than reporting no page. */
    for (int i = 0; i < msg_len_cws && 2 + i < 8; i++) {
        if (!cw_ok[2 + i]) { reset_hunt(c); return; }
    }

    uint32_t ric = (cws[1] >> 11) & 0x1FFFFFu;

    fm_page_t pg;
    memset(&pg, 0, sizeof(pg));
    pg.ts_us    = esp_timer_get_time();
    pg.ts_epoch = ls_time_is_synced() ? (int64_t)time(NULL) : 0;
    pg.address  = ric;
    pg.function = 0;
    pg.protocol = FM_PAGE_PROTOCOL_FLEX;
    pg.baud     = (uint16_t)MODE_BIT_RATE[c->mode];

    if (msg_len_cws == 0) {
        pg.type = 'T';
        snprintf(pg.text, sizeof(pg.text), "(tone)");
    } else {
        char alpha[FM_PAGE_TEXT_MAX]; int an = 0;
        char num  [FM_PAGE_TEXT_MAX]; int nn = 0;

        for (int i = 0; i < msg_len_cws && 2 + i < 8; i++) {
            if (!cw_ok[2 + i]) continue;
            uint32_t d21 = (cws[2 + i] >> 11) & 0x1FFFFFu;

            int c0 = (int)((d21 >> 14) & 0x7F);
            int c1 = (int)((d21 >> 7)  & 0x7F);
            int c2 = (int)( d21        & 0x7F);
            if (an + 3 < FM_PAGE_TEXT_MAX) {
                if (c0) alpha[an++] = (char)c0;
                if (c1) alpha[an++] = (char)c1;
                if (c2) alpha[an++] = (char)c2;
            }

            int d0 = (int)((d21 >> 16) & 0xF);
            int d1 = (int)((d21 >> 12) & 0xF);
            int d2 = (int)((d21 >> 8)  & 0xF);
            int d3 = (int)((d21 >> 4)  & 0xF);
            int d4 = (int)( d21        & 0xF);
            if (nn + 5 < FM_PAGE_TEXT_MAX) {
                num[nn++] = NUM_MAP[d0];
                num[nn++] = NUM_MAP[d1];
                num[nn++] = NUM_MAP[d2];
                num[nn++] = NUM_MAP[d3];
                num[nn++] = NUM_MAP[d4];
            }
        }
        alpha[an] = 0;
        num[nn] = 0;

        if (msg_type == 1 && nn > 0) {
            pg.type = 'N';
            snprintf(pg.text, sizeof(pg.text), "%s", num);
        } else if (msg_type == 0 && an > 0) {

            if (flex_text_score(alpha, an) > 0) {
                pg.type = 'A';
                snprintf(pg.text, sizeof(pg.text), "%s", alpha);
            } else {
                reset_hunt(c);
                return;
            }
        } else {
            /* BIW said this was a message but the payload does not read
               as one - precedent from POCSAG: showing rubbish is
               worse than showing nothing. */
            reset_hunt(c);
            return;
        }
    }

    fm_state_t *o = c->out;
    o->pages[o->page_head] = pg;
    o->page_head = (o->page_head + 1) % FM_PAGE_LOG_MAX;
    if (o->page_count < FM_PAGE_LOG_MAX) o->page_count++;
    c->n_pages++;
    ESP_LOGI(TAG, "flex page RIC=%lu type=%c '%s'",
             (unsigned long)pg.address, pg.type, pg.text);

    reset_hunt(c);
}

flex_ctx_t *flex_create(fm_state_t *out, ls_flex_mode_t mode)
{
    /* Four contexts run in parallel for rate/level detection. Their slicer
       and 256-bit deinterleave state are ordinary task data, never DMA/ISR
       data, so keep the set out of scarce internal RAM. */
    flex_ctx_t *c = heap_caps_calloc(1, sizeof(*c),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) c = heap_caps_calloc(1, sizeof(*c),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!c) return NULL;
    c->out = out;
    flex_set_mode(c, mode);
    flex_reset(c);
    return c;
}

void flex_destroy(flex_ctx_t *c) { if (c) heap_caps_free(c); }

void flex_set_mode(flex_ctx_t *c, ls_flex_mode_t mode)
{
    if ((int)mode < 0 || (int)mode >= 4) mode = LS_FLEX_MODE_1600_2;
    c->mode = mode;
    /* Timing loop clocks the symbol rate, not the bit rate: 4-FSK carries
       2 bits per symbol, so sym_rate = bit_rate / 2. */
    c->inc = (float)MODE_SYM_RATE[mode] / (float)FM_DEMOD_RATE;
    c->bits_per_sym = (MODE_LEVELS[mode] == 4) ? 2 : 1;
}

void flex_reset(flex_ctx_t *c)
{
    c->acc = 0.0f; c->thr = 0.0f; c->prev_slice = 0;
    /* lvl_est is a peak tracker on the per-symbol integrator scale.  Start
       at 1 - well below any real |win| for either 4-FSK rate - so the peak
       tracker jumps onto the true level in the first few preamble symbols
       instead of decaying down from a too-high initial guess.  2-FSK does
       not use this field. */
    c->sum = 0.0f; c->lvl_est = 1.0f;
    reset_hunt(c);
    c->near_min = 32; c->n_near = 0;
    c->detected_mode = c->mode;
    c->n_frames = 0; c->n_pages = 0; c->n_cwerr = 0;
}

bool           flex_synced       (const flex_ctx_t *c) { return c && c->synced; }
ls_flex_mode_t flex_detected_mode(const flex_ctx_t *c) { return c ? c->detected_mode : LS_FLEX_MODE_1600_2; }
uint32_t       flex_n_frames     (const flex_ctx_t *c) { return c ? c->n_frames : 0; }
uint32_t       flex_n_pages      (const flex_ctx_t *c) { return c ? c->n_pages  : 0; }
uint32_t       flex_n_cwerr      (const flex_ctx_t *c) { return c ? c->n_cwerr  : 0; }
int            flex_near_min     (const flex_ctx_t *c) { return c ? c->near_min : 32; }

/* Advance the state machine by one demodulated data bit.  The bit here has
   already been sliced/deskewed - see flex_process(). */
static void feed_data_bit(flex_ctx_t *c, int bit)
{
    if (c->st == ST_MODE) {
        c->reg = (c->reg << 1) | (uint32_t)(bit & 1);
        if (--c->bits_left > 0) return;
        c->mode_word = c->reg;
        int det = mode_of_word(c->mode_word);
        if (det < 0 || det != (int)c->mode) {
            /* Mode word does not match this decoder's configured mode.  This
               is how the sync word decides the rate/level: the frame is
               rejected here if the receiver is not tuned to the right rate.
               Under a real multi-rate scanner, a parallel context at the
               right rate would still take it. */
            reset_hunt(c);
            return;
        }
        c->detected_mode = (ls_flex_mode_t)det;
        c->st = ST_FIW;
        c->bits_left = 32;
        c->reg = 0;
        return;
    }

    if (c->st == ST_FIW) {
        c->reg = (c->reg << 1) | (uint32_t)(bit & 1);
        if (--c->bits_left > 0) return;
        c->fiw = c->reg;
        uint32_t f = c->fiw;
        if (!bch_fix(&f)) { reset_hunt(c); return; }
        c->st = ST_BLOCK;
        c->block_idx = 0;
        return;
    }

    if (c->st == ST_BLOCK) {
        c->block_bits[c->block_idx++] = (uint8_t)(bit & 1);
        if (c->block_idx >= BLOCK_BITS) process_block(c);
        return;
    }
}

/* Emit one bit through the sync hunter or the collector, depending on state.
   Handles the polarity flip that a mistuned discriminator produces. */
static void handle_bit(flex_ctx_t *c, int raw_bit)
{
    if (c->st == ST_HUNT) {
        c->sr = (c->sr << 1) | (uint32_t)(raw_bit & 1);
        uint32_t d0 = popcount32(c->sr ^ FLEX_SYNC_A);
        uint32_t d1 = popcount32((~c->sr) ^ FLEX_SYNC_A);
        uint32_t best = d0 < d1 ? d0 : d1;
        if ((int)best < c->near_min) c->near_min = (int)best;
        if (best <= 6) c->n_near++;
        if (d0 <= SYNC_TOL)      { c->invert = 0; }
        else if (d1 <= SYNC_TOL) { c->invert = 1; }
        else return;

        c->synced = true;
        c->n_frames++;
        c->st = ST_MODE;
        c->bits_left = 32;
        c->reg = 0;
        return;
    }

    int b = c->invert ? (raw_bit ^ 1) : raw_bit;
    feed_data_bit(c, b);
}

static void handle_symbol_4fsk(flex_ctx_t *c, float win)
{
    float aw = win < 0.0f ? -win : win;
    /* Peak tracker: rise moderately fast, decay slowly.  What matters here
       is that lvl_est holds the full-amplitude (+/-1) level even through a
       stretch of mid-amplitude (+/-1/3) symbols in the sync word or payload.
       A running mean would collapse to ~2/3 of the peak and start putting
       +/-1 symbols in the mid bins.  Slow decay (0.9995 per symbol) is a
       half-life of ~1400 symbols - far longer than any FLEX frame here, so
       the peak effectively persists across a whole frame. */
    if (aw > c->lvl_est) c->lvl_est += 0.5f * (aw - c->lvl_est);
    else                 c->lvl_est *= 0.9995f;

    float mid = c->lvl_est * 0.5f;
    int msb, lsb;
    if      (win >  mid) { msb = 0; lsb = 0; }
    else if (win >  0)   { msb = 0; lsb = 1; }
    else if (win > -mid) { msb = 1; lsb = 1; }
    else                 { msb = 1; lsb = 0; }

    handle_bit(c, msb);
    handle_bit(c, lsb);
}

void flex_process(flex_ctx_t *c, const float *demod, int n)
{
    for (int i = 0; i < n; i++) {
        float x = demod[i];

        if (c->st == ST_HUNT) c->thr += 0.0015f * (x - c->thr);
        int slice = (x > c->thr) ? 1 : 0;
        float dev = x - c->thr;
        c->sum += dev;

        if (slice == c->prev_slice) {
            if (++c->idle_run > FM_DEMOD_RATE / 4) {
                reset_hunt(c);
                c->idle_run = 0;
            }
        } else {
            c->idle_run = 0;
            float err = c->acc - 0.5f;
            c->acc -= 0.10f * err;
        }
        c->prev_slice = slice;

        float old = c->acc;
        c->acc += c->inc;

        /* Sample at the transition-aligned phase (acc crosses 0.5) - the
           timing loop above pulls transitions to that phase, so the integrator
           window ends up spanning exactly one symbol.  See in
           pocsag.c for why the OTHER phase (acc crosses 1.0) is not the
           right one to sample. */
        if (old < 0.5f && c->acc >= 0.5f) {
            float win = c->sum;
            c->sum = 0.0f;
            if (c->bits_per_sym == 2) {
                handle_symbol_4fsk(c, win);
            } else {
                int bit = (win > 0.0f) ? 1 : 0;
                handle_bit(c, bit);
            }
        }

        if (c->acc >= 1.0f) c->acc -= 1.0f;
    }
}
