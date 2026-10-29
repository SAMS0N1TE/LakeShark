/* TIME MACHINE: taps, worker, clips, replay, settings. See ls_tmach.h.

   Threads:
     capture tasks  ls_tm_tap / ls_tm_tap_end: an in-flight count, one atomic
                    test, a copy into the queue's stage. Nothing else.
     worker         ls_tm_service: everything that owns state. It holds
                    s_lock only around ring, tape and timeline edits, never
                    across card I/O.
     UI             snapshots under a try-lock: a frame never waits. */
#include "ls_tmach.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define TM_MKDIR(p) _mkdir(p)
#else
#define TM_MKDIR(p) mkdir((p), 0775)
#endif

#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_decoded.h"
#include "rec_file_open.h"
#include "ls_textpack.h"

#if defined(ESP_PLATFORM) && !defined(LS_TM_HOST)
#define TM_DEVICE 1
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "event_bus.h"
#include "ls_gps.h"
#include "ls_time.h"
#include "ls_version.h"
#include "lr_wf_session.h"
#include "radio_endpoint.h"
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static bool lock_try(void) { return s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE; }
static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }
#define CFG_LOCK()   portENTER_CRITICAL(&s_cfg_mux)
#define CFG_UNLOCK() portEXIT_CRITICAL(&s_cfg_mux)
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BIG_FREE(p)  heap_caps_free(p)
static void yield_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }
#define TM_LOG(...) ESP_LOGI("tmach", __VA_ARGS__)
#else
#define TM_DEVICE 0
static bool lock_try(void) { return true; }
static void lock(void) {}
static void unlock(void) {}
#define CFG_LOCK()   ((void)0)
#define CFG_UNLOCK() ((void)0)
#define BIG_ALLOC(n) calloc(1, (n))
#define BIG_FREE(p)  free(p)
static void yield_ms(int ms) { (void)ms; }
#define TM_LOG(...) ((void)0)
#endif

#define FRESH_US    1500000   /* a queue that tapped this recently is running */
#define SLOTS_PASS  48        /* staged slots drained per pass                */
#define CHUNK       32768     /* card I/O and replay output, per step         */
#define PSRAM_SPARE (6u << 20)/* leave this much PSRAM for everyone else      */
#define IDLE_FREE_US 60000000 /* no tap this long: give the ring and stage back */

/* ---------------------------------------------------------------- state -- */

static EXT_RAM_BSS_ATTR ls_tm_ring_t s_ring;
static EXT_RAM_BSS_ATTR ls_tm_timeline_t s_tl;
static EXT_RAM_BSS_ATTR ls_tm_stage_t s_stage[LS_TM_QUEUES];
static EXT_RAM_BSS_ATTR uint8_t s_chunk[CHUNK];
static EXT_RAM_BSS_ATTR uint8_t s_out[CHUNK];

static atomic_bool s_live;               /* stages allocated: taps copy        */
static atomic_bool s_want;               /* a tap came while nothing was ready */
static atomic_int  s_inflight;
static atomic_bool s_enabled;
static _Atomic int64_t s_last_us[LS_TM_QUEUES], s_first_us[LS_TM_QUEUES];
static _Atomic int64_t s_tap_us;         /* the newest tap of any kind      */
static atomic_uint s_too_fast, s_fast_rate;
#if TM_DEVICE
static ls_tm_claim_t s_sdr_claim;         /* which dongle the SDR queue follows */
#endif

static uint8_t *s_ring_mem, *s_stage_mem;
static uint32_t s_ring_kb;
static int s_primary = -1;
static int s_reader = -1;
static bool s_tl_ready;
static char s_why[64] = "off";
static char s_note[64];
static char s_dir[96] = LS_TM_CLIP_DIR;

static ls_tm_config_t s_cfg, s_cfg_req;  /* worker's; the screen's request */
static atomic_bool s_cfg_dirty, s_cfg_loaded;
static int64_t s_cfg_retry_us;

static atomic_bool s_want_clip;
static atomic_uint s_want_replay;        /* clip number + 1; 0 none */

static EXT_RAM_BSS_ATTR struct {
    bool active;
    ls_tm_window_t w;
    FILE *f;
    uint32_t no;
    char base[16];
    uint64_t bytes;
    int64_t asked_us;
    char decoder[16];
} s_save;
static uint32_t s_next_no, s_clips_saved, s_last_clip;
static double s_last_clip_s;

static struct { bool pending; int64_t from_us, to_us; } s_auto;

static EXT_RAM_BSS_ATTR struct {
    bool active;
    FILE *f;
    ls_tm_clip_meta_t m;
    ls_tm_up_t up;
    uint64_t pairs_done;
    int64_t at_us;
    uint32_t no;
} s_play;
static uint32_t s_replay_found;

static const ls_tm_decoder_t *s_decoder;
static const char *(*s_decoder_name)(unsigned radio);

/* ------------------------------------------------------------- settings -- */

void ls_tm_config_defaults(ls_tm_config_t *c)
{
    *c = (ls_tm_config_t){
        .enabled = true, .budget_kb = 4096, .keep_hz = 60000, .source = LS_TM_SOURCE_AUTO,
        .autoclip = false, .clip_s = 30, .max_rate = 1300000,
    };
}

bool ls_tm_config_valid(ls_tm_config_t *c)
{
    const ls_tm_config_t in = *c;
#define CLAMP(f, lo, hi) do { if (c->f < (lo)) c->f = (lo); if (c->f > (hi)) c->f = (hi); } while (0)
    CLAMP(budget_kb, 1024, 16384);
    CLAMP(keep_hz, 25000, 1000000);
    CLAMP(clip_s, 5, 120);
    CLAMP(max_rate, 125000, 20000000);
    if (c->source > LS_TM_SOURCE_LR) c->source = LS_TM_SOURCE_AUTO;
#undef CLAMP
    return !memcmp(&in, c, sizeof(in));
}

int ls_tm_config_format(char *out, size_t n, const ls_tm_config_t *c)
{
    static const char *const SRC[] = { "auto", "sdr", "lr2021" };
    const int k = LS_SNPRINTF(t_ls_tmach_version_d_enabled_s, out, n,
        "{\n  \"version\": %d,\n  \"enabled\": %s,\n  \"budget_kb\": %u,\n  \"keep_hz\": %lu,\n"
        "  \"source\": \"%s\",\n  \"autoclip\": %s,\n  \"clip_s\": %u,\n  \"max_rate\": %lu\n}\n",
        LS_TM_CONFIG_VERSION, c->enabled ? "true" : "false", (unsigned)c->budget_kb,
        (unsigned long)c->keep_hz, SRC[c->source <= 2 ? c->source : 0],
        c->autoclip ? "true" : "false", (unsigned)c->clip_s, (unsigned long)c->max_rate);
    return k > 0 && (size_t)k < n ? k : 0;
}

static const char *cfg_value(const char *in, const char *key)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(in, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;
    for (p++; *p == ' ' || *p == '\t'; p++) {}
    return p;
}

bool ls_tm_config_parse(const char *in, ls_tm_config_t *c)
{
    ls_tm_config_defaults(c);
    if (!in || !strchr(in, '{')) return false;
    const char *v = cfg_value(in, "version");
    if (!v || strtol(v, NULL, 10) != LS_TM_CONFIG_VERSION) return false;
    if ((v = cfg_value(in, "enabled"))) c->enabled = !strncmp(v, "true", 4);
    if ((v = cfg_value(in, "budget_kb"))) c->budget_kb = (uint16_t)strtoul(v, NULL, 10);
    if ((v = cfg_value(in, "keep_hz"))) c->keep_hz = (uint32_t)strtoul(v, NULL, 10);
    if ((v = cfg_value(in, "source")))
        c->source = !strncmp(v, "\"sdr\"", 5) ? LS_TM_SOURCE_SDR : !strncmp(v, "\"lr2021\"", 8) ? LS_TM_SOURCE_LR : LS_TM_SOURCE_AUTO;
    if ((v = cfg_value(in, "autoclip"))) c->autoclip = !strncmp(v, "true", 4);
    if ((v = cfg_value(in, "clip_s"))) c->clip_s = (uint16_t)strtoul(v, NULL, 10);
    if ((v = cfg_value(in, "max_rate"))) c->max_rate = (uint32_t)strtoul(v, NULL, 10);
    ls_tm_config_valid(c);
    return true;
}

void ls_tm_config_get(ls_tm_config_t *c)
{
    CFG_LOCK();
    *c = atomic_load(&s_cfg_dirty) ? s_cfg_req : s_cfg;
    CFG_UNLOCK();
}

void ls_tm_config_set(const ls_tm_config_t *c)
{
    ls_tm_config_t v = *c;
    ls_tm_config_valid(&v);
    CFG_LOCK();
    s_cfg_req = v;
    CFG_UNLOCK();
    atomic_store(&s_enabled, v.enabled);
    atomic_store(&s_cfg_dirty, true);
}

static void config_path(char *out, size_t n) { snprintf(out, n, "%s/config.json", s_dir); }

static void make_dirs(void)
{
    char parent[96];
    snprintf(parent, sizeof(parent), "%s", s_dir);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) { *slash = 0; TM_MKDIR(parent); }
    TM_MKDIR(s_dir);
}

static void config_save(void)
{
    char body[320], path[128];
    if (!ls_tm_config_format(body, sizeof(body), &s_cfg)) return;
    make_dirs();
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

/* The card may mount after boot: keep trying for a while, then settle on
   the defaults (and write them the first time a setting changes). */
static void config_load(int64_t now)
{
    if (atomic_load(&s_cfg_loaded) || now < s_cfg_retry_us) return;
    char path[128], body[400];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f) {
        const size_t n = fread(body, 1, sizeof(body) - 1, f);
        fclose(f);
        body[n] = 0;
        ls_tm_config_t c;
        if (ls_tm_config_parse(body, &c) && !atomic_load(&s_cfg_dirty)) {
            s_cfg = c;
            atomic_store(&s_enabled, c.enabled);
        }
        atomic_store(&s_cfg_loaded, true);
        return;
    }
    s_cfg_retry_us = now + 5000000;
    if (now > 60000000) atomic_store(&s_cfg_loaded, true);
}

/* ----------------------------------------------------------------- taps -- */

void ls_tm_tap(int q, const ls_tm_src_t *src, const void *data, size_t bytes, int64_t now_us)
{
    if (q < 0 || q >= LS_TM_QUEUES || !atomic_load_explicit(&s_enabled, memory_order_relaxed)) return;
    atomic_fetch_add(&s_inflight, 1);
    atomic_store_explicit(&s_tap_us, now_us, memory_order_relaxed);
    if (!atomic_load(&s_live)) {
        atomic_store(&s_want, true);
    } else if (src->rate_hz > s_cfg.max_rate) {
        atomic_fetch_add(&s_too_fast, 1);
        atomic_store(&s_fast_rate, src->rate_hz);
        atomic_store(&s_last_us[q], 0);
    } else {
        if (now_us - atomic_load(&s_last_us[q]) > FRESH_US) atomic_store(&s_first_us[q], now_us);
        ls_tm_stage_put(&s_stage[q], src, data, bytes, now_us);
        atomic_store(&s_last_us[q], now_us);
    }
    atomic_fetch_sub(&s_inflight, 1);
}

void ls_tm_tap_end(int q, int64_t now_us)
{
    if (q < 0 || q >= LS_TM_QUEUES) return;
    atomic_fetch_add(&s_inflight, 1);
    if (atomic_load(&s_live)) ls_tm_stage_flush(&s_stage[q], true, now_us);
    atomic_fetch_sub(&s_inflight, 1);
}

#if TM_DEVICE
static void tap_sdr(const char *id, const ls_radio_iq_config_t *t, const void *data, size_t bytes)
{
    const int64_t now = esp_timer_get_time();
    const unsigned radio = id && !strcmp(id, LS_RADIO_ENDPOINT_HACKRF_USB) ? LS_TM_RADIO_HACKRF : LS_TM_RADIO_RTL;
    /* Two dongles streaming at once, each on its own task: the queue
       follows the one it has, and only one copies into the stage at a time. */
    if (!ls_tm_claim_enter(&s_sdr_claim, radio, now, FRESH_US)) return;
    if (!t || !data) {
        ls_tm_tap_end(LS_TM_Q_SDR, now);
    } else {
        const ls_tm_src_t src = {
            .center_hz = t->center_hz, .rate_hz = t->sample_rate_hz,
            .gain_tenths = (int16_t)(t->gain_mode == LS_RADIO_GAIN_AUTO ? -1 : t->gain_tenths_db),
            .fmt = LS_TM_FMT_CU8, .radio = (uint8_t)radio,
        };
        ls_tm_tap(LS_TM_Q_SDR, &src, data, bytes, now);
    }
    ls_tm_claim_leave(&s_sdr_claim);
}

static void tap_lr(uint32_t hz, const uint8_t *iq16, size_t bytes)
{
    const int64_t now = esp_timer_get_time();
    if (!iq16 || !bytes) { ls_tm_tap_end(LS_TM_Q_LR, now); return; }
    const ls_tm_src_t src = { .center_hz = hz, .rate_hz = 125000, .fmt = LS_TM_FMT_CS16, .radio = LS_TM_RADIO_LR2021 };
    ls_tm_tap(LS_TM_Q_LR, &src, iq16, bytes, now);
}

/* ADS-B contacts reach the timeline through the decoded bus like the rest. */
static void on_bus(const event_t *ev, void *user)
{
    (void)user;
    if (ev->kind != EVT_CONTACT_NEW && ev->kind != EVT_CONTACT_IDENT) return;
    const evt_contact_t *c = &ev->u.contact;
    ls_decoded_t d = { .kind = LS_DECODED_ADSB, .address = c->icao, .has_pos = c->pos_valid,
                       .lat = c->lat, .lon = c->lon, .alt_ft = c->altitude, .at_us = ev->ts_us,
                       .freq_hz = 1090000000 };
    snprintf(d.from, sizeof(d.from), "%s", c->callsign);
    ls_decoded_publish(&d);
}
#endif

/* ------------------------------------------------------------ memory ----- */

static void teardown(void)
{
    atomic_store(&s_live, false);
    while (atomic_load(&s_inflight)) yield_ms(1);
    lock();
    BIG_FREE(s_ring_mem); s_ring_mem = NULL;
    BIG_FREE(s_stage_mem); s_stage_mem = NULL;
    memset(&s_ring, 0, sizeof(s_ring));
    s_ring_kb = 0;
    s_primary = -1;
    unlock();
}

static size_t psram_room(void)
{
#if TM_DEVICE
    const size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t free_b = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_b <= PSRAM_SPARE) return 0;
    const size_t room = free_b - PSRAM_SPARE;
    return room < big ? room : big;
#else
    return (size_t)64 << 20;
#endif
}

static bool allocate(void)
{
    const size_t stage_b = (size_t)LS_TM_QUEUES * LS_TM_SLOTS * LS_TM_SLOT_BYTES;
    size_t want = (size_t)s_cfg.budget_kb * 1024;
    const size_t room = psram_room();
    if (room < stage_b + (1u << 20)) { LS_SNPUTS(t_ls_tmach_psram_short_not_recording, s_why, sizeof(s_why), "PSRAM short: not recording"); return false; }
    if (want > room - stage_b) want = (room - stage_b) & ~(size_t)0xFFFF;
    uint8_t *ring = BIG_ALLOC(want), *stage = BIG_ALLOC(stage_b);
    if (!ring || !stage) {
        BIG_FREE(ring); BIG_FREE(stage);
        LS_SNPUTS(t_ls_tmach_psram_short_not_recording, s_why, sizeof(s_why), "PSRAM short: not recording");
        return false;
    }
    lock();
    s_ring_mem = ring;
    s_stage_mem = stage;
    s_ring_kb = (uint32_t)(want / 1024);
    ls_tm_ring_init(&s_ring, ring, want, s_cfg.keep_hz);
    for (int q = 0; q < LS_TM_QUEUES; q++) {
        ls_tm_stage_init(&s_stage[q], stage + (size_t)q * LS_TM_SLOTS * LS_TM_SLOT_BYTES);
        atomic_store(&s_last_us[q], 0);
    }
    s_primary = -1;
    unlock();
    atomic_store(&s_live, true);
    return true;
}

/* ------------------------------------------------------------ timeline --- */

static bool tuned_at(void *ctx, int64_t at_us, uint32_t *hz, uint8_t *radio)
{
    (void)ctx;
    const ls_tm_seg_t *s = s_ring.cap ? ls_tm_ring_seg_at(&s_ring, at_us) : NULL;
    if (!s) return false;
    *hz = (uint32_t)s->src.center_hz;
    *radio = s->src.radio;
    return true;
}

static void pump_timeline(void)
{
    if (!s_tl_ready) {
        ls_tm_tl_init(&s_tl);
        s_tl_ready = true;
    }
    if (s_reader < 0) s_reader = ls_decoded_open(0xFFFFu);
    if (s_reader < 0) return;
    ls_decoded_t d;
    uint32_t lost = 0;
    for (int n = 0; n < 32 && ls_decoded_next(s_reader, &d, &lost); n++) {
        lock();
        const uint32_t before = s_tl.added;
        ls_tm_tl_add(&s_tl, &d, tuned_at, NULL);
        const bool fresh = s_tl.added != before;
        unlock();
        /* Auto clip: the seconds before a new row and a few after it. */
        if (fresh && s_cfg.autoclip && atomic_load(&s_live) && !s_auto.pending && !s_save.active) {
            s_auto.pending = true;
            s_auto.from_us = d.at_us - (int64_t)(s_cfg.clip_s > 5 ? s_cfg.clip_s - 3 : 2) * 1000000;
            s_auto.to_us = d.at_us + 3000000;
        }
    }
    if (lost) s_tl.dropped += lost;
}

/* ------------------------------------------------------------- capture --- */

static const char *source_word(int q) { return q == LS_TM_Q_LR ? "LR2021" : "SDR"; }

static void drain(int64_t now)
{
    bool fresh[LS_TM_QUEUES];
    for (int q = 0; q < LS_TM_QUEUES; q++) fresh[q] = now - atomic_load(&s_last_us[q]) < FRESH_US;
    int want = s_primary;
    if (s_cfg.source == LS_TM_SOURCE_SDR) want = LS_TM_Q_SDR;
    else if (s_cfg.source == LS_TM_SOURCE_LR) want = LS_TM_Q_LR;
    else if (want < 0 || !fresh[want]) {
        /* Whichever is running; both: the one that started last. */
        if (fresh[0] && fresh[1]) want = atomic_load(&s_first_us[1]) > atomic_load(&s_first_us[0]) ? 1 : 0;
        else if (fresh[0] || fresh[1]) want = fresh[1] ? 1 : 0;
    }
    if (want != s_primary) {
        lock();
        if (s_ring.cap) ls_tm_ring_break(&s_ring);
        unlock();
        s_primary = want;
    }
    for (int q = 0; q < LS_TM_QUEUES; q++) {
        if (q == s_primary) continue;
        ls_tm_stage_discard(&s_stage[q]);
    }
    if (s_primary < 0) return;
    const uint8_t *data;
    const ls_tm_slot_meta_t *m;
    for (int n = 0; n < SLOTS_PASS && (m = ls_tm_stage_peek(&s_stage[s_primary], &data)); n++) {
        lock();
        ls_tm_ring_feed(&s_ring, m, data);
        unlock();
        ls_tm_stage_release(&s_stage[s_primary]);
    }
}

/* ---------------------------------------------------------------- clips -- */

static uint32_t scan_next_number(void)
{
    uint32_t top = 0;
    DIR *d = opendir(s_dir);
    if (!d) return 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        const uint32_t n = ls_tm_clip_number(e->d_name);
        if (n > top) top = n;
    }
    closedir(d);
    return top + 1;
}

static void note(const char *fmt, const char *a, double v)
{
    snprintf(s_note, sizeof(s_note), fmt, a, v);
}

static void save_begin(int64_t from_us, int64_t to_us, int64_t now)
{
    if (s_save.active || !s_ring.cap) return;
    lock();
    const bool ok = ls_tm_window_begin(&s_ring, &s_save.w, from_us, to_us);
    unlock();
    if (!ok) { LS_SNPUTS(t_ls_tmach_nothing_held_to_save, s_note, sizeof(s_note), "Nothing held to save yet"); return; }
    make_dirs();
    if (!s_next_no) s_next_no = scan_next_number();
    char path[160];
    for (int tries = 0; tries < 50; tries++, s_next_no++) {
        ls_tm_clip_base(s_save.base, sizeof(s_save.base), s_next_no);
        snprintf(path, sizeof(path), "%s/%s.%s", s_dir, s_save.base, ls_tm_fmt_name(s_save.w.seg.fmt));
        if ((s_save.f = rec_file_open_new(path))) break;
    }
    if (!s_save.f) {
        lock(); ls_tm_window_end(&s_ring, &s_save.w); unlock();
        LS_SNPUTS(t_ls_tmach_card_not_ready_clip, s_note, sizeof(s_note), "Card not ready: clip not saved");
        return;
    }
    s_save.no = s_next_no++;
    s_save.bytes = 0;
    s_save.asked_us = now;
    s_save.active = true;
    s_save.decoder[0] = 0;
    if (s_decoder_name) {
        const char *n = s_decoder_name(s_save.w.seg.src.radio);
        snprintf(s_save.decoder, sizeof(s_save.decoder), "%s", n ? n : "");
    }
    snprintf(s_note, sizeof(s_note), "Saving %s", s_save.base);
}

static void save_finish(bool ok)
{
    fclose(s_save.f);
    s_save.f = NULL;
    lock();
    ls_tm_window_t w = s_save.w;
    ls_tm_window_end(&s_ring, &s_save.w);
    unlock();
    s_save.active = false;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s.%s", s_dir, s_save.base, ls_tm_fmt_name(w.seg.fmt));
    if (!ok) {
        remove(path);
        LS_SNPRINTF(t_ls_tmach_card_write_failed_s, s_note, sizeof(s_note), "Card write failed: %s not saved", s_save.base);
        return;
    }
    ls_tm_clip_meta_t m;
    memset(&m, 0, sizeof(m));
#if TM_DEVICE
    ls_time_render_stamp(m.rec.time, sizeof(m.rec.time));
    ls_version_info_t vi;
    ls_version_get(&vi);
    snprintf(m.rec.board, sizeof(m.rec.board), "%s", vi.board ? vi.board : "?");
    snprintf(m.rec.firmware, sizeof(m.rec.firmware), "%s", vi.version ? vi.version : "?");
    ls_gps_state_t g;
    ls_gps_get(&g);
    m.fix = ls_gps_fix_current(&g, esp_timer_get_time());
    m.lat = g.lat_deg; m.lon = g.lon_deg; m.alt_m = g.alt_m;
#else
    snprintf(m.rec.time, sizeof(m.rec.time), "host");
    snprintf(m.rec.board, sizeof(m.rec.board), "host");
    snprintf(m.rec.firmware, sizeof(m.rec.firmware), "host");
#endif
    snprintf(m.rec.radio, sizeof(m.rec.radio), "%s", ls_tm_radio_name(w.seg.src.radio));
    m.rec.freq_hz = (uint32_t)w.seg.src.center_hz;
    m.rec.gain_tenths = w.seg.src.gain_tenths;
    m.rec.sample_rate = w.seg.rate_hz;
    m.rec.span_us = (uint32_t)(w.end_us - w.start_us);
    m.version = LS_TM_CLIP_VERSION;
    snprintf(m.iq, sizeof(m.iq), "%s.%s", s_save.base, ls_tm_fmt_name(w.seg.fmt));
    m.iq_fmt = w.seg.fmt;
    m.src_fmt = w.seg.src.fmt;
    m.decim = w.seg.decim;
    m.src_rate = w.seg.src.rate_hz;
    m.center_hz = w.seg.src.center_hz;
    m.pairs = s_save.bytes / w.seg.pair_bytes;
    m.start_us = w.start_us;
    m.end_us = w.end_us;
    m.saved_us = s_save.asked_us;
    snprintf(m.decoder, sizeof(m.decoder), "%s", s_save.decoder);
    if (!ls_tm_clip_write_meta(s_dir, s_save.base, &m)) {
        remove(path);
        LS_SNPRINTF(t_ls_tmach_card_write_failed_s, s_note, sizeof(s_note), "Card write failed: %s not saved", s_save.base);
        return;
    }
    lock();
    ls_tm_tl_clip(&s_tl, s_save.no, w.start_us, w.end_us);
    unlock();
    s_last_clip = s_save.no;
    s_last_clip_s = (double)(w.end_us - w.start_us) / 1e6;
    s_clips_saved++;
    note("%s saved, %.1f s", s_save.base, s_last_clip_s);
}

static void save_step(void)
{
    for (int k = 0; k < 4 && s_save.active; k++) {
        lock();
        const size_t n = ls_tm_window_step(&s_ring, &s_save.w, s_chunk, sizeof(s_chunk));
        unlock();
        if (!n) { save_finish(s_save.w.at >= s_save.w.to); return; }
        if (fwrite(s_chunk, 1, n, s_save.f) != n) { save_finish(false); return; }
        s_save.bytes += n;
    }
}

/* --------------------------------------------------------------- replay -- */

static void replay_end(const char *why)
{
    if (s_play.f) fclose(s_play.f);
    s_play.f = NULL;
    char line[64] = "";
    if (s_play.active && s_decoder) s_replay_found = s_decoder->close(line, sizeof(line));
    s_play.active = false;
    snprintf(s_note, sizeof(s_note), "%s", why ? why : line[0] ? line : "Replay done");
}

static void replay_begin(uint32_t no)
{
    if (s_play.active) return;
    if (!no) no = s_last_clip ? s_last_clip : (s_next_no ? s_next_no - 1 : scan_next_number() - 1);
    char base[16], path[160], why[64] = "";
    ls_tm_clip_base(base, sizeof(base), no);
    if (!no || !ls_tm_clip_read_meta(s_dir, base, &s_play.m)) {
        LS_SNPRINTF(t_ls_tmach_no_clip_s_on, s_note, sizeof(s_note), "No clip %s on the card", no ? base : "yet");
        return;
    }
    if (!s_decoder || !s_decoder->open(&s_play.m, why, sizeof(why))) {
        snprintf(s_note, sizeof(s_note), "%s", why[0] ? why : "No decoder for this clip");
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", s_dir, s_play.m.iq);
    s_play.f = fopen(path, "rb");
    s_play.active = true;
    if (!s_play.f) { replay_end("Clip data missing"); return; }
    ls_tm_up_init(&s_play.up, s_play.m.iq_fmt, s_play.m.src_fmt, s_play.m.decim);
    s_play.pairs_done = 0;
    s_play.at_us = s_play.m.start_us;
    s_play.no = no;
    s_replay_found = 0;
    LS_SNPRINTF(t_ls_tmach_replaying_s_through_s, s_note, sizeof(s_note), "Replaying %s through %s", base, s_play.m.decoder[0] ? s_play.m.decoder : "the decoders");
}

static void replay_step(void)
{
    if (!s_play.active) return;
    const unsigned ipb = ls_tm_pair_bytes(s_play.m.iq_fmt);
    size_t pairs = sizeof(s_out) / ls_tm_up_bytes(&s_play.up, 1);
    if (pairs * ipb > sizeof(s_chunk)) pairs = sizeof(s_chunk) / ipb;
    const size_t n = fread(s_chunk, 1, pairs * ipb, s_play.f);
    if (n < ipb) { replay_end(NULL); return; }
    size_t used = 0;
    const size_t k = ls_tm_up_run(&s_play.up, s_chunk, n - n % ipb, s_out, sizeof(s_out), &used);
    s_decoder->feed(s_out, k, s_play.at_us);
    s_play.pairs_done += used / ipb;
    s_play.at_us = s_play.m.start_us + (int64_t)((double)s_play.pairs_done * 1e6 / s_play.m.rec.sample_rate);
}

/* --------------------------------------------------------------- worker -- */

void ls_tm_service(int64_t now)
{
    config_load(now);
    if (atomic_exchange(&s_cfg_dirty, false)) {
        ls_tm_config_t old = s_cfg;
        CFG_LOCK();
        s_cfg = s_cfg_req;
        CFG_UNLOCK();
        if (s_ring_mem && (old.budget_kb != s_cfg.budget_kb || !s_cfg.enabled)) teardown();
        else if (s_ring_mem && old.keep_hz != s_cfg.keep_hz) {
            lock(); s_ring.keep_hz = s_cfg.keep_hz; ls_tm_ring_break(&s_ring); unlock();
        }
        config_save();
    }
    atomic_store(&s_enabled, s_cfg.enabled);
    pump_timeline();
    if (!s_cfg.enabled) {
        if (s_ring_mem && !s_save.active) teardown();
        LS_SNPUTS(t_ls_tmach_off_options_turns_it, s_why, sizeof(s_why), "Off: OPTIONS turns it on");
    } else if (!s_ring_mem) {
        if (atomic_exchange(&s_want, false)) {
            if (allocate()) {
                atomic_store(&s_tap_us, now);
                TM_LOG("ring %u KB + stage %u KB allocated", (unsigned)s_ring_kb,
                       (unsigned)(LS_TM_QUEUES * LS_TM_SLOTS * LS_TM_SLOT_BYTES / 1024));
            }
        } else if (strncmp(s_why, "Idle", 4)) {
            LS_SNPUTS(t_ls_tmach_waiting_for_a_receiver, s_why, sizeof(s_why), "Waiting for a receiver to run");
        }
    } else if (!s_save.active && now - atomic_load(&s_tap_us) >= IDLE_FREE_US) {
        /* Nothing has run for a minute: the PSRAM goes back until the next
           receiver's first block brings it back (that block is not kept). */
        const uint32_t kb = s_ring_kb;
        teardown();
        snprintf(s_why, sizeof(s_why), "Idle: memory freed, back on the next receiver");
        TM_LOG("idle %d s: ring %u KB + stage freed", IDLE_FREE_US / 1000000, (unsigned)kb);
    }
    if (s_ring_mem) {
        drain(now);
        if (s_primary < 0 || now - atomic_load(&s_last_us[s_primary]) >= FRESH_US) {
            const unsigned fast = atomic_load(&s_fast_rate);
            if (fast && now - atomic_load(&s_last_us[0]) > FRESH_US)
                LS_SNPRINTF(t_ls_tmach_2f_ms_s_is, s_why, sizeof(s_why), "%.2f MS/s is past the %.2f MS/s limit",
                         fast / 1e6, s_cfg.max_rate / 1e6);
            else LS_SNPUTS(t_ls_tmach_waiting_for_a_receiver, s_why, sizeof(s_why), "Waiting for a receiver to run");
        } else {
            s_why[0] = 0;
            atomic_store(&s_fast_rate, 0);
        }
        if (atomic_exchange(&s_want_clip, false))
            save_begin(now - (int64_t)s_cfg.clip_s * 1000000, 0, now);
        else if (s_auto.pending && now >= s_auto.to_us + 500000) {
            s_auto.pending = false;
            save_begin(s_auto.from_us, s_auto.to_us, now);
        }
        save_step();
    }
    const unsigned rp = atomic_exchange(&s_want_replay, 0);
    if (rp) replay_begin(rp - 1);
    replay_step();
}

#if TM_DEVICE
static EXT_RAM_BSS_ATTR StackType_t s_stack[8192 / sizeof(StackType_t)];
static StaticTask_t s_tcb;
static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        ls_tm_service(esp_timer_get_time());
        /* Faster while there is work; the stage holds half a second. */
        yield_ms(s_save.active || s_play.active ? 2 : 10);
    }
}
#endif

void ls_tm_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    ls_tm_config_defaults(&s_cfg);
    s_cfg_req = s_cfg;
    atomic_store(&s_enabled, s_cfg.enabled);
#if TM_DEVICE
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    ls_radio_endpoint_set_capture_tap(tap_sdr);
    lr_wf_set_capture_tap(tap_lr);
    event_bus_subscribe(on_bus, NULL);
    /* Core 0, under the UI (core 1, 4) and every capture task. */
    xTaskCreateStaticPinnedToCore(worker, "tmach", sizeof(s_stack) / sizeof(s_stack[0]),
                                  NULL, 2, s_stack, &s_tcb, 0);
#endif
}

/* ----------------------------------------------------------- snapshots --- */

uint32_t ls_tm_tape(ls_tm_tick_t *out, uint32_t max, uint32_t *tick_count)
{
    if (!lock_try()) return 0;
    const uint32_t have = s_ring.tick_count < LS_TM_TICKS ? s_ring.tick_count : LS_TM_TICKS;
    const uint32_t n = have < max ? have : max;
    for (uint32_t k = 0; k < n; k++)
        out[k] = s_ring.tick[(s_ring.tick_count - n + k) % LS_TM_TICKS];
    if (tick_count) *tick_count = s_ring.tick_count;
    unlock();
    return n;
}

uint32_t ls_tm_events(ls_tm_event_t *out, uint32_t first, uint32_t max, uint32_t *total)
{
    if (!s_tl_ready || !lock_try()) return 0;
    uint32_t n = 0;
    for (; n < max; n++) {
        const ls_tm_event_t *e = ls_tm_tl_newest(&s_tl, first + n);
        if (!e) break;
        out[n] = *e;
    }
    if (total) *total = s_tl.n;
    unlock();
    return n;
}

bool ls_tm_status(ls_tm_status_t *o)
{
    if (!lock_try()) return false;
    memset(o, 0, sizeof(*o));
    o->enabled = atomic_load(&s_enabled);
    o->allocated = s_ring_mem != NULL;
    const ls_tm_seg_t *s = s_ring.cap ? ls_tm_ring_seg_newest(&s_ring) : NULL;
    if (s) o->seg = *s;
    o->recording = o->allocated && !s_why[0];
    snprintf(o->why, sizeof(o->why), "%s", s_why);
    o->held_s = s_ring.cap ? ls_tm_ring_held_s(&s_ring) : 0;
    o->capacity_s = s && s->rate_hz ? (double)s_ring.cap / s->pair_bytes / s->rate_hz : 0;
    o->ring_kb = s_ring_kb;
    o->drops = s_primary >= 0 ? atomic_load(&s_stage[s_primary].drops) : 0;
    o->gaps = s_ring.gaps;
    o->held_drops = s_ring.held_drops;
    o->too_fast = atomic_load(&s_too_fast);
    o->tick_count = s_ring.tick_count;
    o->saving = s_save.active;
    if (s_save.active && s_save.w.to > s_save.w.from)
        o->save_pct = (uint8_t)((s_save.w.at - s_save.w.from) * 100 / (s_save.w.to - s_save.w.from));
    o->saving_no = s_save.active ? s_save.no : 0;
    if (s_save.active) { o->save_from_us = s_save.w.start_us; o->save_to_us = s_save.w.end_us; }
    o->last_clip = s_last_clip;
    o->last_clip_s = s_last_clip_s;
    o->clips_saved = s_clips_saved;
    snprintf(o->note, sizeof(o->note), "%s", s_note);
    o->replaying = s_play.active;
    if (s_play.active && s_play.m.pairs) o->replay_pct = (uint8_t)(s_play.pairs_done * 100 / s_play.m.pairs);
    o->replay_no = s_play.active ? s_play.no : 0;
    o->replay_found = s_replay_found;
    o->tl_version = s_tl.version;
    o->tl_count = s_tl.n;
    unlock();
    return true;
}

bool ls_tm_what_was_that(void)
{
    if (!atomic_load(&s_enabled) || !atomic_load(&s_live) || s_save.active) return false;
    atomic_store(&s_want_clip, true);
    return true;
}

bool ls_tm_replay(uint32_t clip_no)
{
    if (s_play.active) return false;
    atomic_store(&s_want_replay, clip_no + 1);
    return true;
}

void ls_tm_set_decoder(const ls_tm_decoder_t *d) { s_decoder = d; }
void ls_tm_set_decoder_name(const char *(*fn)(unsigned radio)) { s_decoder_name = fn; }

/* ------------------------------------------------------------- console --- */

static void print_status(void)
{
    ls_tm_status_t st;
    while (!ls_tm_status(&st)) yield_ms(1);
    ls_tm_config_t c;
    ls_tm_config_get(&c);
    LS_PRINTF(t_ls_tmach_tm_s_ring_lu, "tm: %s, ring %lu KB of %u, keep >= %lu Hz, source %s, autoclip %s, clip %u s\n",
           st.enabled ? "on" : "off", (unsigned long)st.ring_kb, (unsigned)c.budget_kb,
           (unsigned long)c.keep_hz, c.source == 1 ? "sdr" : c.source == 2 ? "lr2021" : "auto",
           c.autoclip ? "on" : "off", (unsigned)c.clip_s);
    if (st.seg.id)
        LS_PRINTF(t_ls_tmach_tm_s_s_4f, "tm: %s %s %.4f MHz %lu S/s %s (from %lu %s, /%u)  held %.1f of %.1f s\n",
               st.recording ? "recording" : "last", ls_tm_radio_name(st.seg.src.radio),
               st.seg.src.center_hz / 1e6, (unsigned long)st.seg.rate_hz, ls_tm_fmt_name(st.seg.fmt),
               (unsigned long)st.seg.src.rate_hz, ls_tm_fmt_name(st.seg.src.fmt), (unsigned)st.seg.decim,
               st.held_s, st.capacity_s);
    if (st.why[0]) printf("tm: %s\n", st.why);
    LS_PRINTF(t_ls_tmach_tm_ticks_lu_gaps, "tm: ticks %lu, gaps %lu, stage drops %lu, held drops %lu, too fast %lu, clips %lu (last tm%04lu)\n",
           (unsigned long)st.tick_count, (unsigned long)st.gaps, (unsigned long)st.drops,
           (unsigned long)st.held_drops, (unsigned long)st.too_fast, (unsigned long)st.clips_saved,
           (unsigned long)st.last_clip);
    if (st.saving) LS_PRINTF(t_ls_tmach_tm_saving_tm_04lu, "tm: saving tm%04lu %u%%\n", (unsigned long)st.saving_no, st.save_pct);
    if (st.replaying) LS_PRINTF(t_ls_tmach_tm_replaying_tm_04lu, "tm: replaying tm%04lu %u%%\n", (unsigned long)st.replay_no, st.replay_pct);
    LS_PRINTF(t_ls_tmach_tm_timeline_lu_rows, "tm: timeline %lu rows; %s\n", (unsigned long)st.tl_count, st.note[0] ? st.note : "-");
}

int ls_tm_command(int argc, char **argv)
{
    ls_tm_config_t c;
    ls_tm_config_get(&c);
    const char *a = argc > 1 ? argv[1] : "status";
    if (!strcmp(a, "status")) { print_status(); return 0; }
    if (!strcmp(a, "on") || !strcmp(a, "off")) { c.enabled = !strcmp(a, "on"); ls_tm_config_set(&c); printf("tm: %s\n", a); return 0; }
    if (!strcmp(a, "what")) {
        const bool ok = ls_tm_what_was_that();
        printf("tm: %s\n", ok ? "saving the last seconds (tm status shows progress)" : "nothing to save (off, no receiver, or saving)");
        return ok ? 0 : 1;
    }
    if (!strcmp(a, "replay")) {
        const uint32_t no = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
        const bool ok = ls_tm_replay(no);
        printf("tm: %s\n", ok ? "replay queued (tm status shows progress, pages land in tm events)" : "a replay is running");
        return ok ? 0 : 1;
    }
    if (!strcmp(a, "keep") && argc > 2) { c.keep_hz = (uint32_t)strtoul(argv[2], NULL, 10); ls_tm_config_set(&c); print_status(); return 0; }
    if (!strcmp(a, "budget") && argc > 2) { c.budget_kb = (uint16_t)strtoul(argv[2], NULL, 10); ls_tm_config_set(&c); return 0; }
    if (!strcmp(a, "clip") && argc > 2) { c.clip_s = (uint16_t)strtoul(argv[2], NULL, 10); ls_tm_config_set(&c); return 0; }
    if (!strcmp(a, "auto") && argc > 2) { c.autoclip = !strcmp(argv[2], "on"); ls_tm_config_set(&c); return 0; }
    if (!strcmp(a, "source") && argc > 2) {
        c.source = !strcmp(argv[2], "sdr") ? LS_TM_SOURCE_SDR : !strcmp(argv[2], "lr") ? LS_TM_SOURCE_LR : LS_TM_SOURCE_AUTO;
        ls_tm_config_set(&c);
        return 0;
    }
    if (!strcmp(a, "clips")) {
        DIR *d = opendir(s_dir);
        if (!d) { printf("tm: no %s\n", s_dir); return 1; }
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d))) {
            const size_t l = strlen(e->d_name);
            if (!ls_tm_clip_number(e->d_name) || l < 5 || strcmp(e->d_name + l - 5, ".json")) continue;
            char base[24];
            snprintf(base, sizeof(base), "%.*s", (int)(l - 5), e->d_name);
            ls_tm_clip_meta_t m;
            if (!ls_tm_clip_read_meta(s_dir, base, &m)) continue;
            LS_PRINTF(t_ls_tmach_s_s_4f_mhz, "%s  %s  %.4f MHz  %.1f s  %lu S/s %s  %s  %s\n", base, m.rec.radio, m.center_hz / 1e6,
                   m.rec.span_us / 1e6, (unsigned long)m.rec.sample_rate, ls_tm_fmt_name(m.iq_fmt),
                   m.decoder[0] ? m.decoder : "-", m.rec.time);
            n++;
        }
        closedir(d);
        printf("tm: %d clips in %s\n", n, s_dir);
        return 0;
    }
    if (!strcmp(a, "events")) {
        static EXT_RAM_BSS_ATTR ls_tm_event_t ev[20];
        uint32_t total = 0, n;
        while (!(n = ls_tm_events(ev, 0, 20, &total)) && total) yield_ms(1);
        const int64_t now = esp_timer_get_time();
        for (uint32_t k = 0; k < n; k++)
            LS_PRINTF(t_ls_tmach_6_1fs_7s_9, "-%6.1fs %-7s %9.4f MHz %-7s x%-3u %-12s %s%s\n", (now - ev[k].at_us) / 1e6,
                   ls_tm_kind_name(ev[k].kind), ev[k].freq_hz / 1e6, ls_tm_radio_name(ev[k].radio),
                   ev[k].count, ev[k].from, ev[k].text, ev[k].clip ? "  [clip]" : "");
        printf("tm: %lu rows\n", (unsigned long)total);
        return 0;
    }
    printf("tm status | on | off | what | replay [n] | clips | events | keep <hz> | budget <kb> | clip <s> | auto on|off | source auto|sdr|lr\n");
    return 1;
}

#if TM_DEVICE
static int tm_cmd(int argc, char **argv) { return ls_tm_command(argc, argv); }
void ls_tm_console_register(void)
{
    const esp_console_cmd_t cmd = { .command = "tm", .help = "TIME MACHINE: status, on, off, what, replay [n], clips, events",
        .hint = "status|on|off|what|replay|clips|events", .func = tm_cmd, .argtable = NULL };
    esp_console_cmd_register(&cmd);
}
#endif

void ls_tm_set_clip_dir(const char *dir) { snprintf(s_dir, sizeof(s_dir), "%s", dir); }
