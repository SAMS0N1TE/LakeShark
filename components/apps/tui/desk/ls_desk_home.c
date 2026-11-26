/* The Home Base window: this desk's Nano as the home database the field
   units sync to over LSX (../ls_lsx_svc.h). A house with its mast, the
   waves off the mast stepping with each frame sent, the car of the unit in
   session, packets running between them as the bytes cross, the RSSI of
   every frame heard in braille, and what each unit has synced. Nothing
   moves unless frames do. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <string.h>

#include "../ls_braille.h"
#include "../ls_lsx_svc.h"
#include "../ls_plate.h"

static bool s_focus_close;
static char s_note[64];

static uint8_t hue(uint8_t light, uint8_t dark) { return dk_fit(dk_light_face() ? light : dark, dk_face_bg(), 45); }
static uint8_t dim(void) { return dk_attr(R_SHADOW); }

static tui_rect run_rect(tui_rect c) { return tui_rect_make(c.x + c.w - 37, c.y + c.h - 2, 22, 1); }
static tui_rect ok_rect(tui_rect c) { return tui_rect_make(c.x + c.w - 14, c.y + c.h - 2, 12, 1); }

static uint8_t mode_hue(int mode)
{
    const int k = ls_lsx_mode_info(mode)->kind;
    return k == LSX_K_FLRC ? hue(TUI_MAGENTA, TUI_MAGENTA | TUI_BRIGHT)
           : k == LSX_K_GFSK ? hue(TUI_BLUE, TUI_CYAN | TUI_BRIGHT)
                             : hue(TUI_GREEN, TUI_GREEN | TUI_BRIGHT);
}

/* ---- the picture ---------------------------------------------------------- */

static void put(tui_surface *sf, tui_rect clip, int x, int y, int cp, uint8_t at)
{
    if (cp < 128) {
        char s[2] = { (char)cp, 0 };
        dk_text(sf, clip, x, y, s, at);
    } else {
        ls_box_put(sf, clip, x, y, cp, at);
    }
}

static void house(tui_surface *sf, tui_rect clip, int x, int y, const ls_lsx_status_t *l, bool on)
{
    const uint8_t wall = hue(TUI_BLACK, TUI_WHITE | TUI_BRIGHT), roof = hue(TUI_RED, TUI_RED | TUI_BRIGHT);
    const uint8_t mast = hue(TUI_BLUE, TUI_CYAN), ground = hue(TUI_GREEN, TUI_GREEN);
    const bool session = l->state == LSX_ST_SESSION;
    /* The mast and its cross bar, the lead down to the roof. */
    put(sf, clip, x + 13, y, LS_BOX_DH, mast);
    put(sf, clip, x + 11, y + 1, LS_BOX_HALF_R, mast);
    put(sf, clip, x + 12, y + 1, LS_BOX_H, mast);
    put(sf, clip, x + 13, y + 1, LS_BOX_VH, mast);
    put(sf, clip, x + 14, y + 1, LS_BOX_H, mast);
    put(sf, clip, x + 15, y + 1, LS_BOX_HALF_L, mast);
    for (int r = 2; r <= 5; r++) put(sf, clip, x + 13, y + r, LS_BOX_V, mast);
    put(sf, clip, x + 13, y + 6, LS_BOX_UL, mast);
    put(sf, clip, x + 11, y + 6, LS_BOX_H, mast);
    put(sf, clip, x + 12, y + 6, LS_BOX_H, mast);
    /* The roof. */
    for (int r = 0; r < 4; r++) {
        put(sf, clip, x + 6 - r, y + 2 + r, LS_BOX_DIAG_R, roof);
        put(sf, clip, x + 7 + r, y + 2 + r, LS_BOX_DIAG_L, roof);
    }
    for (int i = 4; i <= 9; i++) put(sf, clip, x + i, y + 5, LS_BOX_H, roof);
    /* Walls, windows lit while a unit is syncing, the door. */
    const uint8_t lit = session ? hue(TUI_YELLOW, TUI_YELLOW | TUI_BRIGHT) : on ? hue(TUI_BLUE, TUI_BLUE | TUI_BRIGHT) : dim();
    put(sf, clip, x + 6, y + 4, LS_SHAPE_SQUARE, lit);
    put(sf, clip, x + 7, y + 4, LS_SHAPE_SQUARE, lit);
    for (int r = 6; r <= 7; r++) {
        put(sf, clip, x + 3, y + r, LS_BOX_V, wall);
        put(sf, clip, x + 10, y + r, LS_BOX_V, wall);
    }
    put(sf, clip, x + 5, y + 6, LS_SHAPE_SQUARE, lit);
    put(sf, clip, x + 8, y + 6, LS_SHAPE_SQUARE, lit);
    put(sf, clip, x + 6, y + 7, LS_BOX_DR, wall);
    put(sf, clip, x + 7, y + 7, LS_BOX_DL, wall);
    for (int i = 0; i < 40; i++) put(sf, clip, x + i, y + 8, LS_BOX_H, ground);
    put(sf, clip, x + 3, y + 8, LS_BOX_UH, wall);
    put(sf, clip, x + 10, y + 8, LS_BOX_UH, wall);

    /* Waves off the mast: one arc further out for each frame sent, so they
       march only while this end transmits. Grey when the radio is not ours. */
    const unsigned step = l->frames_tx % 4;
    const uint8_t wave_on = hue(TUI_MAGENTA, TUI_YELLOW | TUI_BRIGHT), wave_off = l->held ? mast : dim();
    for (int k = 0; k < 3; k++) {
        const uint8_t a = (l->held && step == (unsigned)k + 1) ? wave_on : wave_off;
        const int wx = x + 16 + 2 * k;
        if (!k) put(sf, clip, wx, y + 1, ')', a);
        else {                      /* a tall ')' */
            put(sf, clip, wx - 1, y, LS_BOX_DIAG_L, a);
            put(sf, clip, wx, y + 1, LS_BOX_V, a);
            put(sf, clip, wx - 1, y + 2, LS_BOX_DIAG_R, a);
        }
    }
}

/* The unit in session as a car, and the lane between: a packet for every
   chunk that has crossed, moving toward whichever end is receiving. */
static void car_and_lane(tui_surface *sf, tui_rect clip, int x, int y, const ls_lsx_status_t *l)
{
    if (l->state != LSX_ST_SESSION) return;
    const uint8_t body = mode_hue(l->mode), wheel = hue(TUI_BLACK, TUI_WHITE | TUI_BRIGHT);
    const int cx = x + 30;
    put(sf, clip, cx + 1, y + 5, LS_BOX_ARC_DR, body);
    put(sf, clip, cx + 2, y + 5, LS_BOX_H, body);
    put(sf, clip, cx + 3, y + 5, LS_BOX_H, body);
    put(sf, clip, cx + 4, y + 5, LS_BOX_ARC_DL, body);
    put(sf, clip, cx, y + 6, LS_BOX_ARC_DR, body);
    put(sf, clip, cx + 1, y + 6, LS_BOX_ARC_UL, body);
    put(sf, clip, cx + 4, y + 6, LS_BOX_ARC_UR, body);
    put(sf, clip, cx + 5, y + 6, LS_BOX_H, body);
    put(sf, clip, cx + 6, y + 6, LS_BOX_ARC_DL, body);
    put(sf, clip, cx, y + 7, LS_BOX_ARC_UR, body);
    put(sf, clip, cx + 1, y + 7, LS_SHAPE_DOT, wheel);
    put(sf, clip, cx + 2, y + 7, LS_BOX_H, body);
    put(sf, clip, cx + 3, y + 7, LS_BOX_H, body);
    put(sf, clip, cx + 4, y + 7, LS_SHAPE_DOT, wheel);
    put(sf, clip, cx + 5, y + 7, LS_BOX_H, body);
    put(sf, clip, cx + 6, y + 7, LS_BOX_ARC_UL, body);
    char nm[LSX_NAME];
    snprintf(nm, sizeof(nm), "%.9s", l->peer_name);
    dk_text(sf, clip, cx, y + 3, nm, dk_attr(R_STRONG));
    /* The lane, x+15 .. x+28 on the window row: dots, and packets placed by
       how many chunks have crossed each way. */
    const int a = x + 15, n = 14;
    for (int i = 0; i < n; i++) put(sf, clip, a + i, y + 6, '.', dim());
    const unsigned in = l->bytes_rx / 228, out = l->bytes_tx / 228;
    const uint8_t pk_in = hue(TUI_GREEN, TUI_GREEN | TUI_BRIGHT), pk_out = hue(TUI_BLUE, TUI_CYAN | TUI_BRIGHT);
    for (int k = 0; k < 3; k++) {
        if (in > (unsigned)k) put(sf, clip, a + n - 1 - (int)((in + 5u * k) % n), y + 6, LS_SHAPE_LEFT, pk_in);
        if (out > (unsigned)k) put(sf, clip, a + (int)((out + 5u * k) % n), y + 6, LS_SHAPE_RIGHT, pk_out);
    }
}

/* ---- the plates ------------------------------------------------------------ */

/* Text on a plate's slate ground (../ls_plate.h): the same on every face. */
#define P(fg) ls_plate_at((uint8_t)(fg))
#define P_VAL P(TUI_WHITE | TUI_BRIGHT)
#define P_LBL P(TUI_WHITE)
#define P_DIM P(TUI_BLUE | TUI_BRIGHT)

static uint8_t p_rssi(int dbm)
{
    return P(dbm >= -75 ? TUI_GREEN | TUI_BRIGHT : dbm >= -95 ? TUI_YELLOW | TUI_BRIGHT : TUI_RED | TUI_BRIGHT);
}

static uint8_t p_mode(int mode)
{
    const int k = ls_lsx_mode_info(mode)->kind;
    return P(k == LSX_K_FLRC ? TUI_MAGENTA | TUI_BRIGHT : k == LSX_K_GFSK ? TUI_CYAN | TUI_BRIGHT : TUI_GREEN | TUI_BRIGHT);
}

/* A plate kept inside the window's client area: one that would not fit is
   not drawn, so a small window never paints past its own edge. */
static tui_rect plate(tui_surface *sf, tui_rect clip, tui_rect c, tui_rect r, uint8_t h, const char *title,
                      const char *chip, uint8_t chip_hue)
{
    r = tui_rect_intersect(r, c);
    if (r.w < 12 || r.h < 3) return tui_rect_make(r.x, r.y, 0, 0);
    return ls_plate(sf, tui_rect_intersect(clip, r), r, h, title, chip, chip_hue);
}

/* The last 32 frames' RSSI, -130 .. -30 dBm over `rows` braille rows, on a plate. */
static void rssi_chart(tui_surface *sf, tui_rect clip, int x, int y, int rows, const ls_lsx_status_t *l)
{
    const int dots = rows * 4;
    for (int col = 0; col < 16; col++) {
        int lv[2] = { 0, 0 }, dbm = -130;
        for (int side = 0; side < 2; side++) {
            const int h = l->rssi_hist[col * 2 + side];
            if (!h) continue;
            dbm = h - 160;
            lv[side] = (dbm + 130) * dots / 100;
            if (lv[side] < 1) lv[side] = 1;
            if (lv[side] > dots) lv[side] = dots;
        }
        for (int r = 0; r < rows; r++) {
            const int base = (rows - 1 - r) * 4;
            unsigned bits = 0x2800;
            for (int side = 0; side < 2; side++) {
                const int fill = lv[side] - base;
                if (fill > 0) bits |= ls_braille_column((unsigned)(fill > 4 ? 4 : fill), side);
            }
            if (bits != 0x2800) ls_braille_put(sf, clip, x + col, y + r, (int16_t)bits, p_rssi(dbm));
            else if (r == rows - 1) ls_braille_put(sf, clip, x + col, y + r, (int16_t)0x28C0, P_DIM);   /* the floor */
        }
    }
}

static void group(char *out, size_t n, uint32_t v)
{
    if (v >= 1000000) snprintf(out, n, "%lu,%03lu,%03lu", (unsigned long)(v / 1000000), (unsigned long)(v / 1000 % 1000),
                               (unsigned long)(v % 1000));
    else if (v >= 1000) snprintf(out, n, "%lu,%03lu", (unsigned long)(v / 1000), (unsigned long)(v % 1000));
    else snprintf(out, n, "%lu", (unsigned long)v);
}

static void ago(char *out, size_t n, uint32_t last_s, uint32_t now_s)
{
    if (!last_s) { snprintf(out, n, "never"); return; }
    if (last_s == 1 && now_s > 600) { snprintf(out, n, "before boot"); return; }
    const uint32_t d = now_s > last_s ? now_s - last_s : 0;
    if (d < 90) snprintf(out, n, "%lu s ago", (unsigned long)d);
    else if (d < 5400) snprintf(out, n, "%lu min ago", (unsigned long)(d / 60));
    else snprintf(out, n, "%lu h ago", (unsigned long)(d / 3600));
}

/* LINK: where the home base listens, who is in session, every frame heard. */
static void link_plate(tui_surface *sf, tui_rect clip, tui_rect c, tui_rect r, const ls_lsx_status_t *l, bool on)
{
    char b[64];
    const bool sess = l->state == LSX_ST_SESSION;
    const char *word = !on ? "OFF" : sess ? "SESSION" : l->state == LSX_ST_WAIT_RADIO ? "WAITING FOR RADIO" : ls_lsx_state_name(l->state);
    const uint8_t wh = !on ? TUI_WHITE : sess ? TUI_GREEN : l->held ? TUI_CYAN : TUI_YELLOW;
    const tui_rect in = plate(sf, clip, c, r, TUI_CYAN, "LINK", word, wh);
    if (in.w <= 0) return;
    const tui_rect k = tui_rect_intersect(clip, in);
    int y = in.y;
    if (!on) {
        dk_text(sf, k, in.x, y++, "Field units in LoRa range send", P_LBL);
        dk_text(sf, k, in.x, y++, "what they found; this desk", P_LBL);
        dk_text(sf, k, in.x, y++, "keeps it and shares it back.", P_LBL);
    } else {
        snprintf(b, sizeof(b), "%.3f", l->hz / 1e6);
        const int n = dk_text(sf, k, in.x, y, b, P_VAL);
        dk_text(sf, k, in.x + n + 1, y, "MHz", P_LBL);
        y += 2;
        if (sess) {
            dk_text(sf, k, in.x, y, "unit", P_LBL);
            snprintf(b, sizeof(b), "%.12s", l->peer_name);
            const int m = dk_text(sf, k, in.x + 5, y, b, P_VAL);
            dk_text(sf, k, in.x + 6 + m, y, ls_lsx_mode_info(l->mode)->name, p_mode(l->mode));
            y++;
            dk_text(sf, k, in.x, y, "rssi", P_LBL);
            snprintf(b, sizeof(b), "%d", l->rssi);
            const int q = dk_text(sf, k, in.x + 5, y, b, p_rssi(l->rssi));
            if (l->rssi_at_peer > -128) {
                dk_text(sf, k, in.x + 6 + q, y, "hears us", P_LBL);
                snprintf(b, sizeof(b), "%d", l->rssi_at_peer);
                dk_text(sf, k, in.x + 15 + q, y, b, p_rssi(l->rssi_at_peer));
            }
        } else {
            dk_text(sf, k, in.x, y, "waiting for a field unit", P_LBL);
            dk_text(sf, k, in.x, y + 1, "to come in range", P_LBL);
        }
    }
    /* every frame heard, on a -130..-30 dBm scale */
    const int cy = in.y + in.h - 4;
    if (cy < in.y + 5) return;
    rssi_chart(sf, k, in.x, cy, 3, l);
    dk_text(sf, k, in.x + 17, cy, "-30", P_LBL);
    dk_text(sf, k, in.x + 17, cy + 2, "-130", P_LBL);
    if (l->frames_rx) {
        snprintf(b, sizeof(b), "%d dBm", l->rssi);
        dk_text(sf, k, in.x + 22, cy + 1, b, p_rssi(l->rssi));
    }
    snprintf(b, sizeof(b), "%lu/%lu", (unsigned long)l->frames_rx, (unsigned long)l->frames_tx);
    int x = in.x;
    x += dk_text(sf, k, x, cy + 3, "frames", P_LBL) + 1;
    x += dk_text(sf, k, x, cy + 3, b, P_VAL) + 2;
    x += dk_text(sf, k, x, cy + 3, "bad", P_LBL) + 1;
    snprintf(b, sizeof(b), "%lu", (unsigned long)l->frames_bad);
    x += dk_text(sf, k, x, cy + 3, b, l->frames_bad ? P(TUI_RED | TUI_BRIGHT) : P_VAL) + 2;
    x += dk_text(sf, k, x, cy + 3, "air", P_LBL) + 1;
    snprintf(b, sizeof(b), "%lus", (unsigned long)(l->airtime_ms_60s / 1000));
    dk_text(sf, k, x, cy + 3, b, P_VAL);
}

/* SYNC: how far the unit in session has got, as one thick meter. */
static void sync_plate(tui_surface *sf, tui_rect clip, tui_rect c, tui_rect r, const ls_lsx_status_t *l,
                       const ls_lsx_sync_status_t *s, bool on)
{
    static const char *const ph[] = { "HELLO", "PULLING", "PUSHING", "DONE" };
    char b[64], chip[24];
    const bool sess = l->state == LSX_ST_SESSION;
    if (sess) snprintf(chip, sizeof(chip), "%s", ph[s->phase & 3]);
    else snprintf(chip, sizeof(chip), "%lu SESSION%s", (unsigned long)l->sessions, l->sessions == 1 ? "" : "S");
    const tui_rect in = plate(sf, clip, c, r, TUI_GREEN, "SYNC", chip, sess ? TUI_GREEN : TUI_WHITE);
    if (in.w <= 0) return;
    const tui_rect k = tui_rect_intersect(clip, in);
    const int pct = sess ? (s->progress_pct < 0 ? 0 : s->progress_pct > 100 ? 100 : s->progress_pct) : 0;
    snprintf(b, sizeof(b), "%3d%%", pct);
    ls_plate_meter(sf, k, in.x, in.y, in.w - 6, pct / 100.0f, sess ? TUI_GREEN : TUI_BLUE);
    dk_text(sf, k, in.x + in.w - 4, in.y, b, sess ? P_VAL : P_LBL);
    if (in.h < 2) return;
    const int y = in.y + in.h - 1;
    if (sess) {
        char a[16];
        int x = in.x;
        group(a, sizeof(a), s->recs_in);
        x += dk_text(sf, k, x, y, a, P(TUI_GREEN | TUI_BRIGHT)) + 1;
        x += dk_text(sf, k, x, y, "records in", P_LBL) + 3;
        group(a, sizeof(a), s->recs_out);
        x += dk_text(sf, k, x, y, a, P(TUI_CYAN | TUI_BRIGHT)) + 1;
        x += dk_text(sf, k, x, y, "out", P_LBL) + 3;
        snprintf(a, sizeof(a), "%lu", (unsigned long)((l->bytes_rx + l->bytes_tx) / 1024));
        x += dk_text(sf, k, x, y, a, P_VAL) + 1;
        dk_text(sf, k, x, y, "KB this session", P_LBL);
    } else dk_text(sf, k, in.x, y, on ? "a unit syncs when it comes in range" : "start the home base to take syncs", P_LBL);
}

/* UNITS: every field unit this desk has synced with. */
static void units_plate(tui_surface *sf, tui_rect clip, tui_rect c, tui_rect r, const ls_lsx_status_t *l,
                        const ls_lsx_sync_status_t *s, bool on)
{
    ls_lsx_unit_t u[LSX_UNITS];
    const int n = ls_lsx_svc_units(u, LSX_UNITS);
    char b[64];
    snprintf(b, sizeof(b), "%d KNOWN", n);
    const tui_rect in = plate(sf, clip, c, r, TUI_YELLOW, "UNITS", b, TUI_WHITE);
    if (in.w <= 0) return;
    const tui_rect k = tui_rect_intersect(clip, in);
    static const char *const H[] = { "UNIT", "LAST SYNC", "IN", "OUT", "SYNCS", "RSSI" };
    static const int X[] = { 2, 16, 30, 42, 52, 60 };
    for (int i = 0; i < 6; i++) dk_text(sf, k, in.x + X[i], in.y, H[i], P_LBL);
    if (!n) {
        dk_text(sf, k, in.x + 2, in.y + 1, on ? "none yet" : "none yet: start the home base, then 'lsx field on' in the car", P_LBL);
        return;
    }
    for (int i = 0; i < n && in.y + 1 + i < in.y + in.h; i++) {
        const int y = in.y + 1 + i;
        const bool live = l->state == LSX_ST_SESSION && l->peer == u[i].id;
        if (live) put(sf, k, in.x, y, LS_SHAPE_RIGHT, P(TUI_GREEN | TUI_BRIGHT));
        snprintf(b, sizeof(b), "%.12s", u[i].name[0] ? u[i].name : "?");
        dk_text(sf, k, in.x + X[0], y, b, P_VAL);
        if (live) snprintf(b, sizeof(b), "now");
        else ago(b, sizeof(b), u[i].last_sync_s, s->now_s);
        dk_text(sf, k, in.x + X[1], y, b, live ? P(TUI_GREEN | TUI_BRIGHT) : P_VAL);
        group(b, sizeof(b), u[i].recs_in);
        dk_text(sf, k, in.x + X[2], y, b, P(TUI_GREEN | TUI_BRIGHT));
        group(b, sizeof(b), u[i].recs_out);
        dk_text(sf, k, in.x + X[3], y, b, P(TUI_CYAN | TUI_BRIGHT));
        snprintf(b, sizeof(b), "%u", (unsigned)u[i].syncs);
        dk_text(sf, k, in.x + X[4], y, b, P_VAL);
        snprintf(b, sizeof(b), "%d", u[i].rssi);
        dk_text(sf, k, in.x + X[5], y, b, p_rssi(u[i].rssi));
    }
}

void dk_home_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    ls_lsx_status_t l;
    ls_lsx_sync_status_t s;
    ls_lsx_svc_status(&l, &s);
    const bool on = ls_lsx_svc_role() == LSX_ROLE_HOME;
    /* the picture, on the window's face; the home base's name over it */
    house(sf, clip, c.x + 1, c.y + 1, &l, on);
    car_and_lane(sf, clip, c.x + 1, c.y + 1, &l);
    char b[32];
    snprintf(b, sizeof(b), "%.12s", ls_lsx_svc_name());
    dk_text(sf, clip, c.x + 24, c.y + 1, b, dk_attr(R_STRONG));

    /* the plates: LINK beside the picture, SYNC and UNITS across under it */
    const int foot = c.y + c.h - 3;                 /* the rule over the buttons */
    link_plate(sf, clip, c, tui_rect_make(c.x + 42, c.y, c.w - 42, 11), &l, on);
    sync_plate(sf, clip, c, tui_rect_make(c.x, c.y + 11, c.w, 4), &l, &s, on);
    units_plate(sf, clip, c, tui_rect_make(c.x, c.y + 15, c.w, foot - (c.y + 15)), &l, &s, on);

    dk_hline(sf, clip, c.x + 1, foot, c.w - 2, LS_BOX_H, dim());
    const char *note = s_note[0] ? s_note : on ? "" : ls_lsx_svc_has_key() ? "" : "no key yet: lsx key phrase <words>";
    dk_text(sf, clip, c.x + 2, c.y + c.h - 2, note, s_note[0] ? hue(TUI_RED, TUI_RED | TUI_BRIGHT) : dim());
    dk_pushbtn(sf, clip, run_rect(c), on ? "&Stop home base" : "&Start home base", s_focus_close ? 0 : DK_BS_DEFAULT | DK_BS_FOCUS);
    dk_pushbtn(sf, clip, ok_rect(c), "&Close", s_focus_close ? DK_BS_DEFAULT | DK_BS_FOCUS : 0);
}

static void toggle(void)
{
    s_note[0] = 0;
    if (ls_lsx_svc_role() == LSX_ROLE_HOME) ls_lsx_svc_stop();
    else if (!ls_lsx_svc_start(LSX_ROLE_HOME)) snprintf(s_note, sizeof(s_note), "%.60s", ls_lsx_svc_why());
}

bool dk_home_tap(dk_win_t *w, tui_rect c, int col, int row)
{
    if (tui_rect_contains(run_rect(c), col, row)) { toggle(); return true; }
    if (tui_rect_contains(ok_rect(c), col, row)) { ls_desk_close(w->id); return true; }
    return false;
}

bool dk_home_key(dk_win_t *w, ls_tk_t k, char ch)
{
    const char lo = (char)(ch | 0x20);
    if (k == LS_TK_CHAR && (lo == 's')) { toggle(); return true; }
    if (k == LS_TK_TAB || k == LS_TK_LEFT || k == LS_TK_RIGHT) { s_focus_close = !s_focus_close; return true; }
    if (k == LS_TK_ENTER || (k == LS_TK_CHAR && ch == ' ')) {
        if (s_focus_close) ls_desk_close(w->id);
        else toggle();
        return true;
    }
    if (k == LS_TK_ESC || (k == LS_TK_CHAR && lo == 'c')) { ls_desk_close(w->id); return true; }
    return false;
}

int dk_open_home(void)
{
#ifdef ESP_PLATFORM
    extern const ls_lsx_store_t *dk_home_store(void);
    (void)dk_home_store();
#endif
    s_focus_close = false;
    s_note[0] = 0;
    return dk_open_shell(DK_SHELL_HOME, "Home Base", 76, 24);
}
