/* BUGHUNT's tag watch (it was the BLE WATCH app): see bh_watch.h.

   The table lives from the app opening to its closing, so a tag that stays
   with you keeps its time through sweeps and hunts, and the following alert
   fires on any tab. Its clock is the app's, pulled forward by the radio's
   own timestamps, so a capture replayed fast in the simulator runs its
   fourteen minutes in a few seconds. Receive only. */
#include "bh_watch.h"
#include "lakeshark_ui.h"
#include "lap_scroll.h"
#include "lap_art.h"
#include "bh_lookup.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bw_t *w;
/* the source, as main.c reports it */
static int src_kind;
static char src_why[96];
static bool src_ended;
/* the watch's clock: lap_millis, pulled forward by the radio's timestamps */
static uint32_t offset_ms, base_app;
static uint64_t base_us;
static bool based;
/* the screen */
static int W, H, cy0, cy1;              /* clip rows: cy0 <= y < cy1 */
static int view, sel, top, list_y, list_h, row_h, list_x, list_w;
static int lscroll, lsmax, drag_y0, drag_y1;
static bool dragging, show_sel = true;
static lap_scroll_t drag;
static uint32_t sel_uid;
static int idx[BW_MAX_DEV], nidx;
static int info_row = -1, info_x;
/* adverts a second, for the head's braille history */
#define HIST 48
static float aps_hist[HIST];
static int aps_n;
static uint32_t aps_last, aps_ms;
/* GPS */
static int gps_lease, gps_state;
static bool gps_fitted, sd_fitted;
/* the log on the card: log_want is the owner's choice (saved), log_on
   whether it is running (a card that fails turns it off, not the choice) */
static bool log_on, log_want;
static char logbuf[6144], log_path[64];
static int loglen, log_job = -1, log_stage;
static uint32_t log_bytes, log_lines, log_drop, last_flush, last_summary;
static uint32_t logged_uid_mark[BW_MAX_DEV];     /* uid of the device the NEW line was logged for */
/* the logs already on the card: BUGHUNT's and BLE WATCH's */
static lap_sd_entry_t *ents;
#define ENTS 64
static int ls_job = -1, ls_stage, logs_mine = -1, logs_old = -1;
static char old_newest[64];
/* a message for main.c's title line */
static char note[96];
static bool note_new;
/* MINE and WATCH, kept on the card by address (slot "marks"): a tag that
   keeps one address (Tile, many cheap tags) is known again next time; a
   rotating one for as long as it keeps that address. Saved one job at a
   time and polled, as main.c saves its settings. */
#define MARKS 24
typedef struct { uint8_t addr[6], type, mark; } mark_t;
static mark_t marks[MARKS];
static int nmarks, marks_job = -1;
static bool marks_dirty;
static uint32_t marks_at;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(note, sizeof(note), fmt, ap);
    va_end(ap);
    note_new = true;
}
bool bhw_note(char *out, size_t cap)
{
    if (!note_new) return false;
    note_new = false;
    snprintf(out, cap, "%s", note);
    return true;
}
static void put(int x, int y, int wd, uint8_t attr, const char *fmt, ...)
{
    char t[160];
    va_list ap;
    if (wd <= 0 || y < cy0 || y >= cy1 || x >= W || x < 0) return;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (x + wd > W) wd = W - x;
    if ((size_t)wd < sizeof(t)) t[wd] = 0;
    lap_put_str(x, y, t, attr);
}
static void cell(int x, int y, int16_t ch, uint8_t attr)
{
    if (y >= cy0 && y < cy1 && x >= 0 && x < W) lap_put_cell(x, y, ch, attr);
}
static void clip(int y0, int y1)
{
    W = lap_cols(); H = lap_rows();
    cy0 = y0 < 0 ? 0 : y0;
    cy1 = y1 > H ? H : y1;
}

uint32_t bhw_now(void) { return lap_millis() + offset_ms; }
/* A frame's time on the watch's clock. The first frame pins the radio clock
   to now; frames that run ahead (a file replayed fast) pull the clock along. */
static uint32_t frame_time(uint64_t ts_us)
{
    if (!ts_us) return bhw_now();
    if (!based) { based = true; base_us = ts_us; base_app = bhw_now(); }
    const uint32_t t = ts_us >= base_us ? base_app + (uint32_t)((ts_us - base_us) / 1000u) : base_app;
    if (t > bhw_now()) offset_ms = t - lap_millis();
    return t;
}

/* ---- the log ------------------------------------------------------------------------------- */
static void log_text(const char *s)
{
    if (!log_on) return;
    const size_t n = strlen(s);
    if (loglen + n >= sizeof(logbuf)) { log_drop++; return; }
    memcpy(logbuf + loglen, s, n);
    loglen += (int)n;
    log_lines++;
}
static void log_dev(const bw_dev_t *d, const char *event)
{
    char line[160];
    bw_csv(d, event, bhw_now(), line, sizeof(line));
    log_text(line);
}
static void log_step(bool force)
{
    if (log_job >= 0) {
        const int r = lap_sd_poll(log_job);
        if (r == LAP_SD_PENDING) return;
        log_job = -1;
        if (log_stage == 1) log_stage = 2;              /* the folder may already have been there */
        else if (r < 0) { say("log write failed (%d): is there a card?", r); log_on = false; loglen = 0; return; }
    }
    if (!log_on || !loglen) return;
    /* The app's four card jobs are shared (settings saves, the listing):
       when all are out, the log waits for one and keeps what it holds. */
    if (log_stage == 0) {
        if (!log_path[0]) {
            lap_time_t t;
            lap_time(&t);
            snprintf(log_path, sizeof(log_path), BHW_LOG_DIR "/tags-%04d%02d%02d-%02d%02d%02d.csv", t.year, t.month, t.day,
                     t.hour, t.minute, t.second);
        }
        log_job = lap_sd_mkdir(BHW_LOG_DIR);
        if (log_job == LAP_SD_EBUSY) { log_job = -1; return; }
        log_stage = 1;
        if (log_job < 0) log_stage = 2;
        return;
    }
    if (log_stage == 2 && (force || loglen > 2048 || bhw_now() - last_flush > 10000u)) {
        log_job = lap_sd_write(log_path, log_bytes ? LAP_SD_APPEND : 0, logbuf, (uint32_t)loglen);
        if (log_job == LAP_SD_EBUSY) { log_job = -1; return; }
        if (log_job < 0) { say("log write refused (%d)", log_job); log_on = false; }
        else log_bytes += (uint32_t)loglen;
        loglen = 0;
        last_flush = bhw_now();
    }
}
void bhw_log_toggle(void)
{
    if (!log_on && !sd_fitted) { say("no card for the tag log"); return; }
    log_on = !log_on;
    log_want = log_on;
    if (log_on) {
        log_stage = 0; log_bytes = 0; loglen = 0; log_lines = log_drop = 0; log_path[0] = 0;
        log_text(BW_CSV_HEADER);
        memset(logged_uid_mark, 0, sizeof(logged_uid_mark));    /* a new file names every device again */
        say("tag log to " BHW_LOG_DIR);
    } else {
        say("tag log off");
        loglen = 0;
    }
}
bool bhw_log_on(void) { return log_on; }
/* The logs already on the card, counted once when the app opens: this
   app's own, and the ones BLE WATCH wrote before it was folded in (they
   stay where they are; INFO says where). */
static void list_step(void)
{
    if (!ents || ls_stage >= 2) return;
    if (ls_job < 0) {
        ls_job = lap_sd_list(ls_stage ? BHW_OLD_DIR : BHW_LOG_DIR, ents, ENTS);
        if (ls_job == LAP_SD_EBUSY) { ls_job = -1; return; }                   /* all four jobs out: next frame */
        if (ls_job < 0) { if (ls_stage) logs_old = 0; else logs_mine = 0; ls_stage++; }
        return;
    }
    const int r = lap_sd_poll(ls_job);
    if (r == LAP_SD_PENDING) return;
    ls_job = -1;
    int n = 0;
    for (int i = 0; i < r && i < ENTS; i++) {
        const size_t l = strlen(ents[i].name);
        if (ents[i].dir || l < 5 || strcmp(ents[i].name + l - 4, ".csv")) continue;
        n++;
        if (ls_stage && strcmp(ents[i].name, old_newest) > 0) snprintf(old_newest, sizeof(old_newest), "%s", ents[i].name);
    }
    if (ls_stage) logs_old = n; else logs_mine = n;
    if (++ls_stage >= 2) { free(ents); ents = NULL; }
}

/* ---- life ---------------------------------------------------------------------------------- */
bool bhw_open(void)
{
    w = calloc(1, sizeof(*w));
    if (!w) return false;
    bw_init(w);
    src_kind = 0; src_why[0] = 0; src_ended = false;
    offset_ms = 0; based = false;
    view = BHW_TAGS; sel = top = 0; sel_uid = 0; nidx = 0;
    memset(&drag, 0, sizeof(drag));
    lscroll = lsmax = drag_y0 = drag_y1 = 0;
    dragging = false; show_sel = true;
    aps_n = 0; aps_last = 0; aps_ms = 0;
    last_summary = 0;
    memset(logged_uid_mark, 0, sizeof(logged_uid_mark));
    log_on = log_want = false; log_job = -1; log_stage = 0; log_lines = log_drop = 0; loglen = 0; log_path[0] = 0;
    note_new = false;
    gps_lease = 0; gps_state = 0;
    lap_caps_t c;
    memset(&c, 0, sizeof(c));
    gps_fitted = sd_fitted = false;
    if (!lap_caps(&c, sizeof(c))) { gps_fitted = (c.hardware & LAP_HW_GPS) != 0; sd_fitted = (c.hardware & LAP_HW_SD) != 0; }
    if (gps_fitted) gps_lease = lap_gps_acquire(0);
    logs_mine = logs_old = -1; old_newest[0] = 0; ls_job = -1; ls_stage = 2;
    if (sd_fitted && (ents = calloc(ENTS, sizeof(*ents))) != NULL) ls_stage = 0;
    nmarks = 0; marks_job = -1; marks_dirty = false; marks_at = 0;
    const int got = lap_load("marks", marks, sizeof(marks));
    if (got > 0) nmarks = got / (int)sizeof(marks[0]);
    for (int i = 0; i < nmarks; i++) if (marks[i].mark > BW_MARK_WATCH) marks[i].mark = 0;
    return true;
}
static void marks_pump(void)
{
    if (marks_job >= 0) {
        const int r = lap_sd_poll(marks_job);
        if (r == LAP_SD_PENDING) return;
        if (r < 0) marks_dirty = true;
        marks_job = -1;
    }
    if (!marks_dirty || lap_millis() - marks_at < 500u) return;
    marks_at = lap_millis();
    const int id = lap_save("marks", marks, sizeof(marks[0]) * (size_t)nmarks);
    if (id >= 0) { marks_dirty = false; marks_job = id; }
}
static int mark_find(const uint8_t addr[6], uint8_t type)
{
    for (int i = 0; i < nmarks; i++)
        if (!memcmp(marks[i].addr, addr, 6) && marks[i].type == (type & 1)) return i;
    return -1;
}
static void mark_keep(const bw_dev_t *d, uint8_t mark)
{
    int i = mark_find(d->addr, d->addr_type);
    if (!mark) {
        if (i >= 0) { marks[i] = marks[--nmarks]; marks_dirty = true; }
        return;
    }
    if (i < 0) {
        if (nmarks == MARKS) { memmove(marks, marks + 1, sizeof(marks[0]) * (MARKS - 1)); nmarks--; }   /* the oldest goes */
        i = nmarks++;
        memcpy(marks[i].addr, d->addr, 6);
        marks[i].type = d->addr_type & 1;
    }
    marks[i].mark = mark;
    marks_dirty = true;
}
uint8_t bhw_mark_cycle(uint32_t uid)
{
    bw_dev_t *d = bhw_dev(uid);
    if (!w || !d) return 0;
    const uint8_t m = (uint8_t)((d->mark + 1) % 3);
    bw_mark(w, (int)(d - w->dev), m);
    mark_keep(d, m);
    say(m == BW_MARK_MINE ? "MINE: this tag never alerts" : m == BW_MARK_WATCH ? "WATCH: says when this tag is left behind" : "mark cleared");
    return m;
}
int bhw_watch_poll(int *event)
{
    return w ? bw_watch(w, bhw_now(), event) : -1;
}
void bhw_tracking(uint16_t kind_off, uint32_t watch_ms)
{
    if (!w) return;
    w->kind_off = kind_off;
    w->watch_ms = watch_ms;
}
int bhw_marked(int *mine, int *watching)
{
    int m = 0, k = 0;
    for (int i = 0; w && i < w->n; i++) {
        if (w->dev[i].succ) continue;
        m += w->dev[i].mark == BW_MARK_MINE;
        k += w->dev[i].mark == BW_MARK_WATCH;
    }
    if (mine) *mine = m;
    if (watching) *watching = k;
    return nmarks;
}
const char *bhw_label(const bw_dev_t *d, char *buf, size_t cap)
{
    bw_label(d, buf, cap);
    const bw_ad_t *a = &d->ad;
    /* a name it gave once is kept by address (shared with every BLE app) */
    if (a->name[0]) lap_ids_device_learn(d->addr, d->addr_type, a->name);
    if (a->name[0] || a->level) return buf;             /* its own name, or a tracker's kind */
    const char *n = lap_ids_device(d->addr, d->addr_type);
    if (n) { snprintf(buf, cap, "%s", n); return buf; }
    if (!strncmp(buf, "co 0x", 5)) n = bhl_get(BHID_COMPANY, a->company);
    else if (!strncmp(buf, "svc ", 4)) n = bhl_get(BHID_UUID, a->uuid16[0]);
    else if (!strcmp(buf, "(no name)") && !(d->addr_type & 1)) {
        uint8_t be[6];
        for (int i = 0; i < 6; i++) be[i] = d->addr[5 - i];     /* the radio gives it little endian */
        n = bhl_maker(be);
    }
    if (n) snprintf(buf, cap, "%s", n);
    /* what it says it is: "Samsung Watch" */
    const char *ap = a->appearance ? bhl_get(BHID_APPEARANCE, a->appearance) : NULL;
    if (ap && strcmp(ap, "Unknown") && strlen(buf) + strlen(ap) + 2 < cap) {
        if (!strcmp(buf, "(no name)")) snprintf(buf, cap, "%s", ap);
        else { strcat(buf, " "); strcat(buf, ap); }
    }
    return buf;
}
void bhw_close(void)
{
    if (marks_dirty && marks_job < 0) lap_save("marks", marks, sizeof(marks[0]) * (size_t)nmarks);
    if (log_on && loglen && log_job < 0 && log_stage == 2)
        lap_sd_write(log_path, log_bytes ? LAP_SD_APPEND : 0, logbuf, (uint32_t)loglen);
    loglen = 0;
    log_on = false;
    if (gps_lease > 0) lap_gps_release(gps_lease);
    gps_lease = 0;
    free(ents); ents = NULL;
    free(w); w = NULL;
}
void bhw_config(uint32_t follow_ms, bool possible, bool log)
{
    if (!w) return;
    w->follow_ms = follow_ms;
    w->alert_possible = possible;
    if ((log && sd_fitted) != log_on) bhw_log_toggle();
    log_want = log;                     /* no card now is not a choice to keep */
}
void bhw_get_config(uint32_t *follow_ms, bool *possible, bool *log)
{
    if (!w) return;
    *follow_ms = w->follow_ms;
    *possible = w->alert_possible;
    *log = log_want;
}
void bhw_follow_step(int by)
{
    static const uint32_t steps[] = {2, 5, 10, 15, 20, 30, 45, 60};
    if (!w) return;
    int i = 0;
    while (i < 7 && steps[i] * 60000u < w->follow_ms) i++;
    i += by;
    i = i < 0 ? 0 : i > 7 ? 7 : i;
    w->follow_ms = steps[i] * 60000u;
    say("following alert after %lu minutes", (unsigned long)steps[i]);
}
void bhw_possible_toggle(void)
{
    if (!w) return;
    w->alert_possible = !w->alert_possible;
    say("possible trackers: %s", w->alert_possible ? "alert" : "list only");
}
void bhw_source(int kind, const char *why, bool ended)
{
    src_kind = kind;
    snprintf(src_why, sizeof(src_why), "%s", why ? why : "");
    src_ended = ended;
}
void bhw_clear(void)
{
    if (!w) return;
    const bool lp = w->alert_possible;
    const uint32_t f = w->follow_ms;
    const float fm = w->follow_m;
    const bool fix = w->fix;
    const double lat = w->lat, lon = w->lon;
    const uint16_t ko = w->kind_off;
    const uint32_t wm = w->watch_ms;
    bw_init(w);
    w->alert_possible = lp; w->follow_ms = f; w->follow_m = fm;
    w->kind_off = ko; w->watch_ms = wm;
    bw_position(w, fix, lat, lon);
    memset(logged_uid_mark, 0, sizeof(logged_uid_mark));
    sel_uid = 0; nidx = 0;
    say("tag list cleared");
}

void bhw_advert(const uint8_t addr[6], uint8_t type, uint8_t pdu, int rssi, const uint8_t *ad, size_t n, uint64_t ts_us)
{
    if (!w) return;
    const int i = bw_add(w, addr, type & 1, pdu, rssi, ad, n, frame_time(ts_us));
    const bw_dev_t *d = &w->dev[i];
    if (logged_uid_mark[i] != d->uid) {
        logged_uid_mark[i] = d->uid;
        log_dev(d, d->ad.level ? "new-tracker" : "new");
        const int m = nmarks && !d->mark ? mark_find(d->addr, d->addr_type) : -1;   /* known from before */
        if (m >= 0) bw_mark(w, i, marks[m].mark);
    }
}

/* ---- the list -------------------------------------------------------------------------------- */
bw_t *bhw_table(void) { return w; }
bw_dev_t *bhw_dev(uint32_t uid)
{
    if (!w) return NULL;
    int i = bw_find_uid(w, uid);
    for (int hops = 0; i >= 0 && w->dev[i].succ && hops < 64; hops++) {   /* follow a rotation */
        const int j = bw_find_uid(w, w->dev[i].succ);
        if (j < 0) break;
        i = j;
    }
    return i >= 0 ? &w->dev[i] : NULL;
}
uint32_t bhw_uid_of(const uint8_t addr[6], uint8_t type)
{
    if (!w) return 0;
    for (int i = 0; i < w->n; i++)
        if (w->dev[i].addr_type == (type & 1) && !memcmp(w->dev[i].addr, addr, 6)) return w->dev[i].uid;
    for (int i = 0; i < w->n; i++)                  /* a scan that did not say the address type */
        if (!memcmp(w->dev[i].addr, addr, 6)) return w->dev[i].uid;
    return 0;
}
int bhw_counts(int *trackers, int *alerts, int *all)
{
    int t = 0, a = 0, n = 0;
    for (int i = 0; w && i < w->n; i++) {
        if (w->dev[i].succ) continue;
        n++;
        if (w->dev[i].ad.level) { t++; a += w->dev[i].alert != 0; }
    }
    if (trackers) *trackers = t;
    if (alerts) *alerts = a;
    if (all) *all = n;
    return t;
}
static uint32_t sort_now;
static int by_tracker(const void *a, const void *b)
{
    const bw_dev_t *x = &w->dev[*(const int *)a], *y = &w->dev[*(const int *)b];
    const int fx = sort_now - x->last_ms < 60000u, fy = sort_now - y->last_ms < 60000u;
    if (x->alert != y->alert) return y->alert - x->alert;
    if ((x->mark == BW_MARK_MINE) != (y->mark == BW_MARK_MINE)) return x->mark == BW_MARK_MINE ? 1 : -1;  /* your own sink */
    if (fx != fy) return fy - fx;
    if (x->ad.level != y->ad.level) return y->ad.level - x->ad.level;
    return bw_span_ms(y) > bw_span_ms(x) ? 1 : bw_span_ms(y) < bw_span_ms(x) ? -1 : (int)(x->uid - y->uid);
}
static int by_signal(const void *a, const void *b)
{
    const bw_dev_t *x = &w->dev[*(const int *)a], *y = &w->dev[*(const int *)b];
    const int fx = sort_now - x->last_ms < 60000u, fy = sort_now - y->last_ms < 60000u;
    if (fx != fy) return fy - fx;
    if (y->smooth != x->smooth) return y->smooth > x->smooth ? 1 : -1;
    return (int)(x->uid - y->uid);
}
static void build_list(void)
{
    nidx = 0;
    if (!w) return;
    sort_now = bhw_now();
    const bool tags = view != BHW_ALL;
    for (int i = 0; i < w->n; i++) {
        const bw_dev_t *d = &w->dev[i];
        if (d->succ) continue;
        if (tags && !d->ad.level) continue;
        idx[nidx++] = i;
    }
    qsort(idx, (size_t)nidx, sizeof(idx[0]), tags ? by_tracker : by_signal);
    sel = 0;
    for (int i = 0; i < nidx; i++) if (w->dev[idx[i]].uid == sel_uid) sel = i;
    if (nidx && sel_uid != w->dev[idx[sel]].uid) {
        const bw_dev_t *f = bhw_dev(sel_uid);            /* the pick rotated: find its new address */
        for (int i = 0; f && i < nidx; i++) if (&w->dev[idx[i]] == f) sel = i;
    }
    if (nidx) sel_uid = w->dev[idx[sel]].uid;
}
static bw_dev_t *selected(void) { return nidx ? &w->dev[idx[sel]] : NULL; }
static void move_sel(int by)
{
    if (!nidx) return;
    sel += by;
    if (sel < 0) sel = 0;
    if (sel >= nidx) sel = nidx - 1;
    sel_uid = w->dev[idx[sel]].uid;
    show_sel = true;
}
uint32_t bhw_pick(void) { const bw_dev_t *d = selected(); return d ? d->uid : 0; }
void bhw_pick_set(uint32_t uid) { sel_uid = uid; show_sel = true; build_list(); }
uint32_t bhw_step(uint32_t uid, int by)
{
    const bw_dev_t *d = bhw_dev(uid);
    int at = -1;
    for (int i = 0; d && i < nidx; i++) if (&w->dev[idx[i]] == d) at = i;
    if (at < 0 || !nidx) return nidx ? w->dev[idx[0]].uid : 0;
    at = (at + by + nidx) % nidx;
    return w->dev[idx[at]].uid;
}
int bhw_view(void) { return view; }
void bhw_view_set(int v)
{
    view = (v % BHW_VIEWS + BHW_VIEWS) % BHW_VIEWS;
    lap_scroll_stop(&drag);
    show_sel = true;
    build_list();
}
const char *bhw_view_name(int v) { return v == BHW_TAGS ? "TAGS" : v == BHW_ALL ? "ALL" : "INFO"; }

/* ---- once a frame ---------------------------------------------------------------------------- */
static void poll_gps(void)
{
    if (gps_lease <= 0 || lap_frame() % 25) return;
    lap_gps_status_t g = {.size = sizeof(g), .version = 1};
    if (lap_gps_status(&g)) return;
    gps_state = g.state;
    bw_position(w, g.state == LAP_GPS_FIX && g.position.fix, g.position.lat, g.position.lon);
}
int bhw_poll(void)
{
    if (!w) return -1;
    poll_gps();
    marks_pump();
    const int a = bw_tick(w, bhw_now());
    if (a >= 0) log_dev(&w->dev[a], "alert");
    if (bhw_now() - last_summary >= 60000u) {          /* a line a minute per tracker still about */
        last_summary = bhw_now();
        for (int i = 0; i < w->n; i++) {
            const bw_dev_t *d = &w->dev[i];
            if (d->ad.level && !d->succ && last_summary - d->last_ms < 60000u) log_dev(d, "seen");
        }
    }
    log_step(false);
    list_step();
    const uint32_t t = lap_millis();          /* adverts a second, for the head's history */
    if (t - aps_ms >= 1000u) {
        const float secs = aps_ms ? (float)(t - aps_ms) / 1000.0f : 1.0f;
        if (aps_n == HIST) { memmove(aps_hist, aps_hist + 1, sizeof(float) * (HIST - 1)); aps_n--; }
        aps_hist[aps_n++] = (float)(w->adverts - aps_last) / secs;
        aps_last = w->adverts;
        aps_ms = t;
    }
    build_list();
    return a;
}
void bhw_alert_text(int i, char *out, size_t cap)
{
    if (!w || i < 0 || i >= w->n) { snprintf(out, cap, "a tag is following you"); return; }
    const bw_dev_t *d = &w->dev[i];
    char label[32];
    bhw_label(d, label, sizeof(label));
    snprintf(out, cap, "%s with you for %lu min%s", label, (unsigned long)(bw_span_ms(d) / 60000u), d->has_pos0 ? ", moving" : "");
}

/* ---- drawing helpers ------------------------------------------------------------------------ */
uint8_t bhw_colour(const bw_dev_t *d)
{
    if (d->alert) return LAP_RED | LAP_BRIGHT;
    if (d->mark == BW_MARK_MINE) return LAP_GREEN;
    if (d->mark == BW_MARK_WATCH) return LAP_CYAN | LAP_BRIGHT;
    if (d->ad.level == 2) return LAP_YELLOW | LAP_BRIGHT;
    if (d->ad.level == 1) return LAP_MAGENTA | LAP_BRIGHT;
    if (d->ad.cls == BW_C_IBEACON || d->ad.cls == BW_C_EDDYSTONE) return LAP_BLUE | LAP_BRIGHT;
    return LAP_WHITE;
}
const char *bhw_distance_word(float q, bool heard)
{
    return !heard ? "not heard" : q > 0.64f ? "very close" : q > 0.45f ? "close" : q > 0.25f ? "nearby" : "far";
}
static void age_text(uint32_t ms, char *out, size_t cap)
{
    const uint32_t s = ms / 1000u;
    if (s < 60) snprintf(out, cap, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(out, cap, "%lum", (unsigned long)(s / 60));
    else snprintf(out, cap, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}
static int level_of(int rssi, int levels, int lo, int hi)
{
    if (rssi == BW_NO_RSSI) return 0;
    int l = 1 + (rssi - lo) * (levels - 1) / (hi - lo);
    return l < 1 ? 1 : l > levels ? levels : l;
}
/* The last `cells`*2 RSSI buckets as a one-row braille sparkline, scaled to
   the device's own range (at least 12 dB) so a trend shows. */
static void spark(int x, int y, int cells, const bw_dev_t *d, uint8_t attr)
{
    static const uint8_t left[5] = {0, 0x40, 0x44, 0x46, 0x47}, right[5] = {0, 0x80, 0xA0, 0xB0, 0xB8};
    int8_t h[BW_HIST];
    bw_hist(d, bhw_now(), h);
    if (cells * 2 > BW_HIST) cells = BW_HIST / 2;
    int lo = 0, hi = -128;
    for (int b = BW_HIST - cells * 2; b < BW_HIST; b++)
        if (h[b] != BW_NO_RSSI) { if (h[b] < lo) lo = h[b]; if (h[b] > hi) hi = h[b]; }
    if (hi < lo) hi = lo;
    if (hi - lo < 12) { const int mid = (hi + lo) / 2; lo = mid - 6; hi = mid + 6; }
    for (int c = 0; c < cells; c++) {
        const int b = BW_HIST - cells * 2 + c * 2;
        cell(x + c, y, LAP_BRAILLE(left[level_of(h[b], 4, lo, hi)] | right[level_of(h[b + 1], 4, lo, hi)]), attr);
    }
}
/* A taller graph: rows x cells of braille, two buckets per cell, -100 to -30 dBm. */
static void graph(int x, int y, int cells, int rows, const bw_dev_t *d)
{
    static const uint8_t left[5] = {0, 0x40, 0x44, 0x46, 0x47}, right[5] = {0, 0x80, 0xA0, 0xB0, 0xB8};
    int8_t h[BW_HIST];
    if (!d) return;
    bw_hist(d, bhw_now(), h);
    if (cells * 2 > BW_HIST) cells = BW_HIST / 2;
    for (int r = 0; r < rows; r++) {
        const int rb = rows - 1 - r;
        const uint8_t attr = LAP_ATTR(rb >= rows * 2 / 3 ? LAP_GREEN | LAP_BRIGHT : rb >= rows / 3 ? LAP_YELLOW : LAP_RED, LAP_BLACK);
        for (int c = 0; c < cells; c++) {
            const int b = BW_HIST - cells * 2 + c * 2;
            int la = level_of(h[b], rows * 4, -100, -30) - rb * 4, lb = level_of(h[b + 1], rows * 4, -100, -30) - rb * 4;
            la = la < 0 ? 0 : la > 4 ? 4 : la;
            lb = lb < 0 ? 0 : lb > 4 ? 4 : lb;
            cell(x + c, y + r, (la | lb) ? LAP_BRAILLE(left[la] | right[lb]) : (int16_t)(r == rows - 1 ? '_' : ' '),
                 (la | lb) ? attr : LAP_ATTR(LAP_BLUE, LAP_BLACK));
        }
    }
}

void bhw_graph(int x, int y, int cells, int rows, const bw_dev_t *d)
{
    clip(0, lap_rows());
    graph(x, y, cells, rows, d);
}

/* ---- the head: radar and tally ------------------------------------------------------------------ */
/* A radar with its rings and turning beam, every device heard in the last
   minute a blip at its bearing (kept per address), nearer the middle the
   louder it is; a tag that follows you flashes. Beside it: tags, the
   source, GPS and log, the loudest tag on a meter, adverts a second in
   braille and the alert, pulsing. */
void bhw_draw_head(int x, int y, int wd, int hgt)
{
    clip(y, y + hgt);
    if (!w) return;
    const uint32_t now = lap_millis(), t = bhw_now();
    art_blip_t b[32];
    int n = 0, best = -127;
    const bw_dev_t *loud = NULL;
    for (int i = 0; i < w->n && n < 32; i++) {
        const bw_dev_t *d = &w->dev[i];
        if (d->succ || t - d->last_ms > 60000u) continue;
        b[n++] = (art_blip_t){art_bearing(d->addr), art_range(d->rssi), d->ad.level ? bhw_colour(d) & 7 : LAP_BLUE, d->alert != 0};
        if (d->ad.level && d->rssi > best) { best = d->rssi; loud = d; }
    }
    const int aw = 14, ah = hgt > 8 ? 8 : hgt;
    art_radar(x, y, aw, ah, now, src_kind ? 2000u : 0u, b, n);
    if (!src_kind) put(x + aw / 2 - 1, y + ah / 2, 3, LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK), " x ");
    const int fx = x + aw + 2, fw = x + wd - fx;
    if (fw < 14) return;
    int trackers = 0, alerts = 0, all = 0;
    bhw_counts(&trackers, &alerts, &all);
    put(fx, y, fw, LAP_UI_TEXT, "%d tag%s  %d device%s", trackers, trackers == 1 ? "" : "s", all, all == 1 ? "" : "s");
    int cx = fx;
    const char *sname = src_kind == 1 ? (src_ended ? "DONE" : "CAPTURE") : src_kind == 2 ? "SCAN" : "NO BT";
    if (y + 1 >= cy0 && y + 1 < cy1) {
        cx = lap_ui_chip(cx, y + 1, sname, src_kind && !src_ended, src_kind ? LAP_GREEN : LAP_RED) + 1;
        if (gps_fitted && cx + 5 <= x + wd) cx = lap_ui_chip(cx, y + 1, "GPS", gps_state == LAP_GPS_FIX, LAP_CYAN) + 1;
        if (cx + 5 <= x + wd) lap_ui_chip(cx, y + 1, "LOG", log_on, LAP_CYAN);
    }
    if (loud) {
        char label[32];
        bhw_label(loud, label, sizeof(label));
        const int lw = fw - 9 < 14 ? (fw - 9 > 4 ? fw - 9 : 4) : 14;
        put(fx, y + 2, fw, LAP_ATTR(bhw_colour(loud), LAP_BLACK), "%.*s %d dBm", lw, label, loud->rssi);
        const int mw = fw > 30 ? 30 : fw;
        if (y + 3 < cy1) lap_ui_meter(fx, y + 3, mw, art_range(loud->rssi) > 1 ? 0 : 1.0f - art_range(loud->rssi), LAP_GREEN | LAP_BRIGHT);
    } else {
        static const char *const dots[4] = {"listening   ", "listening.  ", "listening.. ", "listening..."};
        put(fx, y + 2, fw, LAP_UI_FAINT, "%s", src_kind ? dots[now / 400u % 4u] : "no Bluetooth source");
    }
    float hi = 5;
    for (int i = 0; i < aps_n; i++) if (aps_hist[i] > hi) hi = aps_hist[i];
    if (aps_n > 1 && y + 4 < cy1) lap_ui_history(fx, y + 4, fw > HIST / 2 ? HIST / 2 : fw, 1, aps_hist, aps_n, 0, hi, LAP_CYAN | LAP_BRIGHT);
    if (alerts) {
        /* the following alert pulses: lit, then dark, twice a second */
        const bool on = (now / 300u) % 2u;
        char t2[32];
        snprintf(t2, sizeof(t2), " ! %d FOLLOWING ", alerts);
        put(fx, y + 5, (int)strlen(t2), on ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK), "%s", t2);
    } else put(fx, y + 5, fw, LAP_UI_FAINT, "%s", trackers ? "none following you" : "no tags near");
    if (hgt >= 8) put(fx, y + 6, fw, LAP_UI_FAINT, "alert after %lu min", (unsigned long)(w->follow_ms / 60000u));
}

/* ---- the list --------------------------------------------------------------------------------- */
static const char *pdu_short(uint8_t pdu)
{
    switch (pdu) {
    case 0: return "IND";
    case 1: return "DIR";
    case 2: return "SCAN";
    case 3: return "NONC";
    case 7: return "EXT";
    default: return "?";
    }
}
static void list_pane(int x, int y, int wd, int h)
{
    const bool tracker_tab = view != BHW_ALL;
    const bool wide = wd >= 64;
    row_h = tracker_tab && !wide ? 2 : 1;
    list_x = x; list_w = wd;
    list_y = y + 1;
    list_h = (h - 1) / row_h;
    const uint8_t hd = LAP_ATTR(LAP_CYAN, LAP_BLACK);
    const int sp = wd >= 100 ? 16 : !tracker_tab && wd < 52 ? 6 : !tracker_tab && wd < 60 ? 8 : 10;     /* sparkline cells */
    /* columns: flag kind name [adv flags] rssi trend signal seen [5min here rot] */
    int fixed = 1 + 7 + 1 + 4 + 1 + 1 + sp + 1 + 5;
    if (!tracker_tab) fixed += 5 + 3;
    if (tracker_tab && wide) fixed += 17;
    const int lw = wd - fixed;
    const int c_adv = x + 8 + lw + 1, c_rssi = c_adv + (tracker_tab ? 0 : 8), c_spark = c_rssi + 6, c_seen = c_spark + sp + 1,
              c_more = c_seen + 5;
    lap_fill(x, y, wd, 1, ' ', hd);
    put(x + 1, y, 6, hd, "KIND");
    put(x + 8, y, lw, hd, "NAME");
    if (!tracker_tab) put(c_adv, y, 8, hd, "ADV  FL");
    put(c_rssi, y, 4, hd, "RSSI");
    put(c_spark, y, sp, hd, "SIGNAL");
    put(c_seen, y, 5, hd, " SEEN");
    if (tracker_tab && wide) put(c_more, y, 17, hd, "  5MIN  HERE ROT");
    if (!nidx) {
        /* a tag on its key ring, sending: the waves step outwards while listening */
        static const char *const tag[3] = {"  .--.   ", " ( () )--o", "  '--'   "};
        const int ax = x + (wd - 16) / 2, ph = src_kind ? (int)(lap_millis() / 350u % 4u) : 0;
        for (int i = 0; i < 3; i++) put(ax, list_y + 1 + i, 10, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%s", tag[i]);
        for (int k = 0; k < ph; k++) put(ax + 10 + k * 2, list_y + 2, 1, LAP_ATTR(k == ph - 1 ? LAP_CYAN | LAP_BRIGHT : LAP_CYAN, LAP_BLACK), ")");
        put(x + 1, list_y + 5, wd - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), tracker_tab ? "No tracker tags heard yet." : "No devices heard yet.");
        put(x + 1, list_y + 7, wd - 2, LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s", src_kind ? "Listening. Tags can take a minute to show." : src_why);
        list_h = 0;
        return;
    }
    /* a drag moves the list by rows; a new pick from the keys brings itself into view */
    lsmax = nidx > list_h ? (nidx - list_h) * row_h : 0;
    drag_y0 = list_y; drag_y1 = list_y + list_h * row_h;
    if (dragging) top = (lscroll + row_h / 2) / row_h;
    else if (show_sel) {
        if (sel < top) top = sel;
        if (sel >= top + list_h) top = sel - list_h + 1;
        show_sel = false;
    }
    if (top > nidx - list_h) top = nidx - list_h;
    if (top < 0) top = 0;
    if (!dragging) lscroll = top * row_h;
    const uint32_t now = bhw_now();
    for (int r = 0; r < list_h && top + r < nidx; r++) {
        const bw_dev_t *d = &w->dev[idx[top + r]];
        const int yy = list_y + r * row_h;
        const bool on = top + r == sel;
        const bool fresh = now - d->last_ms < 60000u;
        const uint8_t base = on ? LAP_ATTR(LAP_BLACK, LAP_WHITE) : LAP_ATTR(fresh ? LAP_WHITE : LAP_BLUE | LAP_BRIGHT, LAP_BLACK);
        const uint8_t kind = on ? LAP_ATTR(LAP_BLACK, LAP_WHITE) : LAP_ATTR(bhw_colour(d), LAP_BLACK);
        const uint8_t dim = on ? base : LAP_ATTR(LAP_CYAN, LAP_BLACK);
        char label[32], age[12], here[12];
        bhw_label(d, label, sizeof(label));
        age_text(now - d->last_ms, age, sizeof(age));
        age_text(bw_span_ms(d), here, sizeof(here));
        lap_fill(x, yy, wd, row_h, ' ', base);
        if (d->alert)
            put(x, yy, 1, (lap_millis() / 300u) % 2u ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : on ? base : LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK), "!");
        else if (d->mark)
            put(x, yy, 1, on ? base : LAP_ATTR(d->mark == BW_MARK_MINE ? LAP_GREEN | LAP_BRIGHT : d->gone ? LAP_RED | LAP_BRIGHT : LAP_CYAN | LAP_BRIGHT, LAP_BLACK),
                d->mark == BW_MARK_MINE ? "M" : "W");
        put(x + 1, yy, 6, kind, "%s", bw_class_tag(d->ad.cls));
        put(x + 8, yy, lw, base, "%s", label);
        if (!tracker_tab) {
            put(c_adv, yy, 4, dim, "%s", pdu_short(d->pdu));
            if (d->ad.has_flags) put(c_adv + 5, yy, 2, dim, "%02x", d->ad.flags);
        }
        put(c_rssi, yy, 4, base, "%4d", d->rssi);
        const int tr = bw_trend(d, now);
        put(c_rssi + 4, yy, 1, on ? base : LAP_ATTR(tr > 0 ? LAP_GREEN | LAP_BRIGHT : LAP_RED, LAP_BLACK), tr > 0 ? "+" : tr < 0 ? "-" : " ");
        spark(c_spark, yy, sp, d, on ? base : LAP_ATTR(fresh ? LAP_GREEN | LAP_BRIGHT : LAP_BLUE, LAP_BLACK));
        put(c_seen, yy, 5, base, "%5s", age);
        if (tracker_tab && wide) {
            put(c_more, yy, 17, base, "  %4d %5s %3u", bw_window(d, now, 5), here, d->rotations);
        } else if (row_h == 2) {
            char moved[16] = "";
            if (d->has_pos0) snprintf(moved, sizeof(moved), "  moved %dm", (int)d->moved_m);
            put(x + 8, yy + 1, wd - 8, dim, "5m %d  15m %d  here %s%s", bw_window(d, now, 5), bw_window(d, now, 15), here, moved);
            char extra[32] = "";
            if (d->mark) snprintf(extra, sizeof(extra), "%s ", d->mark == BW_MARK_MINE ? "MINE" : d->gone ? "WATCH:GONE" : "WATCH");
            if (d->rotations) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "rot %u", d->rotations);
            if (d->ad.separated == 1) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "%sAWAY", extra[0] ? " " : "");
            if (d->ad.separated == 0) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "%sowner near", extra[0] ? " " : "");
            if (extra[0])
                put(x + wd - (int)strlen(extra) - 1, yy + 1, (int)strlen(extra), on ? base : LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK),
                    "%s", extra);
        }
    }
    if (nidx > list_h) {
        char more[32];
        snprintf(more, sizeof(more), "%d/%d", sel + 1, nidx);
        put(x + wd - (int)strlen(more), y, (int)strlen(more), hd, "%s", more);
    }
}

/* Words across lines of wd cells; returns the row after the last. */
static int wrap(int x, int y, int wd, int bottom, uint8_t attr, const char *t)
{
    while (*t && y < bottom && wd > 0) {
        int n = (int)strlen(t);
        if (n > wd) { n = wd; while (n > 0 && t[n] != ' ') n--; if (!n) n = wd; }
        put(x, y++, n, attr, "%.*s", n, t);
        t += n;
        while (*t == ' ') t++;
    }
    return y;
}
static int kv(int x, int y, int wd, const char *k, uint8_t va, const char *fmt, ...)
{
    char t[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    put(x, y, 10, LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s", k);
    put(x + 10, y, wd - 10, va, "%s", t);
    return y + 1;
}
static void detail_pane(int x, int y, int wd, int h, const bw_dev_t *d)
{
    const uint8_t v = LAP_ATTR(LAP_WHITE, LAP_BLACK), dim = LAP_ATTR(LAP_CYAN, LAP_BLACK);
    const int bottom = y + h;
    clip(y, bottom);
    if (!d) { put(x + 1, y + 1, wd - 2, dim, "Pick a tag in the list."); return; }
    const uint32_t now = bhw_now();
    char label[32], a[18], t1[12], t2[12], t3[12];
    bhw_label(d, label, sizeof(label));
    bw_addr_text(d->addr, a);
    put(x, y++, wd, LAP_ATTR(bhw_colour(d), LAP_BLACK), "%s%s", d->alert ? "! " : "", label);
    put(x, y++, wd, LAP_ATTR(bhw_colour(d) & 7, LAP_BLACK), "%s", bw_class_name(d->ad.cls));
    if (h >= 20) y++;
    const char *ak = bw_addr_kind(d->addr, d->addr_type);
    if (wd >= 10 + 18 + (int)strlen(ak)) y = kv(x, y, wd, "address", v, "%s %s", a, ak);
    else { y = kv(x, y, wd, "address", v, "%s", a); y = kv(x, y, wd, "", v, "%s", ak); }    /* narrow: the kind under it */
    const char *co = d->ad.company != 0xFFFF ? bw_company(d->ad.company) : NULL;
    if (!co && d->ad.company != 0xFFFF) co = bhl_get(BHID_COMPANY, d->ad.company);
    if (d->ad.company != 0xFFFF) y = kv(x, y, wd, "company", v, "%s 0x%04x", co ? co : "unlisted", d->ad.company);
    if (!(d->addr_type & 1)) {
        uint8_t be[6];
        for (int i = 0; i < 6; i++) be[i] = d->addr[5 - i];
        const char *mk = bhl_maker(be);
        y = kv(x, y, wd, "maker", v, "%s", mk ? mk : "not in the name file");
    }
    if (d->ad.appearance) {
        const char *ap = bhl_get(BHID_APPEARANCE, d->ad.appearance);
        y = kv(x, y, wd, "looks like", v, "%s 0x%04x", ap ? ap : "?", d->ad.appearance);
    }
    y = kv(x, y, wd, "advert", v, "%s", bw_pdu_name(d->pdu));
    if (d->ad.has_flags)
        y = kv(x, y, wd, "flags", v, "0x%02x%s%s%s", d->ad.flags, d->ad.flags & 1 ? " limited" : "", d->ad.flags & 2 ? " general" : "",
               d->ad.flags & 4 ? " LE-only" : "");
    if (d->ad.has_tx) y = kv(x, y, wd, "tx power", v, "%d dBm", d->ad.tx_power);
    if (d->ad.n_uuid) {
        char u[64] = "";
        for (int i = 0, p = 0; i < d->ad.n_uuid && p < 58; i++) p += snprintf(u + p, sizeof(u) - (size_t)p, "%04X ", d->ad.uuid16[i]);
        y = kv(x, y, wd, "services", v, "%s", u);
        for (int i = 0; i < d->ad.n_uuid && i < 3; i++) {
            const char *un = bhl_get(BHID_UUID, d->ad.uuid16[i]);
            if (un) y = kv(x, y, wd, "", v, "%04X %s", d->ad.uuid16[i], un);
        }
    }
    if (d->ad.level) {
        y = kv(x, y, wd, "owner", d->ad.separated == 1 ? LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK) : v, "%s",
               d->ad.separated == 1 ? "away (separated)" : d->ad.separated == 0 ? "nearby (with owner)" : "not stated");
        if (d->ad.cls == BW_C_FINDMY) y = kv(x, y, wd, "status", v, "0x%02x", d->ad.status);
    }
    age_text(now - d->first_ms, t1, sizeof(t1));
    age_text(now - d->last_ms, t2, sizeof(t2));
    age_text(bw_span_ms(d), t3, sizeof(t3));
    y = kv(x, y, wd, "seen", v, "first %s ago, last %s ago", t1, t2);
    y = kv(x, y, wd, "present", d->alert ? LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK) : v, "%s%s", t3,
           d->rotations ? " (through rotations)" : "");
    if (d->rotations) y = kv(x, y, wd, "rotations", v, "%u address changes followed", d->rotations);
    if (d->has_pos0) y = kv(x, y, wd, "moved", v, "%d m with you", (int)d->moved_m);
    else if (gps_fitted) y = kv(x, y, wd, "moved", dim, "no GPS fix yet");
    y = kv(x, y, wd, "sightings", v, "5 min %d  15 min %d  all %lu", bw_window(d, now, 5), bw_window(d, now, 15),
           (unsigned long)d->count);
    if (y < bottom) {
        const uint32_t p = bw_presence(d, now);
        put(x, y, 10, dim, "minutes");
        for (int i = 0; i < 15; i++)
            cell(x + 10 + i, y, (p >> (14 - i)) & 1 ? LAP_BLOCK : '.', LAP_ATTR((p >> (14 - i)) & 1 ? LAP_GREEN | LAP_BRIGHT : LAP_BLUE, LAP_BLACK));
        put(x + 26, y, wd - 26, dim, "15m ago..now");
        y++;
    }
    y++;
    const int gr = h >= 34 ? 5 : 3;
    if (y + gr + 2 <= bottom) {
        int cells = wd - 2;
        if (cells > BW_HIST / 2) cells = BW_HIST / 2;
        put(x, y++, wd, dim, "RSSI, last %d s", cells * 2 * BW_HIST_MS / 1000);
        graph(x + 1, y, cells, gr, d);
        y += gr;
        const int tr = bw_trend(d, now);
        put(x, y++, wd, v, "now %d  avg %d  peak %d  %s", d->rssi, (int)d->smooth, d->peak, tr > 0 ? "rising" : tr < 0 ? "falling" : "steady");
    }
    const char *nt = bw_class_note(d->ad.cls);
    if (nt[0] && y + 2 < bottom) { y++; y = wrap(x, y, wd, bottom, LAP_ATTR(LAP_YELLOW, LAP_BLACK), nt); }
    if (d->raw_len && y + 2 < bottom) {
        y++;
        put(x, y++, wd, dim, "last advert, %u bytes", d->raw_len);
        const int per = (wd - 1) / 3 > 16 ? 16 : (wd - 1) / 3;
        for (int i = 0; i < d->raw_len && y < bottom && per > 0; i += per, y++) {
            char hex[64];
            int p = 0;
            for (int j = i; j < d->raw_len && j < i + per; j++) p += snprintf(hex + p, sizeof(hex) - (size_t)p, "%02x ", d->raw[j]);
            put(x, y, wd, LAP_ATTR(LAP_GREEN, LAP_BLACK), "%s", hex);
        }
    }
}
void bhw_draw_detail(int x, int y, int wd, int h) { detail_pane(x, y, wd, h, w ? selected() : NULL); }

static void info_pane(int x, int y, int wd, int bottom)
{
    const uint8_t v = LAP_ATTR(LAP_WHITE, LAP_BLACK), dim = LAP_ATTR(LAP_CYAN, LAP_BLACK), hi = LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK);
    put(x, y++, wd, hi, "LISTENING");
    y = kv(x, y, wd, "source", v, "%s", src_kind == 1 ? "advert capture (API 2.7)" : src_kind == 2 ? "BLE scan" : "none");
    if (src_why[0] && src_kind != 1) put(x + 10, y++, wd - 10, dim, "%s", src_why);
    y = kv(x, y, wd, "adverts", v, "%lu, %d devices, %lu let go", (unsigned long)w->adverts, w->n, (unsigned long)w->evicted);
    {
        char ns[72];
        const int st = bhl_state(ns, sizeof(ns));
        y = kv(x, y, wd, "names", st == 2 ? v : dim, "%s", ns);
    }
    y = kv(x, y, wd, "GPS", v, "%s", !gps_fitted ? "none: alerts go by time" : w->fix ? "fix: alerts need you to move" :
                                        "no fix: alerts go by time");
    if (w->fix) y = kv(x, y, wd, "", dim, "%.5f %.5f", w->lat, w->lon);
    y++;
    put(x, y++, wd, hi, "FOLLOWING ALERT");
    kv(x, y, wd, "after", v, "%lu min present", (unsigned long)(w->follow_ms / 60000u));
    put(x + 28, y, 3, LAP_ATTR(LAP_BLACK, LAP_CYAN), "[-]");
    put(x + 33, y, 3, LAP_ATTR(LAP_BLACK, LAP_CYAN), "[+]");
    info_row = y++;                     /* the row the [-] [+] taps land on */
    info_x = x + 28;
    y = kv(x, y, wd, "moved", v, "%d m, when there is a fix", (int)w->follow_m);
    y = kv(x, y, wd, "possible", v, "%s   [P]", w->alert_possible ? "alert too" : "list only");
    y++;
    put(x, y++, wd, hi, "LOG");
    y = kv(x, y, wd, "card", log_on ? v : dim, "%s   [G]", !sd_fitted ? "no card" : log_on ? "on" : "off");
    if (log_on && log_path[0]) y = kv(x, y, wd, "file", v, "%s", log_path + 8);
    if (log_on) y = kv(x, y, wd, "written", v, "%lu lines, %lu bytes%s", (unsigned long)log_lines, (unsigned long)log_bytes,
                       log_drop ? ", some dropped" : "");
    if (logs_mine > 0) y = kv(x, y, wd, "earlier", v, "%d in %s", logs_mine, BHW_LOG_DIR + 8);
    if (logs_old > 0) {
        y = kv(x, y, wd, "BLE WATCH", v, "%d in %s, kept", logs_old, BHW_OLD_DIR + 8);
        if (old_newest[0]) y = kv(x, y, wd, "", dim, "newest %s", old_newest);
    }
    y++;
    put(x, y++, wd, hi, "KINDS");
    static const uint8_t shown[] = {BW_C_FINDMY, BW_C_APPLE_PP, BW_C_TILE, BW_C_SMARTTAG, BW_C_FMDN, BW_C_DULT, BW_C_CHIPOLO, BW_C_FASTPAIR};
    for (size_t i = 0; i < sizeof(shown) && y < bottom - 1; i++)
        put(x, y++, wd, i == 1 || i == 7 ? LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK) : LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%-7s %s",
            bw_class_tag(shown[i]), bw_class_name(shown[i]));
    if (y + 3 <= bottom) {
        y++;
        y = wrap(x, y, wd, bottom, dim, "Tags change address every 15 min to 24 h. A new address of the same kind that starts as the old one stops is counted as the same tag (rot).");
    }
}

/* ---- the TAGS tab ------------------------------------------------------------------------------ */
bool bhw_follow(bool listing)
{
    if (!listing) { lap_scroll_stop(&drag); dragging = false; return false; }
    dragging = lap_scroll_follow(&drag, &lscroll, lsmax, drag_y0, drag_y1);
    return dragging;
}
void bhw_draw(int x, int y, int wd, int h, bool wide)
{
    clip(y, y + h);
    drag_y0 = drag_y1 = 0;
    info_row = -1;
    list_h = 0;
    if (!w) return;
    if (view == BHW_INFO) { info_pane(x + 1, y, wd - 2, y + h); return; }
    const bw_dev_t *d = selected();
    if (!wide && h >= 30 && d) {
        /* a tall handheld: the list on top, the pick below it */
        const int per = view == BHW_TAGS ? 2 : 1;
        int lh = 2 + nidx * per;
        if (lh < 10) lh = 10;
        if (lh > h * 45 / 100) lh = h * 45 / 100;
        list_pane(x, y, wd, lh);
        clip(y, y + h);
        for (int c = 0; c < wd; c++) cell(x + c, y + lh, '-', LAP_ATTR(LAP_BLUE, LAP_BLACK));
        detail_pane(x + 1, y + 1 + lh, wd - 2, h - lh - 1, d);
    } else list_pane(x, y, wd, h);
}

static void select_row(int i)
{
    sel = i;
    sel_uid = w->dev[idx[i]].uid;
    show_sel = true;
}
int bhw_key(lap_key_t k, char ch)
{
    if (!w) return 0;
    if (view != BHW_INFO) {
        switch (k) {
        case LAP_KEY_UP: case LAP_KEY_WHEEL_UP: move_sel(-1); return 1;
        case LAP_KEY_DOWN: case LAP_KEY_WHEEL_DOWN: move_sel(1); return 1;
        case LAP_KEY_PGUP: move_sel(-(list_h > 1 ? list_h : 1)); return 1;
        case LAP_KEY_PGDN: move_sel(list_h > 1 ? list_h : 1); return 1;
        case LAP_KEY_ENTER: return selected() ? 2 : 1;
        default: break;
        }
    }
    if (k != LAP_KEY_CHAR) return 0;
    switch (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch) {
    case '+': case '=': bhw_follow_step(1); return 1;
    case '-': bhw_follow_step(-1); return 1;
    case 'p': bhw_possible_toggle(); return 1;
    case 'g': bhw_log_toggle(); return 1;
    case 'k': { const bw_dev_t *d = selected(); if (d) bhw_mark_cycle(d->uid); return 1; }
    default: return 0;
    }
}
int bhw_touch(int col, int row)
{
    if (!w) return 0;
    if (dragging) return 1;
    if (view == BHW_INFO) {
        if (info_row < 0) return 0;
        if (row == info_row && col >= info_x - 1 && col <= info_x + 3) bhw_follow_step(-1);
        else if (row == info_row && col >= info_x + 4 && col <= info_x + 8) bhw_follow_step(1);
        else if (row == info_row + 2) bhw_possible_toggle();
        else if (row == info_row + 4 || row == info_row + 5) bhw_log_toggle();
        return 1;
    }
    if (list_h && col >= list_x && col < list_x + list_w && row >= list_y && row < list_y + list_h * row_h) {
        const int i = top + (row - list_y) / row_h;
        if (i < nidx) {
            if (i == sel) return 2;                 /* a second tap on the pick hunts it */
            select_row(i);
        }
        return 1;
    }
    return 0;
}
