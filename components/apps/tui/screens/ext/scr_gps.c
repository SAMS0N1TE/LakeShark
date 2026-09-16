/* GPS: where you are, and when it cannot tell you, why. */

#include "../../ls_tui_screen.h"
#include "core/ls_time.h"
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
#include "../../ls_skyview.h"
static int s_sky_selected, s_sky_count;
static bool s_sky_mode=true;
static tui_rect s_view_hit;

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
    tui_put_str(sf,a,a.x+2,a.y+row,"#used .seen",A(DIM_FG,TUI_BLACK));
    const int room = a.w - 15;
    int visible = g->sats_visible;
    int used = g->sats_used;
    if (visible > room) visible = room;
    if (used > visible) used = visible;

    for (int i = 0; i < visible; i++) {
        const bool on = i < used;
        tui_put_char(sf, a, a.x + 13 + i, a.y + row,
                     on ? '#' : '.',
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

static void draw_skyview(tui_surface *sf,tui_rect r,const ls_gps_state_t *g)
{
    s_sky_count=g->sat_count<LS_GPS_MAX_SATS?g->sat_count:LS_GPS_MAX_SATS;
    ls_skyview_draw(sf,r,g,s_sky_selected);
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
    const bool framed=ls_tui_is_wide() && body.w>=85 && body.h>=22;
    tui_rect summary=body;
    if(framed) {
        s_view_hit=tui_rect_make(body.x,body.y,body.w/2-1,4);
        ls_panel_box(sf,s_view_hit,"GPS VIEW",TUI_CYAN);
        summary=tui_rect_make(body.x+body.w/2,body.y,body.w-body.w/2,4);
        ls_panel_box(sf,summary,"TRACK LOG",TUI_CYAN);
    } else s_view_hit=tui_rect_make(body.x,body.y,body.w,2);
    tui_put_str(sf,framed?s_view_hit:body,body.x+2,body.y+(framed?1:0),ls_tui_is_wide()?(s_sky_mode?"[V] SKY / switch to receiver details":"[V] DETAILS / switch to satellite sky"):(s_sky_mode?"[ SKY ]  Tap for receiver details":"[ DETAILS ]  Tap for satellite sky"),A(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    char health[80];snprintf(health,sizeof(health),"TRACK %s / %d points",ls_track_rec_error()!=ESP_OK?"FAILED":ls_track_rec_running()?(g.fix?"RECORDING":"WAIT FIX"):"STOPPED",ls_track_points());
    tui_put_str(sf,summary,summary.x+2,summary.y+1,health,A(ls_track_rec_running()?TUI_GREEN:TUI_YELLOW,TUI_BLACK));
    uint32_t seconds=0;bool epoch=false;
    char stamp[LS_TIME_STAMP_MAX];
    if(ls_track_last_time(&seconds,&epoch)) {
        if(epoch) {
            /* Stored track epochs also cover historical dates before 2024. */
            const time_t t=(time_t)seconds;
            struct tm utc;
#ifdef _WIN32
            gmtime_s(&utc,&t);
#else
            gmtime_r(&t,&utc);
#endif
            strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%SZ",&utc);
        } else ls_time_render_stamp_at(stamp,sizeof(stamp),0,(int64_t)seconds*1000000);
        snprintf(health,sizeof(health),"LAST POINT %s",stamp);
    } else snprintf(health,sizeof(health),"LAST POINT -- / none saved");
    tui_put_str(sf,summary,summary.x+2,summary.y+2,health,LS_ATTR_DIM);
    if(framed) {
        char state[60];snprintf(state,sizeof(state),"%s / %u used of %u seen",state_word(&g),g.sats_used,g.sats_visible);
        tui_put_str(sf,s_view_hit,s_view_hit.x+2,s_view_hit.y+2,state,A(state_hue(&g),TUI_BLACK));
    }
    const int header_h=framed?4:3;
    body.y+=header_h;body.h-=header_h;
    if(s_sky_mode) {
        draw_skyview(sf,body,&g);
        if(ctl_h)ls_quick_draw_posture(sf,s_quick_rect,ls_tui_is_wide(),QUICK,N_QUICK);
        return;
    }

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
        if (left.h >= 16)
            draw_track(sf, tui_rect_make(left.x, left.y + 10, left.w - 1, left.h - 10));
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
    if(k==LS_TK_CHAR && (ch=='v'||ch=='V')) {s_sky_mode=!s_sky_mode;return true;}
    if(k==LS_TK_CHAR && (ch=='j'||ch=='J'||ch=='k'||ch=='K')) {
        if(s_sky_count) s_sky_selected=(s_sky_selected+(ch=='j'||ch=='J'?1:s_sky_count-1))%s_sky_count;
        return true;
    }
    if (k == LS_TK_CHAR)
        return ls_quick_key(ch, QUICK, N_QUICK, ls_quick_grant_builtin(), NULL);
    return false;
}

static bool touch(int col, int row)
{
    if(tui_rect_contains(s_view_hit,col,row)) {s_sky_mode=!s_sky_mode;return true;}
    if (s_quick_rect.h > 0 && row >= s_quick_rect.y &&
        row < s_quick_rect.y + s_quick_rect.h)
        return ls_quick_touch(col, row, QUICK, N_QUICK,
                              ls_quick_grant_builtin(), NULL);
    return false;
}

const ls_tui_screen_t ls_scr_gps = {
    .name = "GPS",
    .hint = "V sky/details  G receiver  M map  J/K satellite",
    .enter = NULL,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
