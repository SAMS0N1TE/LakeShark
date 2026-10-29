/* METERS: the utility meters around you, read off the air.

   Itron ERT meters (SCM, SCM+, IDM) on 902-928 MHz, decoded by rtl_433's
   decoders through rec_ism: on an SDR from REC's stream (rec_ism_listen), on
   the LR2021 by LR433's ERT session. Every meter heard from any radio lands
   in the store (ls_meters_rt.c), WATCH and LR433 included.

   The look is a meter's own: the selected one's register in stroke figures
   with its last wheel lit, the use per hour as braille bars under it, and a
   row per meter with its mechanical counter. A lamp lights for a moment when
   a reading lands and the screen is still otherwise. */

#include "../../ls_tui_screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../ls_tui_ui.h"
#include "../../ls_braille.h"
#include "../../ls_glyph.h"
#include "../../ls_options.h"
#include "../../ls_picker.h"
#include "../../ls_radio_select.h"
#include "../../ls_experiments.h"
#include "../../ls_meters.h"
#include "rec_ism.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_textpack.h"

#define INK(c)  TUI_ATTR((c), TUI_BLACK)
#define CHROME  INK(TUI_CYAN)
#define VALUE   INK(TUI_WHITE | TUI_BRIGHT)
#define NOTE    LS_ATTR_DIM
#define GHOST   LS_ATTR_FAINT
#define WHEEL   INK(TUI_YELLOW | TUI_BRIGHT)
#define LAMP_US 2000000

static EXT_RAM_BSS_ATTR lsm_store_t s_view;
static EXT_RAM_BSS_ATTR float s_use[256];
static uint32_t s_seq = UINT32_MAX;
static uint8_t s_order[LSM_MAX];
static int s_n;
static int64_t s_lamp_until;      /* a reading landed: the lamp is lit */
static int64_t s_last_heard;      /* the newest last_s, to tell a landing */
static uint8_t s_sel_proto;
static char s_sel_id[16];
static char s_hint[96] = "R radio  L listen  P pin  N name  O options";

/* What is listening, and how. */
typedef enum { SRC_NONE, SRC_SDR, SRC_LR } src_t;
static src_t s_src;
static bool s_listen = true;      /* listening while open, unless paused */
static char s_why[64];

/* SDR centres: rtlamr's 912.6 first, then the rest of the band in steps a
   256 kS/s capture can see; SCAN visits them all. */
static const uint32_t CENTRES[] = { 912600000u, 911000000u, 914600000u, 916500000u, 919500000u };
#define N_CENTRES ((int)(sizeof(CENTRES) / sizeof(CENTRES[0])))
static const char *const CENTRE_NAMES[N_CENTRES + 1] = { "912.6", "911.0", "914.6", "916.5", "919.5", "SCAN" };
static int s_centre;
static int s_dwell_s = 30;
static int s_show;                /* 0 all, 1 pinned only */

/* ----------------------------------------------------------- receiving -- */

/* METERS' own choice, first taken from SUB-GHZ READ's: picking one here
   does not move SUB-GHZ's capture source under it. */
static ls_rsel_radio_t s_radio = LS_RSEL_NONE;
static ls_rsel_radio_t radio(void)
{
    if (s_radio == LS_RSEL_NONE) s_radio = ls_rsel_resolved(LS_RSEL_SUBGHZ_READ);
    return s_radio;
}

static const ls_experiment_t *lr433(void) { return ls_exp_find("lr433"); }

static void stop(void)
{
    if (s_src == SRC_SDR) rec_ism_listen(NULL, 0, 0);
    if (s_src == SRC_LR) {
        const ls_experiment_t *e = lr433();
        if (e && (ls_exp_running() == e || ls_exp_wanted() == e)) ls_exp_stop();
    }
    s_src = SRC_NONE;
}

static void start(void)
{
    stop();
    s_why[0] = 0;
    if (!s_listen) return;
    const ls_rsel_radio_t r = radio();
    if (r == LS_RSEL_SDR_RTL || r == LS_RSEL_SDR_HACKRF) {
        ls_tui_radio_want("REC");
        const bool scan = s_centre >= N_CENTRES;
        if (rec_ism_listen(scan ? CENTRES : &CENTRES[s_centre], scan ? N_CENTRES : 1, (uint32_t)s_dwell_s * 1000u))
            s_src = SRC_SDR;
        else LS_SNPUTS(t_scr_meters_stop_sub_ghz_watch, s_why, sizeof(s_why), "Stop SUB-GHZ WATCH first");
    } else if (r == LS_RSEL_LORA) {
        const ls_experiment_t *e = lr433();
        char arg[] = "ert", *argv[] = { arg };
        char why[64] = "";
        if (e && e->configure && e->configure(1, argv, why, sizeof(why))) { ls_exp_start(e); s_src = SRC_LR; }
        else snprintf(s_why, sizeof(s_why), "%s", why[0] ? why : "LR433 is not in this build");
    } else {
        LS_SNPRINTF(t_scr_meters_s_cannot_hear_ert, s_why, sizeof(s_why), "%s cannot hear ERT; pick an SDR or the LR2021", ls_rsel_name(r));
    }
}

/* ------------------------------------------------------------ options -- */

static int o_get_centre(const ls_opt_t *o) { (void)o; return s_centre; }
static void o_set_centre(const ls_opt_t *o, int v) { (void)o; s_centre = v >= 0 && v <= N_CENTRES ? v : 0; start(); }
static double o_dwell(const ls_opt_t *o) { (void)o; return s_dwell_s; }
static void o_set_dwell(const ls_opt_t *o, double v) { (void)o; s_dwell_s = (int)(v + 0.5); if (s_centre >= N_CENTRES) start(); }
static void o_show_dwell(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%d s a centre", s_dwell_s); }
static const char *const SHOW_NAMES[] = { "ALL", "PINNED" };
static int o_get_show(const ls_opt_t *o) { (void)o; return s_show; }
static void o_set_show(const ls_opt_t *o, int v) { (void)o; s_show = v ? 1 : 0; s_seq = UINT32_MAX; }
static void o_clear(const ls_opt_t *o) { (void)o; ls_meters_clear(); }
static void o_show_clear(const ls_opt_t *o, char *out, size_t n) { (void)o; LS_SNPUTS(t_scr_meters_meters_not_pinned_or, out, n, "meters not pinned or named"); }

static const ls_opt_t OPTS[] = {
    { .label = "CENTRE MHz", .kind = LS_OPT_CYCLE, .radios = LS_OPT_SDR, .names = CENTRE_NAMES, .n = N_CENTRES + 1,
      .get = o_get_centre, .set = o_set_centre },
    { .label = "SCAN DWELL", .kind = LS_OPT_LEVEL, .radios = LS_OPT_SDR, .num = o_dwell, .set_num = o_set_dwell,
      .lo = 10, .hi = 300, .step = 10, .unit = "seconds at each centre", .show = o_show_dwell },
    { .label = "SHOW", .kind = LS_OPT_CYCLE, .names = SHOW_NAMES, .n = 2, .get = o_get_show, .set = o_set_show },
    { .label = "CLEAR", .kind = LS_OPT_ACTION, .act = o_clear, .show = o_show_clear },
};
/* The rows follow METERS' own radio, so .radio is set before each use. */
static ls_opt_ctx_t CTX = { .name = "METERS", .job = -1, .radio = LS_RSEL_NONE,
                                  .back_to_screen = true, LS_OPT_ROWS(OPTS) };

/* ----------------------------------------------------------- the store -- */

static const lsm_meter_t *selected(void)
{
    for (int i = 0; i < s_n; i++) {
        const lsm_meter_t *m = &s_view.m[s_order[i]];
        if (m->proto == s_sel_proto && !strcmp(m->id, s_sel_id)) return m;
    }
    return s_n ? &s_view.m[s_order[0]] : NULL;
}

static void pick(const lsm_meter_t *m)
{
    if (!m) return;
    s_sel_proto = m->proto;
    snprintf(s_sel_id, sizeof(s_sel_id), "%s", m->id);
}

static void refresh(int64_t now_us)
{
    if (!ls_meters_snapshot(&s_view, &s_seq)) return;
    uint8_t all[LSM_MAX];
    const int n = lsm_order(&s_view, all);
    s_n = 0;
    int64_t newest = 0;
    for (int i = 0; i < n; i++) {
        const lsm_meter_t *m = &s_view.m[all[i]];
        if (m->last_s > newest) newest = m->last_s;
        if (!s_show || m->pinned) s_order[s_n++] = all[i];
    }
    if (newest > s_last_heard) {
        if (s_last_heard) s_lamp_until = now_us + LAMP_US;
        s_last_heard = newest;
    }
}

/* Each protocol keeps its hue everywhere it appears. */
static uint8_t proto_hue(uint8_t proto)
{
    return proto == LR433_P_ERT_SCMPLUS ? TUI_CYAN : proto == LR433_P_ERT_IDM ? TUI_MAGENTA : TUI_GREEN;
}

static void fmt_rate(char *out, size_t n, double r)
{
    if (isnan(r)) snprintf(out, n, "--");
    else if (r >= 10000) snprintf(out, n, "+%.0fk", r / 1000);
    else if (r >= 100) snprintf(out, n, "+%.0f", r);
    else snprintf(out, n, "+%.1f", r);
}

static void fmt_ago(char *out, size_t n, int64_t s)
{
    if (s < 0) snprintf(out, n, "--");
    else if (s < 60) snprintf(out, n, "%llds", (long long)s);
    else if (s < 3600) snprintf(out, n, "%lldm", (long long)(s / 60));
    else if (s < 86400) snprintf(out, n, "%lldh", (long long)(s / 3600));
    else snprintf(out, n, "%lldd", (long long)(s / 86400));
}

static const char *title_of(const lsm_meter_t *m) { return m->name[0] ? m->name : "METER"; }

/* --------------------------------------------------------------- parts -- */

static void str(tui_surface *sf, tui_rect clip, int x, int y, int w, const char *s, uint8_t at)
{
    if (w > 0) tui_put_str(sf, tui_rect_intersect(clip, tui_rect_make(x, y, w, 1)), x, y, s, at);
}

static void panel(tui_surface *sf, tui_rect r, const char *chip, uint8_t chip_hue, const char *note, uint8_t note_at)
{
    if (r.w < 8 || r.h < 2) return;
    tui_box(sf, r, NULL, CHROME);
    if (chip) str(sf, r, r.x + 2, r.y, r.w - 4, chip, TUI_ATTR(TUI_BLACK, chip_hue));
    if (note && *note) {
        char t[64];
        snprintf(t, sizeof(t), " %s ", note);
        const int n = (int)strlen(t), room = r.w - 4 - (chip ? (int)strlen(chip) + 2 : 0);
        if (n <= room) str(sf, r, r.x + r.w - 2 - n, r.y, n, t, note_at);
    }
}

/* The mechanical counter: [0|5|6|2|4|5|6], the last wheel lit. */
static int counter(tui_surface *sf, tui_rect clip, int x, int y, int64_t v, int digits, bool fresh)
{
    char d[24];
    if (v < 0) memset(d, '-', (size_t)digits), d[digits] = 0;
    else snprintf(d, sizeof(d), "%0*lld", digits, (long long)v);
    const int n = (int)strlen(d);
    tui_put_char(sf, clip, x, y, '[', CHROME);
    for (int i = 0; i < n; i++) {
        tui_put_char(sf, clip, x + 1 + 2 * i, y, d[i], i == n - 1 ? (fresh ? WHEEL : INK(TUI_YELLOW)) : VALUE);
        tui_put_char(sf, clip, x + 2 + 2 * i, y, i == n - 1 ? ']' : '|', CHROME);
    }
    return 2 * n + 1;
}

/* Use per bucket as braille bars, `rows` tall, two buckets a cell. */
static void usage_bars(tui_surface *sf, tui_rect clip, int x, int y, int w, int rows, const lsm_meter_t *m, uint8_t hue,
                       int64_t *span)
{
    int buckets = w * 2;
    if (buckets > (int)(sizeof(s_use) / sizeof(s_use[0]))) buckets = (int)(sizeof(s_use) / sizeof(s_use[0]));
    *span = lsm_usage(m, buckets, s_use);
    float top = 0;
    for (int b = 0; b < buckets; b++) if (s_use[b] > top) top = s_use[b];
    const int levels = rows * 4;
    for (int c = 0; c < w; c++) {
        for (int r = 0; r < rows; r++) {
            unsigned bits = 0;
            for (int half = 0; half < 2; half++) {
                const int b = c * 2 + half;
                if (b >= buckets || !*span) continue;
                int lv = top > 0 ? (int)ceilf(s_use[b] / top * (float)levels) : 0;
                if (s_use[b] <= 0) lv = 0;
                const int here = lv - (rows - 1 - r) * 4;
                if (here > 0) bits |= ls_braille_column((unsigned)(here > 4 ? 4 : here), half);
            }
            /* An empty stretch keeps a faint floor so the time axis reads. */
            if (!bits && r == rows - 1 && *span) bits = ls_braille_dot(0, 0) | ls_braille_dot(0, 1);
            ls_braille_put(sf, clip, x + c, y + r, bits ? (int16_t)(0x2800 | bits) : ' ',
                           bits == (ls_braille_dot(0, 0) | ls_braille_dot(0, 1)) && r == rows - 1 ? GHOST : INK(hue | TUI_BRIGHT));
        }
    }
}

/* The empty register: a meter face with its four dials, still. */
static void face(tui_surface *sf, tui_rect a, const char *line1, const char *line2)
{
    static const char *const ART[] = {
        " .---------------------. ",
        " |  .-.  .-.  .-.  .-.  | ",
        " | ( | )( / )( - )( \\ ) | ",
        " |  '-'  '-'  '-'  '-'  | ",
        " |   [0|0|0|0|0|0|0]    | ",
        " '-----------+----------' ",
        "             |            ",
    };
    const int h = (int)(sizeof(ART) / sizeof(ART[0])), w = (int)strlen(ART[0]);
    int y = a.y + (a.h - h - 4) / 2;
    if (y < a.y) y = a.y;
    const int x = a.x + (a.w - w) / 2;
    for (int i = 0; i < h; i++) {
        str(sf, a, x, y + i, w, ART[i], i == 4 ? INK(TUI_YELLOW) : i >= 1 && i <= 3 ? INK(TUI_GREEN) : CHROME);
    }
    y += h + 1;
    int n = (int)strlen(line1);
    str(sf, a, a.x + (a.w - n) / 2, y, n, line1, VALUE);
    n = (int)strlen(line2);
    str(sf, a, a.x + (a.w - n) / 2, y + 1, n, line2, NOTE);
}

/* The header: what listens where, and the lamp. */
static void header(tui_surface *sf, tui_rect r, int64_t now_us)
{
    char where[48] = "", state[64];
    uint32_t readings = 0;
    for (int i = 0; i < LSM_MAX; i++) if (s_view.m[i].used) readings += s_view.m[i].heard;
    if (s_src == SRC_SDR) {
        rec_ism_listen_t st;
        rec_ism_listen_status(&st);
        snprintf(where, sizeof(where), "%.3f MHz %s%s", st.hz / 1e6, ls_rsel_name(radio()),
                 st.plan_n > 1 ? " SCAN" : "");
        LS_SNPRINTF(t_scr_meters_lu_bursts_lu_decoded, state, sizeof(state), "%lu bursts  %lu decoded", (unsigned long)st.bursts, (unsigned long)st.decoded);
    } else if (s_src == SRC_LR) {
        snprintf(where, sizeof(where), "912.600 MHz %s ERT", ls_rsel_name(LS_RSEL_LORA));
        ls_exp_state_line(lr433(), state, sizeof(state));
    } else {
        snprintf(where, sizeof(where), "%s", ls_rsel_name(radio()));
        snprintf(state, sizeof(state), "%s", s_why[0] ? s_why : "paused");
    }
    panel(sf, r, " METERS ", TUI_YELLOW | TUI_BRIGHT, where, NOTE);
    if (r.h < 3) return;
    const bool lit = now_us < s_lamp_until;
    str(sf, r, r.x + 2, r.y + 1, 3, lit ? "(*)" : s_src ? "(o)" : "( )",
        lit ? WHEEL : s_src ? INK(TUI_GREEN) : GHOST);
    char line[96];
    LS_SNPRINTF(t_scr_meters_s_d_meter_s, line, sizeof(line), "%s  %d meter%s  %lu reading%s", s_src ? "LISTENING" : "STOPPED", s_n,
             s_n == 1 ? "" : "s", (unsigned long)readings, readings == 1 ? "" : "s");
    str(sf, r, r.x + 6, r.y + 1, r.w - 8, line, s_src ? VALUE : NOTE);
    if (r.h >= 4) str(sf, r, r.x + 6, r.y + 2, r.w - 8, state, NOTE);
}

/* The selected meter at size: register, facts, use. */
static void detail(tui_surface *sf, tui_rect r, const lsm_meter_t *m, int64_t now_s, int64_t now_us)
{
    const uint8_t hue = proto_hue(m->proto);
    char t[64];
    snprintf(t, sizeof(t), " %s%s ", m->pinned ? "* " : "", title_of(m));
    panel(sf, r, t, hue | TUI_BRIGHT, lr433_proto_label(m->proto), INK(hue));
    tui_rect in = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    int y = in.y;
    /* The register behind its window, the last wheel lit for a fresh
       reading: the one big value on the screen. */
    char d[24];
    if (m->value < 0) snprintf(d, sizeof(d), "-------");
    else snprintf(d, sizeof(d), "%07lld", (long long)m->value);
    const int len = (int)strlen(d), gw = len * 4 - 1;
    const bool fresh = now_us < s_lamp_until && m->last_s == s_last_heard;
    if (gw + 4 <= in.w && in.h >= 14) {
        const int gx = in.x + (in.w - gw) / 2, fx = gx - 2, fw = gw + 4;
        tui_put_char(sf, in, fx, y, '.', CHROME);
        tui_hline(sf, in, fx + 1, y, fw - 2, '-', CHROME);
        tui_put_char(sf, in, fx + fw - 1, y, '.', CHROME);
        for (int k = 1; k <= 6; k++) {
            tui_put_char(sf, in, fx, y + k, '|', CHROME);
            tui_put_char(sf, in, fx + fw - 1, y + k, '|', CHROME);
        }
        tui_put_char(sf, in, fx, y + 7, '\'', CHROME);
        tui_hline(sf, in, fx + 1, y + 7, fw - 2, '-', CHROME);
        tui_put_char(sf, in, fx + fw - 1, y + 7, '\'', CHROME);
        for (int i = 0; i < len; i++) {
            const uint8_t at = i == len - 1 ? (fresh ? WHEEL : INK(TUI_YELLOW)) : VALUE;
            if (ls_glyph_has(d[i])) ls_glyph_draw(sf, tui_rect_make(gx + i * 4 - 1, y + 1, 5, 6), d[i], at);
            else tui_put_char(sf, in, gx + i * 4 + 1, y + 4, d[i], at);
        }
        y += 9;
    } else {
        counter(sf, in, in.x + 1, y++, m->value, 7, fresh);
    }
    /* Facts on one line, the rate leading. */
    char rate[16], day[16], ago[12];
    const double rph = lsm_rate_per_hour(m);
    fmt_rate(rate, sizeof(rate), rph);
    fmt_rate(day, sizeof(day), isnan(rph) ? NAN : rph * 24);
    fmt_ago(ago, sizeof(ago), m->last_s ? now_s - m->last_s : -1);
    int x = in.x + 1;
    str(sf, in, x, y, 6, rate, INK(hue | TUI_BRIGHT)); x += (int)strlen(rate) + 1;
    str(sf, in, x, y, 3, "/h", NOTE); x += 4;
    str(sf, in, x, y, 8, day, VALUE); x += (int)strlen(day) + 1;
    str(sf, in, x, y, 5, "/day", NOTE); x += 6;
    snprintf(t, sizeof(t), "ID %s", m->id);
    str(sf, in, x, y, in.x + in.w - x, t, VALUE);
    y++;
    if (y < in.y + in.h) {
        snprintf(t, sizeof(t), "heard %lu  last %s", (unsigned long)m->heard, ago);
        if (m->hz) {
            const size_t k = strlen(t);
            snprintf(t + k, sizeof(t) - k, "  %.3f MHz", m->hz / 1e6);
        }
        str(sf, in, in.x + 1, y++, in.w - 2, t, NOTE);
    }
    /* Use over the history: a scale line, the bars, the time axis. */
    int rows = in.y + in.h - y - 3;
    if (rows > 8) rows = 8;
    if (rows >= 2) {
        y++;
        int64_t span = 0;
        usage_bars(sf, in, in.x + 1, y + 1, in.w - 2, rows, m, hue, &span);
        if (span) {
            float top = 0;
            const int buckets = (in.w - 2) * 2 < 256 ? (in.w - 2) * 2 : 256;
            for (int b = 0; b < buckets; b++) if (s_use[b] > top) top = s_use[b];
            char per[16];
            fmt_ago(per, sizeof(per), span);
            snprintf(t, sizeof(t), "USE per %s", per);
            str(sf, in, in.x + 1, y, 16, t, INK(hue));
            snprintf(t, sizeof(t), "peak %.0f", (double)top);
            str(sf, in, in.x + in.w - 1 - (int)strlen(t), y, (int)strlen(t), t, NOTE);
            y += rows + 1;
            char left[16];
            fmt_ago(left, sizeof(left), span * (int64_t)buckets);
            snprintf(t, sizeof(t), "%s ago", left);
            str(sf, in, in.x + 1, y, 12, t, NOTE);
            str(sf, in, in.x + in.w - 4, y, 3, "now", NOTE);
            for (int k = in.x + 2 + (int)strlen(t); k < in.x + in.w - 5; k++) tui_put_char(sf, in, k, y, '-', GHOST);
        } else {
            str(sf, in, in.x + 1, y, in.w - 2, LS_TEXT(t_scr_meters_use_shows_after_two, "USE shows after two readings a minute apart"), NOTE);
        }
    }
}
/* The list: two rows a meter. Remembers which slot each row is. */
static tui_rect s_list;
static int8_t s_row_slot[160];

static void list(tui_surface *sf, tui_rect r, const lsm_meter_t *sel, int64_t now_s, int64_t now_us)
{
    s_list = r;
    memset(s_row_slot, -1, sizeof(s_row_slot));
    panel(sf, r, " HEARD ", TUI_CYAN, s_show ? "pinned" : NULL, NOTE);
    tui_rect in = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    const int per = 2, fit = in.h / per;
    int first = 0;
    for (int i = 0; i < s_n; i++)
        if (&s_view.m[s_order[i]] == sel && i >= fit) first = i - fit + 1;
    for (int k = 0; k < fit && first + k < s_n; k++) {
        const lsm_meter_t *m = &s_view.m[s_order[first + k]];
        const int y = in.y + k * per;
        const uint8_t hue = proto_hue(m->proto);
        const bool fresh = now_us < s_lamp_until && m->last_s == s_last_heard;
        if (m == sel) ls_fill_dither(sf, tui_rect_make(in.x, y, in.w, per), LS_DITHER_LIGHT, hue);
        for (int j = 0; j < per; j++) if (y - r.y + j < (int)sizeof(s_row_slot)) s_row_slot[y - r.y + j] = (int8_t)s_order[first + k];
        /* row one: marker, name, protocol, ID, age */
        tui_put_char(sf, in, in.x, y, m == sel ? '>' : ' ', INK(hue | TUI_BRIGHT));
        tui_put_char(sf, in, in.x + 1, y, m->pinned ? '*' : ' ', WHEEL);
        str(sf, in, in.x + 3, y, 12, title_of(m), m->name[0] ? VALUE : NOTE);
        str(sf, in, in.x + 16, y, 9, lr433_proto_label(m->proto), INK(hue | (fresh ? TUI_BRIGHT : 0)));
        str(sf, in, in.x + 26, y, in.w - 32, m->id, VALUE);
        char ago[12];
        fmt_ago(ago, sizeof(ago), m->last_s ? now_s - m->last_s : -1);
        str(sf, in, in.x + in.w - 5, y, 5, ago, fresh ? WHEEL : NOTE);
        /* row two: the counter, a short use strip, the rate */
        const int cw = counter(sf, in, in.x + 3, y + 1, m->value, m->value > 9999999 ? 10 : 7, fresh);
        char rate[16];
        fmt_rate(rate, sizeof(rate), lsm_rate_per_hour(m));
        const int rw = (int)strlen(rate) + 2;
        const int sx = in.x + 4 + cw, sw = in.w - (sx - in.x) - rw - 2;
        if (sw >= 4) {
            int64_t span;
            usage_bars(sf, in, sx, y + 1, sw > 16 ? 16 : sw, 1, m, hue, &span);
        }
        str(sf, in, in.x + in.w - rw, y + 1, rw, rate, INK(hue));
        str(sf, in, in.x + in.w - 2, y + 1, 2, "/h", NOTE);
    }
    if (!s_n) str(sf, in, in.x + 1, in.y, in.w - 2, s_show ? "No pinned meters" : "None yet", NOTE);
}

/* ---------------------------------------------------------------- draw -- */

static void draw(tui_surface *sf, tui_rect a)
{
    const int64_t now_us = esp_timer_get_time(), now_s = ls_meters_now_s();
    refresh(now_us);
    CTX.radio = radio();
    const lsm_meter_t *sel = selected();
    if (a.w < 30 || a.h < 16) { ls_panel_notice(sf, a, "METERS", LS_TEXT(t_scr_meters_more_screen_space_needed, "More screen space needed"), "Use a smaller font"); return; }

    ls_btn_t b[] = {
        { "RADIO", ls_rsel_name(radio()), 'r', false, false, false },
        { s_src ? "LISTEN" : "PAUSED", s_src ? "ON" : "OFF", 'l', s_src != SRC_NONE, false, false },
        { sel && sel->pinned ? "UNPIN" : "PIN", "METER", 'p', sel && sel->pinned, !sel, false },
        { "NAME", sel && sel->name[0] ? sel->name : "METER", 'n', false, !sel, false },
        ls_opt_button(&CTX),
    };
    const int bh = ls_btn_raised_height(a, 5);
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y, a.w, bh), b, 5, -1);
    a.y += bh;
    a.h -= bh;

    const int hh = 4;
    header(sf, tui_rect_make(a.x, a.y, a.w, hh), now_us);
    a.y += hh;
    a.h -= hh;
    if (!s_n) {
        char l1[64], l2[80];
        if (s_src == SRC_SDR) {
            rec_ism_listen_t st;
            rec_ism_listen_status(&st);
            LS_SNPRINTF(t_scr_meters_listening_at_3f_mhz, l1, sizeof(l1), "Listening at %.3f MHz", st.hz / 1e6);
            LS_SNPUTS(t_scr_meters_sdr_sees_0_25, l2, sizeof(l2), "SDR sees 0.25 MHz of 902-928; meters hop");
        } else if (s_src == SRC_LR) {
            LS_SNPUTS(t_scr_meters_listening_at_912_600, l1, sizeof(l1), "Listening at 912.600 MHz, 2.2 MHz wide");
            LS_SNPUTS(t_scr_meters_meters_send_every_30, l2, sizeof(l2), "Meters send every 30 s to a few minutes");
        } else {
            snprintf(l1, sizeof(l1), "%s", s_why[0] ? s_why : "Paused");
            LS_SNPUTS(t_scr_meters_listen_starts_it_radio, l2, sizeof(l2), "LISTEN starts it; RADIO picks SDR or LR2021");
        }
        face(sf, a, l1, l2);
        return;
    }
    const bool wide = a.w >= 2 * a.h + 10 && a.w >= 90;
    if (wide) {
        const int dw = a.w * 11 / 20;
        list(sf, tui_rect_make(a.x, a.y, a.w - dw, a.h), sel, now_s, now_us);
        detail(sf, tui_rect_make(a.x + a.w - dw, a.y, dw, a.h), sel, now_s, now_us);
    } else {
        /* The list takes what its meters need, the register the rest. */
        const int need = 2 + 2 * (s_n < 4 ? 4 : s_n);
        int dh = a.h - need;
        if (dh > 24) dh = 24;   /* all the register panel draws */
        if (dh < 14) dh = a.h >= 24 ? 14 : a.h >= 18 ? 9 : 0;
        if (dh) detail(sf, tui_rect_make(a.x, a.y, a.w, dh), sel, now_s, now_us);
        list(sf, tui_rect_make(a.x, a.y + dh, a.w, a.h - dh), sel, now_s, now_us);
    }
}

/* ------------------------------------------------------------- control -- */

static const char *const NAMES[] = { "HOUSE", "NEIGHBOUR", "GARAGE", "SHOP", "WATER", "GAS", "POWER", "(none)" };
#define N_NAMES ((int)(sizeof(NAMES) / sizeof(NAMES[0])))

static void named(int i)
{
    if (i < 0 || i >= N_NAMES) return;
    ls_meters_name(s_sel_proto, s_sel_id, i == N_NAMES - 1 ? "" : NAMES[i]);
}

static void move(int dir)
{
    if (!s_n) return;
    int at = 0;
    const lsm_meter_t *sel = selected();
    for (int i = 0; i < s_n; i++) if (&s_view.m[s_order[i]] == sel) at = i;
    pick(&s_view.m[s_order[(at + dir + s_n) % s_n]]);
}

static void radio_picked(ls_rsel_radio_t r) { s_radio = r; start(); }

static bool key(ls_tk_t k, char c)
{
    if (ls_opt_key(&CTX, c)) return true;
    if (k == LS_TK_UP || c == '[') { move(-1); return true; }
    if (k == LS_TK_DOWN || c == ']') { move(1); return true; }
    if (c == 'r' || c == 'R') { ls_rsel_open(LS_RSEL_SUBGHZ_READ, radio_picked); return true; }
    if (c == 'l' || c == 'L') { s_listen = !s_src; if (s_listen) start(); else stop(); return true; }
    const lsm_meter_t *sel = selected();
    if (!sel) return false;
    pick(sel);
    if (c == 'p' || c == 'P') { ls_meters_pin(sel->proto, sel->id, !sel->pinned); return true; }
    if (c == 'n' || c == 'N') {
        ls_picker_open("NAME THIS METER", named);
        for (int i = 0; i < N_NAMES; i++) ls_picker_add(NAMES[i], i == N_NAMES - 1 ? "back to its ID" : "");
        return true;
    }
    return false;
}

static bool touch(int col, int row)
{
    const int b = ls_btn_hit(col, row);
    if (b >= 0 && b < 4) return key(LS_TK_CHAR, "rlpn"[b]);
    if (b == 4) { ls_opt_open(&CTX); return true; }
    const int r = row - s_list.y;
    if (col >= s_list.x && col < s_list.x + s_list.w && r >= 0 && r < (int)sizeof(s_row_slot) && s_row_slot[r] >= 0) {
        pick(&s_view.m[s_row_slot[r]]);
        return true;
    }
    return false;
}

static void enter(void)
{
    ls_meters_begin();
    s_seq = UINT32_MAX;
    start();
}

static void leave(void) { stop(); }

const ls_tui_screen_t ls_scr_meters = {
    .name = "METERS",
    .hint = s_hint,
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
