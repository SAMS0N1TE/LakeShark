/* GPS: where you are, and when it cannot tell you, why. */

#include "../../ls_tui_screen.h"
#include "../../ls_text.h"

#include <stdio.h>
#include <string.h>

#include "../../ls_map.h"
#include "../../ls_quick.h"
#include "../../ls_tui_ui.h"

#include "ls_gps.h"
#include "ls_track.h"
#include "ls_track_log.h"
#include "ls_mesh.h"
#include "../../ls_skyplot.h"

#define A(fg, bg) TUI_ATTR((fg), (bg))

#define DIM_FG LS_DIM_FG

static tui_rect s_quick_rect;
static int      s_pulse;
static uint32_t s_seen_sentences;

static const ls_quick_t QUICK[] = {
    { .label = "GPS", .kind = LS_QUICK_TOGGLE, .action = "gps.on",
      .value = "gps.on", .key = 'g' },
    /* TRACK belongs here and not on the console, because the console
       is a PC on a cable and the person who wants to start recording is
       about to walk out of the door with the board in their hand. It starts
       the receiver too, so one tap is the whole gesture. */
    { .label = "TRACK", .kind = LS_QUICK_TOGGLE, .action = "track.rec",
      .value = "track.on", .key = 'r' },
    { .label = "TO MAP", .kind = LS_QUICK_ACTION, .action = "map.here",
      .key = 'm' },
};
#define N_QUICK ((int)(sizeof(QUICK) / sizeof(QUICK[0])))

/* ---------------------------------------------------------------- parts -- */

static const char *state_word(const ls_gps_state_t *g)
{
    if (!g->alive) return "SILENT";
    if (!g->fix)   return "SEARCHING";
    return "FIX";
}

static uint8_t state_hue(const ls_gps_state_t *g)
{
    if (!g->alive) return TUI_RED;
    if (!g->fix)   return TUI_YELLOW;
    return TUI_GREEN;
}

static const char *diagnosis(const ls_gps_state_t *g)
{
    if (g->fix) return NULL;
    if (!g->bytes)
        return "nothing on the wire - check power and the two pins";
    if (!g->sentences)
        return "bytes but no sentences - the baud rate is wrong";
    if (g->checksum_errors > g->sentences / 4)
        return "sentences arriving corrupt - a marginal wire or rate";
    return "sentences are good, no position yet - antenna or sky";
}

static void field(tui_surface *sf, tui_rect a, int row, const char *label,
                  const char *value, uint8_t vattr)
{
    tui_put_str(sf, a, a.x + 2, a.y + row, label, A(DIM_FG, TUI_BLACK));
    tui_put_str(sf, a, a.x + 13, a.y + row, value, vattr);
}

/* Satellites as a run of blocks: used solid, visible-but-unused hollow.

   A count says four; a row of four filled blocks beside eleven outlines says
   four of eleven at a glance, which is the shape of the question - a
   receiver seeing plenty and using few is a different problem from one
   seeing none. */
static void sats_bar(tui_surface *sf, tui_rect a, int row,
                     const ls_gps_state_t *g)
{
    const int room = a.w - 15;
    int visible = g->sats_visible;
    int used = g->sats_used;
    if (visible > room) visible = room;
    if (used > visible) used = visible;

    for (int i = 0; i < visible; i++) {
        const bool on = i < used;
        tui_put_char(sf, a, a.x + 13 + i, a.y + row,
                     on ? LS_TUI_SHADE_FULL : LS_TUI_SHADE_25,
                     A(on ? (TUI_GREEN | TUI_BRIGHT) : DIM_FG, TUI_BLACK));
    }
    if (!visible)
        tui_put_str(sf, a, a.x + 13, a.y + row, "none", A(DIM_FG, TUI_BLACK));
}

/* ---------------------------------------------------------------- panels -- */

static void draw_position(tui_surface *sf, tui_rect pos,
                          const ls_gps_state_t *g)
{
    const uint8_t val = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    char buf[48];

    ls_panel_box(sf, pos, "POSITION", TUI_CYAN);

    if (!g->fix) {
        field(sf, pos, 1, "LATITUDE", "--", dim);
        field(sf, pos, 2, "LONGITUDE", "--", dim);
        /* The note is wrapped, because it did not fit. */

        const char *why = diagnosis(g);
        if (why && pos.h > 5) {
            char lines[3][64];
            const int rows = (pos.y + pos.h - 1) - (pos.y + 4);
            const int n = ls_wrap_text(why, pos.w - 4, (char *)lines,
                                       sizeof(lines[0]),
                                       rows < 3 ? rows : 3);
            for (int i = 0; i < n; i++)
                tui_put_str(sf, pos, pos.x + 2, pos.y + 4 + i, lines[i], dim);
        }
        return;
    }

    snprintf(buf, sizeof(buf), "%.5f", g->lat_deg);
    field(sf, pos, 1, "LATITUDE", buf, val);
    snprintf(buf, sizeof(buf), "%.5f", g->lon_deg);
    field(sf, pos, 2, "LONGITUDE", buf, val);
    snprintf(buf, sizeof(buf), "%.0f m", (double)g->alt_m);
    field(sf, pos, 3, "ALTITUDE", buf, val);
    snprintf(buf, sizeof(buf), "%.1f kt  %.0f deg",
             (double)g->speed_kts, (double)g->course_deg);
    field(sf, pos, 4, "SPEED", buf, val);
    snprintf(buf, sizeof(buf), "%.1f", (double)g->hdop);
    field(sf, pos, 5, "HDOP", buf,
          g->hdop > 5.0f ? A(TUI_YELLOW, TUI_BLACK) : val);
}

static void draw_sky(tui_surface *sf, tui_rect sky, const ls_gps_state_t *g)
{
    const uint8_t val = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    char buf[48];

    ls_panel_box(sf, sky, "SKY", TUI_CYAN);
    snprintf(buf, sizeof(buf), "%u of %u", g->sats_used, g->sats_visible);
    field(sf, sky, 1, "SATELLITES", buf, val);
    sats_bar(sf, sky, 2, g);
    if (g->year) {
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02uZ",
                 g->year, g->month, g->day, g->hour, g->minute, g->second);
        field(sf, sky, 3, "UTC", buf, val);
    } else {
        field(sf, sky, 3, "UTC", "--", dim);
    }
}

/* The sky, as a picture of the sky. */

static void draw_skyview(tui_surface *sf, tui_rect r, const ls_gps_state_t *g)
{
    ls_panel_box(sf, r, "SKY VIEW", TUI_CYAN);

    const tui_rect in = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    if (in.h < 5 || in.w < 9) return;

    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);

    const uint8_t faint = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const int cy = in.h / 2, cx = in.w / 2;

    /* The horizon, and the two axes. Drawn from the same projection the
       satellites use, so the ring cannot disagree with what is on it - a
       circle drawn by its own arithmetic is how a plot ends up with dots
       outside its own rim. */
    for (int az = 0; az < 360; az += 4) {
        int rr, cc;
        if (ls_sky_project(az, 0, in.h, in.w, cw, ch, &rr, &cc))
            tui_put_char(sf, in, in.x + cc, in.y + rr, '.', faint);
    }

    for (int az = 0; az < 360; az += 12) {
        int rr, cc;
        if (ls_sky_project(az, 45, in.h, in.w, cw, ch, &rr, &cc))
            tui_put_char(sf, in, in.x + cc, in.y + rr, '.', faint);
    }
    tui_put_char(sf, in, in.x + cx, in.y + cy, '+', faint);

    /* N/E/S/W on the rim, which is what stops somebody having to remember
       which way up it is. */
    {
        int rr, cc;
        static const struct { int az; char c; } MARK[] = {
            { 0, 'N' }, { 90, 'E' }, { 180, 'S' }, { 270, 'W' },
        };
        for (int i = 0; i < 4; i++)
            if (ls_sky_project(MARK[i].az, 0, in.h, in.w, cw, ch, &rr, &cc))
                tui_put_char(sf, in, in.x + cc, in.y + rr, MARK[i].c,
                             A(DIM_FG, TUI_BLACK));
    }

    int placed = 0, unlocated = 0;
    for (int i = 0; i < g->sat_count && i < LS_GPS_MAX_SATS; i++) {
        const ls_gps_sat_t *sv = &g->sats[i];
        /* An empty slot is all zeros, which projects to due north on the
           horizon - exactly where the N marker is. Skipped by prn, which is
           the field ls_gps.h says is zero for an empty slot. */
        if (!sv->prn) continue;
        /* And one the receiver has not located yet, which before a
           fix is most of them. See ls_sky_located - an unlocated satellite
           arrives as azimuth 0 elevation 0 and would sit on the N marker. */
        if (!ls_sky_located(sv->azimuth, sv->elevation)) { unlocated++; continue; }

        int rr, cc;
        if (!ls_sky_project(sv->azimuth, sv->elevation, in.h, in.w,
                            cw, ch, &rr, &cc))
            continue;

        const uint8_t at = sv->used ? A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
                                    : A(TUI_CYAN, TUI_BLACK);
        tui_put_char(sf, in, in.x + cc, in.y + rr,
                     ls_sky_glyph(sv->snr), at);
        placed++;
    }

    /* Say how many are NOT on the plot, because otherwise a receiver
       that can see nine and has placed two draws two dots and looks like a
       receiver that can see two. The difference between those is whether to
       wait or to go outside. */
    if (unlocated) {
        char note[32];
        snprintf(note, sizeof(note), "%d not located yet", unlocated);
        tui_put_str(sf, r, r.x + 2, r.y + r.h - 1, note,
                    A(DIM_FG, TUI_BLACK));
    } else if (!placed) {
        tui_put_str(sf, r, r.x + 2, r.y + r.h - 1, "no satellites placed",
                    A(DIM_FG, TUI_BLACK));
    }
}

static void draw_track(tui_surface *sf, tui_rect r)
{
    const uint8_t val = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    char buf[48];

    const bool on = ls_track_rec_running();
    ls_panel_box(sf, r, "TRACK", on ? TUI_GREEN : TUI_CYAN);

    snprintf(buf, sizeof(buf), "%s", on ? "RECORDING" : "stopped");
    field(sf, r, 1, "STATE", buf,
          on ? A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK) : dim);

    const int pts = ls_track_points();
    snprintf(buf, sizeof(buf), "%d of %d", pts, LS_TRACK_CAPACITY);
    field(sf, r, 2, "POINTS", buf, pts ? val : dim);

    const int nodes = ls_mesh_sightings();
    snprintf(buf, sizeof(buf), "%d", nodes);
    field(sf, r, 3, "NODES MET", buf, nodes ? val : dim);
}

/* The counters, which are what separate a wiring fault from a sky problem and
   are worth having on screen even with a fix. */
static void draw_wire(tui_surface *sf, tui_rect wire, const ls_gps_state_t *g)
{
    const uint8_t val = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    char buf[48];

    ls_panel_box(sf, wire, "WIRE", TUI_CYAN);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g->bytes);
    field(sf, wire, 1, "BYTES", buf, g->bytes ? val : A(TUI_RED, TUI_BLACK));
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g->sentences);
    field(sf, wire, 2, "SENTENCES", buf,
          g->sentences ? val : A(TUI_RED, TUI_BLACK));
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g->checksum_errors);
    field(sf, wire, 3, "BAD CKSUM", buf,
          g->checksum_errors ? A(TUI_YELLOW, TUI_BLACK) : dim);
}

/* ----------------------------------------------------------------- draw -- */

static void draw(tui_surface *sf, tui_rect area)
{
    ls_gps_state_t g;
    ls_gps_get(&g);

    /* One pulse per sentence that arrived since the last frame, so the
       indicator moves at the rate the receiver is actually talking rather
       than at the frame rate. A blinker that runs with nothing on the wire
       is worse than no blinker: it says the thing is alive. */
    if (g.sentences != s_seen_sentences) {
        s_seen_sentences = g.sentences;
        s_pulse++;
    }

    const int want = ls_quick_rows(QUICK, N_QUICK, area.w, ls_tui_is_wide());
    const int ctl_h = (area.h > want + 12) ? want : 0;
    tui_rect body = tui_rect_make(area.x, area.y, area.w, area.h - ctl_h);
    s_quick_rect = ctl_h
        ? tui_rect_make(area.x, area.y + body.h, area.w, ctl_h)
        : tui_rect_make(0, -1, 0, 0);

    /* State, as the biggest thing on the screen. */
    {
        const char *w = state_word(&g);
        tui_rect band = tui_rect_make(body.x, body.y, body.w, 3);
        tui_fill(sf, band, LS_TUI_SHADE_25, A(state_hue(&g), TUI_BLACK));
        tui_put_str(sf, body, body.x + (body.w - (int)strlen(w)) / 2,
                    body.y + 1, w, A(state_hue(&g) | TUI_BRIGHT, TUI_BLACK));
        if (g.alive)
            tui_put_char(sf, body, body.x + body.w - 3, body.y + 1,
                         (char)(LS_TUI_SHADE_25 + (s_pulse & 3)),
                         A(state_hue(&g) | TUI_BRIGHT, TUI_BLACK));
    }

    tui_rect rest = tui_rect_make(body.x, body.y + 4, body.w, body.h - 4);
    if (rest.h < 4) {
        if (ctl_h) ls_quick_draw_posture(sf, s_quick_rect, ls_tui_is_wide(), QUICK, N_QUICK);
        return;
    }

    /* Portrait stacks the three panels; landscape puts SKY and WIRE beside
       the position.

       A 27 row grid minus the chrome is twenty rows and the three stacked
       want twenty-six, so stacking them anyway dropped SKY and WIRE off the
       bottom - and WIRE is the counters, which is to say the whole basis of
       the diagnosis line in POSITION. A screen that keeps the conclusion and
       discards the evidence is worse than one that shows neither. */
    if (ls_tui_is_wide()) {
        tui_rect left, right;
        ls_tui_split(rest, &left, &right);
        draw_position(sf, tui_rect_make(left.x, left.y, left.w - 1, 9), &g);
        draw_sky(sf, tui_rect_make(right.x, right.y, right.w, 6), &g);
        if (right.h > 8)
            draw_wire(sf, tui_rect_make(right.x, right.y + 7, right.w, 5), &g);
    } else {

        int y = rest.y;
        draw_position(sf, tui_rect_make(rest.x, y, rest.w, 9), &g);
        y += 10;
        if (y + 6 <= rest.y + rest.h) {
            draw_sky(sf, tui_rect_make(rest.x, y, rest.w, 6), &g);
            y += 7;
        }
        /* The plot wants to be as round and as big as the room allows. Its
           height drives its width through the cell aspect, so it is sized in
           rows and the box follows. */
        {
            int h = rest.y + rest.h - y - 7;      /* leave room for TRACK */
            if (h > 17) h = 17;
            if (h >= 9) {
                draw_skyview(sf, tui_rect_make(rest.x, y, rest.w, h), &g);
                y += h + 1;
            }
        }
        if (y + 6 <= rest.y + rest.h) {
            draw_track(sf, tui_rect_make(rest.x, y, rest.w, 6));
            y += 7;
        }
        if (y + 5 <= rest.y + rest.h)
            draw_wire(sf, tui_rect_make(rest.x, y, rest.w, 5), &g);
    }

    if (ctl_h) ls_quick_draw_posture(sf, s_quick_rect, ls_tui_is_wide(), QUICK, N_QUICK);
}

/* ---------------------------------------------------------------- input -- */

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_CHAR)
        return ls_quick_key(ch, QUICK, N_QUICK, ls_quick_grant_builtin(), NULL);
    return false;
}

static bool touch(int col, int row)
{
    if (s_quick_rect.h > 0 && row >= s_quick_rect.y &&
        row < s_quick_rect.y + s_quick_rect.h)
        return ls_quick_touch(col, row, QUICK, N_QUICK,
                              ls_quick_grant_builtin(), NULL);
    return false;
}

const ls_tui_screen_t ls_scr_gps = {
    .name = "GPS",
    .hint = "G receiver  M to map",
    .enter = NULL,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
