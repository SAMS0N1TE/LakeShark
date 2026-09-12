/* FM screen: VFO on one page, decoded pages on another.

   `FM` is a plain global like `P25`, and `FM.scan_db[256]` is a bare
   float array the sweep writes into - so the spectrum needs no widget, no
   canvas and no allocation, only a loop that maps bins to columns. */
#include "../../ls_tui_screen.h"
#include "../../ls_text.h"

#include <stdio.h>
#include <string.h>

#include "apps/fm/fm_state.h"
#include "apps/fm/fm_mode_label.h"
#include "audio/audio_out.h"

#include "../../ls_quick.h"
#include "../../ls_tui_ui.h"
#include "../../ls_waterfall.h"
#include "../../ls_wf_source.h"

static int s_page;          /* 0 vfo, 1 pages, 2 sweep */

/* The same free-running blink MESH and REC already use (scr_mesh.c
   s_blink, scr_rec.c s_blink): incremented once a draw, read back as
   `(s_blink / 12) & 1`. CARRIER read "OPEN" in plain green the instant
   squelch broke and sat there in exactly the same green for as long as it
   stayed open - a burst and a held carrier looked identical at a glance,
   which is the one distinction a squelch light exists to make. */
static uint32_t s_blink;

/* What a thumb can reach without leaving the screen. */

static const fm_mode_t FM_MODES[] = {
    FM_MODE_LISTEN, FM_MODE_WFM, FM_MODE_POCSAG,
    FM_MODE_FLEX, FM_MODE_ACARS, FM_MODE_SCAN, FM_MODE_AM,
};
#define N_MODES ((int)(sizeof(FM_MODES) / sizeof(FM_MODES[0])))
static tui_rect s_mode_hit[N_MODES];
static int s_last_mode = -1;

static int mode_page(fm_mode_t mode)
{
    if (mode == FM_MODE_SCAN) return 2;
    if (mode == FM_MODE_POCSAG || mode == FM_MODE_FLEX) return 1;
    return 0;
}

static bool choose_mode(fm_mode_t mode)
{
    ls_args_t args = {0};
    args.n = 1;
    args.v[0].kind = LS_VAL_TEXT;
    args.v[0].s = fm_mode_command_name(mode);
    ls_val_t result;
    if (ls_action_call("fm.submode", &args, &result,
                       ls_quick_grant_builtin()) != LS_ACT_OK) return false;
    s_last_mode = (int)FM.mode;
    s_page = mode_page(mode);
    if (mode != FM_MODE_SCAN) ls_wf_source_release();
    return true;
}

static int draw_modes(tui_surface *sf, tui_rect area)
{
    memset(s_mode_hit, 0, sizeof(s_mode_hit));
    const int columns = area.w >= 72 ? 7 : 4;
    const int rows = (N_MODES + columns - 1) / columns;
    const int button_h = ls_tui_is_wide() ? 3 : 5;
    const int height = rows * button_h;
    if (area.w < 24 || area.h < height + 3) return 0;
    for (int i = 0; i < N_MODES; ++i) {
        const int col = i % columns;
        const int x0 = area.x + area.w * col / columns;
        const int x1 = area.x + area.w * (col + 1) / columns;
        tui_rect box = tui_rect_make(x0, area.y + (i / columns) * button_h,
                                     x1 - x0, button_h);
        s_mode_hit[i] = box;
        const bool active = FM.mode == FM_MODES[i];
        const uint8_t attr = active ? TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT)
                                    : TUI_ATTR(TUI_CYAN, TUI_BLACK);
        tui_box(sf, box, NULL, TUI_ATTR(TUI_CYAN, TUI_BLACK));
        const char *label = fm_mode_label(FM_MODES[i]);
        const int width = (int)strlen(label);
        for (int y = box.y + 1; y < box.y + box.h - 1; ++y)
            for (int x = box.x + 1; x < box.x + box.w - 1; ++x)
                tui_put_char(sf, box, x, y, ' ', attr);
        tui_put_str(sf, box, box.x + (box.w - width) / 2,
                    box.y + box.h / 2, label, attr);
    }
    return height + 1;
}

static const ls_quick_t QUICK[] = {
    /* See scr_p25.c: typing a frequency is the common case. */
    { .label = "TUNE", .kind = LS_QUICK_ACTION, .action = "fm.tune",
      .key = 'f' },
    { .label = "VOLUME", .kind = LS_QUICK_STEP, .action = "audio.volume",
      .value = "sys.volume", .delta = 5, .lo = 0, .hi = 100,
      .key = '+', .key_down = '-' },
    { .label = "GAIN", .kind = LS_QUICK_STEP, .action = "fm.gain",
      .value = "fm.gain", .delta = 2.0f, .lo = 0, .hi = 50,
      .key = 'g', .key_down = 'h' },
    { .label = "SQUELCH", .kind = LS_QUICK_STEP, .action = "fm.sql",
      .value = "fm.sql", .delta = 0.5f, .lo = 0, .hi = 100,
      .key = 'q', .key_down = 'w' },
};
#define N_QUICK ((int)(sizeof(QUICK) / sizeof(QUICK[0])))

/* Where the panel was drawn, so the hit test asks the same geometry. */
static tui_rect s_quick_rect;
static tui_rect s_tune_hit[3];

static uint32_t tune_step_hz(void)
{
    if (FM.mode == FM_MODE_WFM) return 100000;
    if (FM.mode == FM_MODE_AM) {
        if (FM.freq_hz >= 26965000 && FM.freq_hz <= 27405000) return 10000;
        if (FM.freq_hz >= 29000000 && FM.freq_hz <= 29200000) return 5000;
        return 25000;
    }
    if (FM.mode == FM_MODE_ACARS) return 25000;
    return 12500;
}

static bool tune_step(int direction)
{
    int64_t hz = (int64_t)FM.freq_hz + direction * (int64_t)tune_step_hz();
    if (hz < 24000000) hz = 24000000;
    if (hz > 1766000000) hz = 1766000000;
    ls_args_t args = {0};
    args.n = 1;
    args.v[0].kind = LS_VAL_INT;
    args.v[0].i = (int32_t)hz;
    ls_val_t result;
    return ls_action_call("fm.freq_hz", &args, &result,
                          ls_quick_grant_builtin()) == LS_ACT_OK;
}

static void draw_tune(tui_surface *sf, tui_rect area)
{
    const char *labels[] = {"-", "TUNE", "+"};
    const uint8_t edge = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t face = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    for (int i = 0; i < 3; ++i) {
        int x0 = area.x + area.w * i / 3;
        int x1 = area.x + area.w * (i + 1) / 3;
        tui_rect box = tui_rect_make(x0, area.y, x1 - x0, area.h);
        s_tune_hit[i] = box;
        tui_box(sf, box, NULL, edge);
        tui_put_str(sf, box, box.x + (box.w - (int)strlen(labels[i])) / 2,
                    box.y + box.h / 2, labels[i], face);
    }
    char step[20];
    snprintf(step, sizeof(step), "%.1f kHz", (double)tune_step_hz() / 1000.0);
    tui_rect center = s_tune_hit[1];
    tui_put_str(sf, center, center.x + (center.w - (int)strlen(step)) / 2,
                center.y + center.h - 2, step, edge);
}

/* draw_vfo splits its rect in two, so in portrait it draws a VFO box above a
   SIGNAL box and needs room for both: six fields and a border, then a bar and
   three readouts and a border. Nine was the first guess and it clipped the
   pair into each other on the panel - two titles landed on the borders below
   them and read as a corrupt frame. */
#define VFO_ROWS 18

static void field(tui_surface *sf, tui_rect a, int row, const char *l,
                  const char *v, uint8_t la, uint8_t va)
{
    tui_put_str(sf, a, a.x + 2, a.y + row, l, la);
    tui_put_str(sf, a, a.x + 12, a.y + row, v, va);
}

static void draw_vfo(tui_surface *sf, tui_rect area)
{
    const uint32_t hz    = FM.freq_hz;
    const int      sq    = FM.squelch_tenths;
    const bool     open  = FM.squelch_open;
    const float    level = FM.iq_level;
    const int      mode  = (int)FM.mode;

    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t lab   = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    char buf[48];

    tui_rect left, right;
    ls_tui_split(area, &left, &right);

    tui_box(sf, left, "RECEIVER", frame);
    snprintf(buf, sizeof(buf), "%u.%04u MHz", (unsigned)(hz / 1000000u),
             (unsigned)((hz % 1000000u) / 100u));
    tui_put_str(sf, left, left.x + (left.w - (int)strlen(buf)) / 2,
                left.y + 2, buf, val);
    snprintf(buf, sizeof(buf), "%d.%d", sq / 10, sq % 10);
    field(sf, left, 5, "SQUELCH", buf, lab, val);
    field(sf, left, 4, "CARRIER", open ? "OPEN" : "closed", lab,
          open ? good : dim);

    if (open && ((s_blink / 12) & 1))
        tui_put_char(sf, left, left.x + 17, left.y + 4, LS_TUI_SHADE_FULL,
                    good);
    snprintf(buf, sizeof(buf), "%d %%", audio_volume_get());
    field(sf, left, 6, "VOLUME", buf, lab, val);
    snprintf(buf, sizeof(buf), "%d.%d dB", FM.gain_tenths / 10,
             FM.gain_tenths % 10);
    field(sf, left, 7, "GAIN", buf, lab, val);

    tui_box(sf, right, "SIGNAL", frame);
    int bw = right.w - 4;
    int lit = (int)(level * (float)bw);
    if (lit < 0) lit = 0;
    if (lit > bw) lit = bw;
    for (int i = 0; i < bw; i++) {
        uint8_t c = i < lit ? (i > bw * 3 / 4 ? TUI_RED | TUI_BRIGHT
                             : i > bw / 2     ? TUI_YELLOW | TUI_BRIGHT
                                              : TUI_GREEN | TUI_BRIGHT)
                            : (TUI_BLACK | TUI_BRIGHT);
        tui_put_char(sf, right, right.x + 2 + i, right.y + 2,
                     i < lit ? LS_TUI_SHADE_FULL : LS_TUI_SHADE_25,
                     TUI_ATTR(c, TUI_BLACK));
    }
    snprintf(buf, sizeof(buf), "%lu B/s", (unsigned long)FM.iq_bytes_sec);
    field(sf, right, 4, "IQ RATE", buf, lab, val);
    snprintf(buf, sizeof(buf), "%u", (unsigned)FM.pocsag_pages);
    field(sf, right, 5, "PAGES", buf, lab, FM.pocsag_pages ? good : dim);
    field(sf, right, 6, "SYNC", FM.pocsag_sync ? "LOCKED" : "hunting", lab,
          FM.pocsag_sync ? good : dim);
}

/* The codeword tape. */

#define TAPE_MAX 256

static uint8_t  s_tape[TAPE_MAX];
static int      s_tape_n;                  /* marks held, up to TAPE_MAX */
static uint32_t s_seen_frames, s_seen_addr, s_seen_msg, s_seen_err;
static bool     s_seen_valid;

enum { TAPE_IDLE = 0, TAPE_ADDR, TAPE_MSG, TAPE_ERR };

static void tape_push(uint8_t mark, int n)
{
    /* A burst is capped: a counter that jumped by thousands because the
       screen was away means the tape cannot show what happened in between,
       and filling it entirely with one mark would claim that it can. */
    if (n > TAPE_MAX / 4) n = TAPE_MAX / 4;
    for (int i = 0; i < n; i++) {
        if (s_tape_n < TAPE_MAX) {
            s_tape[s_tape_n++] = mark;
        } else {
            memmove(s_tape, s_tape + 1, TAPE_MAX - 1);
            s_tape[TAPE_MAX - 1] = mark;
        }
    }
}

static void tape_sample(void)
{
    const uint32_t f = FM.pocsag_frames, a = FM.pocsag_addr;
    const uint32_t m = FM.pocsag_msg,    e = FM.pocsag_cw_errs;

    if (!s_seen_valid) {
        s_seen_valid = true;
        s_seen_frames = f; s_seen_addr = a; s_seen_msg = m; s_seen_err = e;
        return;
    }

    /* Counters only go up, and a decoder restart takes them back to zero.
       Treating that as a huge negative delta would push nothing; treating it
       as a huge positive one would fill the tape. Re-baseline instead. */
    if (f < s_seen_frames || a < s_seen_addr ||
        m < s_seen_msg || e < s_seen_err) {
        s_tape_n = 0;
        s_seen_frames = f; s_seen_addr = a; s_seen_msg = m; s_seen_err = e;
        return;
    }

    const uint32_t d_addr = a - s_seen_addr;
    const uint32_t d_msg  = m - s_seen_msg;
    const uint32_t d_err  = e - s_seen_err;
    uint32_t d_frame = f - s_seen_frames;

    /* The interesting ones first, then whatever frames are left over as
       idle - so a batch that carried one address among fifteen idles reads
       as one address among fifteen idles. */
    tape_push(TAPE_ERR,  (int)d_err);
    tape_push(TAPE_ADDR, (int)d_addr);
    tape_push(TAPE_MSG,  (int)d_msg);

    const uint32_t named = d_addr + d_msg + d_err;
    if (d_frame > named) d_frame -= named; else d_frame = 0;
    /* A frame is sixteen codewords; the tape is codewords. */
    tape_push(TAPE_IDLE, (int)(d_frame * 16));

    s_seen_frames = f; s_seen_addr = a; s_seen_msg = m; s_seen_err = e;
}

static void draw_tape(tui_surface *sf, tui_rect a)
{
    const uint8_t dim = LS_ATTR_DIM;
    const uint8_t yel  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t wht  = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t red  = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    char buf[48];

    ls_panel_box(sf, a, "STREAM", TUI_CYAN);
    if (a.h < 5 || a.w < 24) return;

    /* The counters go on the right, against the frame, so the tape gets the
       width. Sync first: it is the one that decides whether any of the rest
       means anything. */
    const int cw = 13;
    const int tw = a.w - 2 - cw;
    if (tw < 8) return;

    tui_put_str(sf, a, a.x + tw + 2, a.y + 1,
                FM.pocsag_sync ? "SYNC" : "no sync",
                FM.pocsag_sync ? good : dim);

    snprintf(buf, sizeof(buf), "%d bd", FM.pocsag_lock_baud ? FM.pocsag_lock_baud
                                                            : FM.pocsag_baud);
    tui_put_str(sf, a, a.x + tw + 2, a.y + 2, buf, dim);

    snprintf(buf, sizeof(buf), "%lu pg", (unsigned long)FM.pocsag_pages);
    tui_put_str(sf, a, a.x + tw + 2, a.y + 3,
                buf, FM.pocsag_pages ? yel : dim);

    if (a.h > 6) {
        snprintf(buf, sizeof(buf), "%lu err", (unsigned long)FM.pocsag_cw_errs);
        tui_put_str(sf, a, a.x + tw + 2, a.y + 4,
                    buf, FM.pocsag_cw_errs ? red : dim);
    }

    const int rows = a.h - 2;
    const int cap  = rows * tw;
    int start = s_tape_n - cap;
    if (start < 0) start = 0;

    for (int i = start; i < s_tape_n; i++) {
        const int at = i - start;
        const int x = a.x + 1 + (at % tw);
        const int y = a.y + 1 + (at / tw);
        if (y >= a.y + a.h - 1) break;

        char ch; uint8_t attr;
        switch (s_tape[i]) {
        case TAPE_ADDR: ch = 'A'; attr = yel;  break;
        case TAPE_MSG:  ch = '#'; attr = wht;  break;
        case TAPE_ERR:  ch = 'X'; attr = red;  break;
        default:        ch = '.'; attr = dim;  break;
        }
        tui_put_char(sf, a, x, y, ch, attr);
    }

    if (!s_tape_n)
        tui_put_str(sf, a, a.x + 2, a.y + 1, "no codewords yet", dim);
}

/* The pages list, and the page behind it. */

static int  s_page_sel;
static int  s_page_top;          /* first list row shown                  */
static bool s_page_open;         /* the full view of the selected page    */
static tui_rect s_page_rect;     /* where the list landed, for taps       */
static tui_rect s_page_back;     /* the BACK target on the detail view    */
static tui_rect s_pager_hit[3];

/* Newest first, which is the order the list shows and the order a selection
   index means. */
static const fm_page_t *page_at(int i)
{
    if (i < 0 || i >= (int)FM.page_count || i >= FM_PAGE_LOG_MAX) return NULL;
    const int idx = (FM.page_head - 1 - i + 2 * FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
    return &FM.pages[idx];
}

static int page_count(void)
{
    int n = (int)FM.page_count;
    if (n > FM_PAGE_LOG_MAX) n = FM_PAGE_LOG_MAX;
    return n;
}

static void pager_move(int delta)
{
    const int count = page_count();
    s_page_sel += delta;
    if (s_page_sel >= count) s_page_sel = count - 1;
    if (s_page_sel < 0) s_page_sel = 0;
}

static int draw_pager_controls(tui_surface *sf, tui_rect area)
{
    memset(s_pager_hit, 0, sizeof(s_pager_hit));
    const int height = ls_tui_is_wide() ? 3 : 5;
    if (area.w < 24 || area.h < height + 4) return 0;
    pager_move(0);
    const int count = page_count();
    char center[24];
    snprintf(center, sizeof(center), "%s %d/%d", s_page_open ? "LIST" : "OPEN",
             count ? s_page_sel + 1 : 0, count);
    const char *labels[] = {"UP", center, "DOWN"};
    const bool enabled[] = {s_page_sel > 0, count > 0, s_page_sel + 1 < count};
    for (int i = 0; i < 3; ++i) {
        int x0 = area.x + area.w * i / 3;
        int x1 = area.x + area.w * (i + 1) / 3;
        tui_rect box = tui_rect_make(x0, area.y + area.h - height, x1 - x0, height);
        s_pager_hit[i] = box;
        uint8_t attr = enabled[i] ? TUI_ATTR(TUI_BLACK, TUI_CYAN)
                                  : TUI_ATTR(TUI_WHITE, TUI_BLACK);
        tui_box(sf, box, NULL, TUI_ATTR(enabled[i] ? TUI_CYAN : TUI_WHITE, TUI_BLACK));
        for (int y = box.y + 1; y < box.y + box.h - 1; ++y)
            for (int x = box.x + 1; x < box.x + box.w - 1; ++x)
                tui_put_char(sf, box, x, y, ' ', attr);
        tui_put_str(sf, box, box.x + (box.w - (int)strlen(labels[i])) / 2,
                    box.y + box.h / 2, labels[i], attr);
    }
    return height + 1;
}

/* One row per page in landscape, three in portrait: the same measurement the
   mesh node list and the mesh settings use, and for the same reason - one row
   of a 10x17 cell is 1.3 mm and not a target. */
static int page_row_h(void) { return ls_tui_is_wide() ? 1 : 3; }

/* Which page a row belongs to. Draw and hit test both go through this so
   they cannot disagree about where a row is. */
static int page_at_row(tui_rect r, int row)
{
    const int i = (row - r.y - 1) / page_row_h();
    return i >= 0 ? s_page_top + i : -1;
}

static const char *type_word(char t)
{
    switch (t) {
    case 'A': return "ALPHANUMERIC";
    case 'N': return "NUMERIC";
    case 'T': return "TONE ONLY";
    default:  return "NOT SURE - see below";
    }
}

/* The page, whole.

   Wrapped at a word where there is one, because a message broken mid-word
   every forty characters is harder to read than the truncation it replaced.
   Everything the decoder recorded is on here: a page that looks wrong is
   either the wrong baud or the wrong classification, and those are the two
   fields that say which. */
static void draw_page_detail(tui_surface *sf, tui_rect pane)
{
    const uint8_t lab   = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t txt   = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t warn  = TUI_ATTR(TUI_YELLOW, TUI_BLACK);

    s_page_back = tui_rect_make(0, -1, 0, 0);

    const fm_page_t *pg = page_at(s_page_sel);
    if (!pg) {
        ls_panel_box(sf, pane, "PAGE", TUI_CYAN);
        tui_put_str(sf, pane, pane.x + 2, pane.y + 2, "that page is gone", dim);
        return;
    }

    /* Wrapped FIRST, so the box can be the size of what is in it.

       A box stretched over the whole pane with a line of text near the top
       is the fault recorded on the P25 spectrum notice: forty-five
       rows of empty rectangle whose bottom edge lands on the page buttons.
       A page is six fields and a couple of lines; it should look like it. */
#define DETAIL_LINES 8
    char wrap[DETAIL_LINES][96];
    int nlines = 0;
    {

        nlines = ls_wrap_text(pg->text, pane.w - 4,
                              (char *)wrap, sizeof(wrap[0]), DETAIL_LINES);
        if (!nlines) snprintf(wrap[nlines++], sizeof(wrap[0]), "%s", "(no text)");
    }

    const bool unsure = (pg->type == '?');
    const int bh = ls_tui_is_wide() ? 1 : 3;
    /* border, five fields, blank, the unsure line, the message, blank, the
       way back, border. */
    int want = 1 + 5 + 1 + (unsure ? 2 : 0) + nlines + 1 + bh + 1;
    if (want > pane.h) want = pane.h;

    tui_rect area = tui_rect_make(pane.x, pane.y, pane.w, want);
    ls_panel_box(sf, area, "PAGE", TUI_CYAN);

    int y = area.y + 1;
    char buf[64];

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)pg->address);
    field(sf, area, y - area.y, "RIC", buf, lab, val);
    y++;

    field(sf, area, y - area.y, "TYPE", type_word(pg->type), lab,
          unsure ? warn : val);
    y++;

    snprintf(buf, sizeof(buf), "%u", (unsigned)pg->function);
    field(sf, area, y - area.y, "FUNCTION", buf, lab, val);
    y++;

    /* The baud is on the screen for the first time. The receiver
       runs three decoders at once and they all write this one list, so
       "which decoder produced this" was unanswerable from the panel - and
       it is the first question to ask about a page that reads as rubbish. */
    snprintf(buf, sizeof(buf), "%u", (unsigned)pg->baud);
    field(sf, area, y - area.y, "BAUD", pg->baud ? buf : "-", lab, val);
    y++;

    field(sf, area, y - area.y, "PROTOCOL",
          pg->protocol == FM_PAGE_PROTOCOL_FLEX ? "FLEX" : "POCSAG", lab, val);
    y += 2;

    if (unsure && y < area.y + area.h - 2) {
        tui_put_str(sf, area, area.x + 2, y,
                    "not sure this is readable - see BAUD", warn);
        y += 2;
    }

    for (int i = 0; i < nlines && y < area.y + area.h - 1 - bh; i++, y++)
        tui_put_str(sf, area, area.x + 2, y, wrap[i], txt);

    /* A way back that is a target, not a key. Portrait has no
       keyboard, so ESC is not available and a page you cannot leave is worse
       than no page. */
    if (area.h > bh + 4) {
        s_page_back = tui_rect_make(area.x + 2, area.y + area.h - 1 - bh,
                                    area.w - 4, bh);
        ls_fill_dither(sf, s_page_back, LS_DITHER_LIGHT, TUI_CYAN);
        ls_dither_label(sf, s_page_back, (bh - 1) / 2, "BACK TO THE LIST",
                        TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    }
#undef DETAIL_LINES
}

static void draw_pages(tui_surface *sf, tui_rect whole)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t sel   = TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT);
    const uint8_t addr  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t body  = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t dim = LS_ATTR_DIM;

    tape_sample();

    s_page_rect = tui_rect_make(0, -1, 0, 0);
    s_page_back = tui_rect_make(0, -1, 0, 0);
    whole.h -= draw_pager_controls(sf, whole);

    /* The full view takes the whole pane. It is one message and it
       wants the width and the height; the tape above it answers a different
       question and can wait. */
    if (s_page_open) {
        draw_page_detail(sf, whole);
        return;
    }

    /* The stream takes the top third, the pages the rest. Below about
       fourteen rows there is only room for one of them, and it is the pages:
       a decoded page is the answer and the tape is the working. */
    tui_rect area = whole;
    if (whole.h >= 14) {
        int th = whole.h / 3;
        if (th < 6)  th = 6;
        if (th > 10) th = 10;
        draw_tape(sf, tui_rect_make(whole.x, whole.y, whole.w, th));
        area = tui_rect_make(whole.x, whole.y + th, whole.w, whole.h - th);
    }

    tui_box(sf, area, "DECODED PAGES", frame);
    s_page_rect = area;

    const int count = page_count();
    if (!count) {
        tui_put_str(sf, area, area.x + 2, area.y + 2, "listening...", dim);
        return;
    }

    const int rh = page_row_h();
    /* The last interior row carries the hint, so it is not a list row. */
    const int rows = (area.h - 3) / rh;
    if (rows < 1) return;

    /* The window follows the selection instead of the selection
       running off the end of the window. Without this the list showed the
       newest `rows` pages and nothing else could ever be reached - which is
       the "scrolling messages as they get cut" half of the report. */
    if (s_page_sel < 0) s_page_sel = 0;
    if (s_page_sel > count - 1) s_page_sel = count - 1;
    if (s_page_sel < s_page_top) s_page_top = s_page_sel;
    if (s_page_sel >= s_page_top + rows) s_page_top = s_page_sel - rows + 1;
    if (s_page_top > count - rows) s_page_top = count - rows;
    if (s_page_top < 0) s_page_top = 0;

    for (int r = 0; r < rows; r++) {
        const int i = s_page_top + r;
        const fm_page_t *pg = page_at(i);
        if (!pg) break;

        const int top = area.y + 1 + r * rh;
        const int y = top + (rh - 1) / 2;
        const bool on = (i == s_page_sel);

        if (on) {
            tui_fill(sf, tui_rect_make(area.x + 1, top, area.w - 2, rh), ' ', sel);
        } else if (rh > 1) {

            ls_fill_dither(sf, tui_rect_make(area.x + 1, top, area.w - 2, rh),
                           LS_DITHER_LIGHT, TUI_CYAN);
        }

        char line[160];
        snprintf(line, sizeof(line), "%-9lu %c %.*s", (unsigned long)pg->address,
                 pg->type ? pg->type : '?', area.w - 16, pg->text);
        tui_put_str(sf, area, area.x + 2, y, line, on ? sel : body);
        if (!on) {
            char id[12];
            snprintf(id, sizeof(id), "%-9lu", (unsigned long)pg->address);
            tui_put_str(sf, area, area.x + 2, y, id, addr);
        }
    }

    /* Where you are in the list, and how to open one. A count that only
       appears when something is off screen: a list that fits has nothing to
       say about its own position. */
    char hint[64];
    if (count > rows)
        snprintf(hint, sizeof(hint), "%d-%d of %d   %s", s_page_top + 1,
                 s_page_top + rows, count,
                 ls_tui_is_wide() ? "ENTER opens it" : "tap twice to open");
    else
        snprintf(hint, sizeof(hint), "%s",
                 ls_tui_is_wide() ? "ENTER opens it" : "tap a page twice to open it");
    tui_put_str(sf, area, area.x + 2, area.y + area.h - 2, hint, dim);
}

static void draw_sweep(tui_surface *sf, tui_rect area)
{
    ls_wf_source_select(LS_WF_SRC_FM);
    ls_wf_source_pump();

    const char *why = ls_wf_idle_reason();
    /* Keep HOLD reachable while the shared display is paused. */
    if (why && !ls_wf_cfg()->paused) {
        ls_panel_box(sf, area, "SWEEP", TUI_CYAN);
        tui_put_str(sf, area, area.x + 2, area.y + 2, why,
                    LS_ATTR_DIM);
        /* Where the first row is, when one is coming. A sweep of
           VHF land takes eleven seconds before it has a row to show, and
           "waiting for the receiver" alone reads the same as a receiver that
           is not there. */
        char line[48];
        if (FM.mode == FM_MODE_SCAN && FM.scan_tunes > 0)
            snprintf(line, sizeof(line), "sweeping: tune %d of %d",
                     FM.scan_idx + 1, FM.scan_tunes);
        else
            snprintf(line, sizeof(line), "3 SWEEP starts a band sweep");
        tui_put_str(sf, area, area.x + 2, area.y + 4, line, LS_ATTR_DIM);
        return;
    }
    ls_wf_draw(sf, area);
}

static const ls_btn_t PAGES[] = {
    { "VFO",   NULL, '1', false, false },
    { "PAGER", NULL, '2', false, false },
    { "SWEEP", NULL, '3', false, false },
};
#define N_PAGES ((int)(sizeof(PAGES) / sizeof(PAGES[0])))

static tui_rect s_bar;

/* Turning to SWEEP starts the sweep, which is what its own idle text always said it did. */

static void show_page(int i)
{
    s_page = i;
    s_last_mode = (int)FM.mode;
    if (i == 2) {
        if (FM.mode != FM_MODE_SCAN) choose_mode(FM_MODE_SCAN);
    } else {
        ls_wf_source_release();
        if (i == 0 && FM.mode == FM_MODE_SCAN) choose_mode(FM_MODE_LISTEN);
        if (i == 1 && FM.mode != FM_MODE_POCSAG && FM.mode != FM_MODE_FLEX)
            choose_mode(FM_MODE_POCSAG);
    }
}

/* Called by the UI task when the control head chooses FM or POCSAG. */
void ls_scr_fm_show_page(int page)
{
    if (page >= 0 && page < N_PAGES) show_page(page);
}

static void draw(tui_surface *sf, tui_rect area)
{
    s_blink++;
    if (s_last_mode != (int)FM.mode) {
        s_last_mode = (int)FM.mode;
        s_page = mode_page(FM.mode);
    }
    const bool wide = ls_tui_is_wide();
    /* Three rows in portrait: a two row bar with one line of text has no middle row to put it on. */

    const int bar_h = wide ? 3 : 5;
    const int bar_pad = wide ? 0 : 1;

    /* Portrait puts the page bar at the bottom, where the thumb is.

       Right at the top in landscape, where the strip is a legend for the
       function keys and the keyboard is under the screen. Wrong in portrait,
       where the hand holding the thing reaches the bottom third and the top
       of a 1232 pixel panel wants a second hand. */
    tui_rect body;
    if (wide) {
        s_bar = tui_rect_make(area.x, area.y, area.w, bar_h);
        body = tui_rect_make(area.x, area.y + bar_h, area.w, area.h - bar_h);
    } else {
        s_bar = tui_rect_make(area.x, area.y + area.h - bar_h - bar_pad,
                              area.w, bar_h);
        body = tui_rect_make(area.x, area.y, area.w,
                             area.h - bar_h - bar_pad);
    }

    ls_btn_t b[N_PAGES];
    for (int i = 0; i < N_PAGES; i++) { b[i] = PAGES[i]; b[i].on = (i == s_page); }
    ls_btn_bar(sf, s_bar, b, N_PAGES, -1);

    s_quick_rect = tui_rect_make(0, -1, 0, 0);
    memset(s_tune_hit, 0, sizeof(s_tune_hit));

    if (body.h <= 0) return;
    const int mode_rows = draw_modes(sf, body);
    body.y += mode_rows;
    body.h -= mode_rows;
    if (s_page == 1)      { draw_pages(sf, body); return; }
    if (s_page == 2)      { draw_sweep(sf, body); return; }

    /* The VFO needs nine rows. In portrait the rest is the controls; in
       landscape there is no rest and there is a keyboard. */
    if (!wide && body.h > VFO_ROWS + 4) {
        tui_rect vfo = tui_rect_make(body.x, body.y, body.w, VFO_ROWS);
        draw_vfo(sf, vfo);

        s_quick_rect = tui_rect_make(body.x, body.y + VFO_ROWS + 1, body.w,
                                     body.h - VFO_ROWS - 1);
        if (s_quick_rect.h >= 7) {
            draw_tune(sf, tui_rect_make(s_quick_rect.x, s_quick_rect.y,
                                       s_quick_rect.w, 7));
            s_quick_rect.y += 7;
            s_quick_rect.h -= 7;
        }
        ls_quick_draw_posture(sf, s_quick_rect, false, QUICK + 1, N_QUICK - 1);
        return;
    }
    draw_vfo(sf, body);
}

static void leave(void) { ls_wf_source_release(); }

static void enter(void)
{
    s_last_mode = (int)FM.mode;
    s_page = mode_page(FM.mode);
    s_page_open = false;
}

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_CHAR) {
        if (ch == '[') return tune_step(-1);
        if (ch == ']') return tune_step(1);
        if (ch == 'm' || ch == 'M' || ch == 'n' || ch == 'N') {
            int current = 0;
            for (int j = 0; j < N_MODES; ++j)
                if (FM_MODES[j] == FM.mode) current = j;
            return choose_mode(FM_MODES[(current +
                ((ch == 'n' || ch == 'N') ? N_MODES - 1 : 1)) % N_MODES]);
        }
        const int i = ls_btn_key(ch, PAGES, N_PAGES);
        if (i >= 0) { show_page(i); return true; }
        /* The same controls the panel draws, so a keyboard and a thumb reach
           them by one path and cannot drift apart. Checked after the page
           keys so a digit still turns the page. */
        if (ls_quick_key(ch, QUICK, N_QUICK,
                         ls_quick_grant_builtin(), NULL)) return true;
    }
    if (s_page == 2 && ls_wf_key(k, ch)) return true;

    /* The open page owns its keys while it is up. ESC closes it, and
       UP/DOWN walk to the next message without going back to the list first,
       which is what reading a run of pages actually looks like. */
    if (s_page == 1 && s_page_open) {
        switch (k) {
        case LS_TK_ESC:   s_page_open = false; return true;
        case LS_TK_UP:    pager_move(-1); return true;
        case LS_TK_DOWN:  pager_move(1); return true;
        default: return true;
        }
    }

    switch (k) {
    case LS_TK_LEFT:  if (s_page > 0) show_page(s_page - 1); return true;
    case LS_TK_RIGHT: if (s_page < N_PAGES - 1) show_page(s_page + 1); return true;
    case LS_TK_UP:    if (s_page == 1) pager_move(-1); return true;
    case LS_TK_DOWN:
        if (s_page == 1) pager_move(1);
        return true;
    case LS_TK_ENTER:
        if (s_page == 1 && page_count() > 0) { s_page_open = true; return true; }
        return false;
    default: return false;
    }
}

static bool touch(int col, int row)
{
    for (int j = 0; j < N_MODES; ++j)
        if (tui_rect_contains(s_mode_hit[j], col, row))
            return choose_mode(FM_MODES[j]);
    if (row >= s_bar.y && row < s_bar.y + s_bar.h) {
        const int i = ls_btn_hit(col, row);
        if (i >= 0) { show_page(i); return true; }
        return true;
    }
    for (int i = 0; i < 3; ++i) {
        if (!tui_rect_contains(s_tune_hit[i], col, row)) continue;
        if (i == 1) ls_quick_fire(&QUICK[0], ls_quick_grant_builtin());
        else tune_step(i == 0 ? -1 : 1);
        return true;
    }
    if (s_quick_rect.h > 0 && row >= s_quick_rect.y &&
        row < s_quick_rect.y + s_quick_rect.h &&
        ls_quick_touch(col, row, QUICK + 1, N_QUICK - 1,
                       ls_quick_grant_builtin(), NULL))
        return true;
    if (s_page == 2) return ls_wf_touch(col, row);

    if (s_page == 1) {
        for (int i = 0; i < 3; ++i) {
            if (!tui_rect_contains(s_pager_hit[i], col, row)) continue;
            if (i == 0) pager_move(-1);
            else if (i == 2) pager_move(1);
            else if (page_count() > 0) s_page_open = !s_page_open;
            return true;
        }
    }

    /* The open page: its BACK target, and nothing else - a stray tap
       must not lose the message you opened. */
    if (s_page == 1 && s_page_open) {
        if (s_page_back.h > 0 && row >= s_page_back.y &&
            row < s_page_back.y + s_page_back.h)
            s_page_open = false;
        return true;
    }

    /* A page in the list: the first tap picks it and a second tap on
       the same one opens it. The rule the settings fields and the mesh node
       list already use - one tap must never fire an action, because a tap
       that both moves the cursor and does something is one you cannot take
       back. */
    if (s_page == 1 && s_page_rect.h > 0 &&
        row > s_page_rect.y && row < s_page_rect.y + s_page_rect.h - 1 &&
        col >= s_page_rect.x && col < s_page_rect.x + s_page_rect.w) {
        const int i = page_at_row(s_page_rect, row);
        if (i >= 0 && i < page_count()) {
            if (i == s_page_sel) s_page_open = true;
            else                 s_page_sel = i;
        }
        return true;
    }
    return false;
}

const ls_tui_screen_t ls_scr_fm = {
    /* the VFO, the sweep and the pager pages are all this receiver. */
    .radio = "FM",
    .name = "FM",
    .hint = "UP/DOWN messages  1 VFO  2 PAGER  3 SWEEP",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
