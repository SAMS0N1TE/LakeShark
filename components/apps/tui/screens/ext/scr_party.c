/* PARTY: a few LakeShark users together (components/apps/tui/party).

   Not in a party: the LOBBY, a campfire over the saved profiles, and the
   guided SETUP that makes one. In a party: ROSTER (who, how far, which way,
   battery, link), RADAR (members and their finds around me, true bearings,
   heading up when there is a compass), PINGS (what members found, roughly
   where), FILES (send a small file from the card, receive into
   /sdcard/party). MARK pings the party by hand; FIND buzzes faster as you
   close on a member or a find, like HUNT's ticking but felt, not heard.
   Everything that moves here moves because something arrived: the fire
   burns with the traffic, a dot steps when its member's position does, a
   ring opens from a ping when it lands. */
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../party/party_core.h"
#include "../../party/party_io.h"
#include "../../ls_braille.h"
#include "../../ls_calls.h"
#include "../../ls_keyboard.h"
#include "../../ls_notify.h"
#include "../../ls_options.h"
#include "../../ls_picker.h"
#include "../../ls_plate.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "esp_attr.h"

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

extern const ls_tui_screen_t ls_scr_party;
bool ls_party_running(void) { return party_running(); }

enum { PG_ROSTER, PG_RADAR, PG_PINGS, PG_FILES };
static int s_page = PG_RADAR;
static int s_wiz = -1;                 /* guided setup step, -1 off */
static int s_range = -1;               /* RANGES index, -1 automatic */
static char s_say[72];
static uint32_t s_say_ms;
static int s_top;                      /* first row of a list page */
static EXT_RAM_BSS_ATTR party_t s_snap;

static const float RANGES[] = { 100, 250, 500, 1000, 2500, 5000, 10000, 25000 };
#define NRANGES ((int)(sizeof(RANGES) / sizeof(RANGES[0])))
static const char GLYPHS[] = "@#$%&*+=?!ABCDEFGHJKMNPRSTVWXYZ";
static const uint8_t COLOURS[] = { TUI_CYAN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT,
                                   TUI_MAGENTA | TUI_BRIGHT, TUI_RED | TUI_BRIGHT, TUI_BLUE | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT };
static const char *const COLOUR_N[] = { "CYAN", "GREEN", "YELLOW", "MAGENTA", "RED", "BLUE", "WHITE" };
static const uint8_t CLASS_HUE[PARTY_CLASSES] = {
    TUI_WHITE, TUI_RED, TUI_YELLOW, TUI_RED, TUI_BLUE, TUI_CYAN, TUI_MAGENTA, TUI_YELLOW,
    TUI_RED, TUI_MAGENTA, TUI_GREEN, TUI_MAGENTA, TUI_GREEN, TUI_YELLOW, TUI_CYAN, TUI_RED,
};
/* One character per class on the radar. */
static const char CLASS_MARK[PARTY_CLASSES + 1] = "xLCBPDTVFSHWIWK!";

/* Every panel is a plate (ls_plate.h): text sits on its slate ground. Bright
   for values, plain white for labels, colour only when it means something. */
#define A(fg) ls_plate_at((uint8_t)(fg))
#define DIM A(TUI_WHITE)
#define TXT A(TUI_WHITE | TUI_BRIGHT)
#define FAINT A(TUI_BLUE)
#define BK(fg) TUI_ATTR((fg), TUI_BLACK)      /* the screen's own ground, between plates */

static uint32_t ms(void) { return party_io_ms(); }
static void say(const char *t) { snprintf(s_say, sizeof(s_say), "%s", t); s_say_ms = ms(); }

/* ------------------------------------------------------------ taps */

enum { T_NONE, T_PROF, T_MEMBER, T_PING, T_SHARE, T_WIZ, T_RADAR, T_LOBBY };
typedef struct { tui_rect r; uint8_t kind; int arg; } hit_t;
static hit_t s_hit[40];
static int s_nhit;
static tui_rect s_radar;               /* the radar's cells, for a tap */
static float s_radar_range, s_radar_rot;
static void hit_add(int x, int y, int w, int h, int kind, int arg)
{
    if (s_nhit < (int)(sizeof(s_hit) / sizeof(s_hit[0]))) s_hit[s_nhit++] = (hit_t){ tui_rect_make(x, y, w, h), (uint8_t)kind, arg };
}

/* ------------------------------------------------------------ words */

static void fmt_m(char *o, size_t n, float m)
{
    if (m < 0) snprintf(o, n, "--");
    else if (m < 1000) snprintf(o, n, "%d m", (int)m);
    else if (m < 10000) snprintf(o, n, "%.1f km", m / 1000.0f);
    else snprintf(o, n, "%d km", (int)(m / 1000));
}
static void fmt_age(char *o, size_t n, uint32_t age_ms)
{
    const uint32_t s = age_ms / 1000;
    if (s < 60) snprintf(o, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(o, n, "%um", (unsigned)(s / 60));
    else snprintf(o, n, "%uh", (unsigned)(s / 3600));
}
static const char *compass8(float b)
{
    static const char *const D[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return D[((int)floorf((b + 22.5f) / 45.0f)) & 7];
}
static void put(tui_surface *sf, tui_rect c, int x, int y, const char *s, uint8_t at) { tui_put_str(sf, c, x, y, s, at); }
static void putch(tui_surface *sf, tui_rect c, int x, int y, char ch, uint8_t at) { tui_put_char(sf, c, x, y, ch, at); }
static void putf(tui_surface *sf, tui_rect c, int x, int y, uint8_t at, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static void putf(tui_surface *sf, tui_rect c, int x, int y, uint8_t at, const char *fmt, ...)
{
    char b[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    tui_put_str(sf, c, x, y, b, at);
}
/* A second tab on a plate's top edge, right after its title. */
static void tab_after(tui_surface *sf, tui_rect r, const char *title, const char *text, uint8_t hue)
{
    ls_plate_tab(sf, r, r.x + 2 + (int)strlen(title) + 3, r.y, text, hue);
}
/* A tab on a plate's bottom edge: something passing, or more below. */
static void foot_tab(tui_surface *sf, tui_rect r, bool right, const char *text, uint8_t hue)
{
    const int w = (int)strlen(text) + 2;
    ls_plate_tab(sf, r, right ? r.x + r.w - 2 - w : r.x + 2, r.y + r.h - 1, text, hue);
}

/* The one headline: dot-matrix letters three rows tall (ls_calls.h) when
   they fit in w, else one bright line in the middle row. True when big. */
static bool headline(tui_surface *sf, tui_rect c, int x, int y, int w, const char *text, uint8_t hue)
{
    char t[32];
    size_t n = 0;
    for (const char *p = text; *p && n < sizeof(t) - 1; p++) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        t[n++] = (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == ':' ? ch : ' ';
    }
    t[n] = 0;
    if (n && ls_calls_big_width(t) <= w) {
        ls_calls_big_text(sf, c, x, y, t, A(hue | TUI_BRIGHT));
        return true;
    }
    putf(sf, c, x, y + 1, A(hue | TUI_BRIGHT), "%-.*s", w > 0 ? w : 0, text);
    return false;
}

static float link_q(const party_member_t *m)
{
    const float r = (m->rssi + 125) / 60.0f;
    const float loss = m->heard ? (float)m->heard / (float)(m->heard + m->lost) : 0;
    const float q = (r < 0 ? 0 : r > 1 ? 1 : r) * 0.6f + loss * 0.4f;
    return q;
}

/* ------------------------------------------------------------ the fire */

/* The party's banner: a campfire whose flames stand as tall as the
   traffic of the last ten seconds, on a plate with the party's name on its
   tab and the link on its chip; the headline is how many are with you. */
static int fire_level(uint32_t now)
{
    int n = 0;
    for (int i = 0; i < 16; i++) n += s_snap.heard_ms[i] && now - s_snap.heard_ms[i] < 10000;
    return n == 0 ? 0 : n < 3 ? 1 : n < 8 ? 2 : 3;
}

#define BANNER_H 7

static int draw_banner(tui_surface *sf, tui_rect a, uint32_t now)
{
    static const char *const FLAME[4][3] = {
        { "          ", "    ..    ", "   (__)   " },
        { "          ", "    ()    ", "   (  )   " },
        { "    (     ", "   ) )(   ", "  ( (  )  " },
        { "   ) (    ", "  ( ))( ) ", " ) (  )(( " },
    };
    const bool run = s_snap.running;
    const party_prof_t *pr = run ? &s_snap.prof : party_prof(party_prof_cur());
    char name[20], chip[24], t[48];
    snprintf(name, sizeof(name), "%s", pr ? pr->party : "PARTY");
    uint8_t chip_hue = TUI_WHITE;
    if (!run) snprintf(chip, sizeof(chip), "NOT RUNNING");
    else if (s_snap.link_up) {
        snprintf(chip, sizeof(chip), "%.10s %s", s_snap.link.radio ? s_snap.link.radio : "",
                 s_snap.link.bulk_via == PARTY_VIA_FLRC ? "FLRC" : s_snap.link.bulk_via == PARTY_VIA_GFSK ? "GFSK" : "LORA");
        chip_hue = TUI_GREEN;
    } else {
        snprintf(chip, sizeof(chip), "%.18s", s_snap.link_why[0] ? s_snap.link_why : "NO LINK");
        chip_hue = TUI_RED;
    }
    const tui_rect r = tui_rect_make(a.x, a.y, a.w, BANNER_H);
    const tui_rect in = ls_plate(sf, a, r, TUI_MAGENTA, run ? name : "PARTY", chip, chip_hue);
    if (in.w <= 0) return 0;
    /* the fire, on the plate */
    const int lv = run ? fire_level(now) : 0;
    const uint8_t fl[3] = { A(TUI_RED | TUI_BRIGHT), A(TUI_YELLOW | TUI_BRIGHT), A(TUI_YELLOW) };
    for (int i = 0; i < 3; i++) put(sf, in, in.x - 1, in.y + i, FLAME[lv][i], fl[i]);
    put(sf, in, in.x - 1, in.y + 3, "  /\\/\\/\\  ", A(TUI_YELLOW));
    put(sf, in, in.x - 1, in.y + 4, " '=-==-=' ", A(TUI_RED | TUI_BRIGHT));
    const int x = in.x + 10, w = in.x + in.w - x;
    if (!run) {
        const int n = party_prof_count();
        headline(sf, in, x, in.y, w, n ? name : "PARTY", TUI_MAGENTA);
        if (n) {
            putf(sf, in, x, in.y + 3, TXT, "%d", n);
            putf(sf, in, x + (n > 9 ? 3 : 2), in.y + 3, DIM, "profile%s saved", n == 1 ? "" : "s");
            put(sf, in, x, in.y + 4, "START joins this one", DIM);
        } else {
            put(sf, in, x, in.y + 3, "no profiles yet", DIM);
            put(sf, in, x, in.y + 4, "START sets one up", DIM);
        }
        return BANNER_H + 1;
    }
    int nm = 0;
    for (int i = 0; i < PARTY_MEMBERS; i++) nm += s_snap.member[i].used;
    snprintf(t, sizeof(t), "%d WITH YOU", nm);
    if (ls_calls_big_width(t) <= w) headline(sf, in, x, in.y, w, t, TUI_MAGENTA);
    else {
        snprintf(t, sizeof(t), "%d", nm);
        headline(sf, in, x, in.y, w, t, TUI_MAGENTA);
        put(sf, in, x + ls_calls_big_width(t) + 2, in.y + 1, "with you", TXT);
    }
    /* channel and airtime */
    const float budget = s_snap.prof.airtime == PARTY_AIR_1 ? 600 : s_snap.prof.airtime == PARTY_AIR_3 ? 1800 : s_snap.prof.airtime == PARTY_AIR_25 ? 15000 : 6000;
    put(sf, in, x, in.y + 3, "CH", DIM);
    putf(sf, in, x + 3, in.y + 3, TXT, "%u", s_snap.key.channel + 1);
    put(sf, in, x + 6, in.y + 3, "AIR", DIM);
    snprintf(t, sizeof(t), "%u.%u s/min", (unsigned)(s_snap.air_used_ms / 1000), (unsigned)(s_snap.air_used_ms % 1000 / 100));
    const int mw = w - 10 - (int)strlen(t) - 1;
    if (mw >= 3) ls_plate_meter(sf, in, x + 10, in.y + 3, mw, s_snap.air_used_ms / budget, TUI_CYAN);
    put(sf, in, x + w - (int)strlen(t), in.y + 3, t, TXT);
    /* me */
    putch(sf, in, x, in.y + 4, (char)s_snap.prof.glyph, A(s_snap.prof.colour | TUI_BRIGHT));
    putf(sf, in, x + 2, in.y + 4, TXT, "%-8s", s_snap.prof.call);
    put(sf, in, x + 11, in.y + 4, s_snap.fix ? "GPS" : "NO FIX", s_snap.fix ? A(TUI_GREEN | TUI_BRIGHT) : A(TUI_RED | TUI_BRIGHT));
    put(sf, in, x + 18, in.y + 4, s_snap.prof.share == PARTY_SHARE_EXACT ? "EXACT" : s_snap.prof.share == PARTY_SHARE_ROUGH ? "ROUGH" : "HIDDEN",
        A(TUI_YELLOW | TUI_BRIGHT));
    /* what is happening now, on the bottom edge: the motor, HELP, WARDRIVE feeding */
    if (s_snap.vibe_pat >= 0) { snprintf(t, sizeof(t), "))) %s", party_vibe_name(s_snap.vibe_pat)); foot_tab(sf, r, false, t, TUI_YELLOW); }
    else if (s_snap.help) foot_tab(sf, r, false, "HELP SENT", TUI_RED);
    if (s_snap.wd_running) foot_tab(sf, r, true, "WARDRIVE FEEDING", TUI_GREEN);
    return BANNER_H + 1;
}

/* ------------------------------------------------------------ roster */

static void age_at(tui_surface *sf, tui_rect c, int y, uint32_t age)
{
    char ag[8];
    fmt_age(ag, sizeof(ag), age);
    putf(sf, c, c.x + c.w - 5, y, A(age < 30000 ? TUI_GREEN | TUI_BRIGHT : age < 120000 ? TUI_YELLOW | TUI_BRIGHT : TUI_RED | TUI_BRIGHT), "%5s", ag);
}

/* A meter with its label before it and its value after it. */
static void labelled_meter(tui_surface *sf, tui_rect c, int x, int y, const char *label, int w, float v, uint8_t hue, const char *val)
{
    put(sf, c, x, y, label, DIM);
    ls_plate_meter(sf, c, x + 5, y, w, v, hue);
    put(sf, c, x + 6 + w, y, val, TXT);
}

/* Me, as the roster's first plate. Returns its height. */
static int me_plate(tui_surface *sf, tui_rect a, int y)
{
    char title[24], t[24];
    const int h = s_snap.help ? 5 : 4;
    snprintf(title, sizeof(title), "%c %s  YOU", s_snap.prof.glyph, s_snap.prof.call);
    const tui_rect r = tui_rect_make(a.x, y, a.w, h);
    const tui_rect in = ls_plate(sf, a, r, s_snap.prof.colour & 7, title, s_snap.fix ? "GPS" : "NO FIX", s_snap.fix ? TUI_GREEN : TUI_RED);
    if (in.w <= 0) return h;
    const int b = party_io_batt();
    if (b >= 0) { snprintf(t, sizeof(t), "%3d%%", b); labelled_meter(sf, in, in.x, in.y, "batt", 8, b / 100.0f, b > 30 ? TUI_GREEN : TUI_RED, t); }
    else { put(sf, in, in.x, in.y, "batt", DIM); put(sf, in, in.x + 5, in.y, "--", DIM); }
    if (s_snap.speed >= 0) putf(sf, in, in.x + 20, in.y, TXT, "%.1f m/s", s_snap.speed);
    if (s_snap.heading >= 0) { put(sf, in, in.x + 31, in.y, "hdg", DIM); putf(sf, in, in.x + 35, in.y, TXT, "%d", (int)s_snap.heading); }
    if (s_snap.fix) putf(sf, in, in.x, in.y + 1, DIM, "%.5f %.5f", s_snap.lat, s_snap.lon);
    else put(sf, in, in.x, in.y + 1, "waiting for a fix", A(TUI_RED | TUI_BRIGHT));
    if (s_snap.help) put(sf, in, in.x, in.y + 2, "HELP SENT: tap MARK > CLEAR", A(TUI_YELLOW | TUI_BRIGHT));
    return h;
}

/* A member as a plate in their colour: who on the tab, how far on the chip,
   battery and link as meters, what they are doing, what they found last. */
static int s_mh = 6;                   /* a member plate: 6 rows with air, 5 when short of room */
static void member_plate(tui_surface *sf, tui_rect a, int y, const party_member_t *m, uint32_t now)
{
    char d[16], title[24], chip[24], t[24];
    float dist = -1, brg = 0;
    if (s_snap.fix && m->pos) party_geo(s_snap.lat, s_snap.lon, m->lat, m->lon, &dist, &brg);
    const uint32_t ago = now - m->last_ms;
    const bool help = m->pflags & PARTY_POS_HELP;
    snprintf(title, sizeof(title), "%c %s", m->glyph, m->call);
    fmt_m(d, sizeof(d), dist);
    if (dist >= 0) snprintf(chip, sizeof(chip), "%s %s", d, compass8(brg));
    else snprintf(chip, sizeof(chip), "%s", m->pos ? "NO FIX HERE" : "NO POSITION");
    const tui_rect r = tui_rect_make(a.x, y, a.w, s_mh);
    const tui_rect in = ls_plate(sf, a, r, help ? TUI_RED : m->colour & 7, title, chip, dist >= 0 ? TUI_CYAN : TUI_WHITE);
    hit_add(r.x, r.y, r.w, r.h, T_MEMBER, m->id);
    if (in.w <= 0) return;
    if (help && (now / 500) % 2) ls_plate_tab(sf, a, r.x + 2 + (int)strlen(title) + 3, r.y, "HELP!", TUI_RED);
    /* battery, link, last heard */
    if (m->batt <= 100) { snprintf(t, sizeof(t), "%3d%%", m->batt); labelled_meter(sf, in, in.x, in.y, "batt", 8, m->batt / 100.0f, m->batt > 30 ? TUI_GREEN : TUI_RED, t); }
    else { put(sf, in, in.x, in.y, "batt", DIM); put(sf, in, in.x + 5, in.y, "--", DIM); }
    snprintf(t, sizeof(t), "%d dBm", m->rssi);
    if (in.w >= 46) labelled_meter(sf, in, in.x + 20, in.y, "link", 6, link_q(m), TUI_CYAN, t);
    age_at(sf, in, in.y, ago);
    /* what they are doing */
    char st[64] = "";
    size_t n = 0;
    if (help) n += (size_t)snprintf(st + n, sizeof(st) - n, "HELP! ");
    if (m->pflags & PARTY_POS_VEHICLE) n += (size_t)snprintf(st + n, sizeof(st) - n, "vehicle ");
    if (m->speed > 0.7f) n += (size_t)snprintf(st + n, sizeof(st) - n, "%.1f m/s ", m->speed);
    else if (m->pos) n += (size_t)snprintf(st + n, sizeof(st) - n, "still ");
    if (m->course >= 0 && m->speed > 0.7f) n += (size_t)snprintf(st + n, sizeof(st) - n, "to %s ", compass8(m->course));
    if (m->pflags & PARTY_POS_ROUGH) n += (size_t)snprintf(st + n, sizeof(st) - n, "rough +-%u m ", m->acc_m);
    if (m->caps & PARTY_CAP_FLRC) snprintf(st + n, sizeof(st) - n, "LR2021");
    const int sy = in.y + in.h - 2, fy = in.y + in.h - 1;   /* doing, found: with a row of air above when there is room */
    put(sf, in, in.x, sy, st, help ? A(TUI_RED | TUI_BRIGHT) : A(TUI_YELLOW | TUI_BRIGHT));
    if (s_snap.find_kind == 1 && s_snap.find_id == m->id) put(sf, in, in.x + in.w - 6, sy, " FIND ", TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT));
    /* the newest thing this member reported */
    const party_pingrec_t *last = NULL;
    int count = 0;
    for (int i = 0; i < PARTY_PINGS; i++) {
        const party_pingrec_t *p = &s_snap.ping[i];
        bool by = false;
        for (int k = 0; k < p->nrep && k < 4; k++) by |= p->used && p->rep[k] == m->id;
        if (!by) continue;
        count++;
        if (!last || p->rx_ms > last->rx_ms) last = p;
    }
    if (last) {
        putf(sf, in, in.x, fy, TXT, "%d", count);
        const int cx = in.x + (count > 9 ? 3 : 2);
        put(sf, in, cx, fy, "found, last", DIM);
        putf(sf, in, cx + 12, fy, TUI_ATTR(TUI_BLACK, CLASS_HUE[last->cls] | TUI_BRIGHT), "%s", party_class_tag(last->cls));
        putf(sf, in, cx + 16, fy, TXT, "%-.*s", in.x + in.w - 7 - (cx + 16) > 0 ? in.x + in.w - 7 - (cx + 16) : 0, last->label);
        char ag[8];
        fmt_age(ag, sizeof(ag), now - last->rx_ms);
        putf(sf, in, in.x + in.w - 5, fy, DIM, "%5s", ag);
    } else put(sf, in, in.x, fy, "nothing found yet", DIM);
}

static void page_roster(tui_surface *sf, tui_rect a, uint32_t now)
{
    int y = a.y;
    s_mh = a.h >= 30 ? 6 : 5;
    const tui_rect me = tui_rect_make(a.x, y, a.w, me_plate(sf, a, y));
    y += me.h + 1;
    int idx[PARTY_MEMBERS], n = 0;
    for (int i = 0; i < PARTY_MEMBERS; i++) if (s_snap.member[i].used) idx[n++] = i;
    /* nearest first; no position last */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            const party_member_t *p = &s_snap.member[idx[j - 1]], *q = &s_snap.member[idx[j]];
            float dp = 1e9f, dq = 1e9f;
            if (s_snap.fix && p->pos) party_geo(s_snap.lat, s_snap.lon, p->lat, p->lon, &dp, NULL);
            if (s_snap.fix && q->pos) party_geo(s_snap.lat, s_snap.lon, q->lat, q->lon, &dq, NULL);
            if (dq < dp) { const int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; } else break;
        }
    if (!n) {
        const tui_rect in = ls_plate(sf, a, tui_rect_make(a.x, y, a.w, 7), TUI_GREEN, "ROSTER", "NOBODY YET", TUI_WHITE);
        if (in.w <= 0) return;
        put(sf, in, in.x, in.y + 1, "Nobody heard yet.", TXT);
        put(sf, in, in.x, in.y + 3, "Members with the same party code show up", DIM);
        put(sf, in, in.x, in.y + 4, "here after their first HELLO.", DIM);
        return;
    }
    if (s_top > n - 1) s_top = n - 1;
    if (s_top < 0) s_top = 0;
    if (s_top > 0) { char t[32]; snprintf(t, sizeof(t), "^ %d ABOVE", s_top); foot_tab(sf, me, true, t, TUI_WHITE); }
    int k = s_top;
    tui_rect last = me;
    for (; k < n && y + s_mh <= a.y + a.h; k++) {
        member_plate(sf, a, y, &s_snap.member[idx[k]], now);
        last = tui_rect_make(a.x, y, a.w, s_mh);
        y += s_mh + 1;
    }
    if (k < n) { char t[32]; snprintf(t, sizeof(t), "v %d MORE", n - k); foot_tab(sf, last, true, t, TUI_WHITE); }
}

/* Members, then the newest finds, with how far and which way, on a plate:
   under the radar in portrait, beside every page in landscape. */
static int near_rows(void)
{
    int nm = 0, np = 0;
    for (int i = 0; i < PARTY_MEMBERS; i++) nm += s_snap.member[i].used;
    for (int i = 0; i < PARTY_PINGS; i++) np += s_snap.ping[i].used;
    return (s_snap.find_kind && s_snap.find_m >= 0) + nm + np + (nm && np);
}

static void near_list(tui_surface *sf, tui_rect r, uint32_t now)
{
    (void)now;
    int nm = 0, np = 0;
    for (int i = 0; i < PARTY_MEMBERS; i++) nm += s_snap.member[i].used;
    for (int i = 0; i < PARTY_PINGS; i++) np += s_snap.ping[i].used;
    char chip[32];
    snprintf(chip, sizeof(chip), "%d + %d FINDS", nm, np);
    const tui_rect l = ls_plate(sf, r, r, TUI_CYAN, "NEAR", chip, TUI_WHITE);
    if (l.w <= 0) return;
    int y = l.y;
    if (s_snap.find_kind && s_snap.find_m >= 0) {
        char d[16];
        fmt_m(d, sizeof(d), s_snap.find_m);
        float iv = 300.0f + s_snap.find_m * 12.0f;
        iv = iv < 400 ? 400 : iv > 6000 ? 6000 : iv;
        putf(sf, l, l.x, y++, TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT), " FIND %s %s  buzz %.1f s ", d, compass8(s_snap.find_brg), iv / 1000.0f);
    }
    for (int i = 0; i < PARTY_MEMBERS && y < l.y + l.h; i++) {
        const party_member_t *m = &s_snap.member[i];
        float d = -1, b = 0;
        char ds[16];
        if (!m->used) continue;
        if (s_snap.fix && m->pos) party_geo(s_snap.lat, s_snap.lon, m->lat, m->lon, &d, &b);
        fmt_m(ds, sizeof(ds), d);
        putch(sf, l, l.x, y, (char)m->glyph, A(m->colour | TUI_BRIGHT));
        putf(sf, l, l.x + 2, y, A(m->colour | TUI_BRIGHT), "%-8s", m->call);
        putf(sf, l, l.x + 11, y, TXT, "%7s %-2s", ds, d >= 0 ? compass8(b) : "");
        hit_add(r.x, y, r.w, 1, T_MEMBER, m->id);
        y++;
    }
    if (nm && np) y++;
    int order[PARTY_PINGS], n = 0;
    for (int i = 0; i < PARTY_PINGS; i++) if (s_snap.ping[i].used) order[n++] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && s_snap.ping[order[j]].rx_ms > s_snap.ping[order[j - 1]].rx_ms; j--) { const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    int k = 0;
    for (; k < n && y < l.y + l.h; k++) {
        const party_pingrec_t *p = &s_snap.ping[order[k]];
        float d = -1, b = 0;
        char ds[16];
        if (s_snap.fix) party_geo(s_snap.lat, s_snap.lon, p->lat, p->lon, &d, &b);
        fmt_m(ds, sizeof(ds), d);
        putch(sf, l, l.x, y, CLASS_MARK[p->cls], A(CLASS_HUE[p->cls] | TUI_BRIGHT));
        putf(sf, l, l.x + 2, y, A(CLASS_HUE[p->cls] | TUI_BRIGHT), "%-8.8s", party_class_name(p->cls));
        putf(sf, l, l.x + 11, y, TXT, "%7s %-2s", ds, d >= 0 ? compass8(b) : "");
        putf(sf, l, l.x + 22, y, DIM, "%.*s", l.w - 22 > 0 ? l.w - 22 : 0, p->label);
        hit_add(r.x, y, r.w, 1, T_PING, order[k]);
        y++;
    }
    if (k < n) { char t[32]; snprintf(t, sizeof(t), "%d MORE ON PINGS", n - k); foot_tab(sf, r, true, t, TUI_WHITE); }
}

/* ------------------------------------------------------------ radar */

#define RAD_W 64
#define RAD_H 40
static EXT_RAM_BSS_ATTR uint8_t s_dot[RAD_W * RAD_H];
static EXT_RAM_BSS_ATTR uint8_t s_dat[RAD_W * RAD_H];
static int s_rw, s_rh;
/* Braille dots are not square: a cell is about 1.6 times as tall as it is
   wide, so four rows of dots cover more height than two columns cover
   width. North-south distances are stretched by this to draw circles round. */
#define KY 1.24f

static void dot(int x, int y, uint8_t at)
{
    static const uint8_t BIT[4][2] = { { 0x01, 0x08 }, { 0x02, 0x10 }, { 0x04, 0x20 }, { 0x40, 0x80 } };
    if (x < 0 || y < 0 || x >= s_rw * 2 || y >= s_rh * 4) return;
    const int c = (y / 4) * s_rw + x / 2;
    s_dot[c] |= BIT[y & 3][x & 1];
    if (at) s_dat[c] = at;
}
static void ring(int cx, int cy, float r, uint8_t at, int step)
{
    const int n = (int)(r * 6.3f) + 8;
    for (int i = 0; i < n; i += step) {
        const float t = (float)i / n * 6.2832f;
        dot(cx + (int)lroundf(sinf(t) * r), cy - (int)lroundf(cosf(t) * r * KY), at);
    }
}
static void line(int x0, int y0, int x1, int y1, uint8_t at, int skip)
{
    const int n = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    for (int i = 0; i <= n; i++) if (!skip || i % skip == 0) dot(x0 + (x1 - x0) * i / (n ? n : 1), y0 + (y1 - y0) * i / (n ? n : 1), at);
}

/* Dots from the radar's centre for a place `m` metres away on bearing `b`. */
static void project(float m, float b, float R, float range, int *dx, int *dy)
{
    const float r = m / range * R;
    const float t = (b - s_radar_rot) * (float)M_PI / 180.0f;
    *dx = (int)lroundf(sinf(t) * r);
    *dy = -(int)lroundf(cosf(t) * r * KY);
}

static void page_radar(tui_surface *sf, tui_rect a, uint32_t now, bool wide)
{
    /* the radar takes a square of dots on its plate; in portrait the list
       goes on a plate under it, in landscape beside every page */
    int rw, rh, ph;
    if (wide) {
        ph = a.h;
        rh = ph - 2;
        rw = (int)(rh * 4 / KY / 2) & ~1;
        if (rw > a.w - 4) rw = a.w - 4;
    } else {
        int near_h = near_rows() + 2;
        if (near_h < 5) near_h = 5;
        int rh_max = a.h - near_h - 1 - 2;
        if (rh_max < 20) rh_max = 20;
        rw = a.w - 4;
        rh = (int)((rw * KY * 2) / 4.0f) + 1;
        if (rh > rh_max) { rh = rh_max; rw = (int)(rh * 4 / (KY * 2)); }
        ph = rh + 2;
    }
    if (rw > RAD_W) rw = RAD_W;
    if (rh > RAD_H) rh = RAD_H;
    if (rw < 10 || rh < 5) return;
    s_rw = rw; s_rh = rh;
    /* range: as close as fits everything, unless chosen */
    float far = 0;
    if (s_snap.fix) {
        for (int i = 0; i < PARTY_MEMBERS; i++) {
            const party_member_t *m = &s_snap.member[i];
            float d;
            if (!m->used || !m->pos) continue;
            party_geo(s_snap.lat, s_snap.lon, m->lat, m->lon, &d, NULL);
            if (d < 25000 && d > far) far = d;
        }
        for (int i = 0; i < PARTY_PINGS; i++) {
            const party_pingrec_t *p = &s_snap.ping[i];
            float d;
            if (!p->used || now - p->rx_ms > 1800000) continue;
            party_geo(s_snap.lat, s_snap.lon, p->lat, p->lon, &d, NULL);
            if (d < 25000 && d > far) far = d;
        }
    }
    int ri = s_range;
    if (ri < 0) { ri = 0; while (ri < NRANGES - 1 && RANGES[ri] < far * 1.1f) ri++; }
    const float range = RANGES[ri];
    s_radar_range = range;
    /* heading up when the compass has one and the profile allows */
    s_radar_rot = !s_snap.prof.north_up && s_snap.heading >= 0 ? s_snap.heading : 0;
    /* the plate: range and orientation as chips on its top edge */
    char rs[16], rt[24];
    fmt_m(rs, sizeof(rs), range);
    snprintf(rt, sizeof(rt), "%s%s", rs, s_range < 0 ? " AUTO" : "");
    const tui_rect pr = tui_rect_make(a.x, a.y, a.w, ph);
    const tui_rect in = ls_plate(sf, a, pr, TUI_CYAN, "RADAR", s_radar_rot != 0 ? "HDG UP" : "N UP", TUI_CYAN);
    if (in.w <= 0) return;
    tab_after(sf, pr, "RADAR", rt, TUI_WHITE);
    const int ox = in.x + (in.w - rw) / 2, oy = in.y;
    s_radar = tui_rect_make(ox, oy, rw, rh);
    memset(s_dot, 0, (size_t)(rw * rh));
    memset(s_dat, 0, (size_t)(rw * rh));
    const int cx = rw, cy = rh * 2;
    const float R = fminf((float)rw, rh * 2 / KY) - 1.5f;
    /* rings, cross, north */
    ring(cx, cy, R, A(TUI_CYAN), 1);
    ring(cx, cy, R * 2 / 3, A(TUI_BLUE | TUI_BRIGHT), 2);
    ring(cx, cy, R / 3, A(TUI_BLUE | TUI_BRIGHT), 3);
    for (int k = 0; k < 4; k++) {
        int dx, dy;
        project(R * 0.92f, k * 90.0f, R, R, &dx, &dy);
        line(cx, cy, cx + dx, cy + dy, A(TUI_BLUE | TUI_BRIGHT), 4);
    }
    if (s_snap.fix) {
        /* members' trails, then where they are */
        for (int i = 0; i < PARTY_MEMBERS; i++) {
            const party_member_t *m = &s_snap.member[i];
            if (!m->used || !m->pos) continue;
            for (int t = 0; t < m->ntrail; t++) {
                float d, b;
                int dx, dy;
                party_geo(s_snap.lat, s_snap.lon, m->tlat[t], m->tlon[t], &d, &b);
                if (d > range) continue;
                project(d, b, R, range, &dx, &dy);
                dot(cx + dx, cy + dy, A(m->colour & 7));
            }
        }
        /* finds: uncertainty, and a ring opening from each new one */
        for (int i = 0; i < PARTY_PINGS; i++) {
            const party_pingrec_t *p = &s_snap.ping[i];
            float d, b;
            int dx, dy;
            if (!p->used || now - p->rx_ms > 1800000) continue;
            party_geo(s_snap.lat, s_snap.lon, p->lat, p->lon, &d, &b);
            if (d > range * 1.05f) continue;
            project(d, b, R, range, &dx, &dy);
            const float ur = p->unc_m / range * R;
            if (ur >= 2 && ur < R * 1.2f) ring(cx + dx, cy + dy, ur, A(CLASS_HUE[p->cls]), 3);
            const uint32_t age = now - p->rx_ms;
            if (age < 4000) ring(cx + dx, cy + dy, 1.5f + age / 4000.0f * 7.0f, A(CLASS_HUE[p->cls] | TUI_BRIGHT), 1);
        }
        /* FIND: a dotted line to the target */
        if (s_snap.find_kind && s_snap.find_m >= 0) {
            int dx, dy;
            project(s_snap.find_m < range ? s_snap.find_m : range, s_snap.find_brg, R, range, &dx, &dy);
            line(cx, cy, cx + dx, cy + dy, A(TUI_YELLOW | TUI_BRIGHT), 2);
        }
        /* which way I am going: course, else heading */
        const float dir = s_snap.course >= 0 && s_snap.speed > 0.7f ? s_snap.course : s_snap.heading;
        if (dir >= 0) {
            int dx, dy;
            project(R / 4, dir, R, R, &dx, &dy);
            line(cx, cy, cx + dx, cy + dy, A(s_snap.prof.colour), 0);
        }
    }
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            const int c = y * rw + x;
            if (s_dot[c]) ls_braille_glyph_put(sf, in, ox + x, oy + y, (int16_t)(0x2800 | s_dot[c]), s_dat[c] ? s_dat[c] : DIM);
        }
    /* letters over the dots: compass points, finds, members, me */
    static const char NESW[4] = { 'N', 'E', 'S', 'W' };
    for (int k = 0; k < 4; k++) {
        int dx, dy;
        project(R, k * 90.0f, R, R, &dx, &dy);
        putch(sf, in, ox + (cx + dx) / 2, oy + (cy + dy) / 4, NESW[k], k ? TXT : A(TUI_RED | TUI_BRIGHT));
    }
    if (!s_snap.fix) {
        put(sf, in, ox + rw / 2 - 8, oy + rh / 2 + 2, " waiting for GPS ", TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT));
    } else {
        for (int i = 0; i < PARTY_PINGS; i++) {
            const party_pingrec_t *p = &s_snap.ping[i];
            float d, b;
            int dx, dy;
            if (!p->used || now - p->rx_ms > 1800000) continue;
            party_geo(s_snap.lat, s_snap.lon, p->lat, p->lon, &d, &b);
            if (d > range * 1.05f) continue;
            project(d, b, R, range, &dx, &dy);
            const bool fresh = now - p->rx_ms < 60000;
            putch(sf, in, ox + (cx + dx) / 2, oy + (cy + dy) / 4, CLASS_MARK[p->cls],
                  fresh ? TUI_ATTR(TUI_BLACK, CLASS_HUE[p->cls] | TUI_BRIGHT) : A(CLASS_HUE[p->cls] | TUI_BRIGHT));
        }
        for (int i = 0; i < PARTY_MEMBERS; i++) {
            const party_member_t *m = &s_snap.member[i];
            float d, b;
            int dx, dy;
            if (!m->used || !m->pos) continue;
            party_geo(s_snap.lat, s_snap.lon, m->lat, m->lon, &d, &b);
            const bool out = d > range;
            project(out ? range : d, b, R, range, &dx, &dy);
            const int x = ox + (cx + dx) / 2, y = oy + (cy + dy) / 4;
            const bool help = m->pflags & PARTY_POS_HELP;
            putch(sf, in, x, y, out ? '+' : (char)m->glyph, help ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED) : A(m->colour | TUI_BRIGHT));
            if (!out) {
                char lb[6];
                snprintf(lb, sizeof(lb), "%.4s", m->call);
                tui_rect clip = s_radar;
                put(sf, clip, x + 1 + (int)strlen(lb) < ox + rw ? x + 1 : x - (int)strlen(lb), y, lb, A(m->colour | TUI_BRIGHT));
            }
        }
        putch(sf, in, ox + cx / 2, oy + cy / 4, (char)s_snap.prof.glyph, TUI_ATTR(TUI_BLACK, (s_snap.prof.colour & 7) | TUI_BRIGHT));
    }
    if (!wide) near_list(sf, tui_rect_make(a.x, a.y + ph + 1, a.w, a.y + a.h - (a.y + ph + 1)), now);
}

/* ------------------------------------------------------------ pings */

static void page_pings(tui_surface *sf, tui_rect a, uint32_t now)
{
    int order[PARTY_PINGS], n = 0;
    for (int i = 0; i < PARTY_PINGS; i++) if (s_snap.ping[i].used) order[n++] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && s_snap.ping[order[j]].rx_ms > s_snap.ping[order[j - 1]].rx_ms; j--) { const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    char chip[32];
    snprintf(chip, sizeof(chip), "%d FOUND", n);
    const tui_rect in = ls_plate(sf, a, a, TUI_RED, "PINGS", chip, TUI_WHITE);
    if (in.w <= 0) return;
    if (!n) {
        static const char *const ART[] = {
            "        .   *   .        ",
            "    .  ( ( (o) ) )  .    ",
            "        '   |   '        ",
            "            |            ",
            "       ____/_\\____       ",
        };
        for (int i = 0; i < 5; i++) put(sf, in, in.x + (in.w - 25) / 2, in.y + 1 + i, ART[i], i == 1 ? A(TUI_RED | TUI_BRIGHT) : A(TUI_CYAN | TUI_BRIGHT));
        put(sf, in, in.x, in.y + 8, "No pings yet.", TXT);
        put(sf, in, in.x, in.y + 10, "Finds come from a WARDRIVE drive and from", DIM);
        put(sf, in, in.x, in.y + 11, "MARK.", DIM);
        return;
    }
    if (s_top > n - 1) s_top = n - 1;
    if (s_top < 0) s_top = 0;
    if (s_top > 0) { char t[32]; snprintf(t, sizeof(t), "^ %d ABOVE", s_top); foot_tab(sf, a, false, t, TUI_WHITE); }
    int y = in.y, k = s_top;
    for (; k < n && y + 2 <= in.y + in.h; k++) {
        const party_pingrec_t *p = &s_snap.ping[order[k]];
        const uint8_t hue = CLASS_HUE[p->cls];
        const uint32_t age = now - p->rx_ms;
        /* a ring opens from a ping for four seconds after it lands */
        static const char *const RING[5] = { "(*)", "(o)", "( )", "( .", ". ." };
        putf(sf, in, in.x, y, age < 4000 ? A(hue | TUI_BRIGHT) : A(hue), "%s", age < 5000 ? RING[age / 1000] : " o ");
        putf(sf, in, in.x + 4, y, TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT), " %s ", party_class_tag(p->cls));
        putf(sf, in, in.x + 10, y, TXT, "%-.*s", in.w - 16 > 0 ? in.w - 16 : 0, p->label);
        char ag[8];
        fmt_age(ag, sizeof(ag), now - p->seen_ms);
        putf(sf, in, in.x + in.w - 5, y, A(age < 60000 ? TUI_GREEN | TUI_BRIGHT : TUI_YELLOW | TUI_BRIGHT), "%5s", ag);
        char d[16], u[16], line2[96];
        float dist = -1, b = 0;
        if (s_snap.fix) party_geo(s_snap.lat, s_snap.lon, p->lat, p->lon, &dist, &b);
        fmt_m(d, sizeof(d), dist);
        fmt_m(u, sizeof(u), p->unc_m);
        int n2 = snprintf(line2, sizeof(line2), "%s%s", p->mine ? "you" : "by ", p->mine ? "" : p->who);
        if (p->nrep > 1) n2 += snprintf(line2 + n2, sizeof(line2) - (size_t)n2, " +%u", p->nrep - 1);
        put(sf, in, in.x + 10, y + 1, line2, p->nrep > 1 ? A(TUI_YELLOW | TUI_BRIGHT) : DIM);
        n2 = snprintf(line2, sizeof(line2), "%s %s", d, dist >= 0 ? compass8(b) : "");
        const int dx = in.x + 10 + 12;
        put(sf, in, dx, y + 1, line2, TXT);
        n2 = snprintf(line2, sizeof(line2), "+-%s", u);
        if (p->spread_m > 50) {
            char sp[16];
            fmt_m(sp, sizeof(sp), p->spread_m);
            snprintf(line2 + n2, sizeof(line2) - (size_t)n2, "  moved %s", sp);
        }
        put(sf, in, dx + 10, y + 1, line2, DIM);
        if (s_snap.find_kind == 2 && s_snap.find_id == order[k]) put(sf, in, in.x + in.w - 6, y + 1, " FIND ", TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT));
        hit_add(a.x, y, a.w, 2, T_PING, order[k]);
        y += 3;
    }
    if (k < n) { char t[32]; snprintf(t, sizeof(t), "v %d MORE", n - k); foot_tab(sf, a, true, t, TUI_WHITE); }
}

/* ------------------------------------------------------------ files */

#define SHARE_MAX 12
static char s_share[SHARE_MAX][PARTY_FNAME_MAX + 1];
static uint32_t s_share_size[SHARE_MAX];
static int s_nshare;
static uint32_t s_share_ms;

static void share_scan(void)
{
    char dir[96];
    snprintf(dir, sizeof(dir), "%s/share", party_io_dir());
    s_nshare = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_nshare < SHARE_MAX) {
        if (e->d_name[0] == '.') continue;
        char p[400];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        snprintf(s_share[s_nshare], sizeof(s_share[0]), "%.32s", e->d_name);
        s_share_size[s_nshare++] = (uint32_t)st.st_size;
    }
    closedir(d);
}

static void kb(char *o, size_t n, uint32_t bytes) { snprintf(o, n, "%.1f KB", bytes / 1024.0f); }

static void page_files(tui_surface *sf, tui_rect a, uint32_t now)
{
    int y = a.y;
    char t[48];
    const party_xfer_view_t *x = &s_snap.tx;
    /* the file going out, as its own plate while it goes */
    if (x->state != PARTY_X_NONE) {
        static const char *const ST[] = { "", "OFFERING", "SENDING", "CHECKING", "DELIVERED", "NOT SENT" };
        const uint8_t hue = x->state == PARTY_X_FAIL ? TUI_RED : x->state == PARTY_X_DONE ? TUI_GREEN : TUI_YELLOW;
        kb(t, sizeof(t), x->size);
        const tui_rect r = tui_rect_make(a.x, y, a.w, 6);
        const tui_rect in = ls_plate(sf, a, r, hue, ST[x->state], t, TUI_WHITE);
        if (in.w > 0) {
            put(sf, in, in.x, in.y, x->name, TXT);
            const float v = x->state == PARTY_X_DONE ? 1 : x->nchunks ? (float)(x->sent < x->nchunks ? x->sent : x->nchunks) / x->nchunks : 0;
            snprintf(t, sizeof(t), "%3d%%", (int)lroundf(v * 100));
            ls_plate_meter(sf, in, in.x, in.y + 2, in.w - 6, v, hue);
            put(sf, in, in.x + in.w - 4, in.y + 2, t, TXT);
            const uint32_t bps = s_snap.link.bulk_bps ? s_snap.link.bulk_bps : 1760;
            const float eta = x->nchunks > x->sent ? (x->nchunks - x->sent) * 200.0f * 8 / bps : 0;
            putf(sf, in, in.x, in.y + 3, TXT, "%u/%u", x->done, x->receivers);
            putf(sf, in, in.x + 6, in.y + 3, DIM, "have it  %s %u kb/s",
                 s_snap.link.bulk_via == PARTY_VIA_FLRC ? "FLRC" : s_snap.link.bulk_via == PARTY_VIA_GFSK ? "GFSK" : "LORA", (unsigned)(bps / 1000));
            if (x->state == PARTY_X_SEND) putf(sf, in, in.x + in.w - 8, in.y + 3, TXT, "%8s", (snprintf(t, sizeof(t), "~%.0f s", eta + 1), t));
            else if (x->why[0]) putf(sf, in, in.x + in.w - (int)strlen(x->why), in.y + 3, A(TUI_RED | TUI_BRIGHT), "%s", x->why);
        }
        y += 7;
    }
    if (now - s_share_ms > 5000 || !s_share_ms) { share_scan(); s_share_ms = now ? now : 1; }
    /* what this card can send */
    const bool card = party_io_card();
    int rows = !card ? 1 : !s_nshare ? 2 : s_nshare;
    const int room = (a.y + a.h - y) - 8;      /* leave the RECEIVED plate a few rows */
    if (rows + 4 > room) rows = room - 4 > 1 ? room - 4 : 1;
    const tui_rect sr = tui_rect_make(a.x, y, a.w, rows + 4);
    snprintf(t, sizeof(t), "%d ON CARD", s_nshare);
    const tui_rect si = ls_plate(sf, a, sr, TUI_YELLOW, "SEND", card ? t : "NO CARD", card ? TUI_WHITE : TUI_RED);
    if (si.w > 0) {
        put(sf, si, si.x, si.y, "from /sdcard/party/share, tap one", DIM);
        int yy = si.y + 2;
        if (!card) put(sf, si, si.x, yy, "No card.", A(TUI_RED | TUI_BRIGHT));
        else if (!s_nshare) { put(sf, si, si.x, yy++, "Put files of up to 64 KB in party/share", TXT); put(sf, si, si.x, yy, "on the card.", TXT); }
        for (int i = 0; i < s_nshare && yy < si.y + si.h; i++) {
            const bool big = s_share_size[i] > PARTY_FILE_MAX;
            put(sf, si, si.x, yy, ">", A(TUI_YELLOW | TUI_BRIGHT));
            putf(sf, si, si.x + 2, yy, big ? DIM : TXT, "%-.*s", si.w - 13 > 0 ? si.w - 13 : 0, s_share[i]);
            kb(t, sizeof(t), s_share_size[i]);
            putf(sf, si, si.x + si.w - 9, yy, big ? A(TUI_RED | TUI_BRIGHT) : DIM, "%9s", t);
            hit_add(a.x, yy, a.w, 1, T_SHARE, i);
            yy++;
        }
    }
    y += sr.h + 1;
    /* what came in */
    if (y + 4 > a.y + a.h) return;
    snprintf(t, sizeof(t), "%d IN", s_snap.nrxf);
    int rh = 4 + (s_snap.nrxf ? s_snap.nrxf * 3 - 1 : 1);
    if (rh > a.y + a.h - y) rh = a.y + a.h - y;
    const tui_rect rr = tui_rect_make(a.x, y, a.w, rh);
    const tui_rect ri = ls_plate(sf, a, rr, TUI_GREEN, "RECEIVED", t, TUI_WHITE);
    if (ri.w <= 0) return;
    put(sf, ri, ri.x, ri.y, "into /sdcard/party", DIM);
    int yy = ri.y + 2;
    if (!s_snap.nrxf) put(sf, ri, ri.x, yy, s_snap.prof.files ? "Files members send land here." : "Taking files is off (OPTIONS > RADIO).", TXT);
    for (int i = 0; i < s_snap.nrxf && yy + 1 < ri.y + ri.h; i++) {
        const party_rxfile_t *f = &s_snap.rxf[i];
        const uint8_t hue = f->state == PARTY_X_DONE ? TUI_GREEN : f->state == PARTY_X_FAIL ? TUI_RED : TUI_WHITE;
        putf(sf, ri, ri.x, yy, A(hue | TUI_BRIGHT), "%-.*s", ri.w - 16 > 0 ? ri.w - 16 : 0, f->name);
        put(sf, ri, ri.x + ri.w - 14, yy, "from", DIM);
        putf(sf, ri, ri.x + ri.w - 9, yy, TXT, "%-.9s", f->from);
        kb(t, sizeof(t), f->size);
        ls_plate_meter(sf, ri, ri.x, yy + 1, ri.w - 11, f->size ? (float)(f->got_bytes > f->size ? f->size : f->got_bytes) / f->size : 0,
                       f->state == PARTY_X_FAIL ? TUI_RED : TUI_GREEN);
        putf(sf, ri, ri.x + ri.w - 9, yy + 1, DIM, "%9s", t);
        yy += 3;
    }
}

/* ------------------------------------------------------------ lobby */

static void page_lobby(tui_surface *sf, tui_rect a)
{
    int y = a.y;
    const int n = party_prof_count();
    if (!n) {
        static const char *const ART[] = {
            "   o      o     o   ",
            "  /|\\    /|\\   /|\\  ",
            "  / \\ )  / \\ ( / \\  ",
            "     (  ) ( ) )     ",
            "    __\\/\\/\\/__      ",
        };
        const int h = a.h < 22 ? a.h : 22;
        const tui_rect in = ls_plate(sf, a, tui_rect_make(a.x, y, a.w, h), TUI_CYAN, "A PARTY", "NEW HERE", TUI_WHITE);
        hit_add(a.x, a.y, a.w, a.h, T_LOBBY, 0);
        if (in.w <= 0) return;
        y = in.y + 1;
        for (int i = 0; i < 5; i++) put(sf, in, in.x + (in.w - 20) / 2, y + i, ART[i], i < 3 ? A(TUI_CYAN | TUI_BRIGHT) : A(TUI_YELLOW | TUI_BRIGHT));
        y += 6;
        put(sf, in, in.x, y++, "Share where you are and what you find with", TXT);
        put(sf, in, in.x, y++, "other LakeShark users: Flock cameras, drones,", TXT);
        put(sf, in, in.x, y++, "police gear, trackers, the same car twice.", TXT);
        y++;
        put(sf, in, in.x, y++, "Everyone in the party types the same code.", DIM);
        y++;
        if (y + 3 <= in.y + in.h) ls_plate_key(sf, in, tui_rect_make(in.x, y, in.w, 3), "START  GUIDED SETUP", TUI_YELLOW, false, true);
        return;
    }
    for (int i = 0; i < n && y + 4 <= a.y + a.h; i++) {
        const party_prof_t *p = party_prof(i);
        const bool cur = i == party_prof_cur();
        char title[24];
        snprintf(title, sizeof(title), "%c %s", p->glyph, p->name);
        const tui_rect r = tui_rect_make(a.x, y, a.w, 4);
        const tui_rect in = ls_plate(sf, a, r, cur ? TUI_MAGENTA : TUI_BLUE, title, p->party, cur ? TUI_MAGENTA : TUI_WHITE);
        hit_add(a.x, y, a.w, 4, T_PROF, i);
        y += 5;
        if (in.w <= 0) continue;
        char watch[64] = "";
        size_t k = 0;
        for (int c = 1; c < PARTY_CLASSES && k + 5 < sizeof(watch); c++)
            if (p->send_mask & PARTY_CMASK(c) && c != PARTY_C_HELP) k += (size_t)snprintf(watch + k, sizeof(watch) - k, "%s ", party_class_tag(c));
        putch(sf, in, in.x, in.y, (char)p->glyph, A(p->colour | TUI_BRIGHT));
        putf(sf, in, in.x + 2, in.y, cur ? TXT : DIM, "%-8s", p->call);
        put(sf, in, in.x + 11, in.y, p->share == PARTY_SHARE_EXACT ? "EXACT" : p->share == PARTY_SHARE_ROUGH ? "ROUGH" : "HIDDEN", A(TUI_YELLOW | (cur ? TUI_BRIGHT : 0)));
        if (!p->code[0]) put(sf, in, in.x + 19, in.y, " NO CODE ", TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT));
        put(sf, in, in.x, in.y + 1, "watch", DIM);
        putf(sf, in, in.x + 6, in.y + 1, A(TUI_CYAN | (cur ? TUI_BRIGHT : 0)), "%-.*s", in.w - 6, watch);
    }
    if (y < a.y + a.h) put(sf, a, a.x + 2, y++, "START joins the lit one. UP/DOWN picks.", BK(TUI_WHITE));
    if (y < a.y + a.h) put(sf, a, a.x + 2, y, "OPTIONS edits it; N makes another.", BK(TUI_WHITE));
}

/* ------------------------------------------------------------ guided setup */

enum { W_HELLO, W_CODE, W_YOU, W_SHARE, W_WATCH, W_ALERT, W_READY, W_STEPS };
static const char *const W_TITLE[W_STEPS] = { "WELCOME", "THE PARTY", "YOU", "YOUR POSITION", "WATCH FOR", "ALERTS", "READY" };
static int s_watch_pick, s_alert_pick = 1;

static party_prof_t *wiz_prof(void) { return party_prof(party_prof_cur()); }

/* The key takes 2048 rounds of SHA-256: worked out again only when the code changes. */
static bool key_of(const char *code, party_key_t *k)
{
    static char last[24];
    static party_key_t lk;
    static bool ok, valid;
    if (!valid || strncmp(last, code, sizeof(last))) {
        snprintf(last, sizeof(last), "%s", code);
        ok = party_key_derive(code, &lk);
        valid = true;
    }
    *k = lk;
    return ok;
}

static void on_text_party(const char *t) { party_prof_t *p = wiz_prof(); if (p) snprintf(p->party, sizeof(p->party), "%s", t); }
static void on_text_code(const char *t) { party_prof_t *p = wiz_prof(); if (p) snprintf(p->code, sizeof(p->code), "%s", t); }
static void on_text_call(const char *t)
{
    party_prof_t *p = wiz_prof();
    if (!p) return;
    char c[PARTY_CALL_MAX + 1];
    party_code_norm(t, c, sizeof(c));
    if (c[0]) snprintf(p->call, sizeof(p->call), "%s", c);
}
static void on_text_name(const char *t) { party_prof_t *p = wiz_prof(); if (p && t[0]) snprintf(p->name, sizeof(p->name), "%s", t); }

/* Choices as big framed keys (ls_plate.h), `per` to a row, three rows tall
   with a row of air between; a tap is the choice's index, as the keys say.
   The value goes beside the label when it fits. Returns the rows used. */
static int wiz_keys(tui_surface *sf, tui_rect c, int x, int y, int w, const ls_btn_t *b, int n, int per, const uint8_t *hues)
{
    if (per < 1) per = 1;
    const int kw = (w - (per - 1)) / per;
    int used = 0;
    for (int i = 0; i < n; i++) {
        const tui_rect k = tui_rect_make(x + (i % per) * (kw + 1), y + (i / per) * 4, kw, 3);
        char lab[40];
        if (b[i].value && b[i].value[0] && (int)(strlen(b[i].label) + strlen(b[i].value)) + 2 <= kw - 4)
            snprintf(lab, sizeof(lab), "%s  %s", b[i].label, b[i].value);
        else snprintf(lab, sizeof(lab), "%s", b[i].label);
        ls_plate_key(sf, c, k, lab, hues ? hues[i] : TUI_CYAN, b[i].on && !b[i].dim, !b[i].dim);
        hit_add(k.x, k.y, k.w, k.h, T_WIZ, i);
        used = (i / per) * 4 + 3;
    }
    return used;
}

static void wiz_apply_alert(party_prof_t *p, int pick)
{
    static const uint8_t STYLE[3] = { PARTY_STYLE_VIBE, PARTY_STYLE_BANNER, PARTY_STYLE_SOUND };
    for (int c = 0; c < PARTY_CLASSES; c++) if (c != PARTY_C_HELP) p->style[c] = STYLE[pick];
}

static const char *const W_HEAD[W_STEPS] = { "WELCOME", "THE PARTY", "YOU", "POSITION", "WATCH FOR", "ALERTS", "READY" };

/* Rows each step takes under its headline, so its plate fits it. */
static int wiz_rows(bool wide)
{
    const int k6 = wide ? 7 : 11;          /* six keys: three a row, or two */
    switch (s_wiz) {
    case W_HELLO: return 9;
    case W_CODE: return 10;
    case W_YOU: return 9;
    case W_SHARE: return 3 + k6;
    case W_WATCH: return 3 + k6 + 2;
    case W_ALERT: return 3 + (wide ? 3 : 7) + 2;
    default: return 11;
    }
}

static void page_wiz(tui_surface *sf, tui_rect a, bool wide)
{
    party_prof_t *p = wiz_prof();
    if (!p) { s_wiz = -1; return; }
    int y = a.y;
    /* steps as campfire logs: lit up to here */
    for (int i = 0; i < W_STEPS; i++)
        putf(sf, a, a.x + 1 + i * 4, y, i <= s_wiz ? BK(TUI_YELLOW | TUI_BRIGHT) : BK(TUI_BLUE | TUI_BRIGHT), "%s", i < s_wiz ? "=#=" : i == s_wiz ? "(*)" : "---");
    y += 2;
    /* BACK and NEXT, big, at the foot of every step */
    const int by = a.y + a.h - 3, bw = (a.w - 1) / 2;
    ls_plate_key(sf, a, tui_rect_make(a.x, by, bw, 3), s_wiz ? "< BACK" : "< CLOSE", TUI_WHITE, false, true);
    hit_add(a.x, by, bw, 3, T_WIZ, 200);
    if (s_wiz < W_READY) {
        ls_plate_key(sf, a, tui_rect_make(a.x + bw + 1, by, a.w - bw - 1, 3), "NEXT >", TUI_YELLOW, false, true);
        hit_add(a.x + bw + 1, by, a.w - bw - 1, 3, T_WIZ, 201);
    }
    /* the step, on its plate, its name as the headline */
    char chip[32];
    snprintf(chip, sizeof(chip), "STEP %d OF %d", s_wiz + 1, W_STEPS);
    int ph = 2 + LS_CALLS_BIG_ROWS + 1 + wiz_rows(wide);
    if (ph > by - 1 - y) ph = by - 1 - y;
    const tui_rect in = ls_plate(sf, a, tui_rect_make(a.x, y, a.w, ph), TUI_YELLOW, W_TITLE[s_wiz], chip, TUI_WHITE);
    if (in.w <= 0) return;
    const int x = in.x, w = in.w;
    y = in.y;
    if (s_wiz == W_HELLO) {
        static const char *const ART[] = {
            "        )  (         ",
            "   o   (  ) )   o    ",
            "  /|\\  _\\/\\/_  /|\\   ",
            "  / \\ '======' / \\   ",
        };
        for (int i = 0; i < 4; i++) put(sf, in, x + w - 21, y + i, ART[i], i < 2 ? A(TUI_RED | TUI_BRIGHT) : i == 2 ? A(TUI_CYAN | TUI_BRIGHT) : A(TUI_YELLOW | TUI_BRIGHT));
        headline(sf, in, x, y, w - 21, W_HEAD[s_wiz], TUI_YELLOW);
    } else headline(sf, in, x, y, w, W_HEAD[s_wiz], TUI_YELLOW);
    y += LS_CALLS_BIG_ROWS + 1;
    const int per6 = wide ? 3 : 2;
    switch (s_wiz) {
    case W_HELLO: {
        put(sf, in, x, y++, "A party is a few LakeSharks on one code.", TXT);
        put(sf, in, x, y++, "Members see each other on RADAR, and what", TXT);
        put(sf, in, x, y++, "anyone finds pings the rest with a rough", TXT);
        put(sf, in, x, y++, "place. Six steps; all of it can be changed", TXT);
        put(sf, in, x, y++, "later in OPTIONS and saved as profiles.", TXT);
        y++;
        const ls_btn_t b[] = { { "NAME", p->name, 'n', false, false, false } };
        wiz_keys(sf, in, x, y, wide ? w / 2 : w, b, 1, 1, NULL);
        s_nhit--;                              /* the key renames: tap 100, as before */
        hit_add(x, y, wide ? w / 2 : w, 3, T_WIZ, 100);
        break;
    }
    case W_CODE: {
        put(sf, in, x, y++, "Everyone types the same code; case and", TXT);
        put(sf, in, x, y++, "spaces do not count. It is the key too:", TXT);
        put(sf, in, x, y++, "pick something others will not guess.", TXT);
        y++;
        party_key_t k;
        const bool ok = key_of(p->code, &k);
        put(sf, in, x, y, "CODE", DIM);
        putf(sf, in, x + 6, y, ok ? A(TUI_GREEN | TUI_BRIGHT) : A(TUI_RED | TUI_BRIGHT), "%s", p->code[0] ? p->code : "(none yet)");
        if (ok) putf(sf, in, x + w - 21, y, DIM, "party %04X  ch %2u", k.party, k.channel + 1);
        else put(sf, in, x + w - 28, y, "at least 4 letters or digits", A(TUI_RED | TUI_BRIGHT));
        y++;
        put(sf, in, x, y, "NAME", DIM);
        putf(sf, in, x + 6, y, A(TUI_MAGENTA | TUI_BRIGHT), "%s", p->party);
        y += 2;
        const ls_btn_t b[] = { { "CODE", "TYPE IT", 'c', false, false, false }, { "NAME", "ON SCREEN", 'n', false, false, false } };
        wiz_keys(sf, in, x, y, w, b, 2, 2, NULL);
        break;
    }
    case W_YOU: {
        put(sf, in, x, y++, "How the others see you on their radar.", TXT);
        y++;
        /* a big glyph */
        for (int r = 0; r < 3; r++) for (int c = 0; c < 5; c++) putch(sf, in, x + 1 + c, y + r, (char)p->glyph, A(p->colour | TUI_BRIGHT));
        int ci = 0;
        while (ci < (int)sizeof(COLOURS) - 1 && COLOURS[ci] != p->colour) ci++;
        putf(sf, in, x + 9, y, TXT, "%s", p->call);
        putf(sf, in, x + 9, y + 2, A(p->colour | TUI_BRIGHT), "%s", COLOUR_N[ci]);
        y += 4;
        const ls_btn_t b[] = { { "CALL", p->call, 'c', false, false, false }, { "GLYPH", "NEXT", 'g', false, false, false },
                               { "COLOUR", COLOUR_N[ci], 'k', false, false, false } };
        wiz_keys(sf, in, x, y, w, b, 3, 3, NULL);
        break;
    }
    case W_SHARE: {
        put(sf, in, x, y++, "EXACT: your fix. ROUGH: the 1 km square", TXT);
        put(sf, in, x, y++, "you are in. HIDDEN: nothing, finds still go.", TXT);
        y++;
        static const char *const RATE_N[] = { "SMART", "10 S", "30 S", "1 MIN", "5 MIN" };
        const ls_btn_t b[] = {
            { "HIDDEN", "OFF", '1', p->share == PARTY_SHARE_OFF, false, false },
            { "ROUGH", "1 KM", '2', p->share == PARTY_SHARE_ROUGH, false, false },
            { "EXACT", "GPS", '3', p->share == PARTY_SHARE_EXACT, false, false },
            { "RATE", RATE_N[p->rate % PARTY_RATES], 'r', false, false, false },
            { "VEHICLE", p->vehicle ? "IN ONE" : "ON FOOT", 'v', p->vehicle, false, false },
            { "NEXT", ">", 'n', false, false, false },
        };
        static const uint8_t H[6] = { TUI_CYAN, TUI_CYAN, TUI_CYAN, TUI_MAGENTA, TUI_MAGENTA, TUI_YELLOW };
        wiz_keys(sf, in, x, y, w, b, 6, per6, H);
        break;
    }
    case W_WATCH: {
        put(sf, in, x, y++, "What you send and want to hear about.", TXT);
        put(sf, in, x, y++, "Fine tune each class in OPTIONS later.", DIM);
        y++;
        ls_btn_t b[6];
        static const char *const SUB[6] = { "LPR POL BWC", "UAS", "TAG FOL", "CAR POL FOL", "SKM HAK WRB", "ALL 16" };
        int nb = 0;
        for (int i = 0; i < 6 && i < party_preset_count(); i++) {
            static char lab[6][18];
            snprintf(lab[i], sizeof(lab[i]), "%s", party_preset_name(i));
            char *pl = strchr(lab[i], ' ');
            if (pl && i != 0) *pl = 0;
            if (i == 0) snprintf(lab[i], sizeof(lab[i]), "FLOCK");
            b[i] = (ls_btn_t){ lab[i], SUB[i], (char)('1' + i), s_watch_pick == i, false, false };
            nb++;
        }
        y += wiz_keys(sf, in, x, y, w, b, nb, per6, NULL) + 1;
        put(sf, in, x, y, "sends", DIM);
        putf(sf, in, x + 6, y, A(TUI_CYAN | TUI_BRIGHT), "%s", party_preset_name(s_watch_pick));
        break;
    }
    case W_ALERT: {
        put(sf, in, x, y++, "BUZZ: a pattern per class, no sound,", TXT);
        put(sf, in, x, y++, "no screen. HELP always sounds.", TXT);
        y++;
        const ls_btn_t b[] = {
            { "BUZZ", "ONLY", '1', s_alert_pick == 0, false, false },
            { "BANNER", "QUIET", '2', s_alert_pick == 1, false, false },
            { "SOUND", "+ BUZZ", '3', s_alert_pick == 2, false, false },
            { "TEST", "BUZZ", 't', false, false, false },
        };
        static const uint8_t H[4] = { TUI_CYAN, TUI_CYAN, TUI_CYAN, TUI_MAGENTA };
        y += wiz_keys(sf, in, x, y, w, b, 4, wide ? 4 : 2, H) + 1;
        put(sf, in, x, y, party_io_vibe_present() ? "vibration motor found" : "no vibration motor answers on this board",
            party_io_vibe_present() ? A(TUI_GREEN | TUI_BRIGHT) : A(TUI_YELLOW | TUI_BRIGHT));
        break;
    }
    case W_READY: {
        char watch[80] = "";
        size_t k = 0;
        for (int c = 1; c < PARTY_CLASSES && k + 5 < sizeof(watch); c++)
            if (p->send_mask & PARTY_CMASK(c)) k += (size_t)snprintf(watch + k, sizeof(watch) - k, "%s ", party_class_tag(c));
        static const char *const L[7] = { "PROFILE", "PARTY", "CODE", "YOU", "SHARE", "WATCH", "ALERTS" };
        for (int i = 0; i < 7; i++) put(sf, in, x, y + i, L[i], DIM);
        const int vx = x + 9;
        put(sf, in, vx, y++, p->name, TXT);
        put(sf, in, vx, y++, p->party, A(TUI_MAGENTA | TUI_BRIGHT));
        put(sf, in, vx, y++, p->code[0] ? p->code : "MISSING", p->code[0] ? A(TUI_GREEN | TUI_BRIGHT) : A(TUI_RED | TUI_BRIGHT));
        putf(sf, in, vx, y++, A(p->colour | TUI_BRIGHT), "%c %s", p->glyph, p->call);
        put(sf, in, vx, y++, p->share == PARTY_SHARE_EXACT ? "EXACT" : p->share == PARTY_SHARE_ROUGH ? "ROUGH" : "HIDDEN", A(TUI_YELLOW | TUI_BRIGHT));
        putf(sf, in, vx, y++, A(TUI_CYAN | TUI_BRIGHT), "%-.*s", w - 9, watch);
        put(sf, in, vx, y++, s_alert_pick == 0 ? "buzz only" : s_alert_pick == 1 ? "quiet banners" : "sound and buzz", TXT);
        y++;
        const ls_btn_t b[] = { { "START", "PARTY", 's', true, !p->code[0], false }, { "SAVE", "ONLY", 'v', false, false, false } };
        static const uint8_t H[2] = { TUI_GREEN, TUI_CYAN };
        wiz_keys(sf, in, x, y, w, b, 2, 2, H);
        break;
    }
    }
}


static void wiz_save_profile(void) { if (!party_prof_save() && !party_io_card()) say("NO CARD: PROFILE KEPT UNTIL REBOOT"); }

static void wiz_open(bool fresh)
{
    if (fresh || !party_prof_count()) {
        char nm[16];
        snprintf(nm, sizeof(nm), "PARTY %u", (unsigned)(party_prof_count() + 1) % 10u);
        if (party_prof_new(nm) < 0) { say("SIX PROFILES: DELETE ONE IN OPTIONS"); return; }
    }
    s_wiz = W_HELLO;
    s_watch_pick = 0;
    s_alert_pick = 1;
}

static bool party_go(void);

static void wiz_act(int what)
{
    party_prof_t *p = wiz_prof();
    if (!p) return;
    if (what == 200) { if (s_wiz > 0) s_wiz--; else { s_wiz = -1; party_prof_save(); } return; }
    if (what == 201) { if (s_wiz < W_READY) s_wiz++; return; }
    switch (s_wiz) {
    case W_HELLO: if (what == 100) ls_keyboard_open("PROFILE NAME", p->name, sizeof(p->name) - 1, on_text_name); break;
    case W_CODE:
        if (what == 0) ls_keyboard_open("PARTY CODE", p->code, sizeof(p->code) - 1, on_text_code);
        else if (what == 1) ls_keyboard_open("PARTY NAME", p->party, sizeof(p->party) - 1, on_text_party);
        break;
    case W_YOU:
        if (what == 0) ls_keyboard_open("CALL SIGN", p->call, PARTY_CALL_MAX, on_text_call);
        else if (what == 1) {
            const char *g = strchr(GLYPHS, p->glyph);
            p->glyph = (uint8_t)(g && g[1] ? g[1] : GLYPHS[0]);
        } else if (what == 2) {
            int ci = 0;
            while (ci < (int)sizeof(COLOURS) && COLOURS[ci] != p->colour) ci++;
            p->colour = COLOURS[(ci + 1) % sizeof(COLOURS)];
        }
        break;
    case W_SHARE:
        if (what <= 2) p->share = (uint8_t)what;
        else if (what == 3) p->rate = (uint8_t)((p->rate + 1) % PARTY_RATES);
        else if (what == 4) p->vehicle = !p->vehicle;
        else if (what == 5) s_wiz++;
        break;
    case W_WATCH: if (what >= 0 && what < party_preset_count()) { s_watch_pick = what; party_prof_preset(p, what); } break;
    case W_ALERT:
        if (what <= 2) { s_alert_pick = what; wiz_apply_alert(p, what); }
        else party_vibe_play(1);
        break;
    case W_READY:
        wiz_save_profile();
        if (what == 0) { if (!p->code[0]) { say("TYPE A PARTY CODE FIRST"); s_wiz = W_CODE; return; } s_wiz = -1; party_go(); }
        else { s_wiz = -1; say("PROFILE SAVED"); }
        break;
    }
}

/* ------------------------------------------------------------ OPTIONS */

static const char *const SHARE_N[] = { "HIDDEN", "ROUGH 1 KM", "EXACT" };
static const char *const RATE_N[] = { "SMART", "10 S", "30 S", "1 MIN", "5 MIN" };
static const char *const MODE_N[] = { "AUTO", "LONG RANGE", "FAST" };
static const char *const AIR_N[] = { "1 %", "3 %", "10 %", "25 %" };
static const char *const NEAR_N[] = { "ANYWHERE", "500 M", "2 KM", "10 KM" };
static const char *const RSSI_N[] = { "ANY", "-85 DBM", "-75 DBM", "-65 DBM" };
static const char *const POCKET_N[] = { "OFF", "30 S", "1 MIN", "2 MIN" };
static const uint32_t POCKET_MS[] = { 0, 30000, 60000, 120000 };
static const char *const VIBE_N[PARTY_VIBES] = { "TAP", "DOUBLE", "TRIPLE", "LONG", "HEARTBEAT", "RISING", "SOS", "CAR" };
#define STYLE_N LS_ALERT_NAMES            /* the shared words (ls_alert.h) */
static const char *const GLYPH_N[] = { "@", "#", "$", "%", "&", "*", "+", "=", "?", "!", "A", "B", "C", "D", "E", "F",
                                        "G", "H", "J", "K", "M", "N", "P", "R", "S", "T", "V", "W", "X", "Y", "Z" };
static const char *const PRESET_N[] = { "FLOCK + POLICE", "DRONES", "TRACKERS + FOLLOW", "VEHICLES", "SKIMMERS + HACKS", "EVERYTHING" };

enum { O_PROFILE = 1, O_SHARE, O_RATE, O_VEHICLE, O_MODE, O_AIR, O_FILES, O_NEAR, O_RSSI, O_QUIET, O_POCKET, O_NORTH,
       O_PRESET, O_GLYPH, O_COLOUR, O_QFROM, O_QTO,
       O_SEND = 100, O_RECV = 200, O_STYLE = 300, O_VIBE = 400 };
enum { X_PARTY = 1, X_CODE, X_CALL, X_NAME, X_KW = 10 };

static party_prof_t *op(void) { return party_prof(party_prof_cur()); }
static void edited(void)
{
    party_prof_save();
    if (party_running()) party_prof_applied();
}

static int o_get(const ls_opt_t *o)
{
    const party_prof_t *p = op();
    if (!p) return 0;
    const int a = o->arg;
    if (a >= O_VIBE) return p->vibe[a - O_VIBE];
    if (a >= O_STYLE) return p->style[a - O_STYLE];
    if (a >= O_RECV) return (p->recv_mask >> (a - O_RECV)) & 1;
    if (a >= O_SEND) return (p->send_mask >> (a - O_SEND)) & 1;
    switch (a) {
    case O_PROFILE: return party_prof_cur();
    case O_SHARE: return p->share;
    case O_RATE: return p->rate;
    case O_VEHICLE: return p->vehicle;
    case O_MODE: return p->mode;
    case O_AIR: return p->airtime;
    case O_FILES: return p->files;
    case O_NEAR: return p->near;
    case O_RSSI: return p->min_rssi;
    case O_QUIET: return p->quiet;
    case O_POCKET: return p->pocket;
    case O_NORTH: return p->north_up;
    case O_PRESET: return 0;
    case O_GLYPH: { const char *g = strchr(GLYPHS, p->glyph); return g ? (int)(g - GLYPHS) : 0; }
    case O_COLOUR: { int ci = 0; while (ci < (int)sizeof(COLOURS) - 1 && COLOURS[ci] != p->colour) ci++; return ci; }
    }
    return 0;
}

static void o_set(const ls_opt_t *o, int v)
{
    party_prof_t *p = op();
    if (!p) return;
    const int a = o->arg;
    if (a >= O_VIBE) { p->vibe[a - O_VIBE] = (uint8_t)v; party_vibe_play(v); }   /* changing a pattern plays it */
    else if (a >= O_STYLE) p->style[a - O_STYLE] = (uint8_t)v;
    else if (a >= O_RECV) p->recv_mask = (uint16_t)(v ? p->recv_mask | PARTY_CMASK(a - O_RECV) : p->recv_mask & ~PARTY_CMASK(a - O_RECV));
    else if (a >= O_SEND) p->send_mask = (uint16_t)(v ? p->send_mask | PARTY_CMASK(a - O_SEND) : p->send_mask & ~PARTY_CMASK(a - O_SEND));
    else switch (a) {
    case O_PROFILE: if (!party_running()) party_prof_select(v); else say("STOP THE PARTY TO SWITCH PROFILE"); break;
    case O_SHARE: p->share = (uint8_t)v; break;
    case O_RATE: p->rate = (uint8_t)v; break;
    case O_VEHICLE: p->vehicle = (uint8_t)v; break;
    case O_MODE: p->mode = (uint8_t)v; break;
    case O_AIR: p->airtime = (uint8_t)v; break;
    case O_FILES: p->files = (uint8_t)v; break;
    case O_NEAR: p->near = (uint8_t)v; break;
    case O_RSSI: p->min_rssi = (uint8_t)v; break;
    case O_QUIET: p->quiet = (uint8_t)v; break;
    case O_POCKET: p->pocket = (uint8_t)(v & 3); ls_tui_pocket_auto(POCKET_MS[p->pocket]); break;
    case O_NORTH: p->north_up = (uint8_t)v; break;
    case O_PRESET: party_prof_preset(p, v); say(party_preset_name(v)); break;
    case O_GLYPH: p->glyph = (uint8_t)GLYPHS[v % (int)(sizeof(GLYPHS) - 1)]; break;
    case O_COLOUR: p->colour = COLOURS[v % (int)sizeof(COLOURS)]; break;
    }
    edited();
}

static double o_num(const ls_opt_t *o)
{
    const party_prof_t *p = op();
    return !p ? 0 : o->arg == O_QFROM ? p->quiet_from : p->quiet_to;
}
static void o_set_num(const ls_opt_t *o, double v)
{
    party_prof_t *p = op();
    if (!p) return;
    if (o->arg == O_QFROM) p->quiet_from = (uint8_t)v; else p->quiet_to = (uint8_t)v;
    edited();
}

static const char *o_text(const ls_opt_t *o)
{
    const party_prof_t *p = op();
    if (!p) return "";
    switch (o->arg) {
    case X_PARTY: return p->party;
    case X_CODE: return p->code;
    case X_CALL: return p->call;
    case X_NAME: return p->name;
    default: return o->arg >= X_KW && o->arg < X_KW + PARTY_KW ? p->kw[o->arg - X_KW] : "";
    }
}
static void o_set_text(const ls_opt_t *o, const char *s)
{
    party_prof_t *p = op();
    if (!p) return;
    switch (o->arg) {
    case X_PARTY: snprintf(p->party, sizeof(p->party), "%s", s); break;
    case X_CODE:
        if (party_running()) { say("NEW CODE: STOP AND START AGAIN"); }
        snprintf(p->code, sizeof(p->code), "%s", s);
        break;
    case X_CALL: on_text_call(s); break;
    case X_NAME: if (s[0]) snprintf(p->name, sizeof(p->name), "%s", s); break;
    default: if (o->arg >= X_KW && o->arg < X_KW + PARTY_KW) snprintf(p->kw[o->arg - X_KW], sizeof(p->kw[0]), "%s", s); break;
    }
    edited();
}

static void act_pocket(const ls_opt_t *o) { (void)o; ls_tui_pocket_set(true); }
static void act_new(const ls_opt_t *o) { (void)o; ls_opt_close(); wiz_open(true); }
static void act_guide(const ls_opt_t *o) { (void)o; ls_opt_close(); wiz_open(false); }
static void act_delete(const ls_opt_t *o)
{
    (void)o;
    party_prof_delete(party_prof_cur());
    party_prof_save();
    say("PROFILE DELETED");
}
static const char *why_running(const ls_opt_t *o) { (void)o; return party_running() ? "stop the party first" : NULL; }
static void act_test(const ls_opt_t *o) { (void)o; party_vibe_play(o->arg); }
static void show_motor(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%s", party_io_vibe_present() ? "MOTOR OK" : "NO MOTOR"); }

#define CLASS_ROWS(base, kind_, names_, n_) \
    {.label = "MARK", .kind = kind_, .arg = base + 0, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "ALPR / FLOCK", .kind = kind_, .arg = base + 1, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "CAMERA", .kind = kind_, .arg = base + 2, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "BODYCAM", .kind = kind_, .arg = base + 3, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "POLICE", .kind = kind_, .arg = base + 4, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "DRONE", .kind = kind_, .arg = base + 5, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "TRACKER", .kind = kind_, .arg = base + 6, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "VEHICLE", .kind = kind_, .arg = base + 7, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "FOLLOWING", .kind = kind_, .arg = base + 8, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "SKIMMER", .kind = kind_, .arg = base + 9, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "HACK TOOL", .kind = kind_, .arg = base + 10, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "WEARABLE", .kind = kind_, .arg = base + 11, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "INFRA", .kind = kind_, .arg = base + 12, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "WATCH LIST", .kind = kind_, .arg = base + 13, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "KEYWORDS", .kind = kind_, .arg = base + 14, .names = names_, .n = n_, .get = o_get, .set = o_set}, \
    {.label = "HELP", .kind = kind_, .arg = base + 15, .names = names_, .n = n_, .get = o_get, .set = o_set}

static const ls_opt_t O_SENDS[] = {
    {.label = "ONLY STRONGER THAN", .kind = LS_OPT_CYCLE, .arg = O_RSSI, .names = RSSI_N, .n = 4, .get = o_get, .set = o_set},
    CLASS_ROWS(O_SEND, LS_OPT_TOGGLE, NULL, 0),
};
static const ls_opt_t O_RECVS[] = {
    {.label = "ONLY WITHIN", .kind = LS_OPT_CYCLE, .arg = O_NEAR, .names = NEAR_N, .n = 4, .get = o_get, .set = o_set},
    CLASS_ROWS(O_RECV, LS_OPT_TOGGLE, NULL, 0),
};
static const ls_opt_t O_STYLES[] = { CLASS_ROWS(O_STYLE, LS_OPT_CYCLE, STYLE_N, PARTY_STYLES) };
static const ls_opt_t O_VIBES[] = {
    {.label = "MOTOR", .kind = LS_OPT_ACTION, .arg = 0, .act = act_test, .show = show_motor},
    CLASS_ROWS(O_VIBE, LS_OPT_CYCLE, VIBE_N, PARTY_VIBES),
};
static const ls_opt_t O_KWS[] = {
    {.label = "KEYWORD 1", .kind = LS_OPT_TEXT, .arg = X_KW + 0, .text = o_text, .set_text = o_set_text, .max = 12},
    {.label = "KEYWORD 2", .kind = LS_OPT_TEXT, .arg = X_KW + 1, .text = o_text, .set_text = o_set_text, .max = 12},
    {.label = "KEYWORD 3", .kind = LS_OPT_TEXT, .arg = X_KW + 2, .text = o_text, .set_text = o_set_text, .max = 12},
};
static const ls_opt_t O_POS[] = {
    {.label = "SHARE", .kind = LS_OPT_CYCLE, .arg = O_SHARE, .names = SHARE_N, .n = 3, .get = o_get, .set = o_set},
    {.label = "RATE", .kind = LS_OPT_CYCLE, .arg = O_RATE, .names = RATE_N, .n = 5, .get = o_get, .set = o_set},
    {.label = "IN A VEHICLE", .kind = LS_OPT_TOGGLE, .arg = O_VEHICLE, .get = o_get, .set = o_set},
};
static const ls_opt_t O_QUIETS[] = {
    {.label = "QUIET HOURS", .kind = LS_OPT_TOGGLE, .arg = O_QUIET, .get = o_get, .set = o_set},
    {.label = "FROM", .kind = LS_OPT_LEVEL, .arg = O_QFROM, .num = o_num, .set_num = o_set_num, .lo = 0, .hi = 23, .step = 1, .unit = "hour, 0-23"},
    {.label = "TO", .kind = LS_OPT_LEVEL, .arg = O_QTO, .num = o_num, .set_num = o_set_num, .lo = 0, .hi = 23, .step = 1, .unit = "hour, 0-23"},
};
static const ls_opt_t O_RADIO[] = {
    {.label = "MODE", .kind = LS_OPT_CYCLE, .arg = O_MODE, .names = MODE_N, .n = 3, .get = o_get, .set = o_set},
    {.label = "MAX AIRTIME", .kind = LS_OPT_CYCLE, .arg = O_AIR, .names = AIR_N, .n = 4, .get = o_get, .set = o_set},
    {.label = "TAKE FILES", .kind = LS_OPT_TOGGLE, .arg = O_FILES, .get = o_get, .set = o_set},
};
static const ls_opt_ctx_t C_SENDS = {.name = "SEND FINDS", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_SENDS)};
static const ls_opt_ctx_t C_RECVS = {.name = "RECEIVE", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_RECVS)};
static const ls_opt_ctx_t C_STYLES = {.name = "ALERTS", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_STYLES)};
static const ls_opt_ctx_t C_VIBES = {.name = "BUZZ PATTERNS", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_VIBES)};
static const ls_opt_ctx_t C_KWS = {.name = "KEYWORDS", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_KWS)};
static const ls_opt_ctx_t C_POS = {.name = "MY POSITION", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_POS)};
static const ls_opt_ctx_t C_QUIET = {.name = "QUIET HOURS", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_QUIETS)};
static const ls_opt_ctx_t C_RADIO = {.name = "RADIO", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_RADIO)};

static void on_pick_prof(int i) { if (i >= 0) { party_prof_select(i); party_prof_save(); } }
static void act_profile(const ls_opt_t *o)
{
    (void)o;
    ls_picker_open("PARTY PROFILE", on_pick_prof);
    for (int i = 0; i < party_prof_count(); i++) ls_picker_add(party_prof(i)->name, party_prof(i)->party);
    ls_picker_select(party_prof_cur());
}
static void show_profile(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    const party_prof_t *p = party_prof(party_prof_cur());
    snprintf(out, n, "%s", p ? p->name : "NONE");
}
static const ls_opt_t O_ROOT[] = {
    {.label = "PROFILE", .kind = LS_OPT_ACTION, .act = act_profile, .show = show_profile, .why_not = why_running},
    {.label = "PROFILE NAME", .kind = LS_OPT_TEXT, .arg = X_NAME, .text = o_text, .set_text = o_set_text, .max = 15},
    {.label = "PARTY NAME", .kind = LS_OPT_TEXT, .arg = X_PARTY, .text = o_text, .set_text = o_set_text, .max = 15},
    {.label = "PARTY CODE", .kind = LS_OPT_TEXT, .arg = X_CODE, .text = o_text, .set_text = o_set_text, .max = 23},
    {.label = "CALL SIGN", .kind = LS_OPT_TEXT, .arg = X_CALL, .text = o_text, .set_text = o_set_text, .max = PARTY_CALL_MAX},
    {.label = "GLYPH", .kind = LS_OPT_CYCLE, .arg = O_GLYPH, .names = GLYPH_N, .n = (int)(sizeof(GLYPH_N) / sizeof(GLYPH_N[0])), .get = o_get, .set = o_set},
    {.label = "COLOUR", .kind = LS_OPT_CYCLE, .arg = O_COLOUR, .names = COLOUR_N, .n = 7, .get = o_get, .set = o_set},
    {.label = "MY POSITION", .kind = LS_OPT_MENU, .sub = &C_POS},
    {.label = "WATCH PRESET", .kind = LS_OPT_CYCLE, .arg = O_PRESET, .names = PRESET_N, .n = 6, .get = o_get, .set = o_set},
    {.label = "SEND FINDS", .kind = LS_OPT_MENU, .sub = &C_SENDS},
    {.label = "RECEIVE", .kind = LS_OPT_MENU, .sub = &C_RECVS},
    {.label = "ALERTS", .kind = LS_OPT_MENU, .sub = &C_STYLES},
    {.label = "BUZZ PATTERNS", .kind = LS_OPT_MENU, .sub = &C_VIBES},
    {.label = "KEYWORDS", .kind = LS_OPT_MENU, .sub = &C_KWS},
    {.label = "QUIET HOURS", .kind = LS_OPT_MENU, .sub = &C_QUIET},
    {.label = "RADIO", .kind = LS_OPT_MENU, .sub = &C_RADIO},
    {.label = "RADAR NORTH UP", .kind = LS_OPT_TOGGLE, .arg = O_NORTH, .get = o_get, .set = o_set},
    {.label = "POCKET LOCK", .kind = LS_OPT_ACTION, .act = act_pocket},
    {.label = "AUTO POCKET LOCK", .kind = LS_OPT_CYCLE, .arg = O_POCKET, .names = POCKET_N, .n = 4, .get = o_get, .set = o_set},
    {.label = "GUIDED SETUP", .kind = LS_OPT_ACTION, .act = act_guide, .leaves = true, .why_not = why_running},
    {.label = "NEW PROFILE", .kind = LS_OPT_ACTION, .act = act_new, .leaves = true, .why_not = why_running},
    {.label = "DELETE PROFILE", .kind = LS_OPT_ACTION, .act = act_delete, .why_not = why_running},
};
static const ls_opt_ctx_t CTX = {.name = "PARTY", .job = -1, .radio = LS_RSEL_NONE, .back_to_screen = true, LS_OPT_ROWS(O_ROOT)};

/* ------------------------------------------------------------ MARK and FIND */

static const struct { int cls; const char *label; } MARKS[] = {
    { PARTY_C_MARK, "LOOK HERE" }, { PARTY_C_POLICE, "POLICE" }, { PARTY_C_ALPR, "PLATE READER" },
    { PARTY_C_CAMERA, "CAMERA" }, { PARTY_C_DRONE, "DRONE" }, { PARTY_C_VEHICLE, "THIS VEHICLE" },
    { PARTY_C_FOLLOW, "FOLLOWING ME" }, { PARTY_C_HELP, "HELP: COME TO ME" },
};
#define NMARKS ((int)(sizeof(MARKS) / sizeof(MARKS[0])))

static void on_mark(int i)
{
    if (s_snap.help && i == 0) { party_set_help(false); say("HELP CLEARED"); return; }
    if (s_snap.help) i--;
    if (i < 0 || i >= NMARKS) return;
    if (MARKS[i].cls == PARTY_C_HELP) party_set_help(true);
    else party_mark(MARKS[i].cls, MARKS[i].label);
    char t[48];
    snprintf(t, sizeof(t), "%s SENT%s", MARKS[i].label, s_snap.fix ? "" : ": NEEDS A FIX");
    say(t);
}
static void open_mark(void)
{
    if (!party_running()) { say("START THE PARTY FIRST"); return; }
    ls_picker_open("MARK FOR THE PARTY", on_mark);
    if (s_snap.help) ls_picker_add("CLEAR HELP", "tell the party you are fine");
    for (int i = 0; i < NMARKS; i++) ls_picker_add(MARKS[i].label, party_class_name(MARKS[i].cls));
    ls_picker_note("Pings everyone with where you are now.");
}

static int s_find_map[PARTY_MEMBERS + PARTY_PINGS + 1];
static void on_find(int i)
{
    if (i < 0) return;
    const int v = s_find_map[i];
    if (v == 0) { party_find_set(0, 0); say("FIND OFF"); return; }
    if (v < 1000) party_find_set(1, v);
    else party_find_set(2, v - 1000);
    say("FIND: BUZZES FASTER AS YOU GET CLOSE");
}
static void open_find(void)
{
    if (!party_running()) { say("START THE PARTY FIRST"); return; }
    ls_picker_open("FIND: HOME ON", on_find);
    int n = 0;
    if (s_snap.find_kind) { ls_picker_add("STOP FINDING", NULL); s_find_map[n++] = 0; }
    for (int i = 0; i < PARTY_MEMBERS; i++) {
        const party_member_t *m = &s_snap.member[i];
        if (!m->used || !m->pos) continue;
        char d[16];
        float dist = -1, b = 0;
        if (s_snap.fix) party_geo(s_snap.lat, s_snap.lon, m->lat, m->lon, &dist, &b);
        fmt_m(d, sizeof(d), dist);
        char det[32];
        snprintf(det, sizeof(det), "%s %s", d, dist >= 0 ? compass8(b) : "");
        ls_picker_add(m->call, det);
        s_find_map[n++] = m->id;
    }
    for (int i = 0; i < PARTY_PINGS; i++) {
        const party_pingrec_t *p = &s_snap.ping[i];
        if (!p->used) continue;
        char lab[48];
        snprintf(lab, sizeof(lab), "%s %s", party_class_tag(p->cls), p->label);
        ls_picker_add(lab, p->who);
        s_find_map[n++] = 1000 + i;
    }
    if (!n) ls_picker_empty_reason("No member positions or pings yet.");
}

/* ------------------------------------------------------------ notices */

static bool probe(ls_notice_t *out)
{
    char t[24], b[128];
    uint8_t hue;
    bool quiet;
    if (!party_notice_pop(t, sizeof(t), b, sizeof(b), &hue, &quiet)) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "%.19s", t);
    snprintf(out->body, sizeof(out->body), "%s", b);
    out->hue = hue;
    out->accent = hue ? hue : TUI_MAGENTA;
    out->screen = ls_tui_screen_index_of(&ls_scr_party);
    out->quiet = quiet;
    out->steady = true;
    return true;
}

/* ------------------------------------------------------------ the screen */

static bool party_go(void)
{
    const party_prof_t *p = party_prof(party_prof_cur());
    if (!p) { wiz_open(true); return false; }
    party_key_t k;
    if (!key_of(p->code, &k)) { say("PARTY CODE: AT LEAST 4 LETTERS OR DIGITS"); return false; }
    if (!party_start()) { say("COULD NOT START"); return false; }
    ls_notify_add_probe(probe);
    s_page = PG_RADAR;
    s_top = 0;
    char t[48];
    snprintf(t, sizeof(t), "JOINED %s", p->party);
    say(t);
    return true;
}

static void toggle(void)
{
    if (party_running()) { party_stop(); say("LEFT THE PARTY"); }
    else if (!party_prof_count()) wiz_open(true);
    else party_go();
}

static void draw(tui_surface *sf, tui_rect area)
{
    tui_rect a = area;
    s_nhit = 0;
    ls_btn_clear_hits();               /* a step without tiles leaves none behind */
    s_radar = tui_rect_make(0, 0, 0, 0);
    if (!party_init()) { ls_panel_notice(sf, a, "PARTY", "Out of memory", "Close an app and try again"); return; }
    if (party_running() && !party_io_worker_alive()) party_tick();     /* no worker: the simulator */
    party_io_lock();
    memcpy(&s_snap, party_state(), sizeof(s_snap));
    party_io_unlock();
    const uint32_t now = ms();
    if (a.w < 40 || a.h < 20) { ls_panel_notice(sf, a, NULL, "More screen space needed", "Use a smaller font"); return; }
    const bool wide = ls_tui_is_wide();
    const bool run = s_snap.running;
    const int bar_rows = wide ? 1 : 3;
    const int bar_h = wide ? 3 : 9;
    int npings = 0, nmem = 0;
    for (int i = 0; i < PARTY_PINGS; i++) npings += s_snap.ping[i].used;
    for (int i = 0; i < PARTY_MEMBERS; i++) nmem += s_snap.member[i].used;
    char nm[8], np[8];
    snprintf(nm, sizeof(nm), "%d", nmem);
    snprintf(np, sizeof(np), "%d", npings);
    ls_btn_t b[] = {
        { run ? "STOP" : "START", run ? "LEAVE" : party_prof_count() ? "JOIN" : "SETUP", 's', run, false, false },
        { "ROSTER", nm, '1', run && s_wiz < 0 && s_page == PG_ROSTER, !run, false },
        { "RADAR", s_range < 0 ? "AUTO" : "FIXED", '2', run && s_wiz < 0 && s_page == PG_RADAR, !run, false },
        { "PINGS", np, '3', run && s_wiz < 0 && s_page == PG_PINGS, !run, false },
        { "FILES", s_snap.tx.state == PARTY_X_SEND ? "SENDING" : "CARD", '4', run && s_wiz < 0 && s_page == PG_FILES, !run, false },
        { "MARK", s_snap.help ? "HELP ON" : "PING", 'm', s_snap.help, !run, false },
        { "FIND", s_snap.find_kind ? "ON" : "BUZZ", 'f', s_snap.find_kind != 0, !run, false },
        { "LOCK", "POCKET", 'l', false, false, false },
        party_prof_count() ? ls_opt_button(&CTX) : (ls_btn_t){ "NEW", "PROFILE", 'n', false, false, false },
    };
    static const uint8_t hues[9] = { TUI_RED, TUI_GREEN, TUI_CYAN, TUI_RED, TUI_YELLOW, TUI_MAGENTA, TUI_YELLOW, TUI_BLUE, 0 };
    ls_btn_bar_grid_slot(sf, tui_rect_make(a.x, a.y + a.h - bar_h, a.w, bar_h), b, 9, -1, LS_BTN_SLOT_SCREEN, bar_rows, hues);
    a.h -= bar_h + 1;
    if (s_say[0] && now - s_say_ms < 4000) {
        char t[80];
        snprintf(t, sizeof(t), " %s ", s_say);
        const int x = a.x + (a.w - (int)strlen(t)) / 2;
        put(sf, a, x > a.x ? x : a.x, a.y + a.h - 1, t, TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT));
        a.h--;
    } else s_say[0] = 0;
    if (s_wiz >= 0) { page_wiz(sf, a, wide); return; }
    if (wide) {
        /* landscape: the fire and the nearest things down the left, the page beside them */
        tui_rect side = tui_rect_make(a.x, a.y, 40, a.h);
        const int bh = draw_banner(sf, side, now);
        if (run) near_list(sf, tui_rect_make(side.x, side.y + bh, side.w, side.h - bh), now);
        a.x += 41; a.w -= 41;
    } else {
        const int bh = draw_banner(sf, a, now);
        a.y += bh; a.h -= bh;
    }
    if (!run) { page_lobby(sf, a); return; }
    switch (s_page) {
    case PG_ROSTER: page_roster(sf, a, now); break;
    case PG_PINGS: page_pings(sf, a, now); break;
    case PG_FILES: page_files(sf, a, now); break;
    default: page_radar(sf, a, now, wide); break;
    }
}

static void page_to(int p) { if (party_running()) { s_page = p; s_top = 0; s_wiz = -1; } else say("START A PARTY FIRST"); }

static void send_share(int i)
{
    char path[160], why[32];
    snprintf(path, sizeof(path), "%s/share/%s", party_io_dir(), s_share[i]);
    if (party_send_file(path, why, sizeof(why))) { char t[64]; snprintf(t, sizeof(t), "OFFERING %s", s_share[i]); say(t); }
    else say(why);
}

static bool wiz_key(ls_tk_t k, char ch)
{
    if (k == LS_TK_ESC || ch == 'b' || ch == 'B' || k == LS_TK_LEFT) {
        if (s_wiz > 0) s_wiz--; else { s_wiz = -1; party_prof_save(); }
        return true;
    }
    if (k == LS_TK_RIGHT || k == LS_TK_ENTER || ch == ' ') {
        if (s_wiz == W_READY) wiz_act(0);
        else s_wiz++;
        return true;
    }
    int what = -1;
    switch (s_wiz) {
    case W_HELLO: if (ch == 'n' || ch == 'N') what = 100; break;
    case W_CODE: what = ch == 'c' || ch == 'C' ? 0 : ch == 'n' || ch == 'N' ? 1 : -1; break;
    case W_YOU: what = ch == 'c' || ch == 'C' ? 0 : ch == 'g' || ch == 'G' ? 1 : ch == 'k' || ch == 'K' ? 2 : -1; break;
    case W_SHARE: what = ch >= '1' && ch <= '3' ? ch - '1' : ch == 'r' || ch == 'R' ? 3 : ch == 'v' || ch == 'V' ? 4 : -1; break;
    case W_WATCH: what = ch >= '1' && ch <= '6' ? ch - '1' : -1; break;
    case W_ALERT: what = ch >= '1' && ch <= '3' ? ch - '1' : ch == 't' || ch == 'T' ? 3 : -1; break;
    case W_READY: what = ch == 's' || ch == 'S' ? 0 : ch == 'v' || ch == 'V' ? 1 : -1; break;
    }
    if (what >= 0) { wiz_act(what); return true; }
    return true;            /* the setup keeps its keys */
}

static bool key(ls_tk_t k, char ch)
{
    if (s_wiz >= 0) return wiz_key(k, ch);
    if (party_prof_count() && ls_opt_key(&CTX, ch)) return true;
    if (ch == 's' || ch == 'S') { toggle(); return true; }
    if (ch == '1') { page_to(PG_ROSTER); return true; }
    if (ch == '2') { page_to(PG_RADAR); return true; }
    if (ch == '3') { page_to(PG_PINGS); return true; }
    if (ch == '4') { page_to(PG_FILES); return true; }
    if (ch == 'm' || ch == 'M') { open_mark(); return true; }
    if (ch == 'f' || ch == 'F') { open_find(); return true; }
    if (ch == 'n' || ch == 'N') { if (!party_running()) wiz_open(true); return true; }
    if (ch == 'l' || ch == 'L') { ls_tui_pocket_set(true); return true; }
    if (!party_running()) {
        const int n = party_prof_count();
        if (k == LS_TK_UP && n) { party_prof_select((party_prof_cur() + n - 1) % n); return true; }
        if (k == LS_TK_DOWN && n) { party_prof_select((party_prof_cur() + 1) % n); return true; }
        if (k == LS_TK_ENTER) { toggle(); return true; }
        return false;
    }
    if (s_page == PG_RADAR) {
        if (ch == '+' || ch == '=') { s_range = s_range < 0 ? 0 : s_range > 0 ? s_range - 1 : 0; return true; }
        if (ch == '-' || ch == '_') { s_range = s_range < 0 ? NRANGES - 1 : s_range < NRANGES - 1 ? s_range + 1 : NRANGES - 1; return true; }
        if (ch == 'a' || ch == 'A') { s_range = -1; return true; }
    }
    if (k == LS_TK_UP) { s_top--; return true; }
    if (k == LS_TK_DOWN) { s_top++; return true; }
    if (k == LS_TK_LEFT || k == LS_TK_RIGHT) { s_page = (s_page + (k == LS_TK_LEFT ? 3 : 1)) % 4; s_top = 0; return true; }
    if (k == LS_TK_ESC || ch == 'b' || ch == 'B') {
        if (s_page != PG_RADAR) { s_page = PG_RADAR; return true; }
        return false;
    }
    return false;
}

static bool touch(int x, int y)
{
    if (s_wiz >= 0) {
        const int q = ls_btn_hit_slot(x, y, LS_BTN_SLOT_QUICK);
        if (q >= 0) { wiz_act(q); return true; }
        for (int i = 0; i < s_nhit; i++)
            if (s_hit[i].kind == T_WIZ && tui_rect_contains(s_hit[i].r, x, y)) { wiz_act(s_hit[i].arg); return true; }
    }
    const int hit = ls_btn_hit(x, y);
    if (hit >= 0) {
        if (s_wiz >= 0 && hit != 0) s_wiz = -1;
        switch (hit) {
        case 0:
            if (s_wiz >= 0) { s_wiz = W_READY; wiz_act(0); }
            else toggle();
            return true;
        case 1: page_to(PG_ROSTER); return true;
        case 2: page_to(PG_RADAR); return true;
        case 3: page_to(PG_PINGS); return true;
        case 4: page_to(PG_FILES); return true;
        case 5: open_mark(); return true;
        case 6: open_find(); return true;
        case 7: ls_tui_pocket_set(true); return true;
        case 8: if (party_prof_count()) ls_opt_open(&CTX); else wiz_open(true); return true;
        }
    }
    if (s_wiz >= 0) {
        /* a tap on the step logs: back or on */
        if (y == 0 || x < 0) return true;
        return true;
    }
    for (int i = 0; i < s_nhit; i++) {
        const hit_t *h = &s_hit[i];
        if (!tui_rect_contains(h->r, x, y)) continue;
        switch (h->kind) {
        case T_PROF: party_prof_select(h->arg); return true;
        case T_LOBBY: wiz_open(true); return true;
        case T_MEMBER:
            if (s_snap.find_kind == 1 && s_snap.find_id == h->arg) { party_find_set(0, 0); say("FIND OFF"); }
            else { party_find_set(1, h->arg); say("FIND: BUZZES FASTER AS YOU GET CLOSE"); }
            return true;
        case T_PING:
            if (s_snap.find_kind == 2 && s_snap.find_id == h->arg) { party_find_set(0, 0); say("FIND OFF"); }
            else { party_find_set(2, h->arg); say("FIND: BUZZES FASTER AS YOU GET CLOSE"); }
            return true;
        case T_SHARE: send_share(h->arg); return true;
        }
    }
    /* a tap on the radar: the nearest thing to it is the one to find */
    if (s_radar.w && tui_rect_contains(s_radar, x, y) && s_snap.fix) {
        const float R = fminf((float)s_rw, s_rh * 2 / KY) - 1.5f;
        const float tx = (float)((x - s_radar.x) * 2 + 1 - s_rw), ty = (float)((y - s_radar.y) * 4 + 2 - s_rh * 2);
        int best_kind = 0, best_id = 0;
        float best = 6;
        for (int i = 0; i < PARTY_MEMBERS + PARTY_PINGS; i++) {
            const bool mem = i < PARTY_MEMBERS;
            double lat, lon;
            if (mem) { const party_member_t *m = &s_snap.member[i]; if (!m->used || !m->pos) continue; lat = m->lat; lon = m->lon; }
            else { const party_pingrec_t *p = &s_snap.ping[i - PARTY_MEMBERS]; if (!p->used) continue; lat = p->lat; lon = p->lon; }
            float d, b;
            int dx, dy;
            party_geo(s_snap.lat, s_snap.lon, lat, lon, &d, &b);
            if (d > s_radar_range) continue;
            project(d, b, R, s_radar_range, &dx, &dy);
            const float e = hypotf(dx - tx, dy - ty);
            if (e < best) { best = e; best_kind = mem ? 1 : 2; best_id = mem ? s_snap.member[i].id : i - PARTY_MEMBERS; }
        }
        if (best_kind) { party_find_set(best_kind, best_id); say("FIND: BUZZES FASTER AS YOU GET CLOSE"); }
        return true;
    }
    return false;
}

static uint32_t pocket_ms(void)
{
    const party_prof_t *p = party_prof(party_prof_cur());
    return p ? POCKET_MS[p->pocket & 3] : 0;
}

static void enter(void)
{
    if (!party_init()) return;
    s_share_ms = 0;
    ls_tui_pocket_auto(pocket_ms());
    if (party_running()) ls_notify_add_probe(probe);
}

static void leave(void)
{
    ls_tui_pocket_auto(0);              /* the auto lock is PARTY's; a lock already on stays until unlocked */
    party_prof_save();
}

const ls_tui_screen_t ls_scr_party = {
    .name = "PARTY",
    .hint = "S join/leave  1 roster  2 radar  3 pings  4 files  M mark  F find  L lock  O options",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch,
};
