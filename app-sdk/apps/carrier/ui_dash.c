/* The dash: panel tabs, the message line, the instruments (gauges, the 3D
 * scanner, the compass, the throttle) and the five flight keys. Amber on black;
 * shields cyan, danger red, real aircraft on the scanner cyan too. */
#include "internal.h"
#include "art.h"
#include "ui.h"

static char line[80];
#define SCOPE_RANGE 1400.f

bool ui_in(const ui_rect *r, int col, int row) {
    return col >= r->x && row >= r->y && col < r->x + r->w && row < r->y + r->h;
}

void ui_layout(void) {
    int C = lap_cols(), R = lap_rows();
    ui.cols = C;
    ui.rows = R;
    ui.landscape = C > R * 2;
    ui.wide = ui.landscape && R >= 36;
    ui.panel_full = false;
    ui.nlamps = ui.nctls = 0;
    ui_rect d;
    if (ui.wide) {      /* a desktop grid: the glass across the top, a cockpit console low across the bottom */
        int dh = R >= 50 ? 18 : 16;
        ui.view = (ui_rect){0, 0, C, R - dh};
        d = (ui_rect){0, R - dh, C, dh};
        ui.dash = d;
        /* far left, stacked: the four panels, then the four dash modes */
        const int bw = 12, bh = dh / 8;
        for (int i = 0; i < 4; i++) ui.tab_keys[i] = (ui_rect){d.x, d.y + i * bh, bw, bh};
        for (int i = 0; i < UI_MODES; i++) ui.mode_keys[i] = (ui_rect){d.x, d.y + (4 + i) * bh, bw, i == UI_MODES - 1 ? dh - (4 + i) * bh : bh};
        ui.tabs = ui.modebar = ui.mode_info = (ui_rect){0, 0, 0, 0};
        /* far right, stacked: the five flight keys */
        const int kw = 14;
        for (int i = 0; i < 5; i++) {
            int y = d.y + dh * i / 5;
            ui.keys[i] = (ui_rect){d.x + d.w - kw, y, kw, d.y + dh * (i + 1) / 5 - y};
        }
        /* between: status lamps, the scanner wide in the middle, push buttons, the throttle */
        int ix = d.x + bw + 1, iy = d.y, ih = dh;
        const int tc = C >= 140 ? 3 : 2, tw = 11, th = 3, lw = tc * tw, rows = ih / th;
        ui.left = (ui_rect){ix, iy, lw, ih};
        ui.right = (ui_rect){0, 0, 0, 0};
        ui.throttle = (ui_rect){d.x + d.w - kw - 8, iy, 6, ih};
        ui.ctl_box = (ui_rect){ui.throttle.x - 2 - lw, iy, lw, ih};
        ui.scope = (ui_rect){ui.left.x + lw + 1, iy, ui.ctl_box.x - 1 - (ui.left.x + lw + 1), ih};
        ui.throttle_grab = (ui_rect){ui.throttle.x - 2, iy, 10, ih};
        ui.middle = ui.scope;           /* NAV, COMMS and SYS take the scanner's place */
        ui.inst = (ui_rect){ui.left.x, iy, ui.throttle.x + ui.throttle.w - ui.left.x, ih};
        ui.nlamps = ui.nctls = tc * rows > 15 ? 15 : tc * rows;
        for (int k = 0; k < ui.nlamps; k++) {
            ui.lamps[k] = (ui_rect){ui.left.x + (k % tc) * tw, iy + (k / tc) * th, tw, th};
            ui.ctls[k] = (ui_rect){ui.ctl_box.x + (k % tc) * tw, iy + (k / tc) * th, tw, th};
        }
        /* a panel open: the instruments step aside and it takes the full height
           between the tab column and the keys, no wider than reads well */
        const game_view *gv = game_state();
        ui.panel_full = gv && gv->panel != PANEL_NONE;
        if (ui.panel_full) {
            int x0 = d.x + bw + 2, x1 = d.x + d.w - kw - 2, w = x1 - x0 > 104 ? 104 : x1 - x0;
            ui.view = (ui_rect){x0 + (x1 - x0 - w) / 2, 0, w, R};
            /* the console is not drawn, so none of it takes taps (its NAV page and
               buttons lay under the panel's own keys); the four panel tabs run the
               full height of the left column, and the dash modes wait */
            ui.left = ui.scope = ui.middle = ui.inst = ui.ctl_box = (ui_rect){0, 0, 0, 0};
            ui.throttle = ui.throttle_grab = (ui_rect){0, 0, 0, 0};
            ui.nlamps = ui.nctls = 0;
            const int th = R / 4 > 6 ? 6 : R / 4;
            for (int i = 0; i < 4; i++) ui.tab_keys[i] = (ui_rect){0, 2 + i * th, bw, th};
            for (int i = 0; i < UI_MODES; i++) ui.mode_keys[i] = (ui_rect){0, 0, 0, 0};
        }
    } else if (!ui.landscape) {
        int dh = R >= 66 ? 25 : R >= 60 ? 23 : R - 30;
        ui.view = (ui_rect){0, 0, C, R - dh};
        d = (ui_rect){0, R - dh, C, dh};
    } else {    /* dash on the left, so the close corner falls on the view */
        int dw = C * 2 / 5 < 44 ? 44 : C * 2 / 5 > 52 ? 52 : C * 2 / 5;   /* a wide desktop grid gives the view the rest */
        d = (ui_rect){0, 0, dw, R};
        ui.view = (ui_rect){dw, 0, C - dw, R};
    }
    if (!ui.wide) {
        ui.dash = d;
        ui.tabs = (ui_rect){d.x, d.y, d.w, 3};
        for (int i = 0; i < 4; i++) {
            int x = d.x + d.w * i / 4;
            ui.tab_keys[i] = (ui_rect){x, d.y, d.x + d.w * (i + 1) / 4 - x, 3};
        }
        /* the mode bar: FLIGHT / NAV / COMMS, and the radio's corner */
        ui.modebar = (ui_rect){d.x, d.y + 3, d.w, 3};
        for (int i = 0; i < UI_MODES; i++) {
            int x = d.x + d.w * i / UI_MODES;
            ui.mode_keys[i] = (ui_rect){x, d.y + 3, d.x + d.w * (i + 1) / UI_MODES - x, 3};
        }
        ui.mode_info = (ui_rect){0, 0, 0, 0};
        int iy = d.y + 6, ky = d.y + d.h - 5;
        int ih = ky - iy;
        /* the throttle sits inboard, away from the glass edge; the gauges take the edge */
        int rw = d.w >= 48 ? 8 : 0;
        ui.right = (ui_rect){d.x + d.w - rw, iy, rw, ih};
        ui.throttle = (ui_rect){ui.right.x - 6, iy, 6, ih};
        ui.left = (ui_rect){d.x, iy, d.w * 26 / 100, ih};
        ui.scope = (ui_rect){ui.left.x + ui.left.w, iy, ui.throttle.x - (ui.left.x + ui.left.w), ih};
        ui.throttle_grab = (ui_rect){ui.throttle.x - 2, iy, d.x + d.w - (ui.throttle.x - 2), ih};
        ui.middle = (ui_rect){d.x, iy, ui.throttle.x - 1 - d.x, ih};   /* NAV and COMMS use this */
        ui.inst = (ui_rect){d.x, iy, d.w, ih};
        static const int share[5] = {14, 9, 8, 10, 11};
        int x = d.x;
        for (int i = 0; i < 5; i++) {
            int w = i == 4 ? d.x + d.w - x : d.w * share[i] / 52;
            ui.keys[i] = (ui_rect){x, ky, w, 5};
            x += w;
        }
    }
    ui.close = (ui_rect){ui.view.x + ui.view.w - 7, ui.view.y, 7, 3};     /* the EXIT placard */
    ui.viewkey = (ui_rect){ui.view.x + ui.view.w - 8, ui.view.y + 4, 8, 3};   /* VIEW under it */
    if (ui.wide) ui.viewkey = (ui_rect){0, 0, 0, 0};   /* the console has VIEW, and O */
    int qw = ui.view.w - 8 < 40 ? ui.view.w - 8 : 40, qx = ui.view.x + (ui.view.w - qw) / 2;
    int qy = ui.view.y + ui.view.h / 2 - 1;
    ui.quit_stay = (ui_rect){qx + 1, qy + 3, qw / 2 - 1, 4};
    ui.quit_leave = (ui_rect){qx + qw / 2, qy + 3, qw / 2 - 1, 4};
}

/* ---- dots on the instrument canvas ------------------------------------- */

static lap_canvas_t *cv;
static int ox, oy;      /* the instrument block's top-left, in cells, for text */
static inline void px(int x, int y, uint8_t ink) { lc_pset(cv, x, y, ink); }

/* A segmented bar two dots high: lit segments in ink, the rest a dim rail. */
static void bar(int x, int y, int w, float frac, uint8_t ink) {
    int lit = (int)(cg_clamp(frac, 0, 1) * w + .5f);
    for (int i = 0; i < w; i++) {
        if (i % 3 == 2) continue;
        if (i < lit) { px(x + i, y, ink); px(x + i, y + 1, ink); }
        else px(x + i, y + 1, HUD_DIM);
    }
}
/* A centred marker on a rail: roll and pitch. */
static void needle(int x, int y, int w, float v, uint8_t ink) {
    for (int i = 0; i < w; i += 2) px(x + i, y + 1, HUD_DIM);
    px(x + w / 2, y, HUD_DIM);
    int m = x + (int)((cg_clamp(v, -1, 1) + 1) * .5f * (w - 1));
    for (int k = -1; k <= 1; k++) { px(m + k, y, ink); px(m + k, y + 1, ink); }
}

static bool labels_on;
static void label(int cell_x, int cell_y, const char *s, uint8_t ink) {
    if (!labels_on) return;
    lap_put_str(ox + cell_x, oy + cell_y, s, LAP_ATTR(ink, 0));
}

static uint8_t level_ink(float f, bool low_bad) {
    float bad = low_bad ? 1 - f : f;
    return bad > .75f ? HUD_WARN : bad > .5f ? HUD_HI : HUD;
}

static float altitude(const game_view *v) {
    float best = 1e9f;
    for (int i = 0; i < v->contents.planet_count; i++) {
        float d = cg_len(l3_sub(v->player.position, v->contents.planets[i].position)) - v->contents.planets[i].radius;
        if (d < best) best = d;
    }
    return best;
}

static void gauges_left(const game_view *v) {
    const game_player *p = &v->player;
    const game_ship_stats *me = game_player_ship();
    int w = ui.left.w * 2 - 7;
    float alt = altitude(v);
    struct { const char *k; float f; uint8_t ink; } g[7] = {
        {"FS", p->shield_fore / 100, HUD_COOL},
        {"AS", p->shield_aft / 100, HUD_COOL},
        {"HU", me ? p->hull / me->hull : 1, 0},
        {"FU", p->fuel / 7, 0},
        {"CT", p->cabin_heat, 0},
        {"LT", p->heat / 100, 0},
        {"AL", alt < 3000 ? alt / 3000 : 1, HUD},
    };
    g[2].ink = level_ink(g[2].f, true);
    g[3].ink = level_ink(g[3].f, true);
    g[4].ink = level_ink(g[4].f, false);
    g[5].ink = level_ink(g[5].f, false);
    int rows = ui.left.h, step = rows >= 13 ? 2 : 1;
    for (int i = 0; i < 7 && i * step < rows; i++) {
        int r = i * step + (rows >= 14 ? 0 : 0);
        label(0, r, g[i].k, HUD_SUB);
        bar(6, r * 3 + 1, w, g[i].f, g[i].ink);
    }
}

static void gauges_right(const game_view *v) {
    if (ui.right.w <= 0) return;
    const game_player *p = &v->player;
    const game_ship_stats *me = game_player_ship();
    int x = (ui.right.x - ui.left.x) * 2 + 5, w = ui.right.w * 2 - 6, cx = ui.right.x - ui.left.x;
    label(cx, 1, "SP", HUD_SUB);
    bar(x, 1 * 3 + 1, w, me ? p->speed / me->speed : 0, HUD);
    label(cx, 3, "RL", HUD_SUB);
    needle(x, 3 * 3 + 1, w, p->roll, HUD_HI);
    label(cx, 5, "DC", HUD_SUB);
    needle(x, 5 * 3 + 1, w, p->pitch, HUD_HI);
    static const char *const pk[3] = {"SY", "EN", "WP"};
    static const uint8_t pink[3] = {HUD_COOL, HUD_OK, HUD_WARN};
    for (int k = 0; k < 3; k++) {
        int r = 7 + k * 2;
        if (r >= ui.right.h) break;
        label(cx, r, pk[k], HUD_SUB);
        bar(x, r * 3 + 1, w, p->pips[k] / 4, pink[k]);
    }
    if (13 < ui.right.h) {
        label(cx, 13, "CP", HUD_SUB);
        bar(x, 13 * 3 + 1, w, cg_clamp(p->weapon_capacitor, 0, 1), p->weapon_capacitor < .25f ? HUD_WARN : HUD);
    }
}

/* The scanner: a disc around the ship seen from above-behind; each contact a
 * dot on a stalk, up from the disc when above you, down when below. */
static void scope(const game_view *v) {
    const game_player *p = &v->player;
    int x0 = (ui.scope.x - ui.left.x) * 2, w = ui.scope.w * 2, h = ui.scope.h * 3;
    int cx = x0 + w / 2, cy = h / 2 + 1;
    float ry = fminf((w / 2 - 2) * .42f, h / 2 - 4), rx = fminf(w / 2 - 2, ry * 2.8f);   /* an oval, however wide the console */
    /* rings, cross, the field of view */
    for (int i = 0; i < 96; i++) {
        float a = i * 2 * CG_PI / 96;
        int ex = cx + UI_ROUND(cosf(a) * rx), ey = cy + UI_ROUND(sinf(a) * ry);
        px(ex, ey, i % 2 ? HUD : HUD_DIM);
        if (i % 3 == 0) px(cx + UI_ROUND(cosf(a) * rx * .5f), cy + UI_ROUND(sinf(a) * ry * .5f), HUD_DIM);
    }
    for (int i = -(int)rx; i <= (int)rx; i += 3) px(cx + i, cy, HUD_DIM);
    for (int i = -(int)ry; i <= (int)ry; i += 2) px(cx, cy + i, HUD_DIM);
    float half = UI_FOV * .5f * CG_RAD * 1.25f;
    for (int k = 0; k < 18; k++) {
        float t = k / 18.f;
        px(cx + (int)(sinf(half) * rx * t), cy - (int)(cosf(half) * ry * t), HUD_DIM);
        px(cx - (int)(sinf(half) * rx * t), cy - (int)(cosf(half) * ry * t), HUD_DIM);
    }
    l3_vec f = l3_quat_rotate(p->attitude, l3v(0, 0, -1)), up = l3_quat_rotate(p->attitude, l3v(0, 1, 0));
    l3_vec rt = l3_cross(f, up);
    /* station, then ships, then missiles and canisters */
    for (int pass = 0; pass < 3; pass++) {
        int n = pass == 0 ? 1 : pass == 1 ? GAME_CONTACTS : CG_MISSILES + CG_CANS;
        for (int i = 0; i < n; i++) {
            l3_vec at;
            uint8_t ink;
            int size = 1;
            if (pass == 0) { at = v->contents.station; ink = 15; size = 2; }
            else if (pass == 1) {
                const game_contact *c = &v->contacts[i];
                if (!c->active) continue;
                at = c->position;
                ink = c->hostile ? HUD_WARN : c->role == ROLE_POLICE ? HUD_LAW : c->role == ROLE_BEACON ? HUD_OK : c->live ? HUD_COOL : HUD_HI;
                if (i == v->selected && ((int)(ui.t * 4) & 1)) ink = 15;
            } else if (i < CG_MISSILES) {
                if (cg->missiles[i].life <= 0) continue;
                at = cg->missiles[i].p; ink = HUD_WARN;
            } else {
                if (cg->cans[i - CG_MISSILES].life <= 0) continue;
                at = cg->cans[i - CG_MISSILES].p; ink = LAP_WHITE;
            }
            l3_vec d = l3_sub(at, p->position);
            float lx = l3_dot(d, rt), lz = l3_dot(d, f), ly = l3_dot(d, up);
            float dist = sqrtf(lx * lx + lz * lz);
            float k = 1 / SCOPE_RANGE;
            if (dist > SCOPE_RANGE) {     /* past the edge: pinned to the rim, dim */
                if (pass == 0) { k = 1 / dist; ink = HUD; } else continue;
            }
            int sx = cx + (int)(lx * k * rx), sy = cy - (int)(lz * k * ry);
            int ty = sy - (int)(cg_clamp(ly * k, -1.2f, 1.2f) * ry * 1.6f);
            ty = ty < 1 ? 1 : ty > h - 2 ? h - 2 : ty;      /* a stalk stays on the instrument */
            for (int y = sy < ty ? sy : ty; y <= (sy < ty ? ty : sy); y++) px(sx, y, ink == 15 ? HUD : ink);
            for (int a = 0; a < size + 1; a++)
                for (int b = 0; b < 2; b++) px(sx + a - size / 2, ty + b - 1, ink);
        }
    }
    for (int i = 0; i < ui_net_count(); i++) {      /* commanders on the mesh */
        l3_vec dd = l3_sub(ui_net_peer(i)->position, p->position);
        float lx = l3_dot(dd, rt), lz = l3_dot(dd, f), ly = l3_dot(dd, up), k = 1 / SCOPE_RANGE;
        if (lx * lx + lz * lz > SCOPE_RANGE * SCOPE_RANGE) continue;
        int sx = cx + (int)(lx * k * rx), sy = cy - (int)(lz * k * ry);
        int ty = sy - (int)(cg_clamp(ly * k, -1.2f, 1.2f) * ry * 1.6f);
        ty = ty < 1 ? 1 : ty > h - 2 ? h - 2 : ty;
        for (int y = sy < ty ? sy : ty; y <= (sy < ty ? ty : sy); y++) px(sx, y, HUD_COOL);
        px(sx - 1, ty, HUD_COOL); px(sx + 1, ty, HUD_COOL); px(sx, ty - 1, HUD_COOL); px(sx, ty + 1, HUD_COOL);
    }
    for (int i = 0; i < v->poi_count; i++) {        /* places */
        const game_poi *q = &v->pois[i];
        if (!q->discovered && !game_poi_detectable(p->position, q->position)) continue;
        l3_vec dd = l3_sub(q->position, p->position);
        float lx = l3_dot(dd, rt), lz = l3_dot(dd, f), dist = sqrtf(lx * lx + lz * lz), k = 1 / SCOPE_RANGE;
        if (dist > SCOPE_RANGE) k = 1 / dist;          /* far ones sit on the rim, pointing the way */
        int sx = cx + (int)(lx * k * rx), sy = cy - (int)(lz * k * ry);
        uint8_t ink = q->scanned ? HUD_COOL : (uint8_t)((q->colour & 7) | 8);
        px(sx - 1, sy - 1, ink); px(sx + 1, sy - 1, ink); px(sx - 1, sy + 1, ink); px(sx + 1, sy + 1, ink);
    }
    if (v->scanner_scrambled)
        for (int k = 0; k < 90; k++) {
            uint32_t h = cg_hash((uint32_t)k * 7919u + (uint32_t)(ui.t * 15));
            px(cx + (int)((h & 255) / 255.f * rx * 2 - rx), cy + (int)(((h >> 8) & 255) / 255.f * ry * 2 - ry),
               (h >> 16) & 1 ? 15 : HUD_DIM);
        }
    /* the compass, bottom right of the scanner: the station, or the target */
    int ccx = x0 + w - 7, ccy = h - 7;
    l3_vec goal = v->contents.station;
    ui_dest nav;
    if (ui_nav_target(&nav)) goal = nav.position;
    else if (v->selected >= 0 && v->contacts[v->selected].active) goal = v->contacts[v->selected].position;
    l3_vec d = l3_norm(l3_sub(goal, p->position));
    for (int i = 0; i < 24; i++) {
        float a = i * 2 * CG_PI / 24;
        px(ccx + UI_ROUND(cosf(a) * 5), ccy + UI_ROUND(sinf(a) * 5), HUD_DIM);
    }
    int dx = ccx + UI_ROUND(l3_dot(d, rt) * 4), dy = ccy - UI_ROUND(l3_dot(d, up) * 4);
    bool ahead = l3_dot(d, f) > 0;
    px(dx, dy, ahead ? HUD_OK : HUD_DIM);
    px(dx + 1, dy, ahead ? HUD_OK : HUD_DIM);
    if (ahead) { px(dx, dy + 1, HUD_OK); px(dx + 1, dy + 1, HUD_OK); }
}

static void throttle(const game_view *v) {
    const game_player *p = &v->player;
    int x0 = (ui.throttle.x - ui.left.x) * 2 + 2, h = ui.throttle.h * 3;
    int top = 6, bottom = h - 2, span = bottom - top;
    /* reverse is the bottom of the lever, below a zero detent, in blue */
    float rev = game_reverse_max(), all = 1 + rev;
    int zero = bottom - (int)(rev / all * span + .5f);
    int lvl = zero - (int)(p->throttle / all * span + (p->throttle >= 0 ? .5f : -.5f));
    for (int y = top; y <= bottom; y++) {
        bool back = y > zero;
        bool lit = p->throttle >= 0 ? y >= lvl && y <= zero : y > zero && y <= lvl;
        uint8_t ink = back ? HUD_COOL : HUD;
        px(x0 + 3, y, lit ? ink : back ? LAP_BLUE : HUD_DIM);
        if (lit) { px(x0 + 4, y, ink); px(x0 + 2, y, (y & 1) ? ink : HUD_DIM); }
        if (y <= zero && (zero - y) % (span / 4 ? span / 4 : 1) == 0) { px(x0, y, HUD_DIM); px(x0 + 6, y, HUD_DIM); }
    }
    for (int k = -1; k < 8; k++) px(x0 + k, zero, HUD_SUB);           /* the zero detent */
    uint8_t hand = p->throttle < 0 ? HUD_COOL : HUD_HI;
    for (int k = -1; k < 9; k++) { px(x0 + k, lvl, hand); px(x0 + k, lvl - 1, hand); px(x0 + k, lvl + 1, p->throttle < 0 ? LAP_BLUE : HUD); }
    label(ui.throttle.x - ui.left.x + 1, 0, "THR", HUD_SUB);
    label(ui.throttle.x - ui.left.x, (bottom + 1) / 3 - 1, "R", p->throttle < 0 ? HUD_COOL : LAP_BLUE);
}

void ui_key_box(const ui_rect *r, const char *label_s, const char *sub, uint8_t fg, uint8_t bg, bool lit) {
    uint8_t a = lit ? LAP_ATTR(LAP_BLACK, fg) : LAP_ATTR(fg, bg);
    lap_fill(r->x, r->y, r->w, r->h, ' ', a);
    /* a frame of light lines so neighbouring keys read apart */
    lap_hline(r->x, r->y, r->w, LAP_BRAILLE(0x09), LAP_ATTR(lit ? fg : HUD_DIM, lit ? fg : bg));
    int n = (int)strlen(label_s);
    lap_put_str(r->x + (r->w - n) / 2, r->y + r->h / 2, label_s, a);
    if (sub && *sub) {
        int m = (int)strlen(sub);
        lap_put_str(r->x + (r->w - m) / 2, r->y + r->h / 2 + 1, sub,
                    lit ? a : LAP_ATTR(HUD_SUB, bg));
    }
}

/* A setting's key: on is bright words over a short green bar, not a block
   of solid amber (a panel of those read as one slab). */
void ui_toggle_box(const ui_rect *r, const char *label_s, const char *sub, uint8_t fg, bool on) {
    lap_fill(r->x, r->y, r->w, r->h, ' ', LAP_ATTR(fg, LAP_BLACK));
    lap_hline(r->x, r->y, r->w, LAP_BRAILLE(0x09), LAP_ATTR(HUD_DIM, LAP_BLACK));
    int n = (int)strlen(label_s), row = r->y + r->h / 2;
    lap_put_str(r->x + (r->w - n) / 2, row, label_s, LAP_ATTR(on ? HUD_HI : fg, LAP_BLACK));
    if (sub && *sub) {
        int m = (int)strlen(sub);
        lap_put_str(r->x + (r->w - m) / 2, row + 1, sub, LAP_ATTR(HUD_SUB, LAP_BLACK));
    }
    int bw = n < 6 ? 6 : n, by = r->y + r->h - 1 > row + 1 ? r->y + r->h - 1 : row + (sub && *sub ? 2 : 1);
    if (by < r->y + r->h)
        lap_hline(r->x + (r->w - bw) / 2, by, bw, LAP_BRAILLE(0x1B), LAP_ATTR(on ? HUD_OK : HUD_DIM, LAP_BLACK));
}

/* A tab or mode key in a strip (three rows) or a stacked column (two). */
void ui_tab_key(const ui_rect *r, const char *s, bool on, bool usable, bool divider) {
    uint8_t fg = on ? LAP_BLACK : usable ? HUD : HUD_SUB, bg = on ? HUD : LAP_BLACK;
    lap_fill(r->x, r->y, r->w, r->h, ' ', LAP_ATTR(fg, bg));
    lap_put_str(r->x + (r->w - (int)strlen(s)) / 2, r->y + (r->h - 1) / 2, s, LAP_ATTR(fg, bg));
    lap_hline(r->x, r->y + r->h - 1, r->w, LAP_BRAILLE(0xC0), LAP_ATTR(on ? LAP_BLACK : HUD_DIM, bg));
    if (divider) lap_vline(r->x, r->y, r->h - 1, LAP_BRAILLE(0x47), LAP_ATTR(HUD_DIM, bg));
}

/* The dash modes, in cyan so they never read as the amber panel tabs: the chosen
   one is marked > NAV < with a lit underline rather than filled. */
void ui_mode_key(const ui_rect *r, const char *s, bool on, bool divider) {
    char t[24];
    snprintf(t, sizeof t, on ? "> %s <" : "%s", s);
    if ((int)strlen(t) > r->w) snprintf(t, sizeof t, "%s", s);
    lap_fill(r->x, r->y, r->w, r->h, ' ', LAP_ATTR(HUD_COOL, LAP_BLACK));
    lap_put_str(r->x + (r->w - (int)strlen(t)) / 2, r->y + (r->h - 1) / 2, t, LAP_ATTR(on ? HUD_COOL : LAP_CYAN, LAP_BLACK));
    lap_hline(r->x, r->y + r->h - 1, r->w, LAP_BRAILLE(on ? 0xC0 : 0x40), LAP_ATTR(on ? HUD_COOL : HUD_DIM, LAP_BLACK));
    if (divider) lap_vline(r->x, r->y, r->h - 1, LAP_BRAILLE(0x47), LAP_ATTR(HUD_DIM, LAP_BLACK));
}

static void tabs(const game_view *v) {
    static const char *names[4] = {"STATUS", "MARKET", "GALAXY", "LOG"};
    for (int i = 0; i < 4; i++)
        ui_tab_key(&ui.tab_keys[i], names[i], v->panel == (game_panel)(PANEL_STATUS + i), i != 1 || v->player.docked,
                   i && ui.tab_keys[i].y == ui.tab_keys[i - 1].y);
}


static void keys(const game_view *v) {
    const game_player *p = &v->player;
    bool fire_ok = !p->docked && v->jump == JUMP_IDLE;
    ui_key_box(&ui.keys[UI_K_FIRE], "FIRE", ui.fire_latch ? "LOCKED ON" : "tap/hold", HUD_WARN, LAP_BLACK,
               p->firing && fire_ok);
    /* the second group: whatever is fitted there, its ammo if it has any */
    int k2 = -1, ammo = -1;
    for (int m = 0; m < GAME_MOUNTS; m++)
        if (p->weapons[m].kind < WEAPON_COUNT && p->weapons[m].group == 1) {
            k2 = p->weapons[m].kind;
            if (p->weapons[m].kind == WEAPON_RATTLE) ammo = p->weapons[m].ammo;
            break;
        }
    if (ammo >= 0) snprintf(line, sizeof line, "%s %d", ui_weapon_name(k2), ammo);
    else if (k2 >= 0) snprintf(line, sizeof line, "%s", ui_weapon_name(k2));
    else snprintf(line, sizeof line, "MSL x%d", p->missiles);
    ui_key_box(&ui.keys[UI_K_FIRE2], "ALT", line, k2 >= 0 || p->missiles ? HUD_WARN : HUD_SUB, LAP_BLACK,
               p->firing2 && !p->docked);
    bool ecm = p->equipment & (1u << EQ_ECM);
    (void)ecm;
    char cs[16] = "";
    if (ui_boost_context()) {     /* in a fight the drive key boosts */
        if (p->boost_cooldown > 0) snprintf(cs, sizeof cs, "%.0fs", p->boost_cooldown);
        ui_key_box(&ui.keys[UI_K_CRUISE], "BOOST", cs, p->boost_cooldown > 0 ? HUD_SUB : HUD_OK, LAP_BLACK, p->boost_left > 0);
    } else {
        if (p->cruising) snprintf(cs, sizeof cs, "x%.0f", p->cruise_factor);
        else if (p->mass_locked) snprintf(cs, sizeof cs, "locked");
        ui_key_box(&ui.keys[UI_K_CRUISE], "CRUISE", cs, p->mass_locked && !p->cruising ? HUD_SUB : HUD_COOL, LAP_BLACK,
                   p->cruising);
    }
    ui_key_box(&ui.keys[UI_K_TARGET], "TARGET", "hold:foe", HUD, LAP_BLACK, false);
    bool act = v->action != ACT_NONE;
    ui_key_box(&ui.keys[UI_K_ACTION], v->action_label ? v->action_label : "---", act ? "" : "",
               act ? HUD_HI : HUD_SUB, LAP_BLACK, act && ((int)(ui.t * 2) & 1) == 0 && v->action != ACT_HAIL);
}

void ui_dash_draw(void) {
    const game_view *v = game_state();
    tabs(v);
    ui_modebar_draw();
    if (ui.panel_full) { keys(v); return; }     /* the panel has the instruments' room */
    int iw = ui.inst.w, ih = ui.inst.h;
    if (ui.mode != UI_MODE_FLIGHT && lap_canvas_init(&ui.dash_c, iw, ih)) {
        /* the middle is words; only the throttle keeps its dots */
        cv = &ui.dash_c;
        ox = ui.left.x;
        oy = ui.left.y;
        lc_clear(cv, LAP_DOT_CLEAR);
        labels_on = false;
        throttle(v);
        lap_canvas_show(cv, ox, oy);
        labels_on = true;
        throttle(v);
        if (ui.mode == UI_MODE_NAV) ui_nav_draw(&ui.middle);
        else if (ui.mode == UI_MODE_SYS) ui_sys_draw(&ui.middle);
        else ui_comms_draw(&ui.middle);
    } else if (lap_canvas_init(&ui.dash_c, iw, ih)) {
        cv = &ui.dash_c;
        ox = ui.left.x;
        oy = ui.left.y;
        /* dots first with the words held back, then the words over their cells */
        lc_clear(cv, LAP_DOT_CLEAR);
        labels_on = false;
        scope(v);
        if (!ui.nlamps) { gauges_left(v); gauges_right(v); }   /* the wide console has lamps instead */
        throttle(v);
        lap_canvas_show(cv, ox, oy);
        labels_on = true;
        if (!ui.nlamps) { gauges_left(v); gauges_right(v); }
        throttle(v);
    }
    ui_console_draw();
    keys(v);
}

/* A hostile within 2.5 km and no cruise under way: the drive key is BOOST. */
bool ui_boost_context(void) {
    const game_view *v = game_state();
    if (v->player.cruising || v->player.docked) return false;
    for (int i = 0; i < GAME_CONTACTS; i++)
        if (v->contacts[i].active && v->contacts[i].hostile && v->contacts[i].range < 2500) return true;
    return false;
}

