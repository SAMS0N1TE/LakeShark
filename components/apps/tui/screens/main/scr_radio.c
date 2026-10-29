/* RADIO: the RX HEAD's screen - one receiver, its decoders as plugins.

   Behind CONFIG_LS_RX_HEAD (off by default) while it grows: the receiver it
   shows is FM's (lakeshark_fm_rx_head), whose presets name the plugins.

     the shared rail     RADIO chip, the plugins as mode chips (SHED when the
                         receiver is behind), REC; the frequency in the
                         glass's figures and the squelch meter under it,
                         drawn by the same ls_radio_glass_* every receiver uses;
     the waterfall strip ls_wf_draw_mini on the FM source, the one spectrum;
     the tabs            one per attached plugin, in its own hue;
     the panel           the focused plugin's own (rx_panels/), which keeps
                         that decoder's look: the pager's tape and pages, the
                         voice's speaker, APRS's stations;
     the deck            RADIO r, MODE m, OPTIONS o, CALLS c, SWEEP w.

   It reads FM's state and the plugin descriptors only: a preset change frees
   plugin state on the receive task, so nothing here touches it. */
#if defined(CONFIG_LS_RX_HEAD) && CONFIG_LS_RX_HEAD

#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_i18n.h"
#include "../../ls_radio_panel.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_picker.h"
#include "../../ls_calls.h"
#include "../../ls_waterfall.h"
#include "../../ls_wf_source.h"
#include "../../ls_app.h"
#include "../../rx_panels/ls_rx_panel.h"
#include "ls_rx_head.h"
#include "apps/fm/fm_state.h"
#include "apps/fm/fm_dsp.h"
#include "lakeshark_backend.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>

static char s_hint[96] = "r radio  m mode  o options  TAB decoder";
static EXT_RAM_BSS_ATTR ls_radio_glass_t s_glass;
static uint32_t s_frame;
static int s_focus = -1;               /* which plugin's panel; -1 the decoder */
static tui_rect s_tabs, s_deck, s_panel;
static const ls_rx_panel_t *s_shown;

/* ---- presets: what MODE offers --------------------------------------------- */

typedef struct { const char *name, *detail; int mode; bool voice; ls_rsel_job_t job; } preset_t;
static const preset_t PRESETS[] = {
    { "LISTEN NFM",     "voice, squelch and tone",          FM_MODE_LISTEN, false, LS_RSEL_FM },
    { "POCSAG",         "pagers, three baud rates",         FM_MODE_POCSAG, false, LS_RSEL_PAGER },
    { "POCSAG + VOICE", "pagers with the channel audible",  FM_MODE_POCSAG, true,  LS_RSEL_PAGER },
    { "FLEX",           "pagers, 1600-6400 bps",            FM_MODE_FLEX,   false, LS_RSEL_PAGER },
    { "APRS",           "packets, 144.39 MHz",              FM_MODE_APRS,   false, LS_RSEL_FM },
    { "SAME",           "weather radio alerts",             FM_MODE_SAME,   false, LS_RSEL_FM },
    { "ACARS",          "aircraft datalink",                FM_MODE_ACARS,  false, LS_RSEL_ACARS },
    { "AIS",            "ships, SDR only",                  FM_MODE_AIS,    false, LS_RSEL_FM },
};
#define N_PRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

static int preset_now(void)
{
    for (int i = 0; i < N_PRESETS; i++)
        if (PRESETS[i].mode == (int)FM.mode && PRESETS[i].voice == (FM.mode == FM_MODE_POCSAG && FM.pager_voice))
            return i;
    return 0;
}

static void preset_picked(int i)
{
    if (i < 0 || i >= N_PRESETS) return;
    lakeshark_fm_pager_voice(PRESETS[i].voice);
    lakeshark_fm_set_mode(PRESETS[i].mode);
    s_focus = -1;
}

static void open_presets(void)
{
    ls_picker_open("RECEIVE", preset_picked);
    for (int i = 0; i < N_PRESETS; i++) ls_picker_add(PRESETS[i].name, PRESETS[i].detail);
    ls_picker_select(preset_now());
}

static void radio_chosen(ls_rsel_radio_t r) { (void)r; }
static void open_radio(void) { ls_rsel_open(PRESETS[preset_now()].job, radio_chosen); }

/* ---- OPTIONS: the head's rows ---------------------------------------------- */

static int o_voice(const ls_opt_t *o) { (void)o; return FM.pager_voice; }
static void o_set_voice(const ls_opt_t *o, int v) { (void)o; lakeshark_fm_pager_voice(v != 0); }
static const char *pager_only(const ls_opt_t *o) { (void)o; return FM.mode == FM_MODE_POCSAG ? NULL : "POCSAG only"; }
static double o_squelch(const ls_opt_t *o) { (void)o; return lakeshark_fm_squelch_get(); }
static void o_set_squelch(const ls_opt_t *o, double v) { (void)o; lakeshark_fm_set_squelch((int)v); }
static const ls_opt_t OPT_HEAD[] = {
    { .label = "HEAR CHANNEL", .kind = LS_OPT_TOGGLE, .get = o_voice, .set = o_set_voice, .why_not = pager_only },
    { .label = "SQUELCH", .kind = LS_OPT_LEVEL, .num = o_squelch, .set_num = o_set_squelch,
      .lo = 0, .hi = 100, .step = 5, .unit = "0 opens on anything, 100 on nothing" },
};
static const ls_opt_ctx_t CTX_HEAD = { .name = "RADIO", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_HEAD) };

/* ---- the rail ----------------------------------------------------------------- */

static void draw_glass(tui_surface *sf, tui_rect r, const ls_rx_head_t *h)
{
    const bool wide = ls_tui_is_wide();
    ls_radio_glass_bezel(sf, r, "RX HEAD");
    const int x0 = r.x + 2, iw = r.w - 4, last = r.y + r.h - 2;
    if (iw < 16 || r.h < 4) return;

    /* The chips: the radio, then every plugin running, then what the focused
       panel adds (sync, baud). A plugin the receiver is shedding says so. */
    ls_radio_chip_t left[LS_RX_SLOTS + 4];
    int nl = 0;
    const ls_btn_t radio = ls_rsel_button(PRESETS[preset_now()].job);
    left[nl++] = (ls_radio_chip_t){ radio.value ? radio.value : "RADIO", true, TUI_CYAN };
    const int n = h ? h->n : 0;
    for (int i = 0; i < n && nl < LS_RX_SLOTS + 1; i++) {
        const ls_rx_plugin_t *p = h->slot[i].p;
        left[nl++] = (ls_radio_chip_t){ h->slot[i].shedding ? "SHED" : p->name, true,
                                       h->slot[i].shedding ? TUI_RED : ls_rx_plugin_hue(p) };
    }
    if (s_shown && s_shown->chips) nl += s_shown->chips(left + nl, (int)(sizeof(left) / sizeof(left[0])) - nl);
    const ls_radio_chip_t right[1] = { { "REC", FM.squelch_open && lakeshark_fm_squelch_get() > 0, TUI_RED } };
    ls_radio_glass_rail(sf, r, x0, r.y + 1, iw, left, nl, right, 1);

    int y = ls_radio_glass_frequency(&s_glass, sf, r, r.y + 2, FM.freq_hz, NULL, s_frame, wide);
    ls_radio_glass_wipe(&s_glass, sf, r, r.y + 2, y, (int)FM.mode);

    /* The meter reads quieting where the voice runs, else the carrier. */
    const bool voice = FM.mode == FM_MODE_LISTEN || (FM.mode == FM_MODE_POCSAG && FM.pager_voice);
    float sig = voice ? 1.0f - FM.noise : FM.iq_level * 2.0f;
    if (sig < 0.0f) sig = 0.0f;
    if (sig > 1.0f) sig = 1.0f;
    ls_radio_glass_sample(&s_glass, sig);
    const int rows = last - y >= 2 ? 2 : 1;
    if (y + rows - 1 <= last)
        ls_radio_glass_meter(&s_glass, sf, r, x0, last - rows + 1, wide ? iw * 2 / 3 : iw, rows, sig,
                             true, voice && FM.squelch_open, rows > 1,
                             voice ? fm_nfm_squelch_gate(lakeshark_fm_squelch_get()) : 0.0f, voice ? "SQL" : NULL);
    if (wide) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%lu B/s", (unsigned long)FM.iq_bytes_sec);
        tui_put_str(sf, r, x0 + iw * 2 / 3 + 2, r.y + 2, ls_tr("IQ"), TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, r, x0 + iw * 2 / 3 + 5, r.y + 2, buf, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        if (h && h->dropped) {
            snprintf(buf, sizeof(buf), "behind %lu", (unsigned long)h->dropped);
            tui_put_str(sf, r, x0 + iw * 2 / 3 + 2, r.y + 3, buf, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
        }
    }
}

/* ---- the screen ----------------------------------------------------------------- */

static const ls_btn_t DECK[] = {
    { "RADIO",   NULL, 'r', false, false },
    { "MODE",    NULL, 'm', false, false },
    { "OPTIONS", NULL, 'o', false, false },
    { "CALLS",   NULL, 'c', false, false },
    { "SWEEP",   NULL, 'w', false, false },
};
#define N_DECK ((int)(sizeof(DECK) / sizeof(DECK[0])))
#define SLOT_DECK LS_BTN_SLOT_SCREEN
#define SLOT_TABS LS_BTN_SLOT_QUICK

static void draw(tui_surface *sf, tui_rect area)
{
    if (ls_calls_active()) { ls_wf_source_release(); ls_calls_draw(sf, area); return; }
    s_frame++;
    const bool wide = ls_tui_is_wide();
    const ls_rx_head_t *h = lakeshark_fm_rx_head();
    const int n = h ? h->n : 0;
    /* First look at a preset: the decoder's panel, not the voice's - the
       pages are what is new; the speaker is heard anyway. */
    if (s_focus < 0 || s_focus >= n) {
        s_focus = 0;
        for (int i = 0; i < n; i++)
            if (!(h->slot[i].p->needs.flags & LS_RX_F_AUDIO)) { s_focus = i; break; }
    }
    s_shown = n ? ls_rx_panel_for(h->slot[s_focus].p) : NULL;

    /* The deck at the bottom in portrait, where the thumb is; at the top in
       landscape, as every receiver lays it out. */
    ls_btn_t deck[N_DECK];
    memcpy(deck, DECK, sizeof(deck));
    deck[1].value = PRESETS[preset_now()].name;
    deck[2].value = ls_opt_button(&CTX_HEAD).value;
    const int deck_h = wide ? 3 : 5;
    s_deck = wide ? tui_rect_make(area.x, area.y, area.w, deck_h)
                  : tui_rect_make(area.x, area.y + area.h - deck_h, area.w, deck_h);
    tui_rect body = wide ? tui_rect_make(area.x, area.y + deck_h, area.w, area.h - deck_h)
                         : tui_rect_make(area.x, area.y, area.w, area.h - deck_h);

    const int glass_h = wide ? 6 : 11;
    if (body.h < glass_h + 8) { draw_glass(sf, body, h); goto deck; }
    draw_glass(sf, tui_rect_make(body.x, body.y, body.w, glass_h), h);
    body.y += glass_h; body.h -= glass_h;

    /* The one spectrum. */
    const int wf_h = wide ? 5 : 8;
    ls_wf_source_select(LS_WF_SRC_FM);
    ls_wf_source_pump();
    const char *idle = ls_wf_idle_reason();
    if (idle) {
        /* No spectrum yet: one line saying why, not an empty plot. */
        tui_put_str(sf, body, body.x + 2, body.y, idle, LS_ATTR_DIM);
        body.y += 1; body.h -= 1;
    } else {
        ls_wf_draw_mini(sf, tui_rect_make(body.x, body.y, body.w, wf_h));
        body.y += wf_h; body.h -= wf_h;
    }

    /* A tab for each plugin, in its hue's words. */
    ls_btn_t tabs[LS_RX_SLOTS];
    for (int i = 0; i < n; i++) {
        const ls_rx_panel_t *pn = ls_rx_panel_for(h->slot[i].p);
        tabs[i] = (ls_btn_t){ pn->tab ? pn->tab : h->slot[i].p->name, NULL, (char)('1' + i), i == s_focus };
    }
    const int tab_h = 3;
    s_tabs = tui_rect_make(body.x, body.y, body.w, tab_h);
    if (n > 1) {
        ls_btn_bar_slot(sf, s_tabs, tabs, n, -1, SLOT_TABS);
        body.y += tab_h; body.h -= tab_h;
    } else s_tabs = tui_rect_make(0, -1, 0, 0);

    s_panel = body;
    if (s_shown) {
        s_shown = ls_rx_panel_for(h->slot[s_focus].p);
        s_shown->draw(sf, body, s_frame);
        snprintf(s_hint, sizeof(s_hint), "%s  m mode  o options%s", s_shown->hint,
                 n > 1 ? "  1-9 decoder" : "");
    } else {
        ls_panel_box(sf, body, "RX HEAD", TUI_CYAN);
        tui_put_str(sf, body, body.x + 2, body.y + 2, ls_tr("no decoder running: MODE picks one"), LS_ATTR_DIM);
    }
deck:
    ls_btn_bar_slot(sf, s_deck, deck, N_DECK, -1, SLOT_DECK);
}

static bool deck_action(int i)
{
    switch (i) {
    case 0: open_radio(); return true;
    case 1: open_presets(); return true;
    case 2: ls_opt_open(&CTX_HEAD); return true;
    case 3: ls_calls_open("FM"); ls_wf_source_release(); return true;
    case 4: ls_wf_fm_sweep(true); return true;
    default: return false;
    }
}

static bool key(ls_tk_t k, char ch)
{
    if (ls_calls_active()) return ls_calls_key(k, ch);
    if (k == LS_TK_CHAR && ls_opt_key(&CTX_HEAD, ch)) return true;
    const ls_rx_head_t *h = lakeshark_fm_rx_head();
    const int n = h ? h->n : 0;
    if (k == LS_TK_CHAR && ch >= '1' && ch < '1' + n) { s_focus = ch - '1'; return true; }
    if (k == LS_TK_TAB && n) { s_focus = (s_focus + 1) % n; return true; }
    if (k == LS_TK_CHAR) {
        const int d = ls_btn_key(ch, DECK, N_DECK);
        if (d >= 0) return deck_action(d);
    }
    return s_shown && s_shown->key && s_shown->key(k, ch);
}

static bool touch(int col, int row)
{
    if (ls_calls_active()) return ls_calls_touch(col, row);
    const int d = ls_btn_hit_slot(col, row, SLOT_DECK);
    if (d >= 0) return deck_action(d);
    const int t = ls_btn_hit_slot(col, row, SLOT_TABS);
    if (t >= 0) { s_focus = t; return true; }
    return s_shown && s_shown->touch && s_shown->touch(col, row);
}

static void enter(void) { s_focus = -1; }
static void leave(void) { ls_wf_source_release(); }

const ls_tui_screen_t ls_scr_radio = {
    .radio = "FM",
    .name = "RADIO",
    .hint = s_hint,
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};

const ls_app_doc_t ls_doc_radio = {
    .purpose = "One receiver with its decoders as plugins: pick what to receive under "
               "MODE and the decoders it names run side by side on the same channel.",
    .records = LS_APP_RECORDS_AUTOMATIC,
    .record_note = "as FM: CALLS saves squelched voice when recording is on",
    .gps = LS_APP_GPS_STAMPS,
    .gps_note = "as FM",
};

#endif
