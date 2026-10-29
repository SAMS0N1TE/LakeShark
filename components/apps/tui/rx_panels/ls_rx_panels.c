/* See ls_rx_panel.h. The pager panels are the tape and page list moved out of
   the FM screen (rx_tape.c, rx_pages.c); APRS, SAME and AIS are FM's own
   views; the voice has a speaker of its own. */
#include "ls_rx_panel.h"
#include "rx_pages.h"
#include "rx_tape.h"
#include "rx_exp.h"
#include "../ls_i18n.h"
#include "../ls_text.h"
#include "../screens/main/fm_aprs_view.h"
#include "../screens/main/fm_same_view.h"
#include "../screens/main/fm_ais_view.h"
#include "apps/fm/fm_state.h"
#include "lakeshark_backend.h"
#include "apps/fm/fm_dsp.h"
#include "apps/fm/fm_tone.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>

/* ---- pages: POCSAG and FLEX write FM's page ring ------------------------- */

static int fm_count(void) { return FM.page_count < FM_PAGE_LOG_MAX ? FM.page_count : FM_PAGE_LOG_MAX; }
static const fm_page_t *fm_at(int i)
{
    if (i < 0 || i >= fm_count()) return NULL;
    return &FM.pages[(FM.page_head - 1 - i + 2 * FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX];
}
static const rx_pages_src_t FM_RING = { .count = fm_count, .at = fm_at };
static rx_pages_t s_pv;
static EXT_RAM_BSS_ATTR rx_tape_t s_tape;

static rx_pager_counts_t pocsag_counts(void)
{
    return (rx_pager_counts_t){
        .on = true, .tape = true, .sync = FM.pocsag_sync,
        .baud = FM.pocsag_lock_baud ? FM.pocsag_lock_baud : FM.pocsag_baud,
        .pages = FM.pocsag_pages, .errs = FM.pocsag_cw_errs,
        .frames = FM.pocsag_frames, .addr = FM.pocsag_addr, .msg = FM.pocsag_msg,
    };
}

static void pages_body(tui_surface *sf, tui_rect whole, bool tape)
{
    rx_pages_refind(&s_pv, &FM_RING);
    s_pv.list = s_pv.back = tui_rect_make(0, -1, 0, 0);
    whole.h -= rx_pages_controls(&s_pv, &FM_RING, sf, whole);
    if (s_pv.open) { rx_pages_detail(&s_pv, &FM_RING, sf, whole); return; }
    tui_rect area = whole;
    if (tape && whole.h >= 14) {
        int th = whole.h / 3;
        if (th < 6) th = 6;
        if (th > 9) th = 9;
        const rx_pager_counts_t c = pocsag_counts();
        rx_tape_sample(&s_tape, &c);
        rx_tape_draw(&s_tape, &c, sf, tui_rect_make(whole.x, whole.y, whole.w, th));
        area = tui_rect_make(whole.x, whole.y + th, whole.w, whole.h - th);
    }
    rx_pages_list(&s_pv, &FM_RING, sf, area, "DECODED PAGES", NULL);
    rx_pages_remember(&s_pv, &FM_RING);
}

static void pocsag_draw(tui_surface *sf, tui_rect r, uint32_t f) { (void)f; pages_body(sf, r, true); }
static void flex_draw(tui_surface *sf, tui_rect r, uint32_t f) { (void)f; pages_body(sf, r, false); }

static bool pages_key(ls_tk_t k, char ch)
{
    (void)ch;
    rx_pages_refind(&s_pv, &FM_RING);
    const bool used = rx_pages_key(&s_pv, &FM_RING, k);
    rx_pages_remember(&s_pv, &FM_RING);
    return used;
}

static bool pages_touch(int col, int row)
{
    rx_pages_refind(&s_pv, &FM_RING);
    const bool used = rx_pages_touch(&s_pv, &FM_RING, col, row);
    rx_pages_remember(&s_pv, &FM_RING);
    return used;
}

static int pocsag_chips(ls_radio_chip_t *out, int max)
{
    static char baud[12];
    if (max < 2) return 0;
    const int b = FM.pocsag_lock_baud ? FM.pocsag_lock_baud : FM.pocsag_baud;
    snprintf(baud, sizeof(baud), "%s%d", FM.pocsag_auto && !FM.pocsag_lock_baud ? "AUTO " : "", b);
    out[0] = (ls_radio_chip_t){ FM.pocsag_sync ? "SYNC" : "HUNT", FM.pocsag_sync, TUI_GREEN };
    out[1] = (ls_radio_chip_t){ baud, false, TUI_YELLOW };
    return 2;
}

static const ls_rx_panel_t P_POCSAG = {
    .tab = "PAGES", .hint = "UP/DOWN pick  ENTER open  ESC list",
    .draw = pocsag_draw, .key = pages_key, .touch = pages_touch, .chips = pocsag_chips,
};
static const ls_rx_panel_t P_FLEX = {
    .tab = "FLEX", .hint = "UP/DOWN pick  ENTER open  ESC list",
    .draw = flex_draw, .key = pages_key, .touch = pages_touch,
};

/* ---- the voice: a speaker that opens with the squelch --------------------- */

#define LEVEL_N 96
static uint8_t s_level[LEVEL_N];     /* audio level history, 0..64 */
static int s_level_head;
static uint32_t s_level_frame;

/* The audio level as a scrolling column chart under the speaker: '|' for a
   full cell, ':' and '.' for the top of a column, green low, yellow loud,
   magenta near clipping. It scrolls only while the squelch is open. */
static void level_chart(tui_surface *sf, tui_rect r, int x0, int y0, int w, int h)
{
    if (w > LEVEL_N) w = LEVEL_N;
    for (int c = 0; c < w; c++) {
        const int v = s_level[(s_level_head - w + c + 2 * LEVEL_N) % LEVEL_N];
        const int units = v * h * 3 / 64;          /* thirds of a row */
        for (int row = 0; row < h; row++) {
            const int have = units - row * 3;
            const char ch = have >= 3 ? '|' : have == 2 ? ':' : have == 1 ? '.' : row == 0 ? '_' : ' ';
            const int frac = row * 3 / h;
            const uint8_t a = have <= 0 ? LS_ATTR_DIM
                            : TUI_ATTR(frac >= 2 ? TUI_MAGENTA | TUI_BRIGHT : frac == 1 ? TUI_YELLOW | TUI_BRIGHT
                                                                       : TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
            tui_put_char(sf, r, x0 + c, y0 + h - 1 - row, ch, a);
        }
    }
}

static void voice_draw(tui_surface *sf, tui_rect r, uint32_t frame)
{
    const uint8_t lab  = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim  = LS_ATTR_DIM;
    const uint8_t mag  = TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK);
    ls_panel_box(sf, r, "VOICE", TUI_GREEN);
    if (r.w < 24 || r.h < 7) return;
    const bool open = FM.squelch_open;
    float lv = FM.audio_level / 0.65f;
    if (lv > 1.0f) lv = 1.0f;
    if (lv < 0.0f) lv = 0.0f;

    /* One sample a drawn frame while the channel is open: the chart moves
       only with real audio and stands still when the squelch is shut. */
    if (frame != s_level_frame) {
        s_level_frame = frame;
        if (open) {
            s_level[s_level_head] = (uint8_t)(lv * 64.0f + 0.5f);
            s_level_head = (s_level_head + 1) % LEVEL_N;
        }
    }

    /* The speaker: its cone and as many waves as the audio is loud. */
    static const char *const CONE[4] = { "   __", "  |  \\", "==|   |", "  |__/" };
    const int sx = r.x + 2, sy = r.y + 1;
    for (int i = 0; i < 4; i++) tui_put_str(sf, r, sx, sy + i, CONE[i], open ? good : dim);
    const int waves = open ? 1 + (int)(lv * 2.99f) : 0;
    static const char WAVE[3] = { '\\', ')', '/' };
    for (int w = 0; w < 3; w++)
        for (int i = 0; i < 3; i++)
            tui_put_char(sf, r, sx + 8 + 2 * w, sy + 1 + i, WAVE[i],
                         w < waves ? (w == 2 ? mag : good) : dim);

    /* The readouts beside it. */
    char buf[32];
    const int x = sx + 16;
    int y = sy;
    tui_put_str(sf, r, x, y, ls_tr("SQUELCH"), lab);
    tui_put_str(sf, r, x + 9, y++, open ? "OPEN" : "shut", open ? good : dim);
    snprintf(buf, sizeof(buf), "%d%%", lakeshark_fm_squelch_get());
    tui_put_str(sf, r, x, y, ls_tr("GATE"), lab);
    tui_put_str(sf, r, x + 9, y++, lakeshark_fm_squelch_get() ? buf : "open", val);
    tui_put_str(sf, r, x, y, ls_tr("TONE"), lab);
    if (FM.tone_detected.selection) fm_tone_label(FM.tone_detected.selection, buf, sizeof(buf));
    else snprintf(buf, sizeof(buf), "%s", FM.tone_required ? "waiting" : "--");
    tui_put_str(sf, r, x + 9, y++, buf, FM.tone_detected.selection ? good : dim);
    snprintf(buf, sizeof(buf), "%.2f", (double)FM.noise);
    tui_put_str(sf, r, x, y, ls_tr("NOISE"), lab);
    tui_put_str(sf, r, x + 9, y++, buf, val);

    /* The chart takes the rest of the panel. */
    const int cy = sy + 5;
    const int ch = r.y + r.h - 1 - cy - 1;
    if (ch < 2) return;
    tui_put_str(sf, r, sx, cy, ls_tr("AUDIO"), lab);
    level_chart(sf, r, sx, cy + 1, r.w - 4, ch);
}

static int voice_chips(ls_radio_chip_t *out, int max)
{
    if (max < 1) return 0;
    out[0] = (ls_radio_chip_t){ FM.squelch_open ? "OPEN" : "SQL", FM.squelch_open, TUI_GREEN };
    return 1;
}

static const ls_rx_panel_t P_VOICE = {
    .tab = "VOICE", .hint = "the channel's audio: squelch, tone and level",
    .draw = voice_draw, .chips = voice_chips,
};

/* ---- FM's data views ------------------------------------------------------- */

static void aprs_draw(tui_surface *sf, tui_rect r, uint32_t f) { (void)f; fm_aprs_draw(sf, r); }
static void same_draw(tui_surface *sf, tui_rect r, uint32_t f) { (void)f; fm_same_draw(sf, r); }
static void ais_draw(tui_surface *sf, tui_rect r, uint32_t f) { (void)f; fm_ais_draw(sf, r); }
static const ls_rx_panel_t P_APRS = { .tab = "STATIONS", .hint = "tap a station", .draw = aprs_draw, .touch = fm_aprs_touch };
static const ls_rx_panel_t P_SAME = { .tab = "ALERTS", .hint = "weather alerts heard", .draw = same_draw, .touch = fm_same_touch };
static const ls_rx_panel_t P_AIS  = { .tab = "VESSELS", .hint = "ships heard", .draw = ais_draw, .touch = fm_ais_touch };

/* ---- an EXPERIMENT: its own readout under its maturity badge ---------------- */

static const ls_experiment_t *s_exp;
static void exp_draw(tui_surface *sf, tui_rect r, uint32_t f)
{
    (void)f;
    const ls_experiment_t *e = s_exp;
    if (!e) return;
    ls_panel_box(sf, r, e->name, TUI_MAGENTA);
    if (r.w < 16 || r.h < 4) return;
    const bool works = e->maturity == LS_EXP_WORKS;
    ls_radio_glass_chip(sf, r, r.x + 2, r.y + 1, ls_exp_maturity_name(e->maturity), true,
                        works ? TUI_GREEN : TUI_YELLOW);
    tui_put_str(sf, r, r.x + 12, r.y + 1, e->sub ? e->sub : "", TUI_ATTR(TUI_CYAN, TUI_BLACK));
    static char lines[16][LS_EXP_LINE];
    const int n = ls_exp_read_lines(e, lines, 16);
    for (int i = 0; i < n && r.y + 3 + i < r.y + r.h - 1; i++)
        tui_put_str(sf, r, r.x + 2, r.y + 3 + i, lines[i],
                    i == 0 ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : TUI_ATTR(TUI_WHITE, TUI_BLACK));
    if (!n) tui_put_str(sf, r, r.x + 2, r.y + 3, ls_tr("no readout yet"), LS_ATTR_DIM);
}
static const ls_rx_panel_t P_EXP = { .tab = NULL, .hint = "the experiment's own readout", .draw = exp_draw };

/* ---- any other plugin: what it is and what it needs ------------------------ */

static const ls_rx_plugin_t *s_plain;
static void plain_draw(tui_surface *sf, tui_rect r, uint32_t f)
{
    (void)f;
    const ls_rx_plugin_t *p = s_plain;
    ls_panel_box(sf, r, p ? p->name : "DECODER", TUI_CYAN);
    if (!p || r.w < 12) return;
    char wrap[8][96];
    tui_put_str(sf, r, r.x + 2, r.y + 1, p->sub ? p->sub : "", TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    const int n = ls_wrap_text(p->doc, r.w - 4 < 95 ? r.w - 4 : 95, (char *)wrap, sizeof(wrap[0]), 8);
    for (int i = 0; i < n && r.y + 3 + i < r.y + r.h - 1; i++)
        tui_put_str(sf, r, r.x + 2, r.y + 3 + i, wrap[i], TUI_ATTR(TUI_WHITE, TUI_BLACK));
}
static const ls_rx_panel_t P_PLAIN = { .tab = NULL, .hint = "", .draw = plain_draw };

const ls_rx_panel_t *ls_rx_panel_for(const ls_rx_plugin_t *p)
{
    static const struct { const char *id; const ls_rx_panel_t *panel; } MAP[] = {
        { "pocsag", &P_POCSAG }, { "flex", &P_FLEX }, { "voice", &P_VOICE },
        { "aprs", &P_APRS }, { "same", &P_SAME }, { "ais", &P_AIS },
    };
    const ls_experiment_t *e = ls_rx_plugin_exp(p);
    if (e) { s_exp = e; return &P_EXP; }
    for (unsigned i = 0; p && i < sizeof(MAP) / sizeof(MAP[0]); i++)
        if (!strcmp(p->id, MAP[i].id)) return MAP[i].panel;
    s_plain = p;
    return &P_PLAIN;
}

uint8_t ls_rx_plugin_hue(const ls_rx_plugin_t *p)
{
    if (!p) return TUI_WHITE;
    if (p->maturity != LS_RX_WORKS || (p->needs.flags & LS_RX_F_TRYING)) return TUI_YELLOW;
    if (p->needs.flags & LS_RX_F_AUDIO) return TUI_GREEN;
    return p->needs.inputs & (LS_RX_BITS | LS_RX_BATCH | LS_RX_DISC) ? TUI_CYAN : TUI_MAGENTA;
}
