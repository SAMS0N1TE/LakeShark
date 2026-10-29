/* P25's live face inside CALLS. See p25_calls_live.h. */
#include "p25_calls_live.h"

#include "../../ls_calls.h"
#include "../../ls_braille.h"
#include "../../ls_motion.h"
#include "../../ls_tui.h"
#include "../../ls_tui_ui.h"

#include "esp_attr.h"
#include "esp_timer.h"
#include "p25_p2_runtime.h"
#include "p25_program.h"
#include "p25_state.h"
#include "p25_tg_observed.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ls_textpack.h"

#ifdef ESP_PLATFORM
#include "scan_ctrl.h"
#else
/* The bench supplies names from its fixture (lssim_p25_settings.c). */
const char *p25_calls_host_alias(uint16_t talkgroup);
#endif

/* ------------------------------------------------------------ the call -- */

/* One call as the card tells it. A call starts when the receiver is on
   voice (or on an encrypted call it cannot play) for a talkgroup, and ends
   two seconds after it last was: voice frames come every 180 ms and the
   decoder already holds voice_active_until_us half a second, so two
   seconds of nothing is the end of a transmission, not a gap in one. */
typedef struct {
    bool     on, enc, p2, emergency;
    uint16_t tg, kid;
    uint8_t  algid;
    uint32_t unit;
    uint64_t hz;
    int64_t  start_us, last_us;
    char     talker[32];
} p25_call_t;
#define CALL_END_US 2000000LL

static p25_call_t s_now, s_last;

/* The voice trace: one level every 100 ms, the loudest in that tenth, for
   as long as the card's call is on air. A new call starts it afresh; a
   quiet system leaves it where it stopped. */
#define TRACE_N    128
#define TRACE_US   100000LL
static EXT_RAM_BSS_ATTR uint8_t s_trace[TRACE_N];
static int     s_trace_n, s_trace_head;
static int64_t s_trace_due;
static uint8_t s_trace_bin;
static ls_fresh_t s_arrival;

static uint16_t live_tg(void)
{
    if (P25.grant_on_traffic && P25.grant_talkgroup) return P25.grant_talkgroup;
    if (P25.p25_lcw_valid && P25.p25_lcw_talkgroup) return P25.p25_lcw_talkgroup;
    return P25.dsd_tg > 0 && P25.dsd_tg < 65536 ? (uint16_t)P25.dsd_tg : 0;
}

static uint32_t live_unit(void)
{
    if (P25.dsd_src > 0) return (uint32_t)P25.dsd_src;
    if (P25.p25_lcw_valid && P25.p25_lcw_source) return P25.p25_lcw_source;
    return P25.grant_on_traffic ? P25.grant_source : 0;
}

static void trace_push(uint8_t v)
{
    s_trace[s_trace_head] = v;
    s_trace_head = (s_trace_head + 1) % TRACE_N;
    if (s_trace_n < TRACE_N) s_trace_n++;
}

void p25_calls_live_tick(void)
{
    const int64_t now = esp_timer_get_time();
    const bool voice = P25.voice_active_until_us > now;
    const bool enc = P25.receive_state == P25_RX_ENCRYPTED || P25.p25_enc_muted;
    const bool on = voice || P25.receive_state == P25_RX_AUDIO ||
                    P25.receive_state == P25_RX_ENCRYPTED;
    const uint16_t tg = live_tg();

    if (on && tg) {
        if (!s_now.on || s_now.tg != tg) {
            if (s_now.on) s_last = s_now;
            memset(&s_now, 0, sizeof(s_now));
            s_now.on = true;
            s_now.tg = tg;
            s_now.start_us = now;
            s_trace_n = s_trace_head = 0;
            s_trace_bin = 0;
            s_trace_due = now + TRACE_US;
            ls_fresh_bump(&s_arrival);
        }
        s_now.last_us = now;
        s_now.unit = live_unit() ? live_unit() : s_now.unit;
        s_now.enc = s_now.enc || enc;
        s_now.p2 = p25_p2_enabled() ||
                   (P25.grant_on_traffic && P25.grant_active.support == P25_CALL_PHASE2);
        s_now.emergency = s_now.emergency ||
            (P25.p25_lcw_valid && P25.p25_lcw_emergency) ||
            (P25.grant_on_traffic && (P25.grant_active.service_options & 0x80));
        s_now.algid = P25.p25_algid;
        s_now.kid = P25.p25_kid;
        s_now.hz = P25.grant_on_traffic && P25.grant_freq_hz ? P25.grant_freq_hz
                                                             : s_tune_freq_hz;
        if (P25.p25_lcw_alias_ready && P25.p25_lcw_alias[0])
            snprintf(s_now.talker, sizeof(s_now.talker), "%s", P25.p25_lcw_alias);
    } else if (s_now.on && now - s_now.last_us > CALL_END_US) {
        s_last = s_now;
        s_last.on = false;
        s_now.on = false;
    }

    if (!s_now.on) return;
    /* Clear calls draw what is heard; an encrypted call has nothing to
       hear, so its trace is the carrier it arrives on instead. */
    unsigned level;
    if (s_now.enc && !voice) {
        float sig = P25.iq_level;
        level = sig <= 0.0f ? 0 : sig >= 1.0f ? 255 : (unsigned)(sig * 255.0f);
    } else if (voice) {
        /* Square root: speech sits far below full scale, and a linear trace
           of it is a flat line with the odd spike. */
        level = (unsigned)(sqrtf(P25.voice_peak / 32768.0f) * 255.0f);
        if (level > 255) level = 255;
    } else {
        level = 0;
    }
    if (level > s_trace_bin) s_trace_bin = (uint8_t)level;
    while (now >= s_trace_due) {
        trace_push(s_trace_bin);
        s_trace_bin = 0;
        s_trace_due += TRACE_US;
        if (now - s_trace_due > 10 * TRACE_US) s_trace_due = now + TRACE_US;
    }
}

bool p25_calls_live_now(uint16_t *talkgroup, uint32_t *unit, bool *on_air,
                        uint32_t *elapsed_ms)
{
    const p25_call_t *c = s_now.on ? &s_now : &s_last;
    if (!c->tg) return false;
    if (talkgroup) *talkgroup = c->tg;
    if (unit) *unit = c->unit;
    if (on_air) *on_air = s_now.on;
    if (elapsed_ms)
        *elapsed_ms = (uint32_t)(((s_now.on ? esp_timer_get_time() : c->last_us) -
                                  c->start_us) / 1000);
    return true;
}

/* ----------------------------------------------------------- names ---- */

const char *p25_calls_alias(uint16_t talkgroup)
{
    if (!talkgroup) return NULL;
#ifdef ESP_PLATFORM
    /* The names file wins over the profile, as the roster has it. */
    const p25_scan_name_t *n = p25_scan_name_lookup(&g_p25_scan, talkgroup);
    if (n && n->name[0]) return n->name;
#endif
    const p25_program_t *pg = p25_program_session();
    if (pg && pg->active_valid)
        for (unsigned i = 0; i < pg->active.talkgroup_count; i++)
            if (pg->active.talkgroups[i].id == talkgroup &&
                pg->active.talkgroups[i].alias[0])
                return pg->active.talkgroups[i].alias;
#ifndef ESP_PLATFORM
    return p25_calls_host_alias(talkgroup);
#else
    return NULL;
#endif
}
static const char *alias_hook(uint32_t tg)
{
    return tg <= 0xffff ? p25_calls_alias((uint16_t)tg) : NULL;
}

/* ------------------------------------------------------- the geometry -- */

/* The columns of row y inside r that are glass, not the camera: a row the
   hole crosses ends before it (or starts after it, at the left edge). */
static void row_span(tui_rect r, int y, int *x0, int *x1)
{
    *x0 = r.x;
    *x1 = r.x + r.w;
    tui_rect hole;
    if (!ls_tui_cutout(&hole) || y < hole.y || y >= hole.y + hole.h) return;
    if (hole.x + hole.w <= r.x || hole.x >= r.x + r.w) return;
    if (hole.x - r.x >= r.x + r.w - (hole.x + hole.w)) *x1 = hole.x - 1;
    else *x0 = hole.x + hole.w + 1;
}

static bool held(uint16_t tg) { return tg && P25.hold_tg == tg; }
static bool locked(uint16_t tg)
{
    for (unsigned i = 0; i < P25.lockout_count && i < 64; i++)
        if (P25.lockout[i] == tg) return true;
    return false;
}
static bool encrypted_tg(uint16_t tg)
{
    for (unsigned i = 0; i < P25.p25_enc_tg_count && i < P25_STATE_ENC_TG_MAX; i++)
        if (P25.p25_enc_tg[i].talkgroup == tg) return true;
    return false;
}

static void clock_text(char *out, size_t n, uint32_t ms)
{
    const unsigned s = ms / 1000;
    if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, n, "%u:%02u", s / 60, s % 60);
}

static void age_text(char *out, size_t n, int64_t us)
{
    const int64_t s = us / 1000000;
    if (s < 5) snprintf(out, n, "now");
    else if (s < 60) snprintf(out, n, "%ds", (int)s);
    else if (s < 3600) snprintf(out, n, "%dm", (int)(s / 60));
    else snprintf(out, n, "%dh", (int)(s / 3600));
}

/* A '=' rule across r at y with a title set into it, as CALLS draws its
   bands, routed round the camera. */
static void band(tui_surface *sf, tui_rect r, int y, const char *title,
                 uint8_t hue, uint8_t title_attr)
{
    int x0, x1;
    row_span(r, y, &x0, &x1);
    for (int x = x0; x < x1; x++)
        tui_put_char(sf, r, x, y, '=', TUI_ATTR(hue, TUI_BLACK));
    if (title && x1 - x0 > (int)strlen(title) + 6) {
        char cap[40];
        snprintf(cap, sizeof(cap), "[ %s ]", title);
        tui_put_str(sf, r, x0 + 2, y, cap, title_attr);
    }
}

/* ---------------------------------------------------------- the card --- */

#define CARD_ROWS 10

static int card_rows(int w) { return w >= 30 ? CARD_ROWS : 0; }

/* The trace: two rows of braille, eight levels, two samples a cell, the
   newest at the right. Voice fills down to the floor, an envelope; a
   carrier (`line`) is only its top dot, a level drawn as a line. */
static void draw_trace(tui_surface *sf, tui_rect clip, int x, int y, int w,
                       uint8_t attr, bool line)
{
    if (w < 2) return;
    const int samples = w * 2;
    for (int c = 0; c < w; c++) {
        unsigned top = 0, bottom = 0;
        for (int half = 0; half < 2; half++) {
            const int age = samples - 1 - (c * 2 + half);   /* 0 = newest */
            if (age >= s_trace_n) continue;
            const uint8_t v = s_trace[(s_trace_head - 1 - age + TRACE_N * 2) % TRACE_N];
            unsigned level = (v * 8u + 254u) / 255u;          /* 0..8, any sound shows */
            if (level > 8) level = 8;
            if (line && level) {
                if (level > 4) top |= ls_braille_dot(level - 5, half);
                else bottom |= ls_braille_dot(level - 1, half);
                continue;
            }
            bottom |= ls_braille_column(level > 4 ? 4 : level, half);
            if (level > 4) top |= ls_braille_column(level - 4, half);
            /* A dot on the floor for silence inside the call, so a pause
               reads as the call breathing, not as the trace ending. */
            if (!level) bottom |= ls_braille_dot(0, half);
        }
        if (top) ls_braille_put(sf, clip, x + c, y, (int16_t)(0x2800 | top), attr);
        if (bottom) ls_braille_put(sf, clip, x + c, y + 1, (int16_t)(0x2800 | bottom), attr);
    }
}

static void card_draw(tui_surface *sf, tui_rect r)
{
    const int64_t now = esp_timer_get_time();
    const bool on = s_now.on;
    const p25_call_t *c = on ? &s_now : &s_last;
    const bool any = c->tg != 0;
    const uint8_t hue = !any ? TUI_CYAN : c->emergency ? TUI_RED : ls_calls_tg_hue(c->tg);
    /* On air is bright and in the talkgroup's colour; the last call keeps
       its colour, dimmed; nothing heard is the receiver's cyan. */
    const uint8_t ink = TUI_ATTR(on ? (hue | TUI_BRIGHT) : hue, TUI_BLACK);
    const uint8_t pulse = on ? ls_fresh_level(&s_arrival, 1200) : 0;
    const char *title = on ? (c->emergency ? "EMERGENCY" : "ON AIR")
                      : any ? "LAST CALL" : "LISTENING";
    band(sf, r, r.y, title, hue,
         TUI_ATTR(pulse >= 128 ? (TUI_WHITE | TUI_BRIGHT) : (hue | TUI_BRIGHT),
                  pulse >= 128 ? hue : TUI_BLACK));

    /* The chips on the band, right to left: how it is carried and whether
       it can be heard. */
    if (any) {
        const char *chip[4];
        uint8_t chue[4];
        int n = 0;
        chip[n] = c->p2 ? "P2" : "P1"; chue[n++] = c->p2 ? TUI_MAGENTA : TUI_BLUE;
        chip[n] = c->enc ? "ENC" : "CLEAR"; chue[n++] = c->enc ? TUI_RED : TUI_GREEN;
        if (held(c->tg)) { chip[n] = "HOLD"; chue[n++] = TUI_YELLOW; }
        if (c->emergency) { chip[n] = "EMERG"; chue[n++] = TUI_RED; }
        int width = 0;
        for (int i = 0; i < n; i++) width += (int)strlen(chip[i]) + 3;
        int x0, x1;
        row_span(r, r.y, &x0, &x1);
        int x = x1 - width - 1;
        if (x > x0 + (int)strlen(title) + 7)
            for (int i = 0; i < n; i++) x = ls_calls_chip(sf, r, x, r.y, chip[i], chue[i]);
    }

    const int big_y = r.y + 2;
    char text[96], clock[16];
    if (!any) {
        ls_safe_line(sf, r, big_y, "No calls heard yet", LS_ATTR_DIM);
        snprintf(text, sizeof(text), "%.4f MHz  %s", s_tune_freq_hz / 1e6,
                 P25.dsd_has_sync ? "synced, waiting for a call"
                                  : "no sync - nothing to hear");
        ls_safe_line(sf, r, big_y + 1, text, LS_ATTR_FAINT);
    } else {
        const char *alias = p25_calls_alias(c->tg);
        const uint32_t ms = (uint32_t)(((on ? now : c->last_us) - c->start_us) / 1000);
        clock_text(clock, sizeof(clock), ms);
        /* Big: the talkgroup's name when it fits beside the clock, else the
           name alone, else the number with the clock. */
        const int room = r.w - 4;
        const int clock_w = ls_calls_big_width(clock);
        char shout[24];
        bool big_clock = true, big_alias = false;
        if (alias && ls_calls_big_width(alias) + clock_w + 3 <= room) big_alias = true;
        else if (alias && ls_calls_big_width(alias) <= room) { big_alias = true; big_clock = false; }
        if (big_alias) snprintf(shout, sizeof(shout), "%s", alias);
        else snprintf(shout, sizeof(shout), "TG %u", c->tg);
        if (ls_calls_big_width(shout) + clock_w + 3 > room) big_clock = false;
        int bx0, bx1, left = r.x;
        for (int k = 0; k < LS_CALLS_BIG_ROWS; k++) {
            row_span(r, big_y + k, &bx0, &bx1);
            if (bx0 > left) left = bx0;
        }
        ls_calls_big_text(sf, r, left + 2, big_y, shout, ink);
        if (big_clock)
            ls_calls_big_text(sf, r, r.x + r.w - 2 - clock_w, big_y, clock,
                              TUI_ATTR(on ? (TUI_YELLOW | TUI_BRIGHT) : TUI_YELLOW, TUI_BLACK));

        /* The line under it: whatever the headline did not say. */
        int y = big_y + LS_CALLS_BIG_ROWS + 1, x = r.x + 2;
        int x0, x1;
        row_span(r, y, &x0, &x1);
        if (x < x0 + 1) x = x0 + 1;
        snprintf(text, sizeof(text), "TG %u", c->tg);
        tui_put_str(sf, r, x, y, text, ink);
        x += (int)strlen(text) + 2;
        if (alias && !big_alias) {
            tui_put_str(sf, tui_rect_make(x, y, x1 - x - 1, 1), x, y, alias,
                        TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
            x += (int)strlen(alias) + 2;
        }
        if (c->unit) {
            snprintf(text, sizeof(text), "UNIT %lu", (unsigned long)c->unit);
            if (x + (int)strlen(text) < x1 - 1) {
                tui_put_str(sf, r, x, y, text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
                x += (int)strlen(text) + 2;
            }
        }
        /* The right end: the clock when it is not big, or how long ago. */
        if (!on) {
            char ago[12];
            age_text(ago, sizeof(ago), now - c->last_us);
            if (!strcmp(ago, "now"))
                snprintf(text, sizeof(text), "%s%sended just now",
                         big_clock ? "" : clock, big_clock ? "" : ", ");
            else
                snprintf(text, sizeof(text), "%s%sended %s ago",
                         big_clock ? "" : clock, big_clock ? "" : ", ", ago);
        } else snprintf(text, sizeof(text), "%s", big_clock ? "" : clock);
        int limit = x1 - 1;
        if (text[0] && x1 - 1 - (int)strlen(text) > x) {
            limit = x1 - 1 - (int)strlen(text);
            tui_put_str(sf, r, limit, y, text,
                        on ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
        }
        /* The talker's own alias, last: it yields to the time. */
        if (c->talker[0] && x + (int)strlen(c->talker) + 2 < limit - 1) {
            snprintf(text, sizeof(text), "\"%s\"", c->talker);
            tui_put_str(sf, r, x, y, text, TUI_ATTR(TUI_CYAN, TUI_BLACK));
        }

        /* The trace: VOICE while the call can be heard, SIGNAL while it is
           encrypted and only its carrier can. */
        y += 1;
        row_span(r, y, &x0, &x1);
        int x1b;
        row_span(r, y + 1, &x0, &x1b);
        if (x1b < x1) x1 = x1b;
        const char *label = c->enc ? "SIGNAL" : "VOICE";
        int lx0, lx1;
        row_span(r, y + 1, &lx0, &lx1);
        if (lx0 < x0) lx0 = x0;
        tui_put_str(sf, r, lx0 + 2, y + 1, label,
                    on ? TUI_ATTR(c->enc ? TUI_RED : TUI_CYAN, TUI_BLACK) : LS_ATTR_FAINT);
        const int tx = lx0 + 2 + 7;
        if (c == &s_now || s_trace_n)
            draw_trace(sf, r, tx, y, x1 - tx - 1,
                       on ? TUI_ATTR(c->enc ? TUI_RED | TUI_BRIGHT : hue | TUI_BRIGHT, TUI_BLACK)
                          : LS_ATTR_FAINT, c->enc);
    }

    /* The foot: where the call is carried, and the site. */
    const int fy = r.y + r.h - 1;
    band(sf, r, fy, NULL, hue, 0);
    if (any || P25.dsd_has_sync) {
        const uint64_t hz = any ? c->hz : s_tune_freq_hz;
        if (p25_nac_valid(&P25))
            LS_SNPRINTF(t_p25_calls_live_4f_mhz_nac_03x, text, sizeof(text), " %.4f MHz  NAC %03X ", hz / 1e6, P25.dsd_nac);
        else
            snprintf(text, sizeof(text), " %.4f MHz ", hz / 1e6);
        if (c->enc && any && c->algid && c->algid != 0x80)
            snprintf(text + strlen(text), sizeof(text) - strlen(text), " ALG %02X KEY %04X ",
                     c->algid, c->kid);
        int x0, x1;
        row_span(r, fy, &x0, &x1);
        if (x1 - x0 > (int)strlen(text) + 4)
            tui_put_str(sf, r, x1 - (int)strlen(text) - 2, fy, text,
                        TUI_ATTR(TUI_WHITE, TUI_BLACK));
    }
}

/* ------------------------------------------------------------ the keys -- */

static uint16_t card_tg(void) { return s_now.on ? s_now.tg : s_last.tg; }

static int live_keys(ls_calls_live_key_t out[2])
{
    const uint16_t tg = card_tg();
    if (P25.hold_tg) {
        snprintf(out[0].label, sizeof(out[0].label), "RELEASE %u", P25.hold_tg);
        out[0].on = true; out[0].can = true;
    } else {
        snprintf(out[0].label, sizeof(out[0].label), tg ? "HOLD %u" : "HOLD", tg);
        out[0].on = false; out[0].can = tg != 0;
    }
    if (tg && locked(tg)) {
        snprintf(out[1].label, sizeof(out[1].label), "UNLOCK %u", tg);
        out[1].on = true;
    } else {
        snprintf(out[1].label, sizeof(out[1].label), tg ? "LOCKOUT %u" : "LOCKOUT", tg);
        out[1].on = false;
    }
    out[1].can = tg != 0;
    return 2;
}

static void live_press(int index)
{
    const uint16_t tg = card_tg();
    if (index == 0) {
        (void)p25_ui_policy(P25_UI_HOLD_SET, P25.hold_tg ? 0 : tg);
    } else if (index == 1 && tg) {
        if (locked(tg)) (void)p25_ui_lockout_remove(tg);
        else (void)p25_ui_policy(P25_UI_LOCKOUT_ADD, tg);
    }
}

/* ----------------------------------------------------------- the strip -- */

/* The observed store, merged across channels by talkgroup and ranked by
   hits in the last sixteen minutes. Read once a second: the store only
   moves on decoded frames and a second is finer than anyone reads ages. */
typedef struct {
    uint16_t tg;
    uint32_t recent, last_ms;
    uint8_t  minute[P25_TG_OBSERVED_MINUTES];   /* [0] oldest .. newest */
} strip_row_t;
static EXT_RAM_BSS_ATTR p25_tg_observed_snapshot_t s_seen;
static EXT_RAM_BSS_ATTR strip_row_t s_strip[P25_TG_OBSERVED_MAX];
static int s_strip_n;
static int64_t s_strip_read_us;
static tui_rect s_strip_area;
/* Badge, age and hits, from the right edge in. */
#define STRIP_TAIL 16
static int s_strip_first_row, s_strip_shown;

static void strip_refresh(int64_t now_us)
{
    if (s_strip_read_us && now_us >= s_strip_read_us && now_us - s_strip_read_us < 1000000)
        return;
    if (!p25_tg_observed_read(&s_seen)) return;   /* busy: keep the last read */
    s_strip_read_us = now_us;
    const uint32_t now_ms = (uint32_t)(now_us / 1000);
    s_strip_n = 0;
    for (unsigned i = 0; i < s_seen.count && i < P25_TG_OBSERVED_MAX; i++) {
        const p25_tg_observed_row_t *o = &s_seen.rows[i];
        int k = 0;
        while (k < s_strip_n && s_strip[k].tg != o->talkgroup) k++;
        if (k == s_strip_n) {
            memset(&s_strip[k], 0, sizeof(s_strip[k]));
            s_strip[k].tg = o->talkgroup;
            s_strip_n++;
        }
        strip_row_t *row = &s_strip[k];
        if (o->last_seen_ms > row->last_ms) row->last_ms = o->last_seen_ms;
        for (unsigned ago = 0; ago < P25_TG_OBSERVED_MINUTES; ago++) {
            const unsigned h = p25_tg_observed_minute_hits(o, now_ms, ago);
            const unsigned slot = P25_TG_OBSERVED_MINUTES - 1 - ago;
            const unsigned sum = row->minute[slot] + h;
            row->minute[slot] = (uint8_t)(sum > 255 ? 255 : sum);
            row->recent += h;
        }
    }
    /* Busiest first; a tie goes to the more recent. Thirty-two rows. */
    for (int i = 1; i < s_strip_n; i++) {
        strip_row_t t = s_strip[i];
        int j = i - 1;
        while (j >= 0 && (s_strip[j].recent < t.recent ||
               (s_strip[j].recent == t.recent && s_strip[j].last_ms < t.last_ms))) {
            s_strip[j + 1] = s_strip[j];
            j--;
        }
        s_strip[j + 1] = t;
    }
}

static int strip_rows(int w, int most)
{
    if (w < 36 || most < 3) return 0;
    strip_refresh(esp_timer_get_time());
    int n = s_strip_n ? s_strip_n : 1;
    if (n > 8) n = 8;
    if (n > most - 1) n = most - 1;
    return n + 1;
}

static void strip_draw(tui_surface *sf, tui_rect r)
{
    const int64_t now_us = esp_timer_get_time();
    strip_refresh(now_us);
    s_strip_area = r;
    s_strip_first_row = r.y + 1;
    s_strip_shown = 0;
    band(sf, r, r.y, "BUSIEST / 16 MIN", TUI_CYAN, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    {
        int x0, x1;
        row_span(r, r.y, &x0, &x1);
        if (x1 - x0 >= 46)
            tui_put_str(sf, r, x1 - 17, r.y, " HITS  AGE ", TUI_ATTR(TUI_CYAN, TUI_BLACK));
    }
    if (!s_strip_n) {
        ls_safe_line(sf, r, r.y + 1,
                     P25.dsd_has_sync ? "synced, no talkgroups heard yet"
                                      : "no sync - nothing to hear",
                     LS_ATTR_DIM);
        return;
    }
    unsigned top = 1;
    for (int i = 0; i < s_strip_n; i++)
        for (int m = 0; m < P25_TG_OBSERVED_MINUTES; m++)
            if (s_strip[i].minute[m] > top) top = s_strip[i].minute[m];
    const uint32_t now_ms = (uint32_t)(now_us / 1000);
    const uint16_t on_tg = s_now.on ? s_now.tg : 0;
    for (int i = 0; i < s_strip_n && r.y + 1 + i < r.y + r.h; i++) {
        const strip_row_t *row = &s_strip[i];
        const int y = r.y + 1 + i;
        int x0, x1;
        row_span(r, y, &x0, &x1);
        const uint32_t age_ms = now_ms - row->last_ms;
        /* Brightness is freshness: heard in the last half minute is lit,
           the last five minutes is plain, older is faint. */
        const bool hot = age_ms < 30000, warm = age_ms < 300000;
        const uint8_t hue = ls_calls_tg_hue(row->tg);
        const uint8_t id_ink = TUI_ATTR(hot ? hue | TUI_BRIGHT : warm ? hue : LS_FAINT_FG, TUI_BLACK);
        int x = x0 + 1;
        if (row->tg == on_tg)
            tui_put_char(sf, r, x, y, '>', TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        char text[32];
        snprintf(text, sizeof(text), "%5u", row->tg);
        tui_put_str(sf, r, x + 1, y, text, id_ink);
        x += 8;
        /* Fixed columns from the right - badge, age, hits, sparkline - so
           the rows read down as a table; the name gets what is left. */
        int right = x1 - 1 - STRIP_TAIL;
        const char *badge = locked(row->tg) ? "LOCK" : held(row->tg) ? "HOLD"
                          : encrypted_tg(row->tg) ? "ENC" : NULL;
        if (badge)
            tui_put_str(sf, r, x1 - 5, y, badge,
                        TUI_ATTR(!strcmp(badge, "ENC") ? TUI_RED | TUI_BRIGHT
                                 : !strcmp(badge, "HOLD") ? TUI_YELLOW | TUI_BRIGHT
                                 : LS_FAINT_FG, TUI_BLACK));
        age_text(text, sizeof(text), (int64_t)age_ms * 1000);
        tui_put_str(sf, r, x1 - 6 - (int)strlen(text), y, text,
                    hot ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
        snprintf(text, sizeof(text), "%lu", (unsigned long)row->recent);
        tui_put_str(sf, r, x1 - 11 - (int)strlen(text), y, text,
                    TUI_ATTR(TUI_YELLOW | (hot ? TUI_BRIGHT : 0), TUI_BLACK));
        /* One cell a minute, newest at the right: as many minutes as the
           width leaves after a ten-letter name, sixteen at most. */
        int minutes = right - x - 12;
        if (minutes > P25_TG_OBSERVED_MINUTES) minutes = P25_TG_OBSERVED_MINUTES;
        if (minutes >= 6) {
            right -= minutes;
            const uint8_t ink = TUI_ATTR(hot ? hue | TUI_BRIGHT : warm ? hue : LS_FAINT_FG,
                                         TUI_BLACK);
            for (int cell = 0; cell < minutes; cell++) {
                const unsigned v = row->minute[P25_TG_OBSERVED_MINUTES - minutes + cell];
                unsigned bits;
                if (v) {
                    unsigned h = (unsigned)ceilf(sqrtf((float)v / (float)top) * 4.0f);
                    if (h < 1) h = 1;
                    if (h > 4) h = 4;
                    bits = ls_braille_column(h, 0) | ls_braille_column(h, 1);
                } else {
                    bits = ls_braille_dot(0, 0) | ls_braille_dot(0, 1);
                }
                ls_braille_put(sf, r, right + cell, y, (int16_t)(0x2800 | bits),
                               v ? ink : LS_ATTR_FAINT);
            }
            right -= 1;
        }
        const char *alias = p25_calls_alias(row->tg);
        if (alias && right - 1 > x)
            tui_put_str(sf, tui_rect_make(x, y, right - 1 - x, 1), x, y, alias,
                        TUI_ATTR(hot ? (TUI_WHITE | TUI_BRIGHT) : LS_DIM_FG, TUI_BLACK));
        s_strip_shown = i + 1;
    }
}

static uint32_t strip_hit(int col, int row)
{
    (void)col;
    const int i = row - s_strip_first_row;
    if (!tui_rect_contains(s_strip_area, col, row) || i < 0 || i >= s_strip_shown) return 0;
    return s_strip[i].tg;
}

/* ------------------------------------------------------------- the hook -- */

static const ls_calls_live_t LIVE = {
    .title = "P25 CALLS",
    .hue = TUI_YELLOW,
    .card_rows = card_rows,
    .card_draw = card_draw,
    .strip_rows = strip_rows,
    .strip_draw = strip_draw,
    .strip_hit = strip_hit,
    .keys = live_keys,
    .press = live_press,
    .alias = alias_hook,
};

void p25_calls_live_register(void) { ls_calls_set_live("P25", &LIVE); }
