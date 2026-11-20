/* The page an RF trial app shows, as EXPERIMENTS draws one on the board:
 * START/STOP, RADIO, OPTIONS and NOTE keys, a frame titled with the trial's
 * name and how far along it is, what it hears, what it needs, the live
 * readout and a status line. And the radio under it: the one picked under
 * RADIO, opened through lap_rx when START is pressed and closed on STOP.
 *
 * Shared by BURST SCOPE, CARRIER LEVEL and IRIDIUM BURSTS (each includes it
 * as "../burst/trial_page.h"; see burst_det.h for why it lives here). One
 * trial a program: the picker's callback finds it through trial_cur. */
#ifndef TRIAL_PAGE_H
#define TRIAL_PAGE_H

#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include <stdio.h>
#include <string.h>

#define TRIAL_LINE  52          /* one readout line, with its terminator */
#define TRIAL_LINES 24
#define TRIAL_A(fg) LAP_ATTR((fg), LAP_BLACK)

typedef struct {
    const char *id, *name, *sub, *needs, *maturity;   /* maturity: "IDEA", "TRYING", "WORKS" */
    lap_radio_info_t radios[LAP_RADIOS_MAX];
    int nradios;
    int src;                    /* the picked radio, -1 for ANY */
    int radio, h;               /* the one open and its session, 0 for none */
    bool on;                    /* START pressed and not stopped */
    uint32_t run_ms;            /* when the session first ran, 0 before */
    lap_rx_config_t cfg;        /* handed to the SDK: app memory, not the stack */
    lap_rx_status_t st;
    lap_iq_meta_t meta;
    char why[64];               /* why the last START did not run, or "" */
    char feedback[64];
    int note_job;
    char key_text[2][16];
    lap_btn_t keys[5];
    int nkeys;
    char lines[TRIAL_LINES][TRIAL_LINE];
    char note[1400], path[64];
} trial_t;

static trial_t *trial_cur;

/* In place: a trial_t is too big for the screen task's stack. */
static inline void trial_init(trial_t *t, const char *id, const char *name, const char *sub,
                              const char *needs, const char *maturity)
{
    memset(t, 0, sizeof(*t));
    t->id = id; t->name = name; t->sub = sub; t->needs = needs; t->maturity = maturity;
    t->src = -1;
    t->radio = -1;
    trial_cur = t;
}

static inline bool trial_usable(const lap_radio_info_t *r)
{
    return r->present && !r->busy && (r->features & LAP_RXF_IQ);
}

static inline void trial_refresh(trial_t *t)
{
    if (t->nradios && lap_frame() % 25) return;
    const int n = lap_radio_list(t->radios, LAP_RADIOS_MAX);
    if (n >= 0) t->nradios = n;
}

/* The picked radio, or for ANY the LR2021 in the LoRa socket (where these
   trials began), else the first free SDR. */
static inline int trial_choose(const trial_t *t)
{
    if (t->src >= 0) return t->src < t->nradios ? t->src : -1;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < t->nradios; ++i)
            if (trial_usable(&t->radios[i]) && (t->radios[i].kind == LAP_RADIO_KIND_LR2021) == (pass == 0))
                return i;
    return -1;
}

/* The rate a radio gives that is nearest `want`, within its own range. */
static inline uint32_t trial_rate(const lap_radio_info_t *r, uint32_t want)
{
    if (!(r->features & LAP_RXF_RATE)) return r->min_rate;
    return want < r->min_rate ? r->min_rate : want > r->max_rate ? r->max_rate : want;
}

static inline void trial_close(trial_t *t)
{
    if (t->h > 0) lap_rx_close(t->h);
    t->h = 0;
    t->radio = -1;
    t->run_ms = 0;
}

/* Open the radio at hz, at the rate nearest want_rate. False, with why, when
   there is none or it refuses. */
static inline bool trial_open(trial_t *t, uint64_t hz, uint32_t want_rate)
{
    trial_close(t);
    const int i = trial_choose(t);
    if (i < 0) {
        snprintf(t->why, sizeof(t->why), t->nradios ? "No radio free for I/Q" : "Looking for radios");
        return false;
    }
    const lap_radio_info_t *r = &t->radios[i];
    if (!trial_usable(r)) {
        snprintf(t->why, sizeof(t->why), "%s: %s", r->name, r->why[0] ? r->why : "busy");
        return false;
    }
    if (hz < r->min_hz || hz > r->max_hz) {
        snprintf(t->why, sizeof(t->why), "%.4f MHz is out of the %s's reach", hz / 1e6, r->name);
        return false;
    }
    t->cfg = (lap_rx_config_t){ hz, trial_rate(r, want_rate), LAP_GAIN_AUTO, 0 };
    t->h = lap_rx_open(i, &t->cfg);
    if (t->h < 0) {
        snprintf(t->why, sizeof(t->why), "%s would not open (%d)", r->name, t->h);
        t->h = 0;
        return false;
    }
    t->radio = i;
    t->why[0] = 0;
    return true;
}

/* Each frame: true while the session runs. A session that failed is closed
   and its reason kept for the status line. */
static inline bool trial_live(trial_t *t)
{
    if (t->h <= 0) return false;
    if (lap_rx_status(t->h, &t->st) != 0) return false;
    if (t->st.state < 0) {
        snprintf(t->why, sizeof(t->why), "The radio stopped (%d)", t->st.state);
        trial_close(t);
        t->on = false;
        return false;
    }
    if (t->st.state != LAP_RX_RUNNING) return false;
    if (!t->run_ms) t->run_ms = lap_millis() ? lap_millis() : 1;
    return true;
}

/* "running 1m12s", "starting", "stopped", or why the last start failed. */
static inline void trial_state(const trial_t *t, char *out, size_t n)
{
    if (t->on && t->run_ms) {
        const uint32_t ms = lap_millis();
        const uint32_t s = ms > t->run_ms ? (ms - t->run_ms) / 1000 : 0;
        if (s < 60) snprintf(out, n, "running %lus", (unsigned long)s);
        else if (s < 3600) snprintf(out, n, "running %lum%02lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
        else snprintf(out, n, "running %luh%02lum", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
    } else if (t->on) snprintf(out, n, "starting");
    else snprintf(out, n, "%s", t->why[0] ? t->why : "stopped");
}

/* ---- RADIO ------------------------------------------------------------ */

static inline void trial_radio_picked(int index)
{
    if (trial_cur) trial_cur->src = index - 1;      /* row 0 is ANY */
}

static inline void trial_pick_radio(trial_t *t)
{
    trial_cur = t;
    t->nradios = lap_radio_list(t->radios, LAP_RADIOS_MAX);
    if (t->nradios < 0) t->nradios = 0;
    static char title[24];
    snprintf(title, sizeof(title), "%s RADIO", t->name);
    if (!lap_picker_open(title, trial_radio_picked)) return;
    lap_picker_add("ANY", "the LR2021, else the first free SDR");
    for (int i = 0; i < t->nradios; i++) {
        const lap_radio_info_t *r = &t->radios[i];
        static char detail[48];               /* app memory: the list copies it */
        if (trial_usable(r))
            snprintf(detail, sizeof(detail), "%lu-%lu MHz", (unsigned long)(r->min_hz / 1000000u),
                     (unsigned long)(r->max_hz / 1000000u));
        else
            snprintf(detail, sizeof(detail), "%s", r->why[0] ? r->why : !(r->features & LAP_RXF_IQ) ? "no I/Q" : "busy");
        lap_picker_add(r->name, detail);
    }
    lap_picker_select(t->src + 1);
}

/* ---- NOTE ------------------------------------------------------------- */

/* The readout saved as a note in NOTES' folder, the way the board's NOTE
   key saves one: "# NAME 2026-10-06 14:21", the time, then a "> RADIO" line
   with the name, how far along and the state, then the lines as shown. */
static inline void trial_note(trial_t *t, int n)
{
    lap_time_t now;
    lap_time(&now);
    char state[64];
    trial_state(t, state, sizeof(state));
    int len = snprintf(t->note, sizeof(t->note), "# %s %04d-%02d-%02d %02d:%02d\n%04d-%02d-%02dT%02d:%02d:%02d\n\n"
                       "> RADIO %s %s  %s\n", t->name, now.year, now.month, now.day, now.hour, now.minute,
                       now.year, now.month, now.day, now.hour, now.minute, now.second,
                       t->name, t->maturity, state);
    if (!n) len += snprintf(t->note + len, sizeof(t->note) - (size_t)len, "(no readout yet)\n");
    for (int i = 0; i < n && len < (int)sizeof(t->note) - TRIAL_LINE - 1; i++)
        len += snprintf(t->note + len, sizeof(t->note) - (size_t)len, "%s\n", t->lines[i]);
    snprintf(t->path, sizeof(t->path), "/sdcard/notes/%s-%04d%02d%02d-%02d%02d%02d.md", t->id,
             now.year, now.month, now.day, now.hour, now.minute, now.second);
    lap_sd_mkdir("/sdcard/notes");               /* already there is fine */
    t->note_job = lap_sd_write(t->path, 0, t->note, (uint32_t)len);
    snprintf(t->feedback, sizeof(t->feedback), t->note_job >= 0 ? "SAVING TO NOTES..." : "NOTES is busy; try again");
    if (t->note_job < 0) t->note_job = 0;
}

static inline void trial_note_progress(trial_t *t)
{
    if (!t->note_job) return;
    const int r = lap_sd_poll(t->note_job);
    if (r == LAP_SD_PENDING) return;
    t->note_job = 0;
    snprintf(t->feedback, sizeof(t->feedback), r >= 0 ? "SAVED TO NOTES" : "NOTES unavailable: no SD card");
}

/* ---- the page --------------------------------------------------------- */

/* `text` across up to `rows` lines from x, y in width w, broken at spaces.
   Returns the lines used. */
static inline int trial_wrap(int x, int y, int w, int rows, const char *text, uint8_t attr)
{
    int used = 0;
    while (*text && used < rows && w > 0) {
        int n = (int)strlen(text);
        if (n > w) {
            n = w;
            while (n > 0 && text[n] != ' ') n--;
            if (n == 0) n = w;
        }
        char line[2 * TRIAL_LINE];
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        memcpy(line, text, (size_t)n);
        line[n] = 0;
        lap_put_str(x, y + used, line, attr);
        used++;
        text += n;
        while (*text == ' ') text++;
    }
    return used;
}

/* ---- the frame and its instruments ------------------------------------ */

typedef struct { int x, y, w, h; } trial_rect_t;

/* Keys, the bezel with the trial's name and how far along it is, and the
   status line at its foot. Returns the cells inside for the trial's own
   instrument (h 0 when there is no room). */
static inline trial_rect_t trial_frame(trial_t *t)
{
    trial_note_progress(t);
    const lap_radio_info_t *r = t->src >= 0 && t->src < t->nradios ? &t->radios[t->src] : NULL;
    snprintf(t->key_text[0], sizeof(t->key_text[0]), "%s", t->on && t->run_ms ? "RUNNING" : t->on ? "WAIT" : "STOPPED");
    snprintf(t->key_text[1], sizeof(t->key_text[1]), "%s", r ? r->name : "ANY");
    t->nkeys = 0;
    t->keys[t->nkeys++] = (lap_btn_t){ t->on ? "STOP" : "START", t->key_text[0], 's', t->on, false, false };
    t->keys[t->nkeys++] = (lap_btn_t){ "RADIO", t->key_text[1], 'r', false, t->on, false };
    t->keys[t->nkeys++] = (lap_btn_t)LAP_BTN_OPTIONS;
    t->keys[t->nkeys++] = (lap_btn_t){ "NOTE", "SAVE", 'n', false, false, false };
    const int W = lap_cols(), H = lap_rows();
    const int kh = lap_ui_keys_rows(t->nkeys);
    lap_ui_keys(0, 0, W, kh, t->keys, t->nkeys, -1);

    const int by = kh, bh = H - kh - 1;
    static char title[24];      /* the board's controls take app memory, never the stack */
    snprintf(title, sizeof(title), " %s ", t->name);
    lap_ui_bezel(0, by, W, bh, title);
    /* How far along it is, on the rule at the right, as the list's badge. */
    char badge[12];
    snprintf(badge, sizeof(badge), " %s ", t->maturity);
    lap_put_str(W - 2 - (int)strlen(badge), by, badge,
                TRIAL_A(!strcmp(t->maturity, "WORKS") ? LAP_GREEN | LAP_BRIGHT
                        : !strcmp(t->maturity, "TRYING") ? LAP_YELLOW | LAP_BRIGHT : LAP_WHITE));
    const int x = 2, w = W - 4, top = by + 1, last = by + bh - 2;
    trial_rect_t in = { x, top, w, last - top - 1 };
    if (w < 4 || in.h < 2) in.h = 0;

    char state[64], status[80];
    trial_state(t, state, sizeof(state));
    const bool running = t->on && t->run_ms;
    const bool failed = !t->on && t->why[0];
    snprintf(status, sizeof(status), "%c %s", running ? '*' : failed ? '!' : '-', state);
    lap_put_str(x, last, status, running ? TRIAL_A(LAP_GREEN | LAP_BRIGHT)
                                 : failed ? TRIAL_A(LAP_YELLOW | LAP_BRIGHT) : LAP_UI_DIM);
    if (t->feedback[0]) lap_put_str(1, H - 1, t->feedback, LAP_UI_DIM);
    return in;
}

/* Keys, frame, readout (t->lines[0..n-1]) and status. Returns how many
   readout lines there was room for, which the next frame can fill. */
static inline int trial_draw(trial_t *t, int n)
{
    const trial_rect_t in = trial_frame(t);
    if (!in.h) return 0;
    int y = in.y;
    const int last = in.y + in.h + 1;
    y += trial_wrap(in.x, y, in.w, last - y > 3 ? 2 : 1, t->sub, LAP_UI_DIM);
    if (t->needs && y < last - 1) {
        char needs[160];
        snprintf(needs, sizeof(needs), "NEEDS %s", t->needs);
        y += trial_wrap(in.x, y, in.w, 2, needs, TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
    }
    if (y < last - 1) y++;
    int room = last - y - 1;
    if (room > TRIAL_LINES) room = TRIAL_LINES;
    if (room < 0) room = 0;
    const uint8_t ink = t->on && t->run_ms ? TRIAL_A(LAP_CYAN | LAP_BRIGHT) : LAP_UI_DIM;
    for (int i = 0; i < n && i < room; i++) lap_put_str(in.x, y + i, t->lines[i], ink);
    return room;
}

/* Sections flow down a column and on into the next: one column in portrait,
   two or three side by side in landscape. */
typedef struct { trial_rect_t r; int cols, col, cw, y; } trial_flow_t;

static inline void trial_flow_init(trial_flow_t *f, trial_rect_t r, int min_w)
{
    f->r = r;
    f->cols = min_w > 0 ? r.w / min_w : 1;
    if (f->cols < 1) f->cols = 1;
    if (f->cols > 3) f->cols = 3;
    f->cw = (r.w - 2 * (f->cols - 1)) / f->cols;
    f->col = 0;
    f->y = r.y;
}

/* Room for a section h rows tall, a blank row before it unless it opens a
   column. False when no column has the room left. */
static inline bool trial_flow_take(trial_flow_t *f, int h, int *x, int *y, int *w)
{
    for (;;) {
        const int gap = f->y > f->r.y ? 1 : 0;
        if (f->y + gap + h <= f->r.y + f->r.h) {
            *x = f->r.x + f->col * (f->cw + 2);
            *y = f->y + gap;
            *w = f->cw;
            f->y = *y + h;
            return true;
        }
        if (f->col + 1 >= f->cols || f->y == f->r.y) return false;
        f->col++;
        f->y = f->r.y;
    }
}

/* The rest of the column (at least min_h rows), for a list that fills it. */
static inline bool trial_flow_rest(trial_flow_t *f, int min_h, int *x, int *y, int *w, int *h)
{
    if (!trial_flow_take(f, min_h, x, y, w)) return false;
    *h = f->r.y + f->r.h - *y;
    f->y = f->r.y + f->r.h;
    return true;
}

/* A section rule: "== TITLE ======== right ==". */
static inline void trial_rule(int x, int y, int w, const char *title, const char *right, uint8_t hue)
{
    if (w < 6) return;
    lap_hline(x, y, w, '=', TRIAL_A(LAP_CYAN));
    char t[48];
    snprintf(t, sizeof(t), " %s ", title);
    lap_put_str(x + 2, y, t, TRIAL_A(hue));
    if (right && right[0]) {
        snprintf(t, sizeof(t), " %s ", right);
        const int rx = x + w - 2 - (int)strlen(t);
        if (rx > x + 4 + (int)strlen(title)) lap_put_str(rx, y, t, LAP_UI_DIM);
    }
}

/* How far over the floor, as a colour: cyan faint, green clear, yellow
   strong, red very strong. */
static inline uint8_t trial_ramp(float over_db)
{
    return TRIAL_A(over_db < 8 ? LAP_CYAN | LAP_BRIGHT : over_db < 16 ? LAP_GREEN | LAP_BRIGHT
                   : over_db < 26 ? LAP_YELLOW | LAP_BRIGHT : LAP_RED | LAP_BRIGHT);
}

/* A tick bar, "||||||......" or "======......": `fill` of `cells` lit in
   `ink` with `lit`. */
static inline void trial_ticks(int x, int y, int cells, int fill, uint8_t ink, char lit)
{
    for (int i = 0; i < cells; i++)
        lap_put_cell(x + i, y, i < fill ? lit : '.', i < fill ? ink : LAP_UI_FAINT);
}

/* History in braille, two readings a cell and four dots high a row, newest
   at the right; a column over `mark` is drawn in `hot`, the rest in `ink`.
   trace: a line joining each reading to the one before, as a scope draws;
   else columns filled from the bottom. Values under -199 are holes
   (nothing was listening). Cells without a dot are left as they are, so a
   guide drawn first shows through. */
static inline void trial_braille(int x, int y, int w, int h, const float *v, int n, float lo, float hi,
                                 float mark, uint8_t ink, uint8_t hot, bool trace)
{
    static const uint8_t COL[2][4] = { { 0x40, 0x04, 0x02, 0x01 }, { 0x80, 0x20, 0x10, 0x08 } };
    if (w < 1 || h < 1 || !(hi > lo)) return;
    const int dots = h * 4;
    int prev = -1;
    for (int c = 0; c < w; c++) {
        int a[2] = { -1, -1 }, b[2] = { -2, -2 };
        bool over = false;
        for (int s = 0; s < 2; s++) {
            const int i = n - (w - c) * 2 + s;
            if (i < 0 || v[i] < -199.0f) { prev = -1; continue; }
            float f = (v[i] - lo) / (hi - lo);
            if (!(f > 0)) f = 0;
            if (f > 1) f = 1;
            const int d = (int)(f * (float)(dots - 1) + 0.5f);
            if (!trace) { a[s] = 0; b[s] = d; }
            else if (prev < 0) { a[s] = b[s] = d; }
            else { a[s] = prev < d ? prev : d; b[s] = prev < d ? d : prev; }
            prev = d;
            if (v[i] >= mark) over = true;
        }
        for (int row = 0; row < h; row++) {
            const int base = (h - 1 - row) * 4;
            uint8_t bits = 0;
            for (int s = 0; s < 2; s++)
                for (int d = 0; d < 4; d++)
                    if (base + d >= a[s] && base + d <= b[s]) bits |= COL[s][d];
            if (bits) lap_put_cell(x + c, y + row, LAP_BRAILLE(bits), over ? hot : ink);
        }
    }
}

/* Digits in 3x5 figures (lap_big_char) with a one-cell point; returns the
   width used. */
static inline int trial_big(int x, int y, const char *s, uint8_t attr)
{
    int at = x;
    for (; *s; s++) {
        if (*s == '.') { lap_put_cell(at, y + 4, '.', attr); at += 2; continue; }
        if (*s == '-') { lap_hline(at, y + 2, 3, LAP_BLOCK, attr); at += 4; continue; }
        if (*s == ' ') { at += 2; continue; }
        if (lap_big_char(at, y, *s, attr)) at += 4;
    }
    return at - x;
}

/* The frequency as the radio glass shows it: the board's rolling figures
   when there is the width, 3x5 figures in a narrow column, else one line.
   Returns the first row under it. */
static inline int trial_freq(int x, int y, int w, int h, uint32_t hz)
{
    if (w >= 44) {
        const int below = lap_ui_frequency(x - 2, y, w + 4, h, hz, NULL);
        if (below > y) return below;
    }
    char f[16];
    snprintf(f, sizeof(f), "%lu.%04lu", (unsigned long)(hz / 1000000u), (unsigned long)(hz % 1000000u / 100u));
    if (h >= 5 && w >= 32) {
        const int bw = trial_big(x, y, f, TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
        lap_put_str(x + bw, y + 4, "MHz", TRIAL_A(LAP_CYAN | LAP_BRIGHT));
        return y + 5;
    }
    lap_put_str(x, y, f, TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
    lap_put_str(x + (int)strlen(f) + 1, y, "MHz", TRIAL_A(LAP_CYAN | LAP_BRIGHT));
    return y + 1;
}

/* Art: n lines from x, y, each in its own ink (or all in inks[0] when the
   rest are 0), clipped to w. */
static inline void trial_art(int x, int y, int w, const char *const *lines, int n, const uint8_t *inks)
{
    for (int i = 0; i < n; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%s", lines[i]);
        if ((int)strlen(line) > w && w > 0 && w < (int)sizeof(line)) line[w] = 0;
        lap_put_str(x, y + i, line, inks[i] ? inks[i] : inks[0]);
    }
}

/* The page's own keys: RADIO (while stopped) and NOTE. START/STOP and
   OPTIONS are the trial's. True when used. */
static inline bool trial_key(trial_t *t, char ch, int n)
{
    switch (ch | 0x20) {
    case 'r':
        if (t->on) snprintf(t->feedback, sizeof(t->feedback), "Stop it to change the radio");
        else trial_pick_radio(t);
        return true;
    case 'n':
        trial_note(t, n);
        return true;
    default:
        return false;
    }
}

/* Which key letter a tap pressed, or 0. */
static inline char trial_touch(const trial_t *t, int col, int row)
{
    const int i = lap_ui_key_hit(col, row);
    return i >= 0 && i < t->nkeys ? t->keys[i].key : 0;
}

#endif /* TRIAL_PAGE_H */
