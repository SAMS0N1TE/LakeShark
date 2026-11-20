/* TACTICAL: everything near you on one plot, for a view in which a ship two
 * kilometres off is a single dot. Seen from above and behind: your ship in the
 * middle pointing up the screen, a disc of range rings scaled so near things
 * spread out and far ones still fit, and each contact on a stalk from the disc
 * to its height above or below you, coloured as the halos are. The station,
 * the worlds and the star sit at their bearings; past the last ring they wait
 * on the rim. A tap on a mark targets it. */
#include "internal.h"
#include "art.h"
#include "ui.h"

#define TAC_MAX_M 30000.f       /* the rim */
#define TAC_KNEE  400.f         /* below this, distance is nearly linear */
#define TAC_TILT  .55f          /* the disc seen from above and behind */

static inline void dot(int x, int y, uint8_t ink) {
    if (x >= 0 && y >= 0 && x < ui.dw && y < ui.dh) ui.dots[y * ui.dw + x] = ink;
}
static void dline(int x0, int y0, int x1, int y1, uint8_t ink, int skip) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy, n = 0;
    for (;;) {
        if (!skip || (n++ % skip) == 0) dot(x0, y0, ink);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
static void ellipse(int cx, int cy, float rx, float ry, uint8_t ink, int step) {
    int n = (int)(rx * 3) + 16;
    for (int i = 0; i < n; i += step) {
        float a = i * 2 * CG_PI / n;
        dot(cx + UI_ROUND(cosf(a) * rx), cy + UI_ROUND(sinf(a) * ry), ink);
    }
}

static uint8_t mark_ink(const game_contact *c) {
    if (c->scanned && c->wanted) return HUD_WARN;
    if (c->hostile) return HUD_WARN;
    if (c->role == ROLE_POLICE) return HUD_LAW;
    if (c->role == ROLE_BEACON) return HUD_OK;
    if (c->live) return HUD_COOL;
    return HUD_HI;
}

static int cx, cy;
static float rim;               /* the rim's radius in dots */
static int mark_x[GAME_CONTACTS], mark_y[GAME_CONTACTS];
static bool mark_on[GAME_CONTACTS];
/* the station, the worlds and the star, named as NAV names them */
#define TAC_BODIES 8
static struct { int x, y; float range; uint8_t ink; char name[24]; } body[TAC_BODIES];
static int bodies;
static void body_add(int x, int y, l3_vec w, uint8_t ink, const char *name) {
    if (bodies >= TAC_BODIES) return;
    body[bodies].x = x;
    body[bodies].y = y;
    l3_vec d = l3_sub(w, game_state()->player.position);
    body[bodies].range = sqrtf(l3_dot(d, d));
    body[bodies].ink = ink;
    snprintf(body[bodies].name, sizeof body[bodies].name, "%s", name);
    bodies++;
}

static float scale(float d) { return rim * logf(1 + d / TAC_KNEE) / logf(1 + TAC_MAX_M / TAC_KNEE); }

/* A world position on the plot: the point on the disc and the mark above it. */
static bool place(l3_vec w, int *px, int *py, int *hx, int *hy, bool *beyond) {
    const game_player *p = &game_state()->player;
    l3_vec f = l3_quat_rotate(p->attitude, l3v(0, 0, -1)), up = l3_quat_rotate(p->attitude, l3v(0, 1, 0));
    l3_vec rt = l3_cross(f, up), t = l3_sub(w, p->position);
    float lx = l3_dot(t, rt), lz = l3_dot(t, f), ly = l3_dot(t, up);
    float flat = sqrtf(lx * lx + lz * lz);
    *beyond = flat > TAC_MAX_M;
    float r = scale(fminf(flat, TAC_MAX_M)), k = flat > 1e-3f ? r / flat : 0;
    *px = cx + UI_ROUND(lx * k);
    *py = cy - UI_ROUND(lz * k * TAC_TILT);
    float h = scale(fminf(fabsf(ly), TAC_MAX_M)) * .7f;
    *hx = *px;
    *hy = *py - UI_ROUND(ly >= 0 ? h : -h);
    return true;
}

void ui_tactical_paint(void) {
    const game_view *v = game_state();
    memset(ui.dots, LAP_DOT_CLEAR, (size_t)ui.dw * ui.dh);
    cx = ui.dw / 2;
    cy = ui.dh / 2 + 4;
    rim = fminf(ui.dw * .5f - 4, (ui.dh * .5f - 6) / TAC_TILT);
    /* the disc: rings at 1, 5 and 20 km, the rim, a cross and the nose's line */
    static const float rings[3] = {1000, 5000, 20000};
    for (int i = 0; i < 3; i++) ellipse(cx, cy, scale(rings[i]), scale(rings[i]) * TAC_TILT, HUD_DIM, 2);
    ellipse(cx, cy, rim, rim * TAC_TILT, 8, 1);
    dline(cx - (int)rim, cy, cx + (int)rim, cy, HUD_DIM, 3);
    dline(cx, cy + (int)(rim * TAC_TILT), cx, cy - (int)(rim * TAC_TILT), HUD_DIM, 3);
    /* the star, the worlds, the station */
    int px, py, hx, hy;
    bool beyond;
    const char *sys = v->systems[v->system].name;
    char name[24];
    static const char *const roman[6] = {"I", "II", "III", "IV", "V", "VI"};
    bodies = 0;                         /* labels: the station first, the star last */
    place(v->contents.station, &px, &py, &hx, &hy, &beyond);
    body_add(hx, hy, v->contents.station, HUD_OK, v->contents.station_name[0] ? v->contents.station_name : "Station");
    for (int i = 0; i < v->contents.planet_count; i++) {
        place(v->contents.planets[i].position, &px, &py, &hx, &hy, &beyond);
        snprintf(name, sizeof name, "%s %s", sys, roman[i % 6]);
        body_add(px, py, v->contents.planets[i].position, LAP_BLUE, name);
    }
    place(v->contents.star, &px, &py, &hx, &hy, &beyond);
    snprintf(name, sizeof name, "%s star", sys);
    body_add(px, py, v->contents.star, LAP_YELLOW, name);
    /* drawn the other way round, so the station sits on top */
    for (int a = 0; a < 12; a++) dot(px + UI_ROUND(cosf(a * .52f) * 2), py + UI_ROUND(sinf(a * .52f) * 2), LAP_YELLOW);
    dot(px, py, 15);
    for (int i = 0; i < v->contents.planet_count; i++) {
        place(v->contents.planets[i].position, &px, &py, &hx, &hy, &beyond);
        ellipse(px, py, 3, 2, LAP_BLUE, 1);
        dot(px, py, 12);
    }
    {                                   /* every system has its station */
        place(v->contents.station, &px, &py, &hx, &hy, &beyond);
        if (hy != py) dline(px, py, hx, hy, 8, 2);
        for (int k = -2; k <= 2; k++) { dot(hx + k, hy - 2, HUD_OK); dot(hx + k, hy + 2, HUD_OK); dot(hx - 2, hy + k, HUD_OK); dot(hx + 2, hy + k, HUD_OK); }
    }
    /* the ships: a stalk from the disc, a mark at their height */
    for (int i = 0; i < GAME_CONTACTS; i++) {
        const game_contact *c = &v->contacts[i];
        mark_on[i] = false;
        if (!c->active) continue;
        place(c->position, &px, &py, &hx, &hy, &beyond);
        uint8_t ink = mark_ink(c);
        dot(px, py, ink);
        if (hy != py) dline(px, py, hx, hy, ink == HUD_HI ? 8 : ink, 2);
        if (c->hostile) { for (int k = -2; k <= 2; k++) dot(hx + k, hy - 1, ink); dot(hx - 1, hy, ink); dot(hx, hy, ink); dot(hx + 1, hy, ink); dot(hx, hy + 1, ink); }
        else { dot(hx, hy - 1, ink); dot(hx - 1, hy, ink); dot(hx + 1, hy, ink); dot(hx, hy + 1, ink); }
        if (i == v->selected)               /* the target: brackets */
            for (int k = 0; k < 2; k++) {
                dot(hx - 4 + k, hy - 3, ink); dot(hx + 4 - k, hy - 3, ink); dot(hx - 4 + k, hy + 3, ink); dot(hx + 4 - k, hy + 3, ink);
                dot(hx - 4, hy - 3 + k, ink); dot(hx + 4, hy - 3 + k, ink); dot(hx - 4, hy + 3 - k, ink); dot(hx + 4, hy + 3 - k, ink);
            }
        mark_x[i] = hx;
        mark_y[i] = hy;
        mark_on[i] = true;
    }
    for (int i = 0; i < CG_CANS; i++) {
        if (cg->cans[i].life <= 0) continue;
        place(cg->cans[i].p, &px, &py, &hx, &hy, &beyond);
        dot(hx, hy, HUD);
    }
    /* you: an arrow at the middle, nose up the screen */
    for (int k = 0; k < 4; k++) { dot(cx - k, cy - 3 + k * 2, HUD_HI); dot(cx + k, cy - 3 + k * 2, HUD_HI); }
    dot(cx, cy - 4, 15);
}

/* Labels never overlap: each takes its cells, a later one moves a row or is left out. */
static uint8_t taken[64][32];           /* rows x (columns / 8) */
static void label(int mx, int my, const char *name, float range, uint8_t ink) {
    char line[40];
    if (range >= 1000) snprintf(line, sizeof line, "%s %.1fkm", name, range / 1000);
    else snprintf(line, sizeof line, "%s %.0fm", name, range);
    int col = mx / 2 + 3, row0 = my / 3;
    int len = (int)strlen(line);
    if (col + len > ui.view.w) col = mx / 2 - len - 2;
    if (col < 0) return;
    static const int tries[5] = {0, 1, -1, 2, -2};
    for (int t = 0; t < 5; t++) {
        int row = row0 + tries[t];
        if (row < 1 || row >= ui.view.h - 3 || row >= 64) continue;
        bool clear = true;
        for (int x = col - 1; x <= col + len && clear; x++)
            if (x >= 0 && x < 256 && taken[row][x >> 3] >> (x & 7) & 1) clear = false;
        if (!clear) continue;
        for (int x = col - 1; x <= col + len; x++)
            if (x >= 0 && x < 256) taken[row][x >> 3] |= (uint8_t)(1 << (x & 7));
        ui_put(ui.view.x + col, ui.view.y + row, line, LAP_ATTR(ink, 0));
        return;
    }
}
static void ship_label(int i) {
    const game_contact *c = &game_state()->contacts[i];
    const game_ship_stats *st = game_ship(c->ship_class);
    label(mark_x[i], mark_y[i], c->name[0] ? c->name : st ? st->name : "ship", c->range, mark_ink(c));
}

void ui_tactical_words(void) {
    const game_view *v = game_state();
    ui_put(ui.view.x + 1, ui.view.y + 4, "TACTICAL", LAP_ATTR(HUD_HI, 0));
    ui_put(ui.view.x + 1, ui.view.y + 5, "rings 1, 5, 20 km", LAP_ATTR(HUD_SUB, 0));
    memset(taken, 0, sizeof taken);
    /* the target first, then the station, the six nearest ships, the worlds and the star */
    if (v->selected >= 0 && v->selected < GAME_CONTACTS && mark_on[v->selected]) ship_label(v->selected);
    if (bodies) label(body[0].x, body[0].y, body[0].name, body[0].range, body[0].ink);
    int order[GAME_CONTACTS], n = 0;
    for (int i = 0; i < GAME_CONTACTS; i++) if (mark_on[i] && i != v->selected) order[n++] = i;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && v->contacts[order[b]].range < v->contacts[order[b - 1]].range; b--) {
            int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    for (int k = 0; k < n && k < 6; k++) ship_label(order[k]);
    for (int k = 1; k < bodies; k++) label(body[k].x, body[k].y, body[k].name, body[k].range, body[k].ink);
}

/* A tap on the plot targets the nearest mark within a few cells of it. */
bool ui_tactical_tap(int col, int row) {
    const game_view *v = game_state();
    int x = (col - ui.view.x) * 2 + 1, y = (row - ui.view.y) * 3 + 1, best = -1, bd = 12 * 12;
    for (int i = 0; i < GAME_CONTACTS; i++) {
        if (!mark_on[i] || !v->contacts[i].active) continue;
        int dx = mark_x[i] - x, dy = mark_y[i] - y, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return false;
    for (int guard = 0; v->selected != best && guard < GAME_CONTACTS + 1; guard++) {
        int before = v->selected;
        game_target_next();
        if (v->selected == before && guard) break;
    }
    lap_sound(LAP_SOUND_KEY);
    return true;
}
