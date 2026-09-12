/* P25 screen. */

#include "../../ls_tui_screen.h"

#include <stdio.h>
#include <string.h>

#include "p25_state.h"
#include "p25_tg_observed.h"
#include "esp_attr.h"
#include "iq_app_control.h"
#include "esp_timer.h"

#include "../../ls_quick.h"
#include "../../ls_tui_ui.h"
#include "../../ls_waterfall.h"
#include "../../ls_wf_source.h"
#include "../../ls_text.h"

static int s_page;   /* 0 decode, 1 signal */

/* The same free-running blink REC and FM now use (scr_rec.c,
   scr_fm.c) and MESH originated (scr_mesh.c s_blink): incremented once a
   draw, read back as `(s_blink / 12) & 1`. TG read a number the instant a
   grant landed and held that number, unchanged in appearance, for the rest
   of the call - a live conversation and its own five-second-old tail end
   looked identical. */
static uint32_t s_blink;

static void field(tui_surface *sf, tui_rect a, int row, const char *label,
                  const char *value, uint8_t lattr, uint8_t vattr)
{
    tui_put_str(sf, a, a.x + 2, a.y + row, label, lattr);
    tui_put_str(sf, a, a.x + 10, a.y + row, value, vattr);
}

/* What else has been talking, which the firmware has always known and this screen has never said. */

EXT_RAM_BSS_ATTR static p25_tg_observed_snapshot_t s_tg;
static int64_t s_tg_read_us;
static bool    s_tg_have;

static void tg_refresh(int64_t now_us)
{

    if (s_tg_read_us && now_us >= s_tg_read_us &&
        now_us - s_tg_read_us < 1000000) return;
    s_tg_read_us = now_us;
    if (p25_tg_observed_read(&s_tg)) s_tg_have = true;
}

static void draw_activity(tui_surface *sf, tui_rect r, bool sync, int64_t now_us)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t head  = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t live  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t old   = TUI_ATTR(TUI_WHITE, TUI_BLACK);

    tui_box(sf, r, "ACTIVITY", frame);
    if (r.h < 3 || r.w < 24) return;

    /* Already read, by the layout above - it sizes this frame from the count,
       so the count has to be known before the rect exists. */
    const int n = s_tg_have ? (int)s_tg.count : 0;
    if (n <= 0) {
        /* Two different silences, and only one of them is worth waiting
           through. No sync means nothing has been heard BECAUSE nothing is
           being decoded; sync with an empty table is a genuinely quiet
           system. Saying "no traffic" for both would report a broken
           receiver as a quiet channel, which is the failure this panel
           exists to make visible. */
        tui_put_str(sf, r, r.x + 2, r.y + 1,
                    sync ? "synced, nothing heard yet"
                         : "no sync - nothing to hear",
                    LS_ATTR_DIM);
        return;
    }

    if (r.h < 4) return;
    tui_put_str(sf, r, r.x + 2, r.y + 1,
                "TALKGROUP    HEARD   AGE  SEEN IN", head);

    const uint32_t now_ms = (uint32_t)(now_us / 1000);
    const int rows = r.h - 3;

    uint32_t used = 0;
    for (int slot = 0; slot < rows && slot < n; slot++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (used & (1u << i)) continue;
            if (best < 0 ||
                s_tg.rows[i].last_seen_ms > s_tg.rows[best].last_seen_ms)
                best = i;
        }
        if (best < 0) break;
        used |= 1u << best;

        const p25_tg_observed_row_t *row = &s_tg.rows[best];

        char age[8];
        ls_age_str(age, sizeof(age), row->last_seen_ms, now_ms);

        /* Where it was seen answers a different question from how often: a
           link control word is the talkgroup of a call in progress, a header
           is one starting, and a grant is one being set up on another
           channel - so a row seen only in grants is traffic this radio is
           not listening to. */
        char how[4];
        int hi = 0;
        if (row->sources & P25_TG_SEEN_LCW)   how[hi++] = 'L';
        if (row->sources & P25_TG_SEEN_HDU)   how[hi++] = 'H';
        if (row->sources & P25_TG_SEEN_GRANT) how[hi++] = 'G';
        how[hi] = 0;

        char line[64];
        snprintf(line, sizeof(line), "%-9u  %7lu  %4s  %-3s",
                 (unsigned)row->talkgroup, (unsigned long)row->hits,
                 age, how);

        /* The channel only when it is not the one being listened to. The
           store keys on frequency as well as talkgroup, deliberately, so the
           table can hold rows from a channel this radio has since left - and
           those must not read as traffic on the current one. The tolerance is
           half a 12.5 kHz channel because `channel_hz` is the tuner's
           EFFECTIVE centre, which lands a few kHz off what was asked for;
           comparing for equality would mark every row off-channel. */
        const uint64_t tuned = (uint64_t)s_tune_freq_hz;
        const uint64_t chan  = row->channel_hz;
        const uint64_t delta = chan > tuned ? chan - tuned : tuned - chan;
        if (chan && delta > 6250ULL && r.w >= 46) {
            char freq[16];
            snprintf(freq, sizeof(freq), "  %u.%04u",
                     (unsigned)(chan / 1000000ULL),
                     (unsigned)((chan % 1000000ULL) / 100ULL));
            strncat(line, freq, sizeof(line) - strlen(line) - 1);
        }

        /* Thirty seconds is the span a P25 call and its tail occupy, so a
           row brighter than the rest is one that is or has just been live. */
        const bool recent = now_ms >= row->last_seen_ms &&
                            (now_ms - row->last_seen_ms) < 30000;
        tui_put_str(sf, r, r.x + 2, r.y + 2 + slot, line, recent ? live : old);
    }
}

static const char *voice_status(void)
{
    if (P25.p25_enc_muted)
        return P25.p25_ess_valid ? "encrypted" : "waiting for voice header";
    return P25.voice_active_until_us > esp_timer_get_time()
           ? "voice decoded" : "waiting for voice";
}

static void draw_decode(tui_surface *sf, tui_rect area)
{
    /* One read, at the top. See the header note. */
    const int      nac   = P25.dsd_nac;
    const int      tg    = P25.dsd_tg;
    const int      src   = P25.dsd_src;
    const bool     sync  = P25.dsd_has_sync;
    const float    level = P25.iq_level;
    const uint32_t freq  = s_tune_freq_hz;
    const bool     voice = P25.voice_active_until_us > esp_timer_get_time();
    char mod[8];
    snprintf(mod, sizeof(mod), "%.7s", P25.dsd_modulation);

    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t label = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t value = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t idle  = TUI_ATTR(TUI_WHITE, TUI_BLACK);

    /* Both panes hold a fixed list, so both take what they need. */

    /* Both panels are nine rows tall in both postures, and the activity table goes below them - AS TALL AS ITS CONTENTS, not as tall as the space. */

    const int want = 9;
    tui_rect left, right;
    tui_rect activity = tui_rect_make(0, 0, 0, 0);

    /* Read before the layout, because the layout is sized from the count. */
    tg_refresh(esp_timer_get_time());
    const int held = (s_tg_have && s_tg.count) ? (int)s_tg.count : 0;
    const int fits = held ? 3 + held : 3;   /* border, heading, rows, border */

    if (area.w >= 60) {
        tui_rect top = area;
        if (area.h >= want + 3) {
            int h = area.h - want;
            if (h > fits) h = fits;
            top = tui_rect_make(area.x, area.y, area.w, want);
            activity = tui_rect_make(area.x, area.y + want, area.w, h);
        }
        ls_tui_split(top, &left, &right);
    } else {
        ls_tui_split_at(area, want, &left, &right);
        if (right.h > want) {
            int h = right.h - want;
            if (h > fits) h = fits;
            if (h >= 3)
                activity = tui_rect_make(area.x, right.y + want, area.w, h);
            right.h = want;
        }
    }

    tui_box(sf, left, "DECODE", frame);
    char buf[40];
    snprintf(buf, sizeof(buf), "%u.%04u MHz", (unsigned)(freq / 1000000u),
             (unsigned)((freq % 1000000u) / 100u));
    field(sf, left, 1, "FREQ", buf, label, value);

    if (nac > 0) snprintf(buf, sizeof(buf), "%03X", nac); else snprintf(buf, sizeof(buf), "---");
    field(sf, left, 2, "NAC", buf, label, nac > 0 ? value : idle);

    if (tg > 0) snprintf(buf, sizeof(buf), "%d", tg); else snprintf(buf, sizeof(buf), "--");
    field(sf, left, 3, "TG", buf, label, tg > 0 ? value : idle);

    if (voice && ((s_blink / 12) & 1))
        tui_put_char(sf, left, left.x + 10 + (int)strlen(buf) + 1, left.y + 3,
                    LS_TUI_SHADE_FULL, good);

    if (src > 0) snprintf(buf, sizeof(buf), "%d", src); else snprintf(buf, sizeof(buf), "--");
    field(sf, left, 4, "SRC", buf, label, src > 0 ? value : idle);

    field(sf, left, 5, "MOD", mod[0] ? mod : "----", label, value);
    field(sf, left, 6, "SYNC", sync ? "LOCKED" : "searching", label,
          sync ? good : idle);

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
    snprintf(buf, sizeof(buf), "%.3f", (double)level);
    field(sf, right, 4, "IQ", buf, label, value);

    ls_iq_control_status_t st;
    p25_get_receiver_status(&st);
    field(sf, right, 6, "RX", st.receiver_streaming ? "STREAMING" : "stopped",
          label, st.receiver_streaming ? good : idle);
    field(sf, right, 7, "AUDIO", voice_status(), label,
          P25.p25_enc_muted ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : idle);

    if (activity.h > 0)
        draw_activity(sf, activity, sync, esp_timer_get_time());

}

/* A level, since landscape has nowhere else to put one. */

static void draw_iq_gauge(tui_surface *sf, tui_rect r, float level)
{
    if (r.h < 1 || r.w < 16) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    tui_put_str(sf, r, r.x, r.y, "SIG",
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));

    char pct[6];
    snprintf(pct, sizeof(pct), "%3d%%", (int)(level * 100.0f + 0.5f));
    tui_put_str(sf, r, r.x + r.w - 4, r.y, pct,
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

    /* Rising eighth-height traces, the same reading MESH's signal meter uses
       (scr_mesh.c, sigmeter) - each cell's own threshold so the bar fills
       left to right and grows taller as it goes, just driven by a fraction
       here instead of a dBm span. */
    const int cells = r.w - 9;
    for (int i = 0; i < cells; i++) {
        const float t = (float)(i + 1) / (float)cells;
        const int   h = 2 + (i * 6) / (cells > 1 ? cells - 1 : 1);
        const bool  lit = level >= t - (0.5f / (float)cells);
        const uint8_t hue = i > cells * 3 / 4 ? TUI_RED
                           : i > cells / 2     ? TUI_YELLOW
                                                : TUI_GREEN;
        tui_put_char(sf, r, r.x + 4 + i, r.y, LS_TUI_TRACE(h),
                     TUI_ATTR(lit ? (hue | TUI_BRIGHT) : (TUI_BLACK | TUI_BRIGHT),
                              TUI_BLACK));
    }
}

static void draw_signal(tui_surface *sf, tui_rect area)
{
    ls_wf_source_select(LS_WF_SRC_P25);
    ls_wf_source_pump();

    if (area.h > 10) {
        char status[48];
        snprintf(status, sizeof(status), "Audio: %s", voice_status());
        tui_put_str(sf, area, area.x + 1, area.y, status,
                    P25.p25_enc_muted ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                                      : LS_ATTR_DIM);
        area.y++;
        area.h--;
    }

    const char *why = ls_wf_idle_reason();
    /* Keep HOLD reachable while the shared display is paused. */
    if (why && !ls_wf_cfg()->paused) {
        ls_panel_notice(sf, area, "SPECTRUM", why,
                        "the receiver feeds this once it is streaming");
        return;
    }

    if (ls_tui_is_wide() && area.h > 6) {
        const tui_rect gauge = tui_rect_make(area.x, area.y, area.w, 1);
        const tui_rect plot = tui_rect_make(area.x, area.y + 1, area.w,
                                            area.h - 1);
        draw_iq_gauge(sf, gauge, P25.iq_level);
        ls_wf_draw(sf, plot);
        return;
    }
    ls_wf_draw(sf, area);
}

/* Pages are buttons, not a key you have to know about.

   LEFT/RIGHT still turns the page and always will - it is the fastest thing
   with a keyboard attached. But with no keyboard the only clue that a second
   page existed was the hint bar, and the hint bar is a legend, not a control.
   Two fat buttons cost two rows in portrait and one in landscape, and they
   are the same action from either input. */
static const ls_btn_t PAGES[] = {
    { "DECODE", NULL, '1', false, false },
    { "SIGNAL", NULL, '2', false, false },
};
#define N_PAGES ((int)(sizeof(PAGES) / sizeof(PAGES[0])))

static tui_rect s_bar;
static tui_rect s_quick_rect;

/* The controls a thumb reaches for while watching a decode.

   Portrait has sixty rows and the decode page fills eighteen. Volume and
   gain were console-only here, exactly as they were on FM before the panel
   went in; a scanner you cannot turn down without a cable is not a handheld
   scanner. */
static const ls_quick_t QUICK[] = {

    { .label = "TUNE", .kind = LS_QUICK_ACTION, .action = "p25.tune",
      .key = 'f' },
    { .label = "VOLUME", .kind = LS_QUICK_STEP, .action = "audio.volume",
      .value = "sys.volume", .delta = 5, .lo = 0, .hi = 100,
      .key = '+', .key_down = '-' },
    { .label = "GAIN", .kind = LS_QUICK_STEP, .action = "p25.gain",
      .value = "p25.gain", .delta = 2.0f, .lo = 0, .hi = 50,
      .key = 'g', .key_down = 'j' },
};
#define N_QUICK ((int)(sizeof(QUICK) / sizeof(QUICK[0])))

static void draw(tui_surface *sf, tui_rect area)
{
    s_blink++;
    const bool wide = ls_tui_is_wide();
    /* Three rows in portrait, not two. */

    const int bar_h = 3;

    tui_rect body;
    if (wide) {
        /* LANDSCAPE HAS THE CONTROLS TOO. */

        const int want = ls_quick_rows(QUICK, N_QUICK, area.w, true);
        const int ctl_h = (area.h > want + 8) ? want : 0;
        s_bar = tui_rect_make(area.x, area.y, area.w, bar_h);
        s_quick_rect = ctl_h
            ? tui_rect_make(area.x, area.y + area.h - ctl_h, area.w, ctl_h)
            : tui_rect_make(0, -1, 0, 0);
        body = tui_rect_make(area.x, area.y + bar_h, area.w,
                             area.h - bar_h - ctl_h);
    } else {
        /* Page bar above the settings. Choosing a page and changing a
           setting are different kinds of thing, and the page bar was under
           the settings where it read as one more of them. */
        const int want = ls_quick_rows(QUICK, N_QUICK, area.w, ls_tui_is_wide());
        const int ctl_h = (area.h > want + 12) ? want : 0;
        s_quick_rect = ctl_h
            ? tui_rect_make(area.x, area.y + area.h - ctl_h, area.w, ctl_h)
            : tui_rect_make(0, -1, 0, 0);
        /* The blank row goes ABOVE the page bar.

           The arithmetic reserved one and then put the bar directly under
           the page content, so the row of air ended up between the bar and
           the quick controls - where nothing needed separating - and the
           page's bottom border sat flush on the buttons. */
        const int used = bar_h + 1 + ctl_h;
        body = tui_rect_make(area.x, area.y, area.w, area.h - used);
        s_bar = tui_rect_make(area.x, body.y + body.h + 1, area.w, bar_h);
    }

    ls_btn_t b[N_PAGES];
    for (int i = 0; i < N_PAGES; i++) {
        b[i] = PAGES[i];
        b[i].on = (i == s_page);
    }
    ls_btn_bar(sf, s_bar, b, N_PAGES, -1);

    if (body.h > 0) {
        if (s_page == 1) draw_signal(sf, body);
        else             draw_decode(sf, body);
    }

    if (s_quick_rect.h > 0)
        ls_quick_draw_posture(sf, s_quick_rect, wide, QUICK, N_QUICK);
}

/* The feed follows the page, and leaving the app releases it whichever page
   was up - the spectrum must not keep costing FFTs behind another screen. */
static void leave(void) { ls_wf_source_release(); }

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_CHAR) {
        const int i = ls_btn_key(ch, PAGES, N_PAGES);
        if (i >= 0) { s_page = i; return true; }
        /* The same controls the panel draws, so a keyboard and a thumb reach
           them by one path. After the page keys, so a digit still pages. */
        if (ls_quick_key(ch, QUICK, N_QUICK,
                         ls_quick_grant_builtin(), NULL)) return true;
    }
    /* SIGNAL hands its keys to the waterfall: it is the waterfall, and a
       second set of bindings for the same instrument is how two screens end
       up disagreeing about what R means. */
    if (s_page == 1 && ls_wf_key(k, ch)) return true;

    if (k == LS_TK_LEFT)  { if (s_page > 0) s_page--; return true; }
    if (k == LS_TK_RIGHT) { if (s_page < N_PAGES - 1) s_page++; return true; }
    return false;
}

static bool touch(int col, int row)
{
    if (row >= s_bar.y && row < s_bar.y + s_bar.h) {
        const int i = ls_btn_hit(col, row);
        if (i >= 0) { s_page = i; return true; }
        return true;
    }
    if (s_quick_rect.h > 0 && row >= s_quick_rect.y &&
        row < s_quick_rect.y + s_quick_rect.h &&
        ls_quick_touch(col, row, QUICK, N_QUICK,
                       ls_quick_grant_builtin(), NULL))
        return true;
    if (s_page == 1) return ls_wf_touch(col, row);
    return false;
}

const ls_tui_screen_t ls_scr_p25 = {
    /* P25 is this screen's whole subject. */
    .radio = "P25",
    .name = "P25",
    /* H belongs to the waterfall, which is only on SIGNAL.

       The key reaches ls_wf_key through this screen's handler and that call
       is gated on s_page == 1, while the hint row is drawn on both pages. So
       DECODE advertised a key that does nothing there. Said precisely rather
       than dropped: H is real and worth knowing about, it just lives on the
       other page. */
    .hint = "1 DECODE  2 SIGNAL (H holds it)  TAP mark",
    .enter = NULL,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
