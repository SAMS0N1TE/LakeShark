/* PAGER RECON: which UHF paging channels around here carry POCSAG, at what
   rate and polarity, how cleanly, and how many pagers they address - without
   reading anyone's messages unless OPTIONS is told to.

   One FSK session at a time on the LoRa chip, hopped across a channel plan
   (pager_recon.h says which channels and why). Each visit opens the session
   with the probe the channel last answered to, or the next one to try, and
   reads the instantaneous RSSI for CHECK_MS. A channel no louder than the
   floor plus MARGIN_DB is left at once; one above it is listened to for the
   dwell, stepping through the probes until one decodes, and then staying on
   that one. The first pass only measures, so the floor is the median of
   every channel's level rather than a guess.

   A POCSAG batch arrives as the 64 bytes after the hardware matched the
   frame sync, which it does again at every batch of a transmission. Each
   batch is checked codeword by codeword with the decoder's BCH; a batch with
   at least half its codewords good is a frame. The capcodes it addresses go
   into a per-channel set for a count. With MESSAGE TEXT on, the batches also
   go through the POCSAG decoder and the last pages are shown. */
#include "../ls_experiments.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "fm_state.h"
#include "ls_lora.h"
#include "pager_recon.h"
#include "pocsag.h"

#define MHZ_LO 200.0
#define MHZ_HI 1100.0
#define CHECK_MS   40          /* the RSSI look before deciding to listen    */
#define MARGIN_DB  6.0f        /* above the floor is a carrier               */
#define NOW_MS     100         /* how often the NOW level is read listening  */
#define LOCK_QUIET 3           /* loud visits with no frame before re-probing */
#define SINGLE_QUIET_US (60LL * 1000000)
#define FLEX_RECHECK 4         /* a known FLEX channel is re-synced 1 in 4   */
#define ROWS 10
#define PAGES 3
#define FILTER_HZ 11700u       /* Carson for 2400 bps at 4.5 kHz, rounded up */

extern const ls_experiment_t exp_pagers;

/* ---------------------------------------------------------- settings -- */

/* What the next start uses. The plan PGR_PLAN_N is one frequency. Set from
   OPTIONS or the console, read by start on the worker. */
static volatile int s_set_plan = PGR_PLAN_UHF;
static volatile uint32_t s_set_hz = 929612500u;
static volatile int s_set_dwell = 6;
static volatile bool s_set_text;
/* A frequency given on the console is for that one run: OPTIONS keep theirs. */
static volatile uint32_t s_once_hz;

/* ------------------------------------------------------------- state -- */

typedef struct {
    uint32_t hz;
    float    last_avg, peak;
    bool     measured;
    uint32_t hot;                  /* visits above the floor              */
    int8_t   lock;                 /* the probe that decoded, or -1       */
    uint8_t  next_probe;           /* the one to try next when unlocked   */
    uint8_t  quiet;                /* loud visits since the last frame    */
    uint8_t  probes_seen;          /* bit per probe that ever decoded     */
    uint32_t syncs, frames, codewords, fixed, bad;
    uint32_t flex;                 /* FLEX syncs with a valid mode code   */
    int8_t   flex_mode;
    int64_t  last_us, last_hot_us;
    pgr_capset_t caps;
} chan_t;

typedef enum { ST_HOP, ST_CHECK, ST_LISTEN, ST_STOPPED } st_t;

static EXT_RAM_BSS_ATTR chan_t s_ch[PGR_CH_MAX];
static EXT_RAM_BSS_ATTR float s_levels[PGR_CH_MAX];
static EXT_RAM_BSS_ATTR uint8_t s_buf[PGR_BATCH_BYTES];
static int s_n, s_plan, s_cur, s_probe, s_pass, s_dwell_s;
static bool s_single, s_session, s_text_on;
static st_t s_st;
static int64_t s_t0, s_dwell_end, s_slice_end, s_now_t, s_view_t, s_last_batch_us, s_retry_at;
static float s_chk_max, s_chk_sum, s_floor;
static int s_chk_n, s_n_levels;
static bool s_have_floor, s_flex_visit;
static uint32_t s_visit_frames, s_hop_errors;

/* The message decoder, only while MESSAGE TEXT is on. */
static fm_state_t *s_text_state;
static pocsag_ctx_t *s_text;
static EXT_RAM_BSS_ATTR uint32_t s_plan_hz[PGR_CH_MAX];

/* ------------------------------------------------------------- view -- */

typedef enum { K_QUIET, K_CARRIER, K_FLEX, K_POCSAG } kind_t;

typedef struct {
    uint32_t hz;
    uint8_t  kind;
    int8_t   lock, flex_mode;
    uint8_t  probes_seen;
    float    peak;
    uint32_t syncs, frames, codewords, fixed, bad, flex, hot;
    uint16_t caps;
    bool     caps_over;
    int64_t  last_us;
} row_t;

typedef struct { uint32_t hz, cap; char type; char text[44]; } page_t;

/* The newest pages first, with the channel each came in on. Worker only. */
static EXT_RAM_BSS_ATTR page_t s_pages[PAGES];
static int s_n_pages;

typedef struct {
    bool     started;               /* has run since boot; kept after stop */
    bool     single;
    uint32_t cur_hz;
    int      plan, n, pass, cur, dwell_s;
    uint8_t  st;
    char     tag[7];
    bool     have_now, have_floor;
    float    now, floor;
    int      n_kind[4];
    row_t    rows[ROWS];
    int      n_rows;
    row_t    focus;
    bool     have_focus;
    page_t   pages[PAGES];
    int      n_pages;
    bool     text_on;
    uint32_t hop_errors;
    int64_t  at_us;
} view_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR view_t s_view;
static EXT_RAM_BSS_ATTR view_t s_next;     /* built by the worker, then copied in */

static kind_t kind_of(const chan_t *c)
{
    if (c->frames) return K_POCSAG;
    if (c->flex) return K_FLEX;
    if (c->hot) return K_CARRIER;
    return K_QUIET;
}

static void row_of(row_t *r, const chan_t *c)
{
    r->hz = c->hz;
    r->kind = (uint8_t)kind_of(c);
    r->lock = c->lock;
    r->flex_mode = c->flex_mode;
    r->probes_seen = c->probes_seen;
    r->peak = c->peak;
    r->syncs = c->syncs;
    r->frames = c->frames;
    r->codewords = c->codewords;
    r->fixed = c->fixed;
    r->bad = c->bad;
    r->flex = c->flex;
    r->hot = c->hot;
    r->caps = c->caps.n;
    r->caps_over = c->caps.overflow;
    r->last_us = c->frames || c->flex ? c->last_us : c->last_hot_us;
}

/* How a channel ranks in the table: POCSAG first by frames, then FLEX by
   syncs, then carriers by how often they were up. */
static uint64_t rank_of(const chan_t *c)
{
    const kind_t k = kind_of(c);
    const uint32_t n = k == K_POCSAG ? c->frames : k == K_FLEX ? c->flex : c->hot;
    return ((uint64_t)k << 32) | n;
}

static void publish(int64_t now)
{
    view_t *v = &s_next;
    memset(v, 0, sizeof(*v));
    v->started = true;
    v->single = s_single;
    v->plan = s_plan;
    v->n = s_n;
    v->pass = s_pass;
    v->cur = s_cur;
    v->cur_hz = s_cur < s_n ? s_ch[s_cur].hz : 0;
    v->dwell_s = s_dwell_s;
    v->st = (uint8_t)s_st;
    const pgr_probe_t *p = pgr_probe(s_probe);
    snprintf(v->tag, sizeof(v->tag), "%s", p ? p->tag : "");
    v->have_floor = s_have_floor;
    v->floor = s_floor;
    v->text_on = s_text_on;
    v->hop_errors = s_hop_errors;
    v->at_us = now;

    bool used[PGR_CH_MAX] = { 0 };
    for (int i = 0; i < s_n; i++) v->n_kind[kind_of(&s_ch[i])]++;
    for (int r = 0; r < ROWS; r++) {
        int best = -1;
        uint64_t best_rank = 0;
        for (int i = 0; i < s_n; i++) {
            if (used[i] || kind_of(&s_ch[i]) == K_QUIET) continue;
            const uint64_t rk = rank_of(&s_ch[i]);
            if (best < 0 || rk > best_rank) { best = i; best_rank = rk; }
        }
        if (best < 0) break;
        used[best] = true;
        row_of(&v->rows[v->n_rows++], &s_ch[best]);
    }
    /* The detail line: the channel in hand if it has decoded, else the
       busiest POCSAG channel. */
    const chan_t *f = NULL;
    if (s_cur < s_n && s_ch[s_cur].frames) f = &s_ch[s_cur];
    else if (v->n_rows && v->rows[0].kind == K_POCSAG)
        for (int i = 0; i < s_n; i++) if (s_ch[i].hz == v->rows[0].hz) f = &s_ch[i];
    if (f) { row_of(&v->focus, f); v->have_focus = true; }

    if (s_text_on) {
        memcpy(v->pages, s_pages, sizeof(s_pages));
        v->n_pages = s_n_pages;
    }
    portENTER_CRITICAL(&s_lock);
    /* The level is set_now's, which writes it straight in. */
    v->have_now = s_view.have_now;
    v->now = s_view.now;
    s_view = *v;
    portEXIT_CRITICAL(&s_lock);
    s_view_t = now;
}

static void set_now(float dbm)
{
    portENTER_CRITICAL(&s_lock);
    s_view.now = dbm;
    s_view.have_now = true;
    portEXIT_CRITICAL(&s_lock);
}

/* ------------------------------------------------------------- radio -- */

static esp_err_t open_session(uint32_t hz, int probe)
{
    if (s_session) { ls_lora_fsk_end(); s_session = false; }
    const pgr_probe_t *p = pgr_probe(probe);
    const ls_fsk_cfg_t cfg = {
        .freq_hz = hz,
        .bitrate = p->baud,
        .deviation_hz = p->deviation_hz,
        .bandwidth_hz = ls_lora_fsk_bw_snap(FILTER_HZ),
        .sync_word = p->sync_word,
        .payload_bytes = p->payload_bytes,
    };
    const esp_err_t err = ls_lora_fsk_begin(&cfg);
    s_session = err == ESP_OK;
    s_probe = probe;
    return err;
}

static bool flex_channel(const chan_t *c) { return c->flex >= 2 && c->lock < 0; }

static int first_probe(const chan_t *c)
{
    if (c->lock >= 0) return c->lock;
    if (flex_channel(c)) return pgr_probe_is_flex(c->next_probe) ? c->next_probe : 6;
    return c->next_probe;
}

static void next_channel(int64_t now)
{
    chan_t *c = &s_ch[s_cur];
    if (s_st == ST_LISTEN && c->lock >= 0 && !s_visit_frames && !pgr_probe_is_flex(c->lock)) {
        if (++c->quiet >= LOCK_QUIET) { c->lock = -1; c->quiet = 0; }
    }
    if (++s_cur >= s_n) {
        s_cur = 0;
        if (s_n_levels) {
            s_floor = pgr_median(s_levels, s_n_levels);
            s_have_floor = true;
        }
        s_n_levels = 0;
        s_pass++;
    }
    s_st = ST_HOP;
    publish(now);
}

static void begin_listen(int64_t now)
{
    s_st = ST_LISTEN;
    s_dwell_end = now + (int64_t)s_dwell_s * 1000000;
    s_slice_end = now + (int64_t)pgr_probe(s_probe)->slice_ms * 1000;
    /* A known FLEX channel is only re-synced, for one slice. */
    if (s_flex_visit) s_dwell_end = s_slice_end;
    s_visit_frames = 0;
    s_now_t = now;
}

/* ----------------------------------------------------------- packets -- */

/* The pages the decoder finished on this batch, newest first, in front of
   the ones already held. */
static void take_pages(uint32_t hz, uint32_t before)
{
    const uint32_t after = pocsag_n_pages(s_text);
    int fresh = (int)(after - before);
    if (fresh <= 0 || s_text_state->page_count <= 0) return;
    if (fresh > PAGES) fresh = PAGES;
    if (fresh > s_text_state->page_count) fresh = s_text_state->page_count;
    for (int i = PAGES - 1; i >= fresh; i--) s_pages[i] = s_pages[i - fresh];
    for (int i = 0; i < fresh; i++) {
        const int idx = (s_text_state->page_head - 1 - i + 2 * FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
        const fm_page_t *pg = &s_text_state->pages[idx];
        page_t *o = &s_pages[i];
        o->hz = hz;
        o->cap = pg->address;
        o->type = pg->type;
        snprintf(o->text, sizeof(o->text), "%s", pg->text);
    }
    s_n_pages += fresh;
    if (s_n_pages > PAGES) s_n_pages = PAGES;
}

static void on_packet(int64_t now, const uint8_t *data)
{
    chan_t *c = &s_ch[s_cur];
    const pgr_probe_t *p = pgr_probe(s_probe);
    c->syncs++;
    if (p->kind == PGR_FLEX) {
        const int mode = pgr_flex_mode(data, p->inverted);
        if (mode < 0) return;
        c->flex++;
        c->flex_mode = (int8_t)mode;
        c->last_us = now;
        c->next_probe = (uint8_t)s_probe;
        c->probes_seen |= (uint8_t)(1u << s_probe);
        /* Named: the rest of the dwell is better spent elsewhere. */
        if (!s_single) s_dwell_end = now;
        return;
    }
    pgr_batch_t b;
    pgr_scan_batch(data, p->inverted, &b);
    if (!pgr_batch_real(&b)) return;
    c->frames++;
    c->codewords += 16;
    c->fixed += b.fixed;
    c->bad += b.bad;
    for (int i = 0; i < b.n_caps; i++) pgr_capset_add(&c->caps, b.caps[i]);
    c->last_us = now;
    c->lock = (int8_t)s_probe;
    c->quiet = 0;
    c->probes_seen |= (uint8_t)(1u << s_probe);
    s_visit_frames++;

    if (s_text) {
        /* A batch is 544 bits; one that follows the last by about that is
           the same transmission, which keeps a message whole across it. */
        const int64_t period = 544LL * 1000000 / p->baud;
        const bool contiguous = s_last_batch_us && llabs(now - s_last_batch_us - period) <= period / 10 + 20000;
        const uint32_t before = pocsag_n_pages(s_text);
        pocsag_set_baud(s_text, p->baud);
        pocsag_process_batch(s_text, data, PGR_BATCH_BYTES, p->inverted, contiguous);
        take_pages(c->hz, before);
    }
    s_last_batch_us = now;
}

/* -------------------------------------------------------- experiment -- */

static void text_free(void)
{
    if (s_text) { pocsag_destroy(s_text); s_text = NULL; }
    if (s_text_state) { heap_caps_free(s_text_state); s_text_state = NULL; }
}

static bool pagers_start(char *why, size_t n)
{
    const uint32_t caps = ls_lora_caps();
    if (!(caps & LS_LORA_CAP_FSK)) { snprintf(why, n, "No FSK receiver on this chip"); return false; }
    if (!(caps & LS_LORA_CAP_RSSI_INST)) { snprintf(why, n, "This chip gives no RSSI reading"); return false; }

    const uint32_t once = s_once_hz;
    s_once_hz = 0;
    const int plan = once ? PGR_PLAN_N : s_set_plan;
    memset(s_ch, 0, sizeof(s_ch));
    uint32_t *hz = s_plan_hz;
    if (plan == PGR_PLAN_N) {
        hz[0] = once ? once : s_set_hz;
        s_n = 1;
    } else {
        s_n = pgr_plan((pgr_plan_t)plan, hz, PGR_CH_MAX);
    }
    for (int i = 0; i < s_n; i++) {
        s_ch[i].hz = hz[i];
        s_ch[i].lock = -1;
        s_ch[i].flex_mode = -1;
        s_ch[i].last_us = s_ch[i].last_hot_us = -1;
    }
    s_plan = plan;
    s_single = plan == PGR_PLAN_N;
    s_dwell_s = s_set_dwell;
    s_cur = s_probe = 0;
    /* One frequency has no floor to learn, so nothing to survey. */
    s_pass = s_single ? 1 : 0;
    s_have_floor = false;
    s_floor = 0;
    s_n_levels = 0;
    s_session = false;
    s_hop_errors = 0;
    s_retry_at = 0;
    s_last_batch_us = 0;
    s_n_pages = 0;
    s_st = ST_HOP;

    text_free();
    s_text_on = s_set_text;
    if (s_text_on) {
        s_text_state = heap_caps_calloc(1, sizeof(*s_text_state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_text_state) s_text = pocsag_create(s_text_state, 1200);
        if (!s_text) { text_free(); snprintf(why, n, "No PSRAM for the paging decoder"); return false; }
    }

    /* The first channel is opened here so a chip that refuses the session
       says so now, not as a silent hop error. */
    const esp_err_t err = open_session(s_ch[0].hz, first_probe(&s_ch[0]));
    if (err != ESP_OK) {
        text_free();
        snprintf(why, n, "%.4f MHz refused: %s", s_ch[0].hz / 1e6, esp_err_to_name(err));
        return false;
    }
    const int64_t now = esp_timer_get_time();
    s_st = s_single ? ST_LISTEN : ST_CHECK;
    s_t0 = now;
    s_chk_max = -200.0f; s_chk_sum = 0; s_chk_n = 0;
    s_flex_visit = false;
    if (s_single) begin_listen(now);
    portENTER_CRITICAL(&s_lock);
    memset(&s_view, 0, sizeof(s_view));
    portEXIT_CRITICAL(&s_lock);
    publish(now);
    return true;
}

static void pagers_stop(void)
{
    if (s_session) ls_lora_fsk_end();
    s_session = false;
    text_free();
    /* What was found stays on screen. */
    portENTER_CRITICAL(&s_lock);
    s_view.st = ST_STOPPED;
    s_view.have_now = false;
    portEXIT_CRITICAL(&s_lock);
}

static void poll_check(int64_t now)
{
    float dbm;
    const esp_err_t err = ls_lora_rssi_inst(&dbm);
    if (err == ESP_OK) {
        if (dbm > s_chk_max) s_chk_max = dbm;
        s_chk_sum += dbm;
        s_chk_n++;
    } else if (err == ESP_ERR_INVALID_STATE) {
        ls_lora_fsk_receive();
    }
    if (now - s_t0 < CHECK_MS * 1000 || s_chk_n < 3) {
        /* A chip that never gives a reading still moves on. */
        if (now - s_t0 < 10 * CHECK_MS * 1000) return;
    }
    chan_t *c = &s_ch[s_cur];
    if (s_chk_n) {
        const float avg = s_chk_sum / (float)s_chk_n;
        c->last_avg = avg;
        if (!c->measured || s_chk_max > c->peak) c->peak = s_chk_max;
        c->measured = true;
        if (s_n_levels < PGR_CH_MAX) s_levels[s_n_levels++] = avg;
        set_now(avg);
    }
    /* The survey pass only measures. */
    if (s_pass == 0 || !s_chk_n || !s_have_floor || s_chk_max < s_floor + MARGIN_DB) {
        next_channel(now);
        return;
    }
    c->hot++;
    c->last_hot_us = now;
    s_flex_visit = flex_channel(c);
    if (s_flex_visit && c->hot % FLEX_RECHECK != 0) { next_channel(now); return; }
    begin_listen(now);
}

static void poll_listen(int64_t now)
{
    chan_t *c = &s_ch[s_cur];
    float rssi;
    const int got = ls_lora_fsk_poll(s_buf, sizeof(s_buf), &rssi);
    if (got > 0) on_packet(now, s_buf);

    if (now - s_now_t >= NOW_MS * 1000) {
        s_now_t = now;
        float dbm;
        const esp_err_t err = ls_lora_rssi_inst(&dbm);
        if (err == ESP_OK) {
            set_now(dbm);
            if (!c->measured || dbm > c->peak) c->peak = dbm;
            c->measured = true;
        } else if (err == ESP_ERR_INVALID_STATE) {
            ls_lora_fsk_receive();
        }
    }

    if (!s_single && now >= s_dwell_end) { next_channel(now); return; }

    /* One frequency, locked, gone quiet for a minute: probe again. */
    if (s_single && c->lock >= 0 && c->last_us >= 0 && now - c->last_us > SINGLE_QUIET_US) {
        c->lock = -1;
        c->next_probe = (uint8_t)((s_probe + 1) % PGR_N_PROBES);
        s_slice_end = now;
    }
    if (c->lock < 0 && now >= s_slice_end) {
        /* Step to the next probe on the same channel. A FLEX channel only
           has the FLEX probes to step through. */
        int p = (s_probe + 1) % PGR_N_PROBES;
        if (s_flex_visit && !pgr_probe_is_flex(p)) p = 6;
        c->next_probe = (uint8_t)p;
        if (open_session(c->hz, p) != ESP_OK) {
            s_hop_errors++;
            s_retry_at = now + 1000000;
            if (s_single) s_st = ST_HOP;
            else next_channel(now);
            return;
        }
        s_slice_end = now + (int64_t)pgr_probe(p)->slice_ms * 1000;
    }
}

static void pagers_poll(void)
{
    const int64_t now = esp_timer_get_time();
    switch (s_st) {
    case ST_HOP: {
        if (now < s_retry_at) return;
        chan_t *c = &s_ch[s_cur];
        if (open_session(c->hz, first_probe(c)) != ESP_OK) {
            /* Not this channel; the next is tried in a second, not on the
               next poll, so a chip refusing everything is not hammered. */
            s_hop_errors++;
            s_retry_at = now + 1000000;
            s_st = ST_CHECK;
            next_channel(now);
            return;
        }
        s_t0 = now;
        s_chk_max = -200.0f; s_chk_sum = 0; s_chk_n = 0;
        s_flex_visit = false;
        if (s_single) begin_listen(now);
        else s_st = ST_CHECK;
        break;
    }
    case ST_CHECK:  poll_check(now); break;
    case ST_LISTEN: poll_listen(now); break;
    case ST_STOPPED: return;
    }
    if (now - s_view_t >= 250000) publish(now);
}

/* ------------------------------------------------------------ readout -- */

static const char *kind_name(uint8_t k)
{
    static const char *const N[] = { "-", "CARR", "FLEX", "POC" };
    return k < 4 ? N[k] : "?";
}

static void fmt_ber(char *out, size_t n, const row_t *r)
{
    const float b = pgr_ber_pct(r->fixed, r->bad, r->codewords);
    if (b < 0) snprintf(out, n, "-");
    else if (b < 10.0f) snprintf(out, n, "%.1f%%", (double)b);
    else snprintf(out, n, "%.0f%%", (double)b);
}

static void fmt_caps(char *out, size_t n, const row_t *r)
{
    if (r->kind != K_POCSAG) snprintf(out, n, "-");
    else snprintf(out, n, "%u%s", (unsigned)r->caps, r->caps_over ? "+" : "");
}

static void row_line(char *out, const row_t *r, int64_t at)
{
    char rate[8], ber[8], caps[8], age[8];
    char syncs[12], frames[12];
    pgr_age(age, sizeof(age), r->last_us < 0 ? -1 : at - r->last_us);
    if (r->kind == K_POCSAG) {
        const pgr_probe_t *p = pgr_probe(r->lock);
        snprintf(rate, sizeof(rate), "%s", p ? p->tag : "?");
        snprintf(syncs, sizeof(syncs), "%lu", (unsigned long)r->syncs);
        snprintf(frames, sizeof(frames), "%lu", (unsigned long)r->frames);
    } else if (r->kind == K_FLEX) {
        snprintf(rate, sizeof(rate), "%s", pgr_flex_mode_name(r->flex_mode));
        snprintf(syncs, sizeof(syncs), "%lu", (unsigned long)r->flex);
        snprintf(frames, sizeof(frames), "-");
    } else {
        snprintf(rate, sizeof(rate), "%.0fdB", (double)r->peak);
        snprintf(syncs, sizeof(syncs), "%lu", (unsigned long)r->syncs);
        snprintf(frames, sizeof(frames), "-");
    }
    fmt_ber(ber, sizeof(ber), r);
    fmt_caps(caps, sizeof(caps), r);
    char line[96];
    snprintf(line, sizeof(line), "%8.4f %-4s %-6s%5s %5s %5s %3s %4s",
             r->hz / 1e6, kind_name(r->kind), rate, syncs, frames, ber, caps, age);
    snprintf(out, LS_EXP_LINE, "%.*s", LS_EXP_LINE - 1, line);
}

static int pagers_lines(char (*out)[LS_EXP_LINE], int max)
{
    view_t *v = heap_caps_malloc(sizeof(*v), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!v) {
        if (max > 0) snprintf(out[0], LS_EXP_LINE, "no memory for the readout");
        return max > 0 ? 1 : 0;
    }
    portENTER_CRITICAL(&s_lock);
    *v = s_view;
    portEXIT_CRITICAL(&s_lock);
    int n = 0;

    if (!v->started) {
        const int plan = s_set_plan;
        if (n < max) {
            if (plan == PGR_PLAN_N) snprintf(out[n++], LS_EXP_LINE, "ONE FREQ %.4f MHz", s_set_hz / 1e6);
            else snprintf(out[n++], LS_EXP_LINE, "SCAN %s, dwell %d s", pgr_plan_name((pgr_plan_t)plan), s_set_dwell);
        }
        if (n < max) snprintf(out[n++], LS_EXP_LINE, "POCSAG 512/1200/2400 N+I, FLEX sync");
        if (n < max) snprintf(out[n++], LS_EXP_LINE, "message text %s", s_set_text ? "SHOWN" : "off");
        heap_caps_free(v);
        return n;
    }

    if (n < max) {
        if (v->single) snprintf(out[n++], LS_EXP_LINE, "ONE FREQ  %.4f MHz", v->cur_hz / 1e6);
        else if (v->have_floor)
            snprintf(out[n++], LS_EXP_LINE, "SCAN %s %d ch  pass %d  floor %.0f",
                     pgr_plan_name((pgr_plan_t)v->plan), v->n, v->pass, (double)v->floor);
        else snprintf(out[n++], LS_EXP_LINE, "SCAN %s %d ch  surveying", pgr_plan_name((pgr_plan_t)v->plan), v->n);
    }
    if (n < max) {
        static const char *const ST[] = { "hop", "check", "listen", "stopped" };
        char lvl[16] = "";
        if (v->have_now) snprintf(lvl, sizeof(lvl), "%.0f dBm", (double)v->now);
        if (v->st == ST_STOPPED) snprintf(out[n++], LS_EXP_LINE, "stopped after pass %d", v->pass);
        else snprintf(out[n++], LS_EXP_LINE, "NOW %8.4f %-6s%-6s %s", v->cur_hz / 1e6, v->tag, ST[v->st % 4], lvl);
    }
    if (n < max)
        snprintf(out[n++], LS_EXP_LINE, "POCSAG %d  FLEX %d  CARRIER %d  QUIET %d",
                 v->n_kind[K_POCSAG], v->n_kind[K_FLEX], v->n_kind[K_CARRIER], v->n_kind[K_QUIET]);
    if (v->n_rows && n < max)
        snprintf(out[n++], LS_EXP_LINE, "MHZ      KIND RATE   SYNC   FRM   BER CAP LAST");
    for (int i = 0; i < v->n_rows && n < max; i++) row_line(out[n++], &v->rows[i], v->at_us);
    if (!v->n_rows && n < max) snprintf(out[n++], LS_EXP_LINE, "nothing above the floor yet");

    if (v->have_focus && n < max) {
        const row_t *f = &v->focus;
        snprintf(out[n++], LS_EXP_LINE, "%.4f BCH ok %lu fixed %lu lost %lu",
                 f->hz / 1e6, (unsigned long)(f->codewords - f->fixed - f->bad),
                 (unsigned long)f->fixed, (unsigned long)f->bad);
        if (n < max) {
            char seen[40] = "";
            for (int p = 0; p < PGR_N_PROBES; p++)
                if (f->probes_seen & (1u << p)) {
                    const size_t l = strlen(seen);
                    snprintf(seen + l, sizeof(seen) - l, "%s%s", l ? " " : "", pgr_probe(p)->tag);
                }
            char line[96];
            snprintf(line, sizeof(line), "  heard as %s, peak %.0f dBm", seen, (double)f->peak);
            snprintf(out[n++], LS_EXP_LINE, "%.*s", LS_EXP_LINE - 1, line);
        }
    }
    if (v->text_on) {
        for (int i = 0; i < v->n_pages && n < max; i++) {
            const page_t *p = &v->pages[i];
            snprintf(out[n++], LS_EXP_LINE, "%.4f %7lu %c %s", p->hz / 1e6, (unsigned long)p->cap,
                     p->type ? p->type : '?', p->text);
        }
    } else if (v->n_kind[K_POCSAG] && n < max) {
        snprintf(out[n++], LS_EXP_LINE, "message text off (OPTIONS)");
    }
    if (v->hop_errors && n < max) snprintf(out[n++], LS_EXP_LINE, "sessions refused %lu", (unsigned long)v->hop_errors);
    heap_caps_free(v);
    return n;
}

/* ------------------------------------------------------------ console -- */

static bool parse_mhz(const char *text, double *mhz)
{
    char *end = NULL;
    const double v = strtod(text, &end);
    if (end == text || *end || !(v >= MHZ_LO && v <= MHZ_HI)) return false;
    *mhz = v;
    return true;
}

/* `exp pagers start 929.6125` listens to one channel for that run;
   `exp pagers start uhf` or `onsite` picks the plan. No argument scans the
   plan OPTIONS has. */
static bool pagers_configure(int argc, char **argv, char *why, size_t n)
{
    double mhz;
    if (argc == 1 && !strcmp(argv[0], "uhf")) { s_set_plan = PGR_PLAN_UHF; return true; }
    if (argc == 1 && !strcmp(argv[0], "onsite")) { s_set_plan = PGR_PLAN_ONSITE; return true; }
    if (argc != 1 || !parse_mhz(argv[0], &mhz)) {
        snprintf(why, n, "one frequency %.0f-%.0f MHz, or uhf or onsite", MHZ_LO, MHZ_HI);
        return false;
    }
    s_once_hz = (uint32_t)(mhz * 1e6 + 0.5);
    return true;
}

/* ------------------------------------------------------------ OPTIONS -- */

static void restart(void) { if (ls_exp_running() == &exp_pagers) ls_exp_start(&exp_pagers); }

static const char *const PLAN_NAMES[] = { "UHF PAGING", "ON-SITE", "ONE FREQ" };
static int o_plan(const ls_opt_t *o) { (void)o; return s_set_plan; }
static void o_set_plan(const ls_opt_t *o, int v) { (void)o; s_set_plan = v < 0 || v > PGR_PLAN_N ? 0 : v; restart(); }

static double o_freq(const ls_opt_t *o) { (void)o; return s_set_hz / 1e6; }
static void o_set_freq(const ls_opt_t *o, double v)
{
    (void)o;
    s_set_hz = (uint32_t)(v * 1e6 + 0.5);
    s_set_plan = PGR_PLAN_N;
    restart();
}
static void o_show_freq(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%.4f MHz", s_set_hz / 1e6); }

static double o_dwell(const ls_opt_t *o) { (void)o; return s_set_dwell; }
static void o_set_dwell(const ls_opt_t *o, double v) { (void)o; s_set_dwell = (int)(v + 0.5); restart(); }
static void o_show_dwell(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%d s", s_set_dwell); }

static const char *const TEXT_NAMES[] = { "OFF", "SHOWN" };
static int o_text(const ls_opt_t *o) { (void)o; return s_set_text ? 1 : 0; }
static void o_set_text(const ls_opt_t *o, int v) { (void)o; s_set_text = v != 0; restart(); }

static const ls_opt_t OPTS[] = {
    { .label = "CHANNELS", .kind = LS_OPT_CYCLE, .names = PLAN_NAMES, .n = 3, .get = o_plan, .set = o_set_plan },
    { .label = "FREQUENCY", .kind = LS_OPT_NUMBER, .num = o_freq, .set_num = o_set_freq,
      .lo = MHZ_LO, .hi = MHZ_HI, .unit = "MHz, one channel", .show = o_show_freq },
    { .label = "DWELL", .kind = LS_OPT_NUMBER, .num = o_dwell, .set_num = o_set_dwell,
      .lo = 2, .hi = 60, .unit = "seconds on a live channel", .show = o_show_dwell },
    { .label = "MESSAGE TEXT", .kind = LS_OPT_TOGGLE, .names = TEXT_NAMES, .get = o_text, .set = o_set_text },
};

const ls_experiment_t exp_pagers = {
    .id = "pagers",
    .name = "PAGER RECON",
    .sub = "POCSAG channels, 929/931/454 MHz",
    .maturity = LS_EXP_TRYING,
    .needs = "a paging transmitter in range on UHF",
    .start = pagers_start,
    .stop = pagers_stop,
    .poll = pagers_poll,
    .lines = pagers_lines,
    .configure = pagers_configure,
    .opts = OPTS,
    .n_opts = (int)(sizeof(OPTS) / sizeof(OPTS[0])),
};
