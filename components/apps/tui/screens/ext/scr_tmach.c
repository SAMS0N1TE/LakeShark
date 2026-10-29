/* TIME MACHINE: the last half minute of the air, kept.

   One look answers "what is being kept, and what happened in it": two tape
   reels that turn only while samples are being stored, the tape itself as a
   braille trace of signal energy across the seconds held, a mark under it
   wherever a decoder reported something, and one wide key, WHAT WAS THAT?,
   that saves the moment you just missed. TIMELINE turns the page into
   everything every decoder and radio reported, newest first, each card in
   its kind's colour and linked to the clip it sits in.

   Palette roles, kept strict:
     chrome   dim cyan frames and rules (the house frame colour)
     tape     heat by height: blue floor, cyan, yellow, red at the top
     reels    white hubs on cyan, yellow tape between them; dim when idle
     kinds    one hue each, on the card's chip and spine and the tape mark
     words    bright white values, dim white labels and notes

   Everything drawn comes from ls_tmach snapshots taken without waiting; a
   frame the worker is busy keeps the previous one. Still when nothing
   records: the reels and tape move only as samples are stored. */
#include "../../ls_tui_screen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../ls_app.h"
#include "../../ls_braille.h"
#include "../../ls_options.h"
#include "../../ls_theme.h"
#include "../../ls_tui_ui.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_tmach.h"
#include "ls_textpack.h"

#define INK(c)  TUI_ATTR((c), TUI_BLACK)
#define CHROME  INK(TUI_CYAN)
#define VALUE   INK(TUI_WHITE | TUI_BRIGHT)
#define NOTE    LS_ATTR_DIM
#define GHOST   LS_ATTR_FAINT
#define CHIP    TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT)

#define EV_MAX  24
#define TAPE_ROWS_MAX 10

static EXT_RAM_BSS_ATTR ls_tm_tick_t s_tape[LS_TM_TICKS];
static uint32_t s_tape_n, s_tick_count = UINT32_MAX;
static EXT_RAM_BSS_ATTR ls_tm_event_t s_ev[EV_MAX];
static uint32_t s_ev_n, s_ev_total, s_ev_version = UINT32_MAX, s_ev_first = UINT32_MAX;
static EXT_RAM_BSS_ATTR ls_tm_status_t s_st;
static bool s_have_st;

typedef enum { VIEW_TAPE, VIEW_TIMELINE } view_t;
static view_t s_view;
static uint32_t s_scroll;            /* timeline: first row shown        */
static int s_sel = -1;               /* timeline: selected row, absolute */
static EXT_RAM_BSS_ATTR struct { int x0, x1, y0, y1; uint32_t row; } s_cards[EV_MAX];
static int s_cards_n;
static char s_flash[72];
static int64_t s_flash_until;
static char s_hint[96];

/* ------------------------------------------------------------ OPTIONS -- */

static const char *const KEEP_NAMES[] = { "NARROW 30k", "VOICE 60k", "WIDE 125k", "BROAD 250k" };
static const uint32_t KEEP_HZ[] = { 30000, 60000, 125000, 250000 };
static const char *const MEM_NAMES[] = { "1 MB", "2 MB", "4 MB", "8 MB", "16 MB" };
static const uint16_t MEM_KB[] = { 1024, 2048, 4096, 8192, 16384 };
static const char *const SAVE_NAMES[] = { "10 s", "20 s", "30 s", "60 s", "120 s" };
static const uint16_t SAVE_S[] = { 10, 20, 30, 60, 120 };
static const char *const SRC_NAMES[] = { "AUTO", "SDR", "LR2021" };

static int nearest_u32(const uint32_t *t, int n, uint32_t v)
{
    int best = 0;
    for (int i = 1; i < n; i++) if (llabs((long long)t[i] - v) < llabs((long long)t[best] - v)) best = i;
    return best;
}
static int nearest_u16(const uint16_t *t, int n, uint16_t v)
{
    int best = 0;
    for (int i = 1; i < n; i++) if (abs(t[i] - v) < abs(t[best] - v)) best = i;
    return best;
}

static int opt_get(const ls_opt_t *o)
{
    ls_tm_config_t c;
    ls_tm_config_get(&c);
    switch (o->arg) {
    case 0: return c.enabled;
    case 1: return nearest_u32(KEEP_HZ, 4, c.keep_hz);
    case 2: return nearest_u16(MEM_KB, 5, c.budget_kb);
    case 3: return nearest_u16(SAVE_S, 5, c.clip_s);
    case 4: return c.source;
    case 5: return c.autoclip;
    default: return 0;
    }
}
static void opt_set(const ls_opt_t *o, int v)
{
    ls_tm_config_t c;
    ls_tm_config_get(&c);
    switch (o->arg) {
    case 0: c.enabled = v; break;
    case 1: c.keep_hz = KEEP_HZ[v]; break;
    case 2: c.budget_kb = MEM_KB[v]; break;
    case 3: c.clip_s = SAVE_S[v]; break;
    case 4: c.source = (uint8_t)v; break;
    case 5: c.autoclip = v; break;
    }
    ls_tm_config_set(&c);
}

static const ls_opt_t CAPTURE[] = {
    { .label = "KEEP RATE", .kind = LS_OPT_CYCLE, .arg = 1, .names = KEEP_NAMES, .n = 4, .get = opt_get, .set = opt_set },
    { .label = "MEMORY", .kind = LS_OPT_CYCLE, .arg = 2, .names = MEM_NAMES, .n = 5, .get = opt_get, .set = opt_set },
    { .label = "SOURCE", .kind = LS_OPT_CYCLE, .arg = 4, .names = SRC_NAMES, .n = 3, .get = opt_get, .set = opt_set },
};
static const ls_opt_ctx_t CAPTURE_CTX = { .name = "CAPTURE", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(CAPTURE) };
static const ls_opt_t CLIPS[] = {
    { .label = "WHAT WAS THAT SAVES", .kind = LS_OPT_CYCLE, .arg = 3, .names = SAVE_NAMES, .n = 5, .get = opt_get, .set = opt_set },
    { .label = "CLIP EVERY EVENT", .kind = LS_OPT_TOGGLE, .arg = 5, .get = opt_get, .set = opt_set },
};
static const ls_opt_ctx_t CLIPS_CTX = { .name = "CLIPS", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(CLIPS) };
static const ls_opt_t ROOT[] = {
    { .label = "RECORDING", .kind = LS_OPT_TOGGLE, .arg = 0, .get = opt_get, .set = opt_set },
    { .label = "CAPTURE", .kind = LS_OPT_MENU, .sub = &CAPTURE_CTX },
    { .label = "CLIPS", .kind = LS_OPT_MENU, .sub = &CLIPS_CTX },
};
static const ls_opt_ctx_t OPT = { .name = "TIME MACHINE", .job = -1, .radio = LS_RSEL_NONE,
                                  .back_to_screen = true, LS_OPT_ROWS(ROOT) };

/* ------------------------------------------------------------ helpers -- */

static void str(tui_surface *sf, tui_rect clip, int x, int y, int w, const char *s, uint8_t at)
{
    if (w > 0) tui_put_str(sf, tui_rect_intersect(clip, tui_rect_make(x, y, w, 1)), x, y, s, at);
}
static void right(tui_surface *sf, tui_rect clip, int x, int w, int y, const char *s, uint8_t at)
{
    int n = (int)strlen(s);
    if (n > w) n = w;
    str(sf, clip, x + w - n, y, n, s, at);
}
static void flash(const char *s)
{
    snprintf(s_flash, sizeof(s_flash), "%s", s);
    s_flash_until = esp_timer_get_time() + 4000000;
}

static uint8_t kind_hue(uint16_t kind)
{
    switch (kind) {
    case LS_DECODED_PAGE:   return TUI_YELLOW;
    case LS_DECODED_APRS:   return TUI_GREEN;
    case LS_DECODED_AIS:    return TUI_BLUE;
    case LS_DECODED_ADSB:   return TUI_MAGENTA;
    case LS_DECODED_MESH:   return TUI_CYAN;
    case LS_DECODED_P25:    return TUI_WHITE;
    case LS_DECODED_REC:    return TUI_GREEN | TUI_BRIGHT;
    case LS_DECODED_DETECT: return TUI_RED;
    default:                return TUI_WHITE;
    }
}

static const char *radio_short(unsigned r)
{
    return r == LS_TM_RADIO_RTL ? "RTL" : r == LS_TM_RADIO_HACKRF ? "HackRF" :
           r == LS_TM_RADIO_LR2021 ? "LR2021" : r == LS_TM_RADIO_FILE ? "FILE" : "";
}

static void age_text(char *out, size_t n, int64_t age_us)
{
    const long s = age_us > 0 ? (long)(age_us / 1000000) : 0;
    if (s < 60) snprintf(out, n, "%lds ago", s);
    else if (s < 3600) snprintf(out, n, "%ldm%02lds ago", s / 60, s % 60);
    else snprintf(out, n, "%ldh%02ldm ago", s / 3600, (s / 60) % 60);
}

/* A house panel: dim cyan frame, inverse chip on the top edge, a quiet note
   on the right of it. */
static void panel(tui_surface *sf, tui_rect clip, tui_rect r, const char *chip, const char *note, uint8_t at)
{
    if (r.w < 8 || r.h < 2) return;
    tui_box(sf, tui_rect_intersect(r, clip), NULL, CHROME);
    int used = 0;
    if (chip) { str(sf, clip, r.x + 2, r.y, r.w - 4, chip, CHIP); used = (int)strlen(chip) + 2; }
    if (note && *note) {
        char t[72];
        snprintf(t, sizeof(t), " %s ", note);
        const int n = (int)strlen(t);
        if (n <= r.w - 4 - used) str(sf, clip, r.x + r.w - 2 - n, r.y, n, t, at);
    }
}

/* ----------------------------------------------------------- snapshot -- */

static void refresh(void)
{
    ls_tm_status_t st;
    if (ls_tm_status(&st)) { s_st = st; s_have_st = true; }
    if (s_have_st && s_st.tick_count != s_tick_count) {
        uint32_t tc = 0;
        const uint32_t n = ls_tm_tape(s_tape, LS_TM_TICKS, &tc);
        if (n || !tc) { s_tape_n = n; s_tick_count = tc; }
    }
    if (s_have_st && (s_st.tl_version != s_ev_version || s_ev_first != s_scroll)) {
        uint32_t total = 0;
        const uint32_t n = ls_tm_events(s_ev, s_scroll, EV_MAX, &total);
        if (n || !s_st.tl_count) {
            s_ev_n = n; s_ev_total = total;
            s_ev_version = s_st.tl_version; s_ev_first = s_scroll;
        }
    }
}

/* ------------------------------------------------------------- header -- */

/* Two reels and the tape between them. The spokes advance one step for
   every two tape ticks stored, so they turn exactly as fast as the ring
   fills, and stand still when it does not. */
static int header(tui_surface *sf, tui_rect a)
{
    const bool rec = s_have_st && s_st.recording;
    const char spoke = "|/-\\"[(s_tick_count / 2) & 3];
    const uint8_t reel = rec ? CHROME : GHOST, hub = rec ? VALUE : NOTE;
    const uint8_t tape = rec ? INK(TUI_YELLOW) : GHOST;
    const int x = a.x + 1, y = a.y;
    /*  .-.   .-.
       ( | )=( | )
        `-'===`-'   */
    str(sf, a, x, y, 11, " .-.   .-.", reel);
    str(sf, a, x, y + 1, 11, "(   )=(   )", reel);
    tui_put_char(sf, a, x + 2, y + 1, spoke, hub);
    tui_put_char(sf, a, x + 8, y + 1, spoke, hub);
    tui_put_char(sf, a, x + 5, y + 1, '=', tape);
    str(sf, a, x, y + 2, 11, " `-'   `-'", reel);
    str(sf, a, x + 4, y + 2, 3, "===", tape);
    const int tx = x + 13, tw = a.x + a.w - tx - 1;
    char t[96];
    str(sf, a, tx, y, tw, "TIME MACHINE", VALUE);
    /* The lamp: lit while samples are being kept. */
    right(sf, a, tx, tw, y, rec ? "* REC" : s_have_st && s_st.enabled ? "o IDLE" : "o OFF",
          rec ? INK(TUI_RED | TUI_BRIGHT) : NOTE);
    const ls_tm_seg_t *g = &s_st.seg;
    if (s_have_st && g->id) {
        snprintf(t, sizeof(t), "%s  %.4f MHz", ls_tm_radio_name(g->src.radio), g->src.center_hz / 1e6);
        str(sf, a, tx, y + 1, tw, t, rec ? VALUE : NOTE);
        snprintf(t, sizeof(t), "%lu kS/s %s", (unsigned long)(g->rate_hz / 1000), ls_tm_fmt_name(g->fmt));
        if ((int)strlen(t) + 22 < tw) right(sf, a, tx, tw, y + 1, t, NOTE);
    } else {
        str(sf, a, tx, y + 1, tw, LS_TEXT(t_scr_tmach_no_receiver_kept_yet, "no receiver kept yet"), NOTE);
    }
    if (s_have_st && s_st.why[0]) {
        str(sf, a, tx, y + 2, tw, s_st.why, NOTE);
    } else if (s_have_st) {
        LS_SNPRINTF(t_scr_tmach_holding_1f_s_of, t, sizeof(t), "holding %.1f s of %.1f s", s_st.held_s, s_st.capacity_s);
        str(sf, a, tx, y + 2, tw, t, VALUE);
        if (s_st.gaps) {
            snprintf(t, sizeof(t), "%lu gaps", (unsigned long)s_st.gaps);
            if ((int)strlen(t) + 28 < tw) right(sf, a, tx, tw, y + 2, t, NOTE);
        }
    }
    return 3;
}

/* --------------------------------------------------------------- tape -- */

static uint8_t heat(int row, int rows)
{
    /* Top quarter red, then yellow, cyan, blue at the floor. */
    const int z = rows > 1 ? (rows - 1 - row) * 4 / rows : 0;
    static const uint8_t hue[4] = { TUI_BLUE | TUI_BRIGHT, TUI_CYAN, TUI_YELLOW, TUI_RED | TUI_BRIGHT };
    if (ls_tui_daylight()) {
        static const uint8_t day[4] = { TUI_BLUE, TUI_CYAN, TUI_YELLOW, TUI_RED };
        return INK(day[z]);
    }
    return INK(hue[z]);
}

/* Seconds shown: what the ring holds at this rate; 30 before it knows. */
static int span_ticks(void)
{
    double s = s_have_st && s_st.capacity_s > 1 ? s_st.capacity_s : 30;
    int secs = (int)lround(s);
    if (secs < 10) secs = 10;
    if (secs > (int)(LS_TM_TICKS / 10)) secs = LS_TM_TICKS / 10;
    return secs * 10;
}

/* The tick index (into s_tape) at or after t_us, or -1. */
static int tick_at(int64_t t_us)
{
    int lo = 0, hi = (int)s_tape_n - 1, at = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (s_tape[mid].end_us >= t_us) { at = mid; hi = mid - 1; } else lo = mid + 1;
    }
    return at;
}

static int draw_tape(tui_surface *sf, tui_rect a, int y, int rows)
{
    const int h = rows + 4;                 /* frame, graph, marks, axis, frame */
    tui_rect box = tui_rect_make(a.x, y, a.w, h);
    const int span = span_ticks();
    char note[48];
    snprintf(note, sizeof(note), "last %d s", span / 10);
    panel(sf, a, box, " TAPE ", note, NOTE);
    const int sx = box.x + 6, gw = box.w - 8, gy = box.y + 1;      /* scale 5 wide, then the graph */
    if (gw < 8) return h;
    const int dots = gw * 2, H = rows * 4;
    /* Visible ticks: the newest `span`, right-aligned at now. */
    const int first = (int)s_tape_n - span;          /* may be negative: empty on the left */
    /* Floors: the quieter fifth of what is shown, mean and peak apiece, so
       the trace sits on the first and only a real spike clears the second. */
    static EXT_RAM_BSS_ATTR uint16_t hist[256], phist[256];
    memset(hist, 0, sizeof(hist)); memset(phist, 0, sizeof(phist));
    int have = 0;
    for (int k = first < 0 ? 0 : first; k < (int)s_tape_n; k++)
        if (s_tape[k].flags & LS_TM_TICK_DATA) { hist[s_tape[k].level]++; phist[s_tape[k].peak]++; have++; }
    int floor_lv = 0, floor_pk = 0;
    for (int acc = 0, v = 0; v < 256 && have; v++) { acc += hist[v]; if (acc * 5 >= have) { floor_lv = v; break; } }
    for (int acc = 0, v = 0; v < 256 && have; v++) { acc += phist[v]; if (acc * 2 >= have) { floor_pk = v; break; } }
    const int range = 80;                            /* 40 dB above the floor fills it */
    static EXT_RAM_BSS_ATTR int col_h[256], col_pk[256];
    static EXT_RAM_BSS_ATTR uint8_t col_flags[256];
    static EXT_RAM_BSS_ATTR bool col_has[256];
    for (int c = 0; c < dots && c < 256; c++) {
        const int k0 = first + c * span / dots, k1 = first + (c + 1) * span / dots;
        int lv = -1, pk = -1;
        col_flags[c] = 0;
        for (int k = k0; k < k1 || k == k0; k++) {
            if (k < 0 || k >= (int)s_tape_n) continue;
            if (k > 0) col_flags[c] |= s_tape[k].flags;
            if (!(s_tape[k].flags & LS_TM_TICK_DATA)) continue;
            if (s_tape[k].level > lv) lv = s_tape[k].level;
            if (s_tape[k].peak > pk) pk = s_tape[k].peak;
        }
        col_has[c] = lv >= 0;
        col_h[c] = lv < 0 ? 0 : 1 + (lv - floor_lv) * (H - 1) / range;
        /* A spike shorter than a tick: its peak, where it clears the usual
           peaks by 6 dB. */
        col_pk[c] = pk < floor_pk + 12 ? 0 : 1 + (pk - floor_lv) * (H - 1) / range;
        if (col_h[c] < 1 && col_has[c]) col_h[c] = 1;
        if (col_h[c] > H) col_h[c] = H;
        if (col_pk[c] > H) col_pk[c] = H;
    }
    /* Scale: dB above the floor. */
    for (int r = 0; r < rows; r++) {
        const int above = (rows - r) * range / 2 / rows;
        if (r == 0 || r == rows - 1 || r == rows / 2) {
            char s[8];
            snprintf(s, sizeof(s), "%+3d", r == rows - 1 ? 0 : above);
            str(sf, a, box.x + 1, gy + r, 4, s, NOTE);
        }
        tui_put_char(sf, a, box.x + 5, gy + r, '|', CHROME);
    }
    for (int r = 0; r < rows; r++) {
        const int base = (rows - 1 - r) * 4;
        const uint8_t at = heat(r, rows);
        for (int cx = 0; cx < gw; cx++) {
            uint8_t bits = 0;
            for (int half = 0; half < 2; half++) {
                const int c = cx * 2 + half;
                if (c >= 256 || !col_has[c]) continue;
                int fill = col_h[c] - base;
                if (fill > 4) fill = 4;
                if (fill > 0) bits |= ls_braille_column((unsigned)fill, half);
                const int p = col_pk[c] - 1 - base;
                if (col_pk[c] > col_h[c] && p >= 0 && p < 4) bits |= ls_braille_dot((unsigned)p, half);
            }
            if (bits) ls_braille_put(sf, a, sx + cx, gy + r, (int16_t)(0x2800 | bits), at);
        }
    }
    /* Marks: tunes, gaps and what the decoders said, under the moment. */
    const int my = gy + rows, ay = my + 1;
    /* What the decoders said on this receiver, first, so nothing covers it.
       Rows from another radio happened somewhere else and are not marked. */
    static EXT_RAM_BSS_ATTR uint16_t mark[256];
    memset(mark, 0, sizeof(mark));
    if (s_tape_n && !s_scroll) {
        const int64_t t0 = first >= 0 ? s_tape[first].end_us : s_tape[0].end_us;
        for (int i = (int)s_ev_n - 1; i >= 0; i--) {
            const ls_tm_event_t *e = &s_ev[i];
            if (e->at_us < t0 || (e->radio && s_st.seg.id && e->radio != s_st.seg.src.radio)) continue;
            const int k = tick_at(e->at_us);
            const int tk = k < 0 ? (int)s_tape_n - 1 : k;
            const int c = (int)((long long)(tk - first) * dots / span) / 2;
            if (c >= 0 && c < gw && c < 256) mark[c] = e->kind;
        }
    }
    for (int c = 0; c < dots && c < 256; c++) {
        const uint8_t f = col_flags[c];
        const int cx = c / 2;
        if (mark[cx]) continue;
        if (f & LS_TM_TICK_SEG) {
            /* A retune: the rail, and what it went to when there is room. */
            tui_put_char(sf, a, sx + cx, my, '|', INK(TUI_WHITE | TUI_BRIGHT));
            const int k = first + c * span / dots + span / dots;
            const ls_tm_seg_t *g = &s_st.seg;
            if (k < 0 || k >= (int)s_tape_n - 2 || !g->id || (uint8_t)g->id != s_tape[k].seg) continue;
            char f2[16];
            snprintf(f2, sizeof(f2), "%.2f", g->src.center_hz / 1e6);
            const int n = (int)strlen(f2);
            bool clear = cx + 1 + n < gw;
            for (int j = 1; j <= n + 1 && clear; j++) clear = !mark[cx + j];
            if (clear) str(sf, a, sx + cx + 1, my, n, f2, NOTE);
        } else if (f & (LS_TM_TICK_GAP | LS_TM_TICK_HELD)) tui_put_char(sf, a, sx + cx, my, '~', NOTE);
    }
    for (int cx = 0; cx < gw && cx < 256; cx++)
        if (mark[cx]) tui_put_char(sf, a, sx + cx, my, ls_tm_kind_letter(mark[cx]), INK(kind_hue(mark[cx]) | TUI_BRIGHT));
    /* Axis: a tick every ten seconds, now at the right edge. */
    for (int cx = 0; cx < gw; cx++) tui_put_char(sf, a, sx + cx, ay, '-', CHROME);
    for (int s = 10; s < span / 10; s += 10) {
        const int cx = gw - 1 - (int)((long long)s * 10 * gw / span);
        if (cx < 0) break;
        char l[8];
        snprintf(l, sizeof(l), "+-%ds", s);
        tui_put_char(sf, a, sx + cx, ay, '+', CHROME);
        if (cx + (int)strlen(l) < gw - 4) str(sf, a, sx + cx + 1, ay, (int)strlen(l) - 1, l + 1, NOTE);
    }
    str(sf, a, sx + gw - 3, ay, 3, "now", VALUE);
    /* While a clip is written out: its window, filling as it goes. */
    if (s_have_st && s_st.saving && s_tape_n) {
        const int k0 = tick_at(s_st.save_from_us), k1 = tick_at(s_st.save_to_us);
        int c0 = (int)((long long)((k0 < 0 ? (int)s_tape_n : k0) - first) * gw / span);
        int c1 = (int)((long long)((k1 < 0 ? (int)s_tape_n - 1 : k1) - first) * gw / span);
        if (c0 < 0) c0 = 0;
        if (c1 >= gw) c1 = gw - 1;
        const int w = c1 - c0 + 1, done = w * s_st.save_pct / 100;
        for (int i = 0; i < w; i++)
            tui_put_char(sf, a, sx + c0 + i, ay, i < done ? '=' : ':', INK(TUI_YELLOW | (i < done ? TUI_BRIGHT : 0)));
    }
    if (!s_tape_n) {
        str(sf, a, sx + 2, gy + rows / 2 - 1, gw - 4, "Nothing kept yet.", VALUE);
        str(sf, a, sx + 2, gy + rows / 2, gw - 4, LS_TEXT(t_scr_tmach_start_fm_p25_sub, "Start FM, P25, SUB-GHZ or FALLS:"), NOTE);
        str(sf, a, sx + 2, gy + rows / 2 + 1, gw - 4, LS_TEXT(t_scr_tmach_whatever_they_hear_is, "whatever they hear is kept here."), NOTE);
    }
    return h;
}

/* -------------------------------------------------------------- cards -- */

/* One event: the kind's chip and spine, when, where, what. */
static int card(tui_surface *sf, tui_rect a, int y, const ls_tm_event_t *e, bool sel, int64_t now)
{
    const uint8_t hue = kind_hue(e->kind);
    const uint8_t chip = TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT), spine = INK(hue | TUI_BRIGHT);
    const int x = a.x + 1, w = a.w - 2;
    char t[128], age[24];
    tui_put_char(sf, a, x, y, sel ? '>' : ' ', VALUE);
    char k[4] = { ' ', ls_tm_kind_letter(e->kind), ' ', 0 };
    str(sf, a, x + 1, y, 3, k, chip);
    str(sf, a, x + 5, y, 8, ls_tm_kind_name(e->kind), spine);
    age_text(age, sizeof(age), now - e->at_us);
    int rx = x + 13;
    if (e->freq_hz) {
        snprintf(t, sizeof(t), "%.4f MHz", e->freq_hz / 1e6);
        str(sf, a, rx, y, w - 13, t, VALUE);
        rx += (int)strlen(t) + 2;
    }
    if (e->radio && rx + 8 < x + w - (int)strlen(age) - 1) str(sf, a, rx, y, 7, radio_short(e->radio), NOTE);
    right(sf, a, x, w, y, age, NOTE);
    tui_put_char(sf, a, x + 2, y + 1, '|', spine);
    char who[40] = "";
    if (e->from[0]) snprintf(who, sizeof(who), "%s", e->from);
    else if (e->address) snprintf(who, sizeof(who), "%lu", (unsigned long)e->address);
    /* Heard more than once: how often, beside the name. */
    if (e->count > 1) {
        const size_t wl0 = strlen(who);
        snprintf(who + wl0, sizeof(who) - wl0, "%sx%u", wl0 ? " " : "", e->count);
    }
    const int tx = x + 5;
    char clip[24] = "";
    if (e->clip) snprintf(clip, sizeof(clip), "[> tm%04lu +%.1fs]", (unsigned long)e->clip, e->clip_ms / 1000.0);
    const int room = x + w - tx - (clip[0] ? (int)strlen(clip) + 1 : 0);
    const int wl = who[0] ? (int)strlen(who) + 2 : 0;
    if (who[0]) str(sf, a, tx, y + 1, room, who, VALUE);
    if (e->text[0] && wl < room) str(sf, a, tx + wl, y + 1, room - wl, e->text, NOTE);
    if (clip[0]) right(sf, a, x, w, y + 1, clip, INK(TUI_YELLOW | TUI_BRIGHT));
    return 2;
}

static int draw_clip_card(tui_surface *sf, tui_rect a, int y)
{
    if (!s_have_st || (!s_st.last_clip && !s_st.saving && !s_st.replaying)) return 0;
    tui_rect box = tui_rect_make(a.x, y, a.w, 4);
    char chip[20], t[96];
    const uint32_t no = s_st.saving ? s_st.saving_no : s_st.replaying ? s_st.replay_no : s_st.last_clip;
    snprintf(chip, sizeof(chip), " tm%04lu ", (unsigned long)no);
    panel(sf, a, box, chip, s_st.saving ? "saving" : s_st.replaying ? "replaying" : "last clip", NOTE);
    const int x = box.x + 2, w = box.w - 4;
    if (s_st.saving || s_st.replaying) {
        const int pct = s_st.saving ? s_st.save_pct : s_st.replay_pct;
        const int bw = w - 6;
        for (int i = 0; i < bw; i++)
            tui_put_char(sf, a, x + i, y + 1, i < bw * pct / 100 ? '=' : '-', i < bw * pct / 100 ? INK(TUI_YELLOW | TUI_BRIGHT) : GHOST);
        snprintf(t, sizeof(t), "%3d%%", pct);
        right(sf, a, x, w, y + 1, t, VALUE);
    } else {
        LS_SNPRINTF(t_scr_tmach_1f_s_kept_on, t, sizeof(t), "%.1f s kept on the card", s_st.last_clip_s);
        str(sf, a, x, y + 1, w, t, VALUE);
        right(sf, a, x, w, y + 1, "P plays it back", NOTE);
    }
    str(sf, a, x, y + 2, w, s_st.note[0] ? s_st.note : "", NOTE);
    return 4;
}

static int draw_cards(tui_surface *sf, tui_rect a, int y, int bottom, bool full)
{
    const int64_t now = esp_timer_get_time();
    s_cards_n = 0;
    if (!s_ev_n) {
        if (full) {
            str(sf, a, a.x + 2, y + 1, a.w - 4, "No events yet.", VALUE);
            str(sf, a, a.x + 2, y + 2, a.w - 4, LS_TEXT(t_scr_tmach_pages_aprs_ais_aircraft, "Pages, APRS, AIS, aircraft, P25 calls, SUB-GHZ"), NOTE);
            str(sf, a, a.x + 2, y + 3, a.w - 4, LS_TEXT(t_scr_tmach_catches_and_detector_alerts, "catches and DETECTOR alerts all land here."), NOTE);
        }
        return y;
    }
    char t[48];
    snprintf(t, sizeof(t), full ? " %lu events " : " RECENT ", (unsigned long)s_ev_total);
    for (int i = a.x + 1; i < a.x + a.w - 1; i++) tui_put_char(sf, a, i, y, '-', CHROME);
    str(sf, a, a.x + 3, y, (int)strlen(t), t, VALUE);
    if (full && s_ev_total > 1) {
        snprintf(t, sizeof(t), " %lu-%lu ", (unsigned long)s_scroll + 1,
                 (unsigned long)(s_scroll + s_ev_n < s_ev_total ? s_scroll + s_ev_n : s_ev_total));
        right(sf, a, a.x + 1, a.w - 4, y, t, NOTE);
    }
    y++;
    /* Wide glass: two columns of cards, read down the left one first. */
    const int ncol = full && a.w >= 96 ? 2 : 1, colw = a.w / ncol;
    uint32_t i = 0;
    int end = y;
    for (int col = 0; col < ncol; col++) {
        const tui_rect ca = tui_rect_make(a.x + col * colw, y, colw - (col + 1 < ncol ? 1 : 0), bottom - y + 1);
        int cy = y;
        for (; i < s_ev_n && cy + 2 <= bottom; i++) {
            const uint32_t row = s_scroll + i;
            const int h = card(sf, ca, cy, &s_ev[i], (int)row == s_sel, now);
            if (s_cards_n < EV_MAX) {
                s_cards[s_cards_n].x0 = ca.x; s_cards[s_cards_n].x1 = ca.x + ca.w;
                s_cards[s_cards_n].y0 = cy; s_cards[s_cards_n].y1 = cy + h; s_cards[s_cards_n].row = row; s_cards_n++;
            }
            cy += h;
            /* The rail on to the next one back, and how long nothing happened. */
            if (full && i + 1 < s_ev_n && cy < bottom) {
                tui_put_char(sf, ca, ca.x + 3, cy, ':', CHROME);
                const int64_t quiet = s_ev[i].at_us - s_ev[i + 1].at_us;
                if (quiet >= 60000000) {
                    char q[24];
                    if (quiet >= 3600000000LL) snprintf(q, sizeof(q), "%lldh quiet", (long long)(quiet / 3600000000LL));
                    else snprintf(q, sizeof(q), "%lldm quiet", (long long)(quiet / 60000000));
                    str(sf, ca, ca.x + 6, cy, (int)strlen(q), q, GHOST);
                }
                cy++;
            }
        }
        if (cy > end) end = cy;
    }
    return end;
}

/* ---------------------------------------------------------------- draw -- */

static ls_btn_t big_key(void)
{
    static char value[48];
    const char *label = "WHAT WAS THAT?";
    bool dim = false;
    ls_tm_config_t c;
    ls_tm_config_get(&c);
    if (!s_have_st || !s_st.enabled) { snprintf(value, sizeof(value), "TIME MACHINE is off"); dim = true; }
    else if (s_st.saving) LS_SNPRINTF(t_scr_tmach_saving_tm_04lu_u, value, sizeof(value), "saving tm%04lu  %u%%", (unsigned long)s_st.saving_no, s_st.save_pct);
    else if (s_st.held_s < 0.5) { snprintf(value, sizeof(value), "nothing kept yet"); dim = true; }
    else LS_SNPRINTF(t_scr_tmach_save_the_last_0f, value, sizeof(value), "save the last %.0f s", s_st.held_s < c.clip_s ? s_st.held_s : (double)c.clip_s);
    return (ls_btn_t){ label, value, 'w', s_st.saving, dim, false };
}

static int bar_buttons(ls_btn_t *b)
{
    static char replay[24];
    if (s_have_st && s_st.last_clip) snprintf(replay, sizeof(replay), "tm%04lu", (unsigned long)s_st.last_clip);
    else snprintf(replay, sizeof(replay), "no clip");
    int n = 0;
    b[n++] = (ls_btn_t){ "VIEW", s_view == VIEW_TAPE ? "TAPE" : "TIMELINE", 'v', false, false, false };
    b[n++] = (ls_btn_t){ "REPLAY", replay, 'p', s_have_st && s_st.replaying, !(s_have_st && s_st.last_clip), false };
    if (s_view == VIEW_TIMELINE) {
        b[n++] = (ls_btn_t){ "OLDER", "", 'k', false, s_scroll + s_ev_n >= s_ev_total, false };
        b[n++] = (ls_btn_t){ "NEWER", "", 'j', false, s_scroll == 0, false };
    }
    b[n++] = ls_opt_button(&OPT);
    return n;
}

static void draw(tui_surface *sf, tui_rect a)
{
    refresh();
    snprintf(s_hint, sizeof(s_hint), s_view == VIEW_TIMELINE ?
             "W what was that  V tape  P replay  J/K newer/older  Enter play card  O options" :
             "W what was that  V timeline  P replay  O options");
    if (a.w < 30 || a.h < 18) { ls_panel_notice(sf, a, "TIME MACHINE", LS_TEXT(t_scr_tmach_more_screen_space_needed, "More screen space needed"), "Use a smaller font"); return; }
    const bool wide = ls_tui_is_wide();
    /* The bar at the bottom and the big key by the thumb: over the bar in
       portrait, beside it in landscape, where rows are what is short. */
    ls_btn_t b[6];
    const int nb = bar_buttons(b);
    const ls_btn_t key = big_key();
    int bottom;
    if (wide) {
        const int kw = a.w * 2 / 5;
        const int bh = ls_btn_raised_height(tui_rect_make(a.x, a.y, a.w - kw, a.h), nb);
        ls_btn_bar_raised_slot(sf, tui_rect_make(a.x, a.y + a.h - bh, a.w - kw, bh), b, nb, -1, LS_BTN_SLOT_SCREEN);
        ls_btn_bar_transport_slot(sf, tui_rect_make(a.x + a.w - kw, a.y + a.h - bh, kw, bh), &key, 1, -1, LS_BTN_SLOT_WATERFALL);
        bottom = a.y + a.h - bh - 1;
    } else {
        const int bh = ls_btn_raised_height(a, nb), kh = 5;
        ls_btn_bar_raised_slot(sf, tui_rect_make(a.x, a.y + a.h - bh, a.w, bh), b, nb, -1, LS_BTN_SLOT_SCREEN);
        ls_btn_bar_transport_slot(sf, tui_rect_make(a.x, a.y + a.h - bh - kh, a.w, kh), &key, 1, -1, LS_BTN_SLOT_WATERFALL);
        bottom = a.y + a.h - bh - kh - 1;
    }
    header(sf, a);
    int y = a.y + 4;
    const int64_t now = esp_timer_get_time();
    if (s_flash[0] && now < s_flash_until) {
        str(sf, a, a.x + 1, bottom, a.w - 2, s_flash, VALUE);
        bottom--;
    }
    const bool clip = s_have_st && (s_st.last_clip || s_st.saving || s_st.replaying);
    if (s_view == VIEW_TAPE) {
        const int avail = bottom - y + 1;
        int rows = wide ? avail - 4 - (clip ? 5 : 0) : avail / 3;
        if (rows > (wide ? TAPE_ROWS_MAX : 14)) rows = wide ? TAPE_ROWS_MAX : 14;
        if (rows < 3) rows = 3;
        y += draw_tape(sf, a, y, rows) + 1;
        if (y + 4 <= bottom + 1) y += draw_clip_card(sf, a, y) + 1;
        if (y + 3 <= bottom) draw_cards(sf, a, y, bottom, false);
    } else {
        y += draw_tape(sf, a, y, wide ? 2 : 4) + 1;
        if ((s_st.saving || s_st.replaying) && y + 4 <= bottom) y += draw_clip_card(sf, a, y);
        draw_cards(sf, a, y, bottom, true);
    }
}

/* ------------------------------------------------------------ actions -- */

static void what_was_that(void)
{
    if (!s_have_st || !s_st.enabled) { flash(LS_TEXT(t_scr_tmach_off_options_recording_turns, "Off: OPTIONS > RECORDING turns it on")); return; }
    if (s_st.saving) { flash(LS_TEXT(t_scr_tmach_still_saving_the_last, "Still saving the last one")); return; }
    if (!ls_tm_what_was_that()) { flash(LS_TEXT(t_scr_tmach_nothing_kept_yet_start, "Nothing kept yet: start a receiver first")); return; }
    flash(LS_TEXT(t_scr_tmach_kept_writing_the_last, "Kept: writing the last seconds to the card"));
}

static void replay(uint32_t no)
{
    if (!no && !(s_have_st && s_st.last_clip)) { flash(LS_TEXT(t_scr_tmach_no_clip_yet_what, "No clip yet: WHAT WAS THAT? saves one")); return; }
    if (!ls_tm_replay(no)) { flash(LS_TEXT(t_scr_tmach_a_replay_is_already, "A replay is already running")); return; }
    flash(LS_TEXT(t_scr_tmach_replaying_pages_land_on, "Replaying: pages land on the timeline"));
}

static void scroll(int d)
{
    if (d > 0 && s_scroll + s_ev_n < s_ev_total) s_scroll += EV_MAX / 2 < s_ev_n ? EV_MAX / 2 : (s_ev_n ? s_ev_n : 1);
    else if (d < 0) s_scroll = s_scroll > EV_MAX / 2 ? s_scroll - EV_MAX / 2 : 0;
}

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_CHAR && ls_opt_key(&OPT, ch)) return true;
    if (k == LS_TK_CHAR) {
        switch (ch | 0x20) {
        case 'w': what_was_that(); return true;
        case 'v': s_view = s_view == VIEW_TAPE ? VIEW_TIMELINE : VIEW_TAPE; return true;
        case 'p': {
            uint32_t no = 0;
            if (s_view == VIEW_TIMELINE && s_sel >= (int)s_scroll && s_sel < (int)(s_scroll + s_ev_n))
                no = s_ev[s_sel - (int)s_scroll].clip;
            replay(no);
            return true;
        }
        case 'k': if (s_view == VIEW_TIMELINE) { scroll(1); return true; } break;
        case 'j': if (s_view == VIEW_TIMELINE) { scroll(-1); return true; } break;
        }
    }
    if (s_view == VIEW_TIMELINE && (k == LS_TK_UP || k == LS_TK_DOWN)) {
        const int last = (int)s_ev_total - 1;
        s_sel = s_sel < 0 ? (int)s_scroll : s_sel + (k == LS_TK_DOWN ? 1 : -1);
        if (s_sel < 0) s_sel = 0;
        if (s_sel > last) s_sel = last;
        if (s_sel < (int)s_scroll) s_scroll = (uint32_t)s_sel;
        if (s_sel >= (int)(s_scroll + s_cards_n) && s_cards_n) s_scroll = (uint32_t)(s_sel - s_cards_n + 1);
        return true;
    }
    if (s_view == VIEW_TIMELINE && k == LS_TK_ENTER && s_sel >= (int)s_scroll && s_sel < (int)(s_scroll + s_ev_n)) {
        const ls_tm_event_t *e = &s_ev[s_sel - (int)s_scroll];
        if (e->clip) replay(e->clip); else flash(LS_TEXT(t_scr_tmach_no_clip_holds_this, "No clip holds this one"));
        return true;
    }
    return false;
}

static bool touch(int col, int row)
{
    /* The big key, lit or not: a dim one still says why it is dim. */
    int kx, ky, kw, kh;
    if (ls_btn_hit_slot(col, row, LS_BTN_SLOT_WATERFALL) == 0 ||
        (ls_btn_rect_slot(LS_BTN_SLOT_WATERFALL, 0, &kx, &ky, &kw, &kh) &&
         col >= kx && col < kx + kw && row >= ky && row < ky + kh)) { what_was_that(); return true; }
    const int i = ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN);
    if (i >= 0) {
        ls_btn_t b[6];
        const int n = bar_buttons(b);
        if (i == n - 1) { ls_opt_open(&OPT); return true; }
        return i < n && key(LS_TK_CHAR, b[i].key);
    }
    for (int c = 0; c < s_cards_n; c++) {
        if (row < s_cards[c].y0 || row >= s_cards[c].y1 || col < s_cards[c].x0 || col >= s_cards[c].x1) continue;
        if (s_view == VIEW_TAPE) { s_view = VIEW_TIMELINE; s_scroll = 0; s_sel = (int)s_cards[c].row; return true; }
        if (s_sel == (int)s_cards[c].row) return key(LS_TK_ENTER, 0);
        s_sel = (int)s_cards[c].row;
        return true;
    }
    return false;
}

static void enter(void) { s_tick_count = UINT32_MAX; s_ev_version = UINT32_MAX; }

const ls_tui_screen_t ls_scr_tmach = {
    .name = "TIME MACHINE",
    .hint = s_hint,
    .enter = enter,
    .draw = draw,
    .key = key,
    .touch = touch,
};
