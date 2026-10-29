/* See rx_fm_data.h. The decoding is apps/fm and apps/acars, unchanged. */
#include "rx_fm_data.h"
#include "fm_state.h"
#include "flex.h"
#include "aprs.h"
#include "aprs_store.h"
#include "same.h"
#include "same_store.h"
#include "ais.h"
#include "ais_store.h"
#include "acars.h"
#include "acars_app.h"
#include "acars_resample.h"
#include "fm_lifecycle.h"
#include "esp_heap_caps.h"
#include <string.h>

#define PSRAM_CALLOC(sz) heap_caps_calloc(1, (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* ---- FLEX ---------------------------------------------------------------- */

typedef struct { flex_ctx_t *c[4]; } rx_flex_t;

static void flex_detach(void *st)
{
    rx_flex_t *f = st;
    for (int i = 0; i < 4; i++) flex_destroy(f->c[i]);
    heap_caps_free(f);
}

static void flex_counts_clear(void)
{
    FM.flex_sync = false;
    FM.flex_mode = LS_FLEX_MODE_1600_2;
    FM.flex_frames = FM.flex_pages = FM.flex_cw_errs = 0;
    FM.flex_near_min = 32;
}

static void *flex_attach(struct ls_rx_head *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)h; (void)s; (void)why; (void)n;
    rx_flex_t *f = PSRAM_CALLOC(sizeof(*f));
    if (!f) return NULL;
    for (int i = 0; i < 4; i++)
        if (!(f->c[i] = flex_create(&FM, (ls_flex_mode_t)i))) { flex_detach(f); return NULL; }
    flex_counts_clear();
    return f;
}

static void flex_reset_fn(void *st, ls_rx_reset_t why)
{
    rx_flex_t *f = st;
    for (int i = 0; i < 4; i++) flex_reset(f->c[i]);
    if (why == LS_RX_RESET_USER) flex_counts_clear();
}

/* One timing loop per FLEX rate/level: the mode word rejects the wrong ones,
   and the panel can still say what is being hunted. */
static void flex_process_fn(void *st, const ls_rx_block_t *b)
{
    rx_flex_t *f = st;
    uint32_t frames = 0, pages = 0, errs = 0, best = 0;
    int near = 32; bool sync = false; uint8_t mode = FM.flex_mode;
    for (int i = 0; i < 4; i++) {
        flex_process(f->c[i], b->data, (int)b->n);
        const uint32_t nf = flex_n_frames(f->c[i]);
        frames += nf; pages += flex_n_pages(f->c[i]); errs += flex_n_cwerr(f->c[i]);
        const int nm = flex_near_min(f->c[i]);
        if (nm < near) near = nm;
        if (flex_synced(f->c[i])) sync = true;
        if (nf >= best) { best = nf; mode = (uint8_t)i; }
    }
    FM.flex_sync = sync; FM.flex_mode = mode; FM.flex_frames = frames;
    FM.flex_pages = pages; FM.flex_cw_errs = errs; FM.flex_near_min = near;
}

static void flex_status(const void *st, ls_rx_status_t *out)
{
    (void)st;
    out->synced = FM.flex_sync; out->frames = FM.flex_frames;
    out->decoded = FM.flex_pages; out->errors = FM.flex_cw_errs; out->detail = FM.flex_mode;
}

const ls_rx_plugin_t rx_flex = {
    .abi = LS_RX_ABI, .id = "flex", .name = "FLEX", .sub = "pagers, 1600-6400 bps",
    .maturity = LS_RX_WORKS,
    .needs = { .inputs = LS_RX_DISC, .front = LS_RX_FRONT_NFM, .channel_bw_hz = 25000,
               .min_rate_hz = 32000, .rsel_job = -1, .cpu_permille = 80 },
    .doc = "FLEX pagers, four speeds at once, into the page list. Test: MODE FLEX, 929-932 MHz.",
    .attach = flex_attach, .detach = flex_detach, .reset = flex_reset_fn,
    .process = flex_process_fn, .status = flex_status,
};

/* ---- APRS and SAME: 16 kHz audio into their stores -------------------------- */

static void *aprs_attach(struct ls_rx_head *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)h; (void)s; (void)why; (void)n;
    return aprs_create(aprs_store_receive, NULL);
}
static void aprs_detach_fn(void *st) { aprs_destroy(st); }
static void aprs_reset_fn(void *st, ls_rx_reset_t why) { (void)why; aprs_reset(st); }
static void aprs_process_fn(void *st, const ls_rx_block_t *b) { aprs_process(st, b->data, (int)b->n); }

const ls_rx_plugin_t rx_aprs = {
    .abi = LS_RX_ABI, .id = "aprs", .name = "APRS", .sub = "1200 baud AFSK packets",
    .maturity = LS_RX_WORKS,
    .needs = { .inputs = LS_RX_PCM16, .front = LS_RX_FRONT_NFM, .channel_bw_hz = 25000,
               .min_rate_hz = 16000, .rsel_job = -1, .cpu_permille = 60 },
    .doc = "APRS packets from FM audio into STATIONS and the MAP. Test: MODE APRS, 144.39 MHz.",
    .attach = aprs_attach, .detach = aprs_detach_fn, .reset = aprs_reset_fn,
    .process = aprs_process_fn,
};

static same_callback_t s_same_sink = same_store_receive;
static void *s_same_user;

void rx_same_sink(same_callback_t fn, void *user)
{
    s_same_sink = fn ? fn : same_store_receive;
    s_same_user = fn ? user : NULL;
}

static void *same_attach(struct ls_rx_head *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)h; (void)s; (void)why; (void)n;
    return same_create(s_same_sink, s_same_user);
}
static void same_detach_fn(void *st) { same_destroy(st); }
static void same_reset_fn(void *st, ls_rx_reset_t why) { (void)why; same_reset(st); }
static void same_process_fn(void *st, const ls_rx_block_t *b) { same_process(st, b->data, (int)b->n); }

const ls_rx_plugin_t rx_same = {
    .abi = LS_RX_ABI, .id = "same", .name = "SAME", .sub = "weather radio alerts",
    .maturity = LS_RX_WORKS,
    .needs = { .inputs = LS_RX_PCM16, .front = LS_RX_FRONT_NFM, .channel_bw_hz = 25000,
               .min_rate_hz = 16000, .rsel_job = -1, .cpu_permille = 40 },
    .doc = "Weather radio SAME alerts from FM audio into ALERTS. Test: MODE SAME, 162.55 MHz.",
    .attach = same_attach, .detach = same_detach_fn, .reset = same_reset_fn,
    .process = same_process_fn,
};

/* ---- AIS: both channels from the SDR's own I/Q ------------------------------ */

static void *ais_attach(struct ls_rx_head *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)h; (void)s; (void)why; (void)n;
    return ais_create(ais_store_receive, NULL);
}
static void ais_detach_fn(void *st) { ais_destroy(st); }
static void ais_reset_fn(void *st, ls_rx_reset_t why) { (void)why; ais_reset(st); }
static void ais_process_fn(void *st, const ls_rx_block_t *b)
{
    ais_process(st, b->data, (size_t)b->n * 2u);
}

const ls_rx_plugin_t rx_ais = {
    .abi = LS_RX_ABI, .id = "ais", .name = "AIS", .sub = "ships, 161.975/162.025 MHz",
    .maturity = LS_RX_WORKS,
    .needs = { .inputs = LS_RX_IQ_U8, .front = LS_RX_FRONT_NONE, .channel_bw_hz = 100000,
               .min_rate_hz = 256000, .rsel_job = -1, .cpu_permille = 150,
               .flags = LS_RX_F_EXCLUSIVE },
    .doc = "AIS ships, both channels, from SDR I/Q into VESSELS and the MAP. Test: MODE AIS.",
    .attach = ais_attach, .detach = ais_detach_fn, .reset = ais_reset_fn,
    .process = ais_process_fn,
};

/* ---- ACARS: MSK on the AM envelope ------------------------------------------ */

typedef struct {
    acars_ctx_t *c;
    acars_rs_t   rs;
    float        pcm[700];   /* 1100 * 3/5 = 660, and the resampler's slack */
} rx_acars_t;

static void acars_detach_fn(void *st)
{
    rx_acars_t *a = st;
    acars_destroy(a->c);
    heap_caps_free(a);
}

static void *acars_attach(struct ls_rx_head *h, const ls_rx_stream_t *s, char *why, size_t n)
{
    (void)h; (void)s; (void)why; (void)n;
    rx_acars_t *a = PSRAM_CALLOC(sizeof(*a));
    if (!a) return NULL;
    if (!(a->c = acars_create(acars_app_state_mut()))) { heap_caps_free(a); return NULL; }
    acars_rs_init(&a->rs);
    return a;
}

static void acars_reset_fn(void *st, ls_rx_reset_t why)
{
    (void)why;
    rx_acars_t *a = st;
    acars_reset(a->c);
    acars_rs_init(&a->rs);
}

/* The 32 kHz envelope resampled to ACARS_SAMP_RATE 19.2 kHz (exactly 3:5),
   admitted through FM's lifecycle gate so a stop never frees it mid-call. */
static void acars_process_fn(void *st, const ls_rx_block_t *b)
{
    rx_acars_t *a = st;
    if (!fm_lifecycle_acars_enter()) return;
    const int na = acars_rs_5to3(&a->rs, b->data, (int)b->n, a->pcm, 700);
    if (na > 0) acars_process(a->c, a->pcm, na);
    fm_lifecycle_acars_leave();
}

static void acars_status(const void *st, ls_rx_status_t *out)
{
    const rx_acars_t *a = st;
    const acars_state_t *s = acars_app_state();
    out->synced = acars_synced(a->c);
    out->decoded = s->n_delivered; out->errors = s->n_bad_crc; out->frames = s->n_synced;
}

const ls_rx_plugin_t rx_acars = {
    .abi = LS_RX_ABI, .id = "acars", .name = "ACARS", .sub = "aircraft datalink, 2400 baud",
    .maturity = LS_RX_WORKS,
    .needs = { .inputs = LS_RX_ENV, .front = LS_RX_FRONT_AM, .channel_bw_hz = 25000,
               .min_rate_hz = 32000, .rsel_job = -1, .cpu_permille = 120 },
    .doc = "ACARS aircraft messages from the AM envelope. Test: MODE ACARS, 131.55 MHz.",
    .attach = acars_attach, .detach = acars_detach_fn, .reset = acars_reset_fn,
    .process = acars_process_fn, .status = acars_status,
};
