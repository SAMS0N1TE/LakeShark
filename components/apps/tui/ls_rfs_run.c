#include "ls_rfs_run.h"
#include "ls_rfsim.h"
#include "ls_fox_run.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ls_df_sources.h"
#include "ls_field.h"
#include "ls_lora.h"
#include "ls_mesh.h"
#include "ls_radio_select.h"
#include "ls_time.h"
#include "lr_iq_spectrum.h"
#include "lr_wf_session.h"
#include "ls_textpack.h"

#ifndef LS_RFS_DIR
#define LS_RFS_DIR "/sdcard/lakeshark/rfsurvey"
#endif
#define PLANS_FILE LS_RFS_DIR "/plans.json"
#define GROUP 8                    /* ls_dfs measures this many at once */
#define LR_SPAN_HZ 125000
#define LR_REACH_HZ 52000          /* channel edges kept this close to the look's centre */
#define LR_DC_OFFSET_HZ 25000      /* narrow mode: the channel sits off the LO */
#define SIM_PASS_US 500000
#define FILES_MAX 16

static StaticSemaphore_t s_lock_mem;
static SemaphoreHandle_t s_lock;
static unsigned s_lock_once;
static void lock(void)
{
    /* The UI and the worker can both be first here. */
    unsigned expect = 0;
    if (__atomic_compare_exchange_n(&s_lock_once, &expect, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
        __atomic_store_n(&s_lock_once, 2, __ATOMIC_RELEASE);
    } else while (__atomic_load_n(&s_lock_once, __ATOMIC_ACQUIRE) != 2) vTaskDelay(1);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

typedef enum { REQ_NONE, REQ_START, REQ_STOP, REQ_REPLAY, REQ_EXPORT, REQ_CLEAR } req_t;

/* Shared: written under the lock. */
EXT_RAM_BSS_ATTR static ls_rfs_grid_t s_grid;
EXT_RAM_BSS_ATTR static ls_rfs_view_t s_view;
EXT_RAM_BSS_ATTR static ls_rfs_settings_t s_want;
EXT_RAM_BSS_ATTR static double s_track_lat[LS_RFS_TRACK], s_track_lon[LS_RFS_TRACK];
EXT_RAM_BSS_ATTR static char s_files[FILES_MAX][40];
EXT_RAM_BSS_ATTR static char s_req_path[96];
EXT_RAM_BSS_ATTR static ls_rfs_plan_t s_card[LS_RFS_CARD_PLANS];
static int s_ncard;
static char s_card_why[80] = "not read yet";
static int s_track_n, s_track_head, s_nfiles, s_req_speed;
static req_t s_req;
static bool s_list_req;
static int s_show_ch = -1;
static ls_rfs_metric_t s_show_m = LS_RFS_MAX;

/* Worker only. */
typedef struct {
    bool on;
    ls_rfs_settings_t set;
    ls_rfs_header_t hdr;
    FILE *f;
    char path[192];
    int64_t start_us, pass_start_us;
    float level[LS_RFS_CH_MAX];
    uint32_t got;
    int group;
    int64_t group_until;
    uint32_t pass;
    ls_rfs_floor_t floor;
    bool have_last;
    double last_lat, last_lon, prev_lat, prev_lon;
    bool have_prev;
    /* sources */
    ls_dfs_t dfs;
    bool field, lr, mesh_held, spec_on;
    /* The shared WIDE I/Q setting as the survey found it, put back on
       release: every LR app on FOLLOW (P25) reads it. */
    bool wide_set;
    lr_wf_wide_status wide_saved;
    uint32_t field_sweeps;
    uint32_t lr_tuned, lr_seq;
    int lr_settle;
    int64_t sim_next;
} work_t;
EXT_RAM_BSS_ATTR static work_t w;

/* Replay, worker only. */
EXT_RAM_BSS_ATTR static struct {
    bool on;
    FILE *f;
    ls_rfs_header_t hdr;
    int speed;
    int64_t next_us;
    uint32_t done, total;
} r;

EXT_RAM_BSS_ATTR static uint8_t s_io[LS_RFS_HEADER + LS_RFS_CH_BYTES * LS_RFS_CH_MAX];
EXT_RAM_BSS_ATTR static ls_field_state_t s_fs;
EXT_RAM_BSS_ATTR static float s_bins[LR_SPEC_BINS];

static void say(const char *fmt, const char *arg)
{
    lock(); snprintf(s_view.status, sizeof(s_view.status), fmt, arg ? arg : ""); unlock();
}

/* ------------------------------------------------------------- sources -- */

const char *ls_rfs_src_name(ls_rfs_src_t s)
{
    switch (s) {
    case LS_RFS_SRC_RTL:    return ls_rsel_name(LS_RSEL_SDR_RTL);
    case LS_RFS_SRC_HACKRF: return ls_rsel_name(LS_RSEL_SDR_HACKRF);
    case LS_RFS_SRC_LORA:   return ls_rsel_name(LS_RSEL_LORA);
    case LS_RFS_SRC_SWEEP: {
        static char t[24]; snprintf(t, sizeof(t), "%.12s SWEEP", ls_rsel_name(LS_RSEL_LORA)); return t;
    }
    case LS_RFS_SRC_LR_IQ:  return "LR2021 IQ";
    case LS_RFS_SRC_CC1101: return ls_rsel_name(LS_RSEL_CC1101);
    case LS_RFS_SRC_SIM:    return "SIM";
    default:                return "?";
    }
}

const char *ls_rfs_src_detail(ls_rfs_src_t s)
{
    switch (s) {
    case LS_RFS_SRC_RTL:    return "25 kHz at each channel centre, dB re full scale";
    case LS_RFS_SRC_HACKRF: return "25 kHz at each channel centre, dB re full scale";
    case LS_RFS_SRC_LORA:   return "carrier RSSI on each channel, dBm";
    case LS_RFS_SRC_SWEEP:  return "RSSI sweep across the plan, loudest bin per channel, dBm";
    case LS_RFS_SRC_LR_IQ:  return "125 kHz IQ looks, channel power, dB re full scale";
    case LS_RFS_SRC_CC1101: return "keyboard-board RSSI on each channel, dBm";
    case LS_RFS_SRC_SIM:    return "pretend transmitters around the simulated walk";
    default:                return "";
    }
}

static ls_dfs_t dfs_of(ls_rfs_src_t s)
{
    switch (s) {
    case LS_RFS_SRC_RTL: return LS_DFS_RTL;
    case LS_RFS_SRC_HACKRF: return LS_DFS_HACKRF;
    case LS_RFS_SRC_LORA: return LS_DFS_LORA;
    case LS_RFS_SRC_CC1101: return LS_DFS_CC1101;
    default: return LS_DFS_COUNT;
    }
}

static const char *unit_of(ls_rfs_src_t s)
{
    return s == LS_RFS_SRC_RTL || s == LS_RFS_SRC_HACKRF || s == LS_RFS_SRC_LR_IQ ? "dBFS" : "dBm";
}

const char *ls_rfs_src_unavailable(ls_rfs_src_t s)
{
    if (s == LS_RFS_SRC_SIM) return NULL;
    if (s == LS_RFS_SRC_SWEEP || s == LS_RFS_SRC_LR_IQ) {
        if (!ls_lora_present()) return "No LoRa chip in the socket";
        if (s == LS_RFS_SRC_LR_IQ) {
            ls_rsel_hw_t hw; ls_rsel_hw(&hw);
            if (!hw.lora_lr20xx) return "IQ needs the LR2021: this socket has an SX1262";
        }
        return NULL;
    }
    const ls_dfs_t d = dfs_of(s);
    return d == LS_DFS_COUNT ? "Unknown source" : ls_dfs_unavailable(d);
}

void ls_rfs_settings_default(ls_rfs_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->src = LS_RFS_SRC_LORA;
    ls_rfs_preset(0, &out->plan);
    out->step_m = 20; out->cell_m = 25; out->threshold_db = 10; out->dwell_ms = 250;
}

/* ------------------------------------------------------------ requests -- */

bool ls_rfs_start(const ls_rfs_settings_t *s)
{
    if (!s || s->plan.n < 1 || s->plan.n > LS_RFS_CH_MAX) return false;
    const char *why = ls_rfs_src_unavailable(s->src);
    if (why) { say("%s", why); return false; }
    lock();
    const bool ok = !s_view.running && !s_view.busy && s_req == REQ_NONE;
    if (ok) { s_want = *s; s_req = REQ_START; s_view.busy = true; snprintf(s_view.status, sizeof(s_view.status), "Starting"); }
    unlock();
    ls_field_start();
    return ok;
}

void ls_rfs_stop(void) { lock(); if (s_view.running || s_view.replaying) { s_req = REQ_STOP; s_view.busy = true; } unlock(); }

bool ls_rfs_replay(const char *path, int speed)
{
    lock();
    const bool ok = !s_view.running && !s_view.busy && s_req == REQ_NONE;
    if (ok) {
        snprintf(s_req_path, sizeof(s_req_path), "%s", path ? path : "");
        s_req_speed = speed; s_req = REQ_REPLAY; s_view.busy = true;
    }
    unlock();
    if (ok) ls_field_start();
    return ok;
}

bool ls_rfs_export(const char *path)
{
    lock();
    const bool ok = !s_view.busy && s_req == REQ_NONE && (path || s_view.have_file);
    if (ok) { snprintf(s_req_path, sizeof(s_req_path), "%s", path ? path : ""); s_req = REQ_EXPORT; s_view.busy = true; }
    unlock();
    if (ok) ls_field_start();
    return ok;
}

void ls_rfs_clear(void) { lock(); if (!s_view.running && !s_view.replaying && s_req == REQ_NONE) s_req = REQ_CLEAR; unlock(); }

void ls_rfs_view(ls_rfs_view_t *out)
{
    lock();
    *out = s_view;
    out->cells = s_grid.count; out->dropped = s_grid.dropped;
    if (s_view.running) out->elapsed_s = (uint32_t)((esp_timer_get_time() - w.start_us) / 1000000);
    unlock();
}

bool ls_rfs_running(void) { return s_view.running; }
bool ls_rfs_busy(void) { return s_view.running || s_view.replaying || s_view.busy || s_req != REQ_NONE || s_list_req; }

void ls_rfs_files_refresh(void) { s_list_req = true; ls_field_start(); }

int ls_rfs_files(char names[][40], int max)
{
    lock();
    const int n = s_nfiles < max ? s_nfiles : max;
    for (int i = 0; i < n; i++) memcpy(names[i], s_files[i], 40);
    unlock();
    return n;
}

void ls_rfs_show(int ch, ls_rfs_metric_t m)
{
    lock();
    s_show_ch = ch >= -1 && ch < LS_RFS_CH_MAX ? ch : -1;
    s_show_m = m >= 0 && m < LS_RFS_METRICS ? m : LS_RFS_MAX;
    s_grid.serial++;
    unlock();
}
void ls_rfs_shown(int *ch, ls_rfs_metric_t *m) { if (ch) *ch = s_show_ch; if (m) *m = s_show_m; }

const ls_rfs_grid_t *ls_rfs_grid_lock(void) { lock(); return &s_grid; }
void ls_rfs_grid_unlock(void) { unlock(); }

int ls_rfs_track(double *lat, double *lon, int max)
{
    const int n = s_track_n < max ? s_track_n : max;
    const int first = (s_track_head - s_track_n + LS_RFS_TRACK) % LS_RFS_TRACK + (s_track_n - n);
    for (int i = 0; i < n; i++) {
        const int k = (first + i) % LS_RFS_TRACK;
        lat[i] = s_track_lat[k]; lon[i] = s_track_lon[k];
    }
    return n;
}

/* ---------------------------------------------------------------- card -- */

/* Every directory down to `dir`. */
static void mkdirs(const char *dir)
{
    char p[128];
    snprintf(p, sizeof(p), "%s", dir);
    for (char *c = p + 1; ; c++) {
        if (*c == '/' || !*c) {
            const char keep = *c; *c = 0;
#ifdef _WIN32
            mkdir(p);
#else
            mkdir(p, 0777);
#endif
            *c = keep;
            if (!keep) break;
        }
    }
}

static void make_dirs(void) { mkdirs(LS_RFS_DIR); }

/* The example plan file, written once so there is one to copy and edit.
   The same plans as data/rfsurvey/plans.json. */
static void seed_plans(void)
{
    struct stat st;
    if (!stat(PLANS_FILE, &st)) return;
    FILE *f = fopen(PLANS_FILE, "wb");
    if (!f) return;
    LS_FPUTS(t_ls_rfs_run_schema_lakeshark_rfsurvey_plans, "{\n  \"schema\": \"lakeshark.rfsurvey.plans\",\n  \"version\": 1,\n  \"plans\": [\n"
          "    { \"name\": \"MURS\",\n      \"channels\": [\n"
          "        { \"hz\": 151820000, \"bw\": 11250, \"label\": \"MURS1\" },\n"
          "        { \"hz\": 151880000, \"bw\": 11250, \"label\": \"MURS2\" },\n"
          "        { \"hz\": 151940000, \"bw\": 11250, \"label\": \"MURS3\" },\n"
          "        { \"hz\": 154570000, \"bw\": 20000, \"label\": \"MURS4\" },\n"
          "        { \"hz\": 154600000, \"bw\": 20000, \"label\": \"MURS5\" } ] },\n"
          "    { \"name\": \"2M HAM\", \"range\": { \"lo\": 144000000, \"hi\": 148000000, \"cells\": 16 } },\n"
          "    { \"name\": \"70CM HAM\", \"range\": { \"lo\": 420000000, \"hi\": 450000000, \"cells\": 16 } }\n"
          "  ]\n}\n", f);
    fclose(f);
}

static void read_plans(void)
{
    EXT_RAM_BSS_ATTR static char text[16384];
    EXT_RAM_BSS_ATTR static ls_rfs_plan_t got[LS_RFS_CARD_PLANS];
    char why[80] = "";
    int n = -1;
    FILE *f = fopen(PLANS_FILE, "rb");
    if (f) {
        const size_t len = fread(text, 1, sizeof(text), f);
        fclose(f);
        if (len == sizeof(text)) LS_SNPUTS(t_ls_rfs_run_plans_json_is_over, why, sizeof(why), "plans.json is over 16 KB");
        else n = ls_rfs_plans_parse(text, len, got, LS_RFS_CARD_PLANS, why, sizeof(why));
    } else LS_SNPUTS(t_ls_rfs_run_no_plans_json_on, why, sizeof(why), "no plans.json on the card");
    lock();
    s_ncard = n > 0 ? n : 0;
    memcpy(s_card, got, sizeof(s_card[0]) * s_ncard);
    snprintf(s_card_why, sizeof(s_card_why), "%s", n > 0 ? "" : n == 0 ? "plans.json has no plans" : why);
    unlock();
}

int ls_rfs_card_plans(ls_rfs_plan_t *out, int max)
{
    lock();
    const int n = s_ncard < max ? s_ncard : max;
    memcpy(out, s_card, sizeof(out[0]) * n);
    unlock();
    return n;
}

const char *ls_rfs_card_plans_why(void) { return s_card_why[0] ? s_card_why : NULL; }

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

static void list_files(void)
{
    make_dirs();
    seed_plans();
    read_plans();
    EXT_RAM_BSS_ATTR static char names[FILES_MAX * 4][40];
    int n = 0;
    DIR *d = opendir(LS_RFS_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < FILES_MAX * 4) {
            const size_t len = strlen(e->d_name);
            if (len > 5 && len < 40 && !strcmp(e->d_name + len - 5, ".lsrf")) snprintf(names[n++], 40, "%s", e->d_name);
        }
        closedir(d);
    }
    /* Stamped names sort by time; newest first. */
    qsort(names, n, 40, name_cmp);
    lock();
    s_nfiles = n < FILES_MAX ? n : FILES_MAX;
    memcpy(s_files, names, sizeof(s_files[0]) * s_nfiles);
    unlock();
}

/* --------------------------------------------------------- heat + track -- */

static void track_add(double lat, double lon)
{
    s_track_lat[s_track_head] = lat; s_track_lon[s_track_head] = lon;
    s_track_head = (s_track_head + 1) % LS_RFS_TRACK;
    if (s_track_n < LS_RFS_TRACK) s_track_n++;
}

static void reset_heat(float cell_m, int nch)
{
    lock();
    ls_rfs_grid_reset(&s_grid, cell_m, nch);
    s_track_n = s_track_head = 0;
    unlock();
}

/* --------------------------------------------------------------- start -- */

static void source_release(void)
{
    if (w.dfs != LS_DFS_COUNT) { ls_dfs_stop_slot(0); w.dfs = LS_DFS_COUNT; }
    if (w.field) {
        ls_field_spectrum_span(0, 0);
        ls_field_mode(LS_LAB_PACKETS);
        ls_field_direct(false);
        w.field = false;
    }
    if (w.lr) { lr_wf_stop(); w.lr = false; }
    if (w.spec_on) { lr_spec_enable(false); w.spec_on = false; }
    if (w.wide_set) {
        lr_wf_set_wide(w.wide_saved.on, w.wide_saved.offset_hz);
        w.wide_set = false;
    }
    if (w.mesh_held) { ls_mesh_radio_hold(false); w.mesh_held = false; }
}

static void group_apply(void)
{
    const int base = w.group * GROUP;
    int n = w.set.plan.n - base;
    if (n > GROUP) n = GROUP;
    uint32_t hz[GROUP];
    for (int i = 0; i < n; i++) hz[i] = w.set.plan.ch[base + i].hz;
    ls_dfs_set_channels(0, hz, n, w.set.dwell_ms);
    /* Long enough for every channel a retune each, and the SDR's settling. */
    w.group_until = esp_timer_get_time() + (int64_t)(n * (w.set.dwell_ms + 150) + 2500) * 1000;
    { ls_dfs_reading_t junk[16]; while (ls_dfs_take(junk, 16) > 0) {} }
    lock(); s_view.group = w.group; unlock();
}

static void pass_begin(void)
{
    for (int i = 0; i < LS_RFS_CH_MAX; i++) w.level[i] = NAN;
    w.got = 0;
    w.pass_start_us = esp_timer_get_time();
    if (w.dfs != LS_DFS_COUNT && w.set.plan.n > GROUP) { w.group = 0; group_apply(); }
    else if (w.dfs != LS_DFS_COUNT)
        /* One group: the receiver keeps scanning it; only the wait restarts. */
        w.group_until = w.pass_start_us + (int64_t)(w.set.plan.n * (w.set.dwell_ms + 150) + 2500) * 1000;
}

static void finish(const char *why)
{
    source_release();
    if (w.f) { fflush(w.f); fclose(w.f); w.f = NULL; }
    w.on = false;
    lock();
    s_view.elapsed_s = (uint32_t)((esp_timer_get_time() - w.start_us) / 1000000);
    s_view.running = false; s_view.busy = false;
    if (why) snprintf(s_view.status, sizeof(s_view.status), "%s", why);
    unlock();
}

static void begin(void)
{
    memset(&w, 0, sizeof(w));
    w.dfs = LS_DFS_COUNT;
    lock(); w.set = s_want; unlock();
    const ls_rfs_plan_t *p = &w.set.plan;
    make_dirs();
    seed_plans();

    /* The header first: a file that cannot be opened still lets the survey
       run live, and says so. */
    memset(&w.hdr, 0, sizeof(w.hdr));
    w.hdr.version = LS_RFS_VERSION; w.hdr.nch = (uint16_t)p->n;
    w.hdr.row_bytes = (uint16_t)ls_rfs_row_bytes(p->n);
    w.hdr.start_utc = ls_time_is_synced() ? (uint32_t)time(NULL) : 0;
    snprintf(w.hdr.source, sizeof(w.hdr.source), "%.16s", ls_rfs_src_name(w.set.src));
    snprintf(w.hdr.plan, sizeof(w.hdr.plan), "%.16s", p->name);
    w.hdr.threshold_cdb = (int16_t)lroundf(w.set.threshold_db * 100);
    w.hdr.cell_m = (uint16_t)w.set.cell_m;
    memcpy(w.hdr.ch, p->ch, sizeof(w.hdr.ch));
    char base[48];
    ls_time_render_filename(base, sizeof(base), "rfs");
    snprintf(w.path, sizeof(w.path), LS_RFS_DIR "/%s.lsrf", base);
    w.f = fopen(w.path, "wb");
    const size_t hn = ls_rfs_header_pack(&w.hdr, s_io, sizeof(s_io));
    if (w.f && fwrite(s_io, 1, hn, w.f) != hn) { fclose(w.f); w.f = NULL; }

    reset_heat(w.set.cell_m, p->n);
    ls_rfs_floor_reset(&w.floor);
    uint32_t hz[LS_RFS_CH_MAX];
    for (int i = 0; i < p->n; i++) hz[i] = p->ch[i].hz;
    ls_rfsim_plan(hz, p->n);

    lock();
    memset(&s_view, 0, sizeof(s_view));
    s_view.running = true; s_view.src = w.set.src;
    snprintf(s_view.src_name, sizeof(s_view.src_name), "%s", ls_rfs_src_name(w.set.src));
    snprintf(s_view.unit, sizeof(s_view.unit), "%s", unit_of(w.set.src));
    snprintf(s_view.plan, sizeof(s_view.plan), "%s", p->name);
    s_view.nch = p->n;
    memcpy(s_view.ch, p->ch, sizeof(s_view.ch));
    for (int i = 0; i < LS_RFS_CH_MAX; i++) s_view.level[i] = s_view.floor[i] = NAN;
    s_view.groups = (p->n + GROUP - 1) / GROUP;
    s_view.have_file = w.f != NULL;
    const char *slash = strrchr(w.path, '/');
    snprintf(s_view.file, sizeof(s_view.file), "%s", w.f ? slash + 1 : "");
    snprintf(s_view.status, sizeof(s_view.status), "%s", w.f ? "Surveying" : "No card: surveying without saving");
    unlock();

    w.start_us = esp_timer_get_time();
    w.on = true;

    switch (w.set.src) {
    case LS_RFS_SRC_RTL: case LS_RFS_SRC_HACKRF: case LS_RFS_SRC_LORA: case LS_RFS_SRC_CC1101: {
        const ls_dfs_t d = dfs_of(w.set.src);
        if (ls_dfs_any_active()) { finish(LS_TEXT(t_ls_rfs_run_compass_find_has_a, "COMPASS FIND has a receiver: stop FIND first")); return; }
        if (!ls_dfs_select_slot(0, d, p->ch[0].hz)) { finish(LS_TEXT(t_ls_rfs_run_that_receiver_would_not, "That receiver would not start")); return; }
        w.dfs = d;
        w.group = 0;
        group_apply();
        break;
    }
    case LS_RFS_SRC_SWEEP: {
        const uint32_t lo = p->ch[0].hz - p->ch[0].bw_hz / 2, hi = p->ch[p->n - 1].hz + p->ch[p->n - 1].bw_hz / 2;
        if (!ls_lora_rx_range_ok(ls_lora_caps(), lo, hi)) { finish(LS_TEXT(t_ls_rfs_run_the_lora_chip_does, "The LoRa chip does not sweep that span")); return; }
        ls_field_mode(LS_LAB_SPECTRUM);
        ls_field_direct(true);
        ls_field_spectrum_span(lo, hi);
        w.field = true;
        break;
    }
    case LS_RFS_SRC_LR_IQ:
        if (lr_spec_center()) { finish(LS_TEXT(t_ls_rfs_run_another_app_has_the, "Another app has the LR2021 IQ stream")); return; }
        ls_mesh_radio_hold(true); w.mesh_held = true;
        if (!w.wide_set) { lr_wf_wide_get(&w.wide_saved); w.wide_set = true; }
        lr_wf_set_wide(w.set.wide, LR_WF_WIDE_DEFAULT_HZ);
        break;
    default: break;
    }
    pass_begin();
}

/* --------------------------------------------------------------- a pass -- */

static void keep_row(void)
{
    const int n = w.set.plan.n;
    ls_rfs_row_t row;
    memset(&row, 0, sizeof(row));
    const int64_t now = esp_timer_get_time();
    row.t_ms = (uint32_t)((now - w.start_us) / 1000);
    row.utc_s = ls_time_is_synced() ? (uint32_t)time(NULL) : 0;
    row.pass = (uint16_t)w.pass;
    float floor[LS_RFS_CH_MAX];
    uint32_t occ = 0, heard = 0;
    for (int i = 0; i < LS_RFS_CH_MAX; i++) { row.level[i] = row.floor[i] = LS_RFS_NO_LEVEL; floor[i] = NAN; }
    for (int i = 0; i < n; i++) {
        if (!isfinite(w.level[i])) continue;
        floor[i] = ls_rfs_floor_update(&w.floor, i, w.level[i]);
        row.level[i] = (int16_t)lroundf(fmaxf(-320.0f, fminf(320.0f, w.level[i])) * 100);
        row.floor[i] = (int16_t)lroundf(fmaxf(-320.0f, fminf(320.0f, floor[i])) * 100);
        heard |= 1u << i;
        if (w.level[i] - floor[i] >= w.set.threshold_db) occ |= 1u << i;
    }
    row.occ = occ;
    double lat = 0, lon = 0; uint16_t fl = 0;
    const bool where = ls_rf_where(&lat, &lon, &fl, NULL);
    bool kept = false;
    float moved = 0;
    if (where) {
        row.lat = lat; row.lon = lon; row.flags = fl;
        if (w.have_prev) moved = (float)ls_rfs_distance_m(w.prev_lat, w.prev_lon, lat, lon);
        if (moved > 500) moved = 0;                 /* a jump, not a walk */
        w.prev_lat = lat; w.prev_lon = lon; w.have_prev = true;
        /* A pass that heard nothing (the receiver was busy or gone) keeps no row. */
        if (heard && ls_rfs_moved_enough(w.have_last, w.last_lat, w.last_lon, lat, lon, w.set.step_m)) {
            kept = true;
            w.have_last = true; w.last_lat = lat; w.last_lon = lon;
            if (w.f) {
                const size_t rn = ls_rfs_row_pack(&w.hdr, &row, s_io, sizeof(s_io));
                if (fwrite(s_io, 1, rn, w.f) != rn) { fclose(w.f); w.f = NULL; say("%s", LS_TEXT(t_ls_rfs_run_card_write_failed_surveying, "Card write failed: surveying without saving")); }
                else fflush(w.f);
            }
        }
    }
    lock();
    for (int i = 0; i < n; i++) { s_view.level[i] = w.level[i]; s_view.floor[i] = floor[i]; }
    s_view.occ = occ; s_view.heard = heard;
    s_view.pass_serial++; s_view.passes++; s_view.pass_us = now;
    s_view.where_ok = where; s_view.where_flags = fl; s_view.lat = lat; s_view.lon = lon;
    s_view.walked_m += moved;
    if (!where) s_view.waiting++;
    if (kept) {
        s_view.rows++;
        ls_rfs_grid_add(&s_grid, &row);
        track_add(lat, lon);
    }
    if (!where && !s_view.status[0]) LS_SNPUTS(t_ls_rfs_run_no_position_levels_shown, s_view.status, sizeof(s_view.status), "No position: levels shown, rows wait for GPS");
    unlock();
    w.pass++;
    pass_begin();
}

static void take_dfs(void)
{
    ls_dfs_status_t st;
    ls_dfs_poll_slot(0, &st);
    if (!st.active || st.source != w.dfs) { finish(LS_TEXT(t_ls_rfs_run_another_app_took_the, "Another app took the receiver (COMPASS FIND?)")); return; }
    const int base = w.set.plan.n > GROUP ? w.group * GROUP : 0;
    int span = w.set.plan.n - base;
    if (span > GROUP) span = GROUP;
    ls_dfs_reading_t rd[32];
    const int got = ls_dfs_take(rd, 32);
    for (int k = 0; k < got; k++) {
        if (rd[k].slot != 0) continue;
        const int ch = base + rd[k].channel;
        if (ch >= w.set.plan.n || !isfinite(rd[k].level)) continue;
        /* The loudest read of the pass: a keyed transmitter counts if it
           was on for any of the dwell. */
        if (!(w.got & (1u << ch)) || rd[k].level > w.level[ch]) w.level[ch] = rd[k].level;
        w.got |= 1u << ch;
    }
    const uint32_t want = (span >= 32 ? 0xFFFFFFFFu : ((1u << span) - 1)) << base;
    const bool timed_out = esp_timer_get_time() > w.group_until;
    if ((w.got & want) == want || timed_out) {
        if (timed_out && !(w.got & want)) say("%s", st.status[0] ? st.status : "No levels from the receiver yet");
        if (base + span >= w.set.plan.n) keep_row();
        else { w.group++; group_apply(); }
    }
}

/* LoRa chip sweep: each channel takes the loudest bin inside it, or the
   nearest bin to its centre when the channel is narrower than a bin. */
static void take_sweep(void)
{
    ls_field_snapshot(&s_fs);
    if (!s_fs.direct || s_fs.mode != LS_LAB_SPECTRUM || !s_fs.spectrum_sweeps || s_fs.spectrum_sweeps == w.field_sweeps) return;
    w.field_sweeps = s_fs.spectrum_sweeps;
    const double lo = s_fs.span_lo_hz, hi = s_fs.span_hi_hz;
    if (!(hi > lo)) return;
    const double bin = (hi - lo) / LS_FIELD_BINS;
    for (int i = 0; i < w.set.plan.n; i++) {
        const ls_rfs_ch_t *c = &w.set.plan.ch[i];
        int a = (int)floor((c->hz - c->bw_hz / 2.0 - lo) / bin), b = (int)floor((c->hz + c->bw_hz / 2.0 - lo) / bin);
        if (a < 0) a = 0;
        if (b >= LS_FIELD_BINS) b = LS_FIELD_BINS - 1;
        if (b < a) { a = b = (int)floor((c->hz - lo) / bin); if (a < 0 || a >= LS_FIELD_BINS) continue; }
        float top = -200;
        for (int k = a; k <= b; k++) if (s_fs.spectrum[k] > top) top = s_fs.spectrum[k];
        if (top > -190) { w.level[i] = top; w.got |= 1u << i; }
    }
    keep_row();
}

/* LR2021 IQ: one 125 kHz look at a time, each channel inside it measured
   from the same spectrum line, then on to the next channel not yet heard. */
static uint32_t lr_centre_for(const ls_rfs_ch_t *c) { return w.set.wide ? c->hz : c->hz + LR_DC_OFFSET_HZ; }

static void take_lr(void)
{
    if (!w.mesh_held) return;
    if (!ls_mesh_radio_held()) { say("%s", LS_TEXT(t_ls_rfs_run_waiting_for_mesh_to, "Waiting for Mesh to let go of the LR2021")); return; }
    int next = -1;
    for (int i = 0; i < w.set.plan.n; i++) if (!(w.got & (1u << i))) { next = i; break; }
    if (next < 0) { keep_row(); return; }
    if (!w.lr || !w.lr_tuned) {
        const uint32_t hz = lr_centre_for(&w.set.plan.ch[next]);
        if (!w.spec_on) { if (!lr_spec_enable(true)) { finish(LS_TEXT(t_ls_rfs_run_lr_iq_memory_unavailable, "LR IQ memory unavailable")); return; } w.spec_on = true; }
        if (lr_wf_start(hz) != ESP_OK) { finish(LS_TEXT(t_ls_rfs_run_lr_iq_session_failed, "LR IQ session failed")); return; }
        w.lr = true; w.lr_tuned = hz; w.lr_seq = 0; w.lr_settle = 2;
        return;
    }
    lr_spec_line line;
    if (!lr_spec_read(s_bins, &line, (uint32_t)(esp_timer_get_time() / 1000))) return;
    if (line.sequence == w.lr_seq || line.center_hz != lr_spec_center()) return;
    w.lr_seq = line.sequence;
    if (line.gap || w.lr_settle > 0) { if (w.lr_settle > 0) w.lr_settle--; return; }
    const double centre = line.center_hz, per = (double)LR_SPAN_HZ / LR_SPEC_BINS;
    for (int i = 0; i < w.set.plan.n; i++) {
        if (w.got & (1u << i)) continue;
        const ls_rfs_ch_t *c = &w.set.plan.ch[i];
        double half = c->bw_hz / 2.0;
        if (half > LR_REACH_HZ - 1000) half = LR_REACH_HZ - 1000;     /* a wide cell: its middle */
        const double off = (double)c->hz - centre;
        if (fabs(off) + half > LR_REACH_HZ) continue;
        int a = (int)floor((off - half) / per + LR_SPEC_BINS / 2.0), b = (int)floor((off + half) / per + LR_SPEC_BINS / 2.0);
        if (a < 0) a = 0;
        if (b >= LR_SPEC_BINS) b = LR_SPEC_BINS - 1;
        /* Channel power: the bins' powers summed. Bins arrive as 0..1 over
           -100..0 dBFS. */
        double sum = 0;
        for (int k = a; k <= b; k++) sum += pow(10.0, (s_bins[k] * 100.0 - 100.0) / 10.0);
        w.level[i] = (float)(10.0 * log10(sum > 1e-12 ? sum : 1e-12));
        w.got |= 1u << i;
    }
    /* Not this look's? Then the next look is its own. */
    for (int i = 0; i < w.set.plan.n; i++)
        if (!(w.got & (1u << i))) {
            const uint32_t hz = lr_centre_for(&w.set.plan.ch[i]);
            if (lr_wf_start(hz) != ESP_OK) { finish("LR IQ retune failed"); return; }
            w.lr_tuned = hz; w.lr_settle = 1;
            return;
        }
    keep_row();
}

static void take_sim(void)
{
    const int64_t now = esp_timer_get_time();
    if (now < w.sim_next) return;
    w.sim_next = now + SIM_PASS_US;
    double lat, lon; uint16_t fl;
    if (!ls_rf_where(&lat, &lon, &fl, NULL)) { lat = 0; lon = 0; }
    for (int i = 0; i < w.set.plan.n; i++) {
        w.level[i] = ls_rfsim_level(lat, lon, w.set.plan.ch[i].hz, w.set.plan.ch[i].bw_hz, NAN, now);
        w.got |= 1u << i;
    }
    keep_row();
}

/* --------------------------------------------------------------- replay -- */

static void replay_begin(void)
{
    char path[192];
    lock(); snprintf(path, sizeof(path), "%s", s_req_path); r.speed = s_req_speed; unlock();
    if (!path[0]) {
        list_files();
        lock();
        if (s_nfiles) snprintf(path, sizeof(path), LS_RFS_DIR "/%s", s_files[0]);
        unlock();
        if (!path[0]) { lock(); s_view.busy = false; LS_SNPUTS(t_ls_rfs_run_no_survey_files_on, s_view.status, sizeof(s_view.status), "No survey files on the card"); unlock(); return; }
    } else if (!strchr(path, '/')) {
        char t[192]; snprintf(t, sizeof(t), LS_RFS_DIR "/%.80s", path); snprintf(path, sizeof(path), "%s", t);
    }
    r.f = fopen(path, "rb");
    size_t got = r.f ? fread(s_io, 1, sizeof(s_io), r.f) : 0;
    size_t hn = got ? ls_rfs_header_unpack(s_io, got, &r.hdr) : 0;
    if (!hn) {
        if (r.f) fclose(r.f);
        r.f = NULL;
        lock(); s_view.busy = false; LS_SNPUTS(t_ls_rfs_run_not_a_survey_file, s_view.status, sizeof(s_view.status), "Not a survey file this version reads"); unlock();
        return;
    }
    fseek(r.f, 0, SEEK_END);
    const long size = ftell(r.f);
    fseek(r.f, (long)hn, SEEK_SET);
    r.total = size > (long)hn ? (uint32_t)((size - (long)hn) / r.hdr.row_bytes) : 0;
    r.done = 0; r.on = true; r.next_us = 0;
    reset_heat(r.hdr.cell_m ? r.hdr.cell_m : 25, r.hdr.nch);
    lock();
    memset(&s_view, 0, sizeof(s_view));
    s_view.replaying = true; s_view.have_file = true;
    snprintf(s_view.src_name, sizeof(s_view.src_name), "%s", r.hdr.source);
    snprintf(s_view.unit, sizeof(s_view.unit), "%s", strstr(r.hdr.source, "SDR") || strstr(r.hdr.source, "HackRF") || strstr(r.hdr.source, "IQ") ? "dBFS" : "dBm");
    snprintf(s_view.plan, sizeof(s_view.plan), "%s", r.hdr.plan);
    s_view.nch = r.hdr.nch;
    memcpy(s_view.ch, r.hdr.ch, sizeof(s_view.ch));
    for (int i = 0; i < LS_RFS_CH_MAX; i++) s_view.level[i] = s_view.floor[i] = NAN;
    s_view.replay_total = r.total;
    const char *slash = strrchr(path, '/');
    snprintf(s_view.file, sizeof(s_view.file), "%s", slash ? slash + 1 : path);
    snprintf(s_view.status, sizeof(s_view.status), "Replaying %lu rows", (unsigned long)r.total);
    unlock();
}

static void replay_end(const char *why)
{
    if (r.f) fclose(r.f);
    r.f = NULL; r.on = false;
    lock(); s_view.replaying = false; s_view.busy = false; if (why) snprintf(s_view.status, sizeof(s_view.status), "%s", why); unlock();
}

static void replay_step(void)
{
    const int64_t now = esp_timer_get_time();
    if (now < r.next_us) return;
    /* A batch per step so a slow replay is smooth and a fast one is quick. */
    int batch = r.speed <= 0 ? 400 : 1;
    if (r.speed > 0) r.next_us = now + 1000000 / r.speed;
    while (batch-- > 0) {
        if (fread(s_io, 1, r.hdr.row_bytes, r.f) != r.hdr.row_bytes) {
            char t[64]; snprintf(t, sizeof(t), "Replayed %lu rows", (unsigned long)r.done);
            replay_end(t); return;
        }
        ls_rfs_row_t row;
        if (!ls_rfs_row_unpack(&r.hdr, s_io, r.hdr.row_bytes, &row)) continue;
        lock();
        for (int i = 0; i < r.hdr.nch; i++) {
            s_view.level[i] = row.level[i] == LS_RFS_NO_LEVEL ? NAN : row.level[i] / 100.0f;
            s_view.floor[i] = row.floor[i] == LS_RFS_NO_LEVEL ? NAN : row.floor[i] / 100.0f;
        }
        if (s_view.rows) {
            const float d = (float)ls_rfs_distance_m(s_view.lat, s_view.lon, row.lat, row.lon);
            if (d < 500) s_view.walked_m += d;
        }
        s_view.occ = row.occ; s_view.pass_serial++; s_view.pass_us = now; s_view.rows++; s_view.passes++;
        s_view.where_ok = true; s_view.where_flags = row.flags; s_view.lat = row.lat; s_view.lon = row.lon;
        s_view.elapsed_s = row.t_ms / 1000;
        s_view.replay_done = ++r.done;
        ls_rfs_grid_add(&s_grid, &row);
        track_add(row.lat, row.lon);
        unlock();
    }
}

/* --------------------------------------------------------------- export -- */

static void export_file(void)
{
    char path[192];
    lock();
    if (s_req_path[0]) snprintf(path, sizeof(path), strchr(s_req_path, '/') ? "%s" : LS_RFS_DIR "/%s", s_req_path);
    else snprintf(path, sizeof(path), LS_RFS_DIR "/%s", s_view.file);
    unlock();
    /* A survey still recording is flushed first, so the export has every row. */
    if (w.f) fflush(w.f);
    FILE *in = fopen(path, "rb");
    ls_rfs_header_t h;
    const size_t got = in ? fread(s_io, 1, sizeof(s_io), in) : 0;
    const size_t hn = got ? ls_rfs_header_unpack(s_io, got, &h) : 0;
    if (!hn) {
        if (in) fclose(in);
        lock(); s_view.busy = false; LS_SNPUTS(t_ls_rfs_run_nothing_to_export_no, s_view.status, sizeof(s_view.status), "Nothing to export: no survey file"); unlock();
        return;
    }
    char out[200];
    const size_t stem = strlen(path) > 5 ? strlen(path) - 5 : strlen(path);
    snprintf(out, sizeof(out), "%.*s.csv", (int)stem, path);
    FILE *csv = fopen(out, "wb");
    snprintf(out, sizeof(out), "%.*s.geojson", (int)stem, path);
    FILE *geo = fopen(out, "wb");
    EXT_RAM_BSS_ATTR static char line[4096];
    uint32_t rows = 0;
    bool ok = csv && geo;
    if (ok) {
        fwrite(line, 1, ls_rfs_csv_header(line, sizeof(line)), csv);
        fwrite(line, 1, ls_rfs_geojson_begin(&h, line, sizeof(line)), geo);
        fseek(in, (long)hn, SEEK_SET);
        while (fread(s_io, 1, h.row_bytes, in) == h.row_bytes) {
            ls_rfs_row_t row;
            if (!ls_rfs_row_unpack(&h, s_io, h.row_bytes, &row)) continue;
            for (int c = 0; c < h.nch; c++) fwrite(line, 1, ls_rfs_csv_line(&h, &row, c, (row.occ >> c) & 1, line, sizeof(line)), csv);
            fwrite(line, 1, ls_rfs_geojson_feature(&h, &row, rows == 0, line, sizeof(line)), geo);
            rows++;
        }
        fwrite(line, 1, ls_rfs_geojson_end(line, sizeof(line)), geo);
        ok = !ferror(csv) && !ferror(geo);
    }
    if (csv) fclose(csv);
    if (geo) fclose(geo);
    fclose(in);
    lock();
    s_view.busy = false;
    if (ok) LS_SNPRINTF(fmt_ls_rfs_run_exported_rows_csv_geojson, s_view.status, sizeof(s_view.status), "Exported %lu rows: .csv + .geojson beside the .lsrf", (unsigned long)rows);
    else LS_SNPUTS(t_ls_rfs_run_export_failed_card_full, s_view.status, sizeof(s_view.status), "Export failed: card full or read-only?");
    unlock();
}

/* --------------------------------------------------------------- worker -- */

void ls_rfs_step(void)
{
    ls_fox_io_step();
    lock(); const req_t req = s_req; s_req = REQ_NONE; const bool list = s_list_req; s_list_req = false; unlock();
    if (list) list_files();
    switch (req) {
    case REQ_START: if (r.on) replay_end(NULL); begin(); break;
    case REQ_STOP:
        if (w.on) { char t[64]; LS_SNPRINTF(t_ls_rfs_run_stopped_lu_rows_kept, t, sizeof(t), "Stopped: %lu rows kept", (unsigned long)s_view.rows); finish(t); }
        if (r.on) replay_end("Replay stopped");
        lock(); s_view.busy = false; unlock();
        break;
    case REQ_REPLAY: replay_begin(); break;
    case REQ_EXPORT: export_file(); break;
    case REQ_CLEAR:
        reset_heat(s_grid.cell_m, s_grid.nch);
        lock(); s_view.rows = 0; s_view.walked_m = 0; s_view.passes = 0; s_view.status[0] = 0; unlock();
        break;
    default: break;
    }
    if (r.on) replay_step();
    if (!w.on) return;
    switch (w.set.src) {
    case LS_RFS_SRC_SWEEP: take_sweep(); break;
    case LS_RFS_SRC_LR_IQ: take_lr(); break;
    case LS_RFS_SRC_SIM:   take_sim(); break;
    default:               if (w.dfs != LS_DFS_COUNT) take_dfs(); break;
    }
}

/* -------------------------------------------------------------- console -- */

int ls_rfs_command(int argc, char **argv)
{
    ls_rfs_view_t v;
    if (argc >= 2 && !strcmp(argv[1], "start")) {
        ls_rfs_settings_t s;
        ls_rfs_settings_default(&s);
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            if (!strcmp(a, "sim")) s.src = LS_RFS_SRC_SIM;
            else if (!strcmp(a, "rtl")) s.src = LS_RFS_SRC_RTL;
            else if (!strcmp(a, "hackrf")) s.src = LS_RFS_SRC_HACKRF;
            else if (!strcmp(a, "lora")) s.src = LS_RFS_SRC_LORA;
            else if (!strcmp(a, "sweep")) s.src = LS_RFS_SRC_SWEEP;
            else if (!strcmp(a, "iq")) s.src = LS_RFS_SRC_LR_IQ;
            else if (!strcmp(a, "iqwide")) { s.src = LS_RFS_SRC_LR_IQ; s.wide = true; }
            else if (!strcmp(a, "cc1101")) s.src = LS_RFS_SRC_CC1101;
            else if (!strncmp(a, "plan=", 5)) { if (!ls_rfs_preset(atoi(a + 5), &s.plan)) { printf("no plan %s\n", a + 5); return 1; } }
            else if (!strncmp(a, "range=", 6)) {
                double lo = 0, hi = 0; int n = 16;
                if (sscanf(a + 6, "%lf:%lf:%d", &lo, &hi, &n) < 2 || !ls_rfs_plan_range(&s.plan, "CUSTOM", (uint32_t)(lo * 1e6), (uint32_t)(hi * 1e6), n)) {
                    printf("range=LO_MHZ:HI_MHZ[:CELLS]\n"); return 1;
                }
            }
            else if (!strncmp(a, "step=", 5)) s.step_m = (float)atof(a + 5);
            else if (!strncmp(a, "cell=", 5)) s.cell_m = (float)atof(a + 5);
            else if (!strncmp(a, "thresh=", 7)) s.threshold_db = (float)atof(a + 7);
            else { LS_FPUTS(msg_ls_rfs_run_start_sim_rtl_hackrf, "start [sim|rtl|hackrf|lora|sweep|iq|iqwide|cc1101] [plan=N] [range=LO:HI:N] [step=M] [cell=M] [thresh=DB]\n", stdout); return 1; }
        }
        if (!ls_rfs_start(&s)) { ls_rfs_view(&v); LS_PRINTF(t_ls_rfs_run_rfsurvey_refused_s, "rfsurvey: refused (%s)\n", v.status); return 1; }
        LS_PRINTF(t_ls_rfs_run_rfsurvey_starting_s_on, "rfsurvey: starting %s on %s, %d channels\n", ls_rfs_src_name(s.src), s.plan.name, s.plan.n);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "stop")) { ls_rfs_stop(); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "replay")) {
        return ls_rfs_replay(argc > 2 ? argv[2] : NULL, argc > 3 ? atoi(argv[3]) : 20) ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "export")) return ls_rfs_export(argc > 2 ? argv[2] : NULL) ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "files")) {
        list_files();
        char names[FILES_MAX][40];
        const int n = ls_rfs_files(names, FILES_MAX);
        for (int i = 0; i < n; i++) printf("  %s\n", names[i]);
        printf("%d survey file(s) in " LS_RFS_DIR "\n", n);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "plans")) {
        for (int i = 0; i < ls_rfs_preset_count(); i++) {
            ls_rfs_plan_t p; ls_rfs_preset(i, &p);
            LS_PRINTF(t_ls_rfs_run_plan_d_12s_2d, "  plan=%d  %-12s %2d ch  %.4f-%.4f MHz\n", i, p.name, p.n, p.ch[0].hz / 1e6, p.ch[p.n - 1].hz / 1e6);
        }
        return 0;
    }
    if (argc != 1 && !(argc == 2 && !strcmp(argv[1], "status"))) {
        LS_FPUTS(msg_ls_rfs_run_rfsurvey_status_start_stop, "rfsurvey [status] | start ... | stop | replay [FILE] [ROWS/S, 0 all] | export [FILE] | files | plans\n", stdout);
        return 1;
    }
    ls_rfs_view(&v);
    LS_PRINTF(fmt_ls_rfs_run_rfsurvey_on_rows_passes, "rfsurvey: %s  %s on %s  rows %lu  passes %lu  cells %d  walked %.0f m  %s\n",
           v.running ? "RUNNING" : v.replaying ? "REPLAY" : "idle", v.src_name, v.plan, (unsigned long)v.rows,
           (unsigned long)v.passes, v.cells, v.walked_m, v.file);
    LS_PRINTF(t_ls_rfs_run_where_s_6f_6f, "  where: %s %.6f, %.6f   %s\n", v.where_ok ? (v.where_flags & LS_RFS_ROW_SIM ? "SIM" : "GPS") : "none", v.lat, v.lon, v.status);
    for (int i = 0; i < v.nch; i++)
        LS_PRINTF(fmt_ls_rfs_run_mhz_floor, "  %2d %-8s %10.4f MHz  %7.1f  floor %7.1f %s %s\n", i, v.ch[i].label, v.ch[i].hz / 1e6, v.level[i], v.floor[i], v.unit,
               (v.occ >> i) & 1 ? "BUSY" : "");
    return 0;
}
