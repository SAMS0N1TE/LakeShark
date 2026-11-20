/* The four panels over the view, one tap each from the dash tabs, one tap back:
 * STATUS (tabs: ship, cargo, missions, commander, settings), MARKET (goods and
 * outfitting, docked), GALAXY (the chart; tap a system, plot the jump), LOG (the
 * commander journal). */
#include "internal.h"
#include "art.h"
#include "ui.h"
#include "ui_journal.h"

static char line[96];
static const char *const gov_names[8] = {"Anarchy", "Feudal", "Multi-gov", "Dictatorship",
                                         "Communist", "Confederacy", "Democracy", "Corporate"};
static const char *const eco_names[8] = {"Rich industrial", "Industrial", "Poor industrial",
                                         "Mainly industrial", "Mainly agricultural", "Rich agricultural",
                                         "Agricultural", "Poor agricultural"};
static ui_rect pkeys[6];
static game_yard_offer yard[12];
static int nyard, yard_first = 99, yard_scroll, yard_list_x;
static int arms_kind = -1;      /* the weapon type being looked at in ARMS */
/* MARKET's sub-tabs sit either side of a gap in the middle of the top rows, where the
   T-Display-P4's camera hole is: two to the left, three to the right. */
static ui_rect market_tab_rect(int i) {
    int gap = 8, half = (ui.view.w - gap) / 2;
    if (i < 2) {
        int x = ui.view.x + half * i / 2;
        return (ui_rect){x, ui.view.y, ui.view.x + half * (i + 1) / 2 - x, 3};
    }
    int r0 = ui.view.x + half + gap, rw = ui.view.x + ui.view.w - r0;
    int x = r0 + rw * (i - 2) / 3;
    return (ui_rect){x, ui.view.y, r0 + rw * (i - 1) / 3 - x, 3};
}
static int p_mounts(void) { int m = game_state()->player.mounts; return m ? (m > GAME_MOUNTS ? GAME_MOUNTS : m) : 2; }
static int npkeys;
static int market_stride, arms_stride, board_stride;
static int market_row_stride(int count, int first, int reserve, int min, int max) {
    int h = (ui.view.h - first - reserve) / count;
    return h < min ? min : h > max ? max : h;
}

static void put(int x, int y, const char *s, uint8_t ink) {
    if (x < 0 || y < 0 || x >= ui.view.w || y >= ui.view.h) return;
    int n = ui.view.w - x;
    if ((int)strlen(s) <= n) lap_put_str(ui.view.x + x, ui.view.y + y, s, LAP_ATTR(ink, 0));
    else {
        char clipped[192];
        if (n >= (int)sizeof clipped) n = sizeof clipped - 1;
        memcpy(clipped, s, n); clipped[n] = 0;
        lap_put_str(ui.view.x + x, ui.view.y + y, clipped, LAP_ATTR(ink, 0));
    }
}
/* A heading and its rule, the rule stopping at column `end` of the view. */
static void heading_to(int y, const char *s, int end) {
    put(1, y, s, HUD_HI);
    int n = (int)strlen(s);
    if (end - n - 3 > 0)
        lap_hline(ui.view.x + n + 2, ui.view.y + y, end - n - 3, LAP_BRAILLE(0x24), LAP_ATTR(HUD_DIM, 0));
}
static void heading(int y, const char *s) { heading_to(y, s, ui.view.w); }
/* Keys along the panel's foot, three rows high. */
static void foot_keys(int n, const char *const *labels, const bool *lit, const bool *usable) {
    npkeys = n;
    int y = ui.view.y + ui.view.h - 4, x = ui.view.x;
    for (int i = 0; i < n; i++) {
        int w = ui.view.x + ui.view.w * (i + 1) / n - x;
        pkeys[i] = (ui_rect){x, y, w, 4};
        ui_toggle_box(&pkeys[i], labels[i], "", usable[i] ? HUD : HUD_SUB, lit[i]);
        x += w;
    }
}
static int foot_hit(int col, int row) {
    for (int i = 0; i < npkeys; i++)
        if (ui_in(&pkeys[i], col, row)) return i;
    return -1;
}

static const char *condition(const game_view *v, uint8_t *ink) {
    if (v->player.docked) { *ink = HUD_OK; return "DOCKED"; }
    float near = 1e9f;
    for (int i = 0; i < GAME_CONTACTS; i++)
        if (v->contacts[i].active && v->contacts[i].hostile && v->contacts[i].range < near)
            near = v->contacts[i].range;
    if (near < 700) { *ink = HUD_WARN; return "RED"; }
    if (near < 2000 || v->player.hull < game_player_ship()->hull * .5f) { *ink = HUD_HI; return "YELLOW"; }
    *ink = HUD_OK;
    return "GREEN";
}

/* ---- STATUS ---------------------------------------------------------- */
/* Five tabs along the top, each one job, the way Elite's cockpit panels work:
   SHIP (the hull turning, its integrity, modules and fit), CARGO (the hold's
   ledger and the consumables), MISSIONS (contracts taken), COMMANDER (credits,
   ranks, legal) and SETTINGS (the toggles, framed and grouped). The strip uses
   MARKET's geometry, round the camera hole. Left / right or TAB change tab. */
enum { ST_SHIP, ST_CARGO, ST_MISSIONS, ST_CMDR, ST_SETTINGS, ST_TABS };
enum { S_LADDER, S_TAPES, S_NAV, S_JUMP, S_TBOX, S_AIM, S_ZOOM, S_TILT, S_STEER,
       S_TRAFFIC, S_ENC, S_MESH, S_SOUND, S_VOICE, S_COUNT };
static int status_tab;
static int set_focus = -1;              /* SETTINGS: the box the arrows have reached */
static ui_rect set_rect[S_COUNT];
static const char *const legal_names[3] = {"Clean", "Offender", "Fugitive"};

void ui_status_tab(int tab) { status_tab = tab >= 0 && tab < ST_TABS ? tab : 0; set_focus = -1; }

/* A thin dotted frame in braille, view-relative. */
static void st_frame(int x, int y, int w, int h, uint8_t ink) {
    if (w < 2 || h < 2) return;
    int X = ui.view.x + x, Y = ui.view.y + y;
    uint8_t a = LAP_ATTR(ink, LAP_BLACK);
    lap_hline(X + 1, Y, w - 2, LAP_BRAILLE(0x09), a);
    lap_hline(X + 1, Y + h - 1, w - 2, LAP_BRAILLE(0xC0), a);
    lap_vline(X, Y + 1, h - 2, LAP_BRAILLE(0x47), a);
    lap_vline(X + w - 1, Y + 1, h - 2, LAP_BRAILLE(0xB8), a);
    lap_put_cell(X, Y, LAP_BRAILLE(0x4F), a);
    lap_put_cell(X + w - 1, Y, LAP_BRAILLE(0xB9), a);
    lap_put_cell(X, Y + h - 1, LAP_BRAILLE(0xC7), a);
    lap_put_cell(X + w - 1, Y + h - 1, LAP_BRAILLE(0xF8), a);
}
/* A meter in braille: lit columns in `ink` over a dim dotted track, half-cell steps. */
static void st_bar(int x, int y, int w, float f, uint8_t ink) {
    if (w < 1 || y < 0 || y >= ui.view.h) return;
    int halves = (int)(cg_clamp(f, 0, 1) * w * 2 + .5f);
    for (int i = 0; i < w && x + i < ui.view.w; i++) {
        int h = halves - i * 2;
        int16_t c = h >= 2 ? LAP_BRAILLE(0xFF) : h == 1 ? LAP_BRAILLE(0xC7) : LAP_BRAILLE(0xC0);
        lap_put_cell(ui.view.x + x + i, ui.view.y + y, c, LAP_ATTR(h > 0 ? ink : HUD_DIM, LAP_BLACK));
    }
}
/* NAME  ⣿⣿⣿⣿⣀⣀  78%  across w cells; the name column is `nw` wide. */
static void st_meter(int x, int y, int w, int nw, const char *name, float f, uint8_t ink, bool warn) {
    put(x, y, name, HUD);
    char pct[8];
    snprintf(pct, sizeof pct, "%3.0f%%", cg_clamp(f, 0, 1) * 100);
    st_bar(x + nw, y, w - nw - 6, f, ink);
    put(x + w - 4, y, pct, warn ? HUD_WARN : HUD_HI);
}
/* A small coloured pill, black words on `ink`. */
static void st_pill(int x, int y, const char *s, uint8_t ink) {
    if (y < 0 || y >= ui.view.h || x < 0) return;
    char t[32];
    snprintf(t, sizeof t, " %s ", s);
    if (x + (int)strlen(t) > ui.view.w) return;
    lap_put_str(ui.view.x + x, ui.view.y + y, t, LAP_ATTR(LAP_BLACK, ink));
}
/* A section label and its dotted rule from column x across w. */
static void st_label(int x, int y, int w, const char *s) {
    put(x, y, s, HUD_HI);
    int n = (int)strlen(s);
    if (w - n - 1 > 0 && y >= 0 && y < ui.view.h)
        lap_hline(ui.view.x + x + n + 1, ui.view.y + y, w - n - 1, LAP_BRAILLE(0x24), LAP_ATTR(HUD_DIM, 0));
}

static ui_rect st_tab_rect(int i) { return market_tab_rect(i); }

static void st_tabs(void) {
    static const char *const big[ST_TABS] = {"SHIP", "CARGO", "MISSIONS", "COMMANDER", "SETTINGS"};
    static const char *const small[ST_TABS] = {"SHIP", "CARGO", "JOBS", "CMDR", "SET"};
    for (int i = 0; i < ST_TABS; i++) {
        ui_rect r = st_tab_rect(i);
        const char *s = (int)strlen(big[i]) <= r.w - 2 ? big[i] : small[i];
        ui_tab_key(&r, s, status_tab == i, true, i != 0 && i != 2);
    }
}

/* The player's hull, turning, scaled to fill the box: the shipyard's card renderer. */
static void st_hull(int x, int y, int w, int h) {
    st_frame(x, y, w, h, HUD_DIM);
    int cw = w - 2, ch = h - 2;
    if (!ui.scene || !ui.dots || cw < 4 || ch < 3) return;
    render_hull_card(ui.scene, game_player_class(), ui.t, 0);
    const uint8_t *d = l3_dots(ui.scene);
    int dw = 0, dh = 0;
    l3_size(ui.scene, &dw, &dh);
    int bw = cw * 2, bh = ch * 3;
    if (!d || bw * bh > dw * dh) return;
    /* scene dots per card dot: the hull (at most .4 of the scene's height across,
       flat and seen from a little above) fills the card's width */
    float s = fmaxf(.40f * dh / bw, .16f * dh / bh);
    float sx0 = dw * .5f - bw * s * .5f, sy0 = dh * .5f - bh * s * .5f;
    for (int r = 0; r < bh; r++)
        for (int c = 0; c < bw; c++) {
            int sx = (int)(sx0 + c * s), sy = (int)(sy0 + r * s);
            ui.dots[r * bw + c] = sx >= 0 && sy >= 0 && sx < dw && sy < dh ? d[sy * dw + sx] : LAP_DOT_CLEAR;
        }
    lap_dots(ui.view.x + x + 1, ui.view.y + y + 1, cw, ch, ui.dots);
}

static void st_ship(const game_view *v) {
    const game_player *p = &v->player;
    const game_ship_stats *st = game_player_ship();
    const bool wide = ui.view.w >= 90;
    static const char *const modules[GAME_MODULES] = {"POWER", "DRIVES", "WEAPONS", "SHIELDS", "SENSORS", "LIFE", "CARGO"};
    static const char *const mode[3] = {"FIXED", "GIMBAL", "TURRET"};
    uint8_t cink;
    const char *cond = condition(v, &cink);
    /* the picture, the name on its frame, the condition lamp under it */
    int px = 1, pw = wide ? 38 : ui.view.w - 2, ph = 17, py = 4;
    st_hull(px, py, pw, ph);
    char name[40];
    snprintf(name, sizeof name, " %s ", st->name);
    for (char *c = name; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    put(px + 2, py, name, HUD_HI);
    int y = py + ph + 1;
    put(px, y, "CONDITION", HUD);
    st_pill(px + 10, y, cond, cink);
    snprintf(line, sizeof line, "%d seats free", game_passenger_seats());
    put(px + pw - (int)strlen(line), y, line, HUD_SUB);
    /* integrity and the modules: beside the picture on the desk, under it on the board */
    int mx = wide ? 42 : 1, mw = wide ? 36 : ui.view.w - 2;
    int my = wide ? 4 : y + 2;
    st_label(mx, my, mw, "INTEGRITY");
    struct { const char *name; float f; uint8_t ink; } m[5] = {
        {"HULL", p->hull / fmaxf(1, st->hull), HUD},
        {"SHIELD F", p->shield_fore / 100, HUD_COOL},
        {"SHIELD A", p->shield_aft / 100, HUD_COOL},
        {"FUEL", p->fuel / fmaxf(.1f, p->fuel_max), HUD},
        {"HEAT", p->heat_level / 100, p->heat_level > 80 ? HUD_WARN : HUD}};
    int step = wide ? 2 : 1;
    for (int i = 0; i < 5; i++)
        st_meter(mx, my + 2 + i * step, mw, 10, m[i].name, m[i].f, m[i].ink, i == 4 ? m[i].f > .8f : m[i].f < .25f);
    int ry = my + 2 + 5 * step;
    snprintf(line, sizeof line, "fuel %.1f / %.1f ly", p->fuel, p->fuel_max);
    put(mx, ry, line, HUD_SUB);
    int ox = wide ? 81 : 1, ow = wide ? ui.view.w - 82 : ui.view.w - 2;
    int oy = wide ? 4 : ry + 2;
    st_label(ox, oy, ow, "MODULES");
    for (int i = 0; i < GAME_MODULES; i++) {
        float f = cg_clamp(p->modules[i], 0, 1);
        if (wide) st_meter(ox, oy + 2 + i, ow, 9, modules[i], f, f < .5f ? HUD_WARN : HUD_OK, f < .5f);
        else {
            int half = (ui.view.w - 2) / 2, cx = 1 + (i % 2) * half;
            st_meter(cx, oy + 2 + i / 2, half - 1, 9, modules[i], f, f < .5f ? HUD_WARN : HUD_OK, f < .5f);
        }
    }
    /* hardpoints and the optional fit */
    int hy = wide ? 19 : oy + 7;
    int hx = wide ? 42 : 1, hw = wide ? 36 : ui.view.w - 2;
    st_label(hx, hy, hw, "HARDPOINTS");
    for (int i = 0; i < p_mounts(); i++) {
        const game_mount *w = &p->weapons[i];
        int yy = hy + 2 + i;
        snprintf(line, sizeof line, "%d", i + 1); put(hx, yy, line, HUD_SUB);
        if (w->kind >= WEAPON_COUNT) { put(hx + 3, yy, "empty", HUD_SUB); continue; }
        put(hx + 3, yy, ui_weapon_name(w->kind), HUD_HI);
        put(hx + 13, yy, w->group ? "ALT" : "FIRE", HUD_WARN);
        put(hx + 19, yy, mode[w->mode % 3], HUD);
        if (w->kind == WEAPON_RATTLE) { snprintf(line, sizeof line, "%u rds", w->ammo); put(hx + 27, yy, line, HUD_SUB); }
    }
    int fy = wide ? 19 : hy + 3 + p_mounts();
    int fx = wide ? 81 : 1, fw = wide ? ui.view.w - 82 : ui.view.w - 2;
    st_label(fx, fy, fw, "FITTED");
    int n = 0;
    for (int e = 0; e < EQ_COUNT; e++) {
        game_equipment_offer o;
        if (e == EQ_LASER || e == EQ_MISSILE || !game_equipment_info(e, &o) || !o.owned) continue;
        int cols = wide ? 1 : 2, cx = fx + (n % cols) * (fw / cols);
        put(cx, fy + 2 + n / cols, o.name, HUD);
        n++;
    }
    if (!n) put(fx, fy + 2, "No optional modules", HUD_SUB);
    int sy = wide ? y + 2 : fy + 3 + (n + 1) / 2;
    int sx = 1;
    st_label(sx, sy, wide ? pw : ui.view.w - 2, "SPECIFICATION");
    struct { const char *k; char v[16]; } spec[6];
    spec[0].k = "TOP SPEED"; snprintf(spec[0].v, 16, "%.0f", st->speed);
    spec[1].k = "TURN"; snprintf(spec[1].v, 16, "%.1f", st->turn);
    spec[2].k = "HOLD"; snprintf(spec[2].v, 16, "%d t", game_cargo_capacity());
    spec[3].k = "JUMP"; snprintf(spec[3].v, 16, "%.1f ly", p->fuel_max);
    spec[4].k = "HULL"; snprintf(spec[4].v, 16, "%.0f", st->hull);
    spec[5].k = "SHIELD"; snprintf(spec[5].v, 16, "%.0f", st->shield);
    int cols = 2, cw = (wide ? pw : ui.view.w - 2) / cols;
    for (int i = 0; i < 6; i++) {
        int cx = sx + (i % cols) * cw, cy = sy + 2 + i / cols;
        put(cx, cy, spec[i].k, HUD_SUB);
        put(cx + 10, cy, spec[i].v, HUD_HI);
    }
}

static void st_cargo(const game_view *v) {
    const game_player *p = &v->player;
    const bool wide = ui.view.w >= 90;
    int used = game_cargo_used(), cap = game_cargo_capacity();
    put(1, 4, "HOLD", HUD_HI);
    int bw = wide ? 40 : ui.view.w - 20;
    st_bar(7, 4, bw, cap ? (float)used / cap : 0, used >= cap ? HUD_WARN : HUD);
    snprintf(line, sizeof line, "%d / %d t", used, cap);
    put(8 + bw, 4, line, HUD_HI);
    int count = 0;
    for (int i = 0; i < GAME_GOODS; i++) if (p->cargo[i]) count++;
    /* a second column on the desk only once the first has eight lots */
    int columns = wide && count > 8 ? 2 : 1, width = wide ? (ui.view.w - 2) / 2 : ui.view.w - 2;
    int start = 7;
    for (int c = 0; c < columns; c++) {
        int x = 1 + c * width;
        put(x, 6, "GOOD", HUD_SUB);
        put(x + 16, 6, "QTY   PAID  LOCAL      P/L", HUD_SUB);
    }
    int total_paid = 0, total_worth = 0, total_pl = 0, unknown = 0;
    int per = columns == 2 ? (count + 1) / 2 : count;
    if (per < 1) per = 1;
    int k = 0;
    for (int i = 0; i < GAME_GOODS; i++) {
        if (!p->cargo[i]) continue;
        int c = k / per, row = k % per, x = 1 + c * width, y = start + row;
        k++;
        uint8_t ink = v->market[i].illegal ? HUD_WARN : HUD;
        put(x, y, v->market[i].name, ink);
        uint32_t paid = 0;
        int worth = (int)p->cargo[i] * v->market[i].price;
        total_worth += worth;
        if (ui_cargo_cost(i, &paid)) {
            int pl = worth - (int)paid;
            total_paid += (int)paid;
            total_pl += pl;
            snprintf(line, sizeof line, "%3u %6u %6d", p->cargo[i], (unsigned)paid, v->market[i].price);
            put(x + 16, y, line, ink);
            snprintf(line, sizeof line, "%+6d", pl);
            put(x + 37, y, line, pl > 0 ? HUD_OK : pl < 0 ? HUD_WARN : HUD_SUB);
        } else {
            unknown++;
            snprintf(line, sizeof line, "%3u %6s %6d", p->cargo[i], "?", v->market[i].price);
            put(x + 16, y, line, ink);
            put(x + 42, y, "?", HUD_SUB);
        }
    }
    int y = start + (count ? per : 1) + 1;
    if (!count) put(1, start, "Empty hold. Buy goods at a station MARKET.", HUD_SUB);
    else {
        int pl = total_pl;
        put(1, y, "TOTAL", HUD_HI);
        snprintf(line, sizeof line, "paid %d cr", total_paid); put(8, y, line, HUD);
        snprintf(line, sizeof line, "worth here %d cr", total_worth); put(wide ? 24 : 8, wide ? y : y + 1, line, HUD);
        if (unknown) snprintf(line, sizeof line, "P/L %+d, %d lot%s cost unknown", pl, unknown, unknown == 1 ? "" : "s");
        else snprintf(line, sizeof line, "P/L %+d", pl);
        put(wide ? 48 : 8, wide ? y : y + 2, line, pl > 0 ? HUD_OK : pl < 0 ? HUD_WARN : HUD_SUB);
        y += wide ? 1 : 3;
    }
    put(1, y + 1, wide ? "PAID = what the lot cost   LOCAL = price here per t   P/L = selling it all here, less cost"
                       : "PAID total cost  LOCAL price per t", HUD_SUB);
    if (!wide) put(1, y + 2, "P/L = sale here less cost", HUD_SUB);
    /* consumables: four framed tiles, a pip per round */
    int cy = y + (wide ? 3 : 4);
    st_label(1, cy, ui.view.w - 2, "CONSUMABLES");
    struct { const char *name; int n; uint8_t ink; const char *key; } it[4] = {
        {"MISSILES", p->missiles, HUD_WARN, "M"}, {"CHAFF", p->chaff, HUD, "5"},
        {"HEAT SINKS", p->heat_sinks, HUD_COOL, "V"}, {"SHIELD CELLS", p->shield_cells, HUD_COOL, "6"}};
    int tiles = wide ? 4 : 2, tw = (ui.view.w - 2) / tiles;
    for (int i = 0; i < 4; i++) {
        int tx = 1 + (i % tiles) * tw, ty = cy + 2 + (i / tiles) * 6;
        st_frame(tx, ty, tw - 1, 5, it[i].n ? it[i].ink : HUD_DIM);
        put(tx + 2, ty + 1, it[i].name, HUD_HI);
        snprintf(line, sizeof line, "key %s", it[i].key);
        put(tx + tw - 3 - (int)strlen(line), ty + 1, line, HUD_SUB);
        snprintf(line, sizeof line, "x%d", it[i].n);
        put(tx + 2, ty + 3, line, it[i].n ? HUD_HI : HUD_SUB);
        for (int j = 0; j < it[i].n && tx + 7 + j * 2 < tx + tw - 3; j++)
            lap_put_cell(ui.view.x + tx + 7 + j * 2, ui.view.y + ty + 3, LAP_BRAILLE(0x3F), LAP_ATTR(it[i].ink, 0));
    }
}

static void st_missions(const game_view *v) {
    static const char *const kinds[4] = {"DELIVERY", "COURIER", "BOUNTY", "RESCUE"};
    const bool wide = ui.view.w >= 90;
    int taken = 0, offers = 0;
    for (int i = 0; i < 6; i++) {
        const game_mission *m = &v->missions[i];
        if (m->accepted && !m->done && !m->failed) taken++;
        else if (m->title[0] && !m->accepted) offers++;
    }
    snprintf(line, sizeof line, "DAY %u", (unsigned)v->market_day);
    put(1, 4, line, HUD_HI);
    snprintf(line, sizeof line, "%d of 3 contracts taken", taken);
    put(9, 4, line, HUD);
    for (int s = 0; s < 3; s++)       /* the three contract slots */
        for (int c = 0; c < 3; c++)
            lap_put_cell(ui.view.x + ui.view.w - 13 + s * 4 + c, ui.view.y + 4, LAP_BRAILLE(s < taken ? 0xFF : 0xC0),
                         LAP_ATTR(s < taken ? HUD_COOL : HUD_DIM, 0));
    int y = 6, h = wide ? 6 : 7;
    for (int i = 0; i < 6; i++) {
        const game_mission *m = &v->missions[i];
        if (!m->accepted || m->failed || (m->done && !m->ready)) continue;
        if (y + h > ui.view.h - 3) break;
        bool ready = m->ready;
        st_frame(0, y, ui.view.w, h, ready ? HUD_OK : HUD_COOL);
        st_pill(2, y + 1, kinds[m->kind & 3], HUD_COOL);
        put(4 + (int)strlen(kinds[m->kind & 3]) + 1, y + 1, m->title, HUD_HI);
        snprintf(line, sizeof line, "%u cr", (unsigned)m->reward);
        put(ui.view.w - 2 - (int)strlen(line), y + 1, line, HUD_HI);
        bool there = m->destination >= 0 && m->destination < GAME_SYSTEMS;
        float ly = there ? game_distance(&v->systems[v->system], &v->systems[m->destination]) : 0;
        put(2, y + 2, "TO", HUD_SUB);
        if (there) snprintf(line, sizeof line, "%s  %.1f ly", v->systems[m->destination].name, ly);
        else snprintf(line, sizeof line, "local space");
        put(5, y + 2, line, HUD_HI);
        int left = (int)m->deadline - (int)v->market_day;
        snprintf(line, sizeof line, "DUE day %u  (%d day%s)", (unsigned)m->deadline, left < 0 ? 0 : left, left == 1 ? "" : "s");
        if (wide) put(ui.view.w - 2 - (int)strlen(line), y + 2, line, left <= 1 ? HUD_WARN : HUD);
        else put(2, y + 3, line, left <= 1 ? HUD_WARN : HUD);
        int py = y + (wide ? 3 : 4);
        if (ready) {
            put(2, py, "READY", HUD_OK);
            put(9, py, "deliver at the destination station", HUD_SUB);
        } else {
            put(2, py, "PROGRESS", HUD);
            int bw = wide ? 30 : ui.view.w - 20;
            st_bar(12, py, bw, m->amount ? (float)m->progress / m->amount : 0, HUD_COOL);
            snprintf(line, sizeof line, "%u/%u", m->progress, m->amount);
            put(13 + bw, py, line, HUD_HI);
        }
        if (m->brief[0]) put(2, py + 1, m->brief, HUD_SUB);
        y += h + 1;
    }
    if (!taken) {
        static const char *const art[4] = {"  .-------------.", "  |   CONTRACTS  |", "  |   - none -   |", "  '-------------'"};
        for (int i = 0; i < 4; i++) put(1, 7 + i, art[i], HUD_SUB);
        put(1, 13, "No active contract.", HUD);
        y = 15;
    }
    if (offers && y < ui.view.h - 6) {
        st_label(1, y, ui.view.w - 2, "ON THIS STATION'S BOARD");
        y += 2;
        for (int i = 0; i < 6 && y < ui.view.h - 4; i++) {
            const game_mission *m = &v->missions[i];
            if (!m->title[0] || m->accepted) continue;
            put(1, y, kinds[m->kind & 3], HUD_SUB);
            put(11, y, m->title, HUD);
            snprintf(line, sizeof line, "%u cr", (unsigned)m->reward);
            put(ui.view.w - 1 - (int)strlen(line), y, line, HUD);
            if (wide && m->destination >= 0 && m->destination < GAME_SYSTEMS) put(38, y, v->systems[m->destination].name, HUD_SUB);
            y++;
        }
        put(1, y, v->player.docked ? "Take one in MARKET, BOARD tab." : "Dock to take one.", HUD_SUB);
        y += 2;
    }
    if (v->event_text[0] && y < ui.view.h - 3) {
        st_label(1, y, ui.view.w - 2, "NEWS");
        put(1, y + 1, v->event_text, HUD_COOL);
    }
}

static void st_commander(const game_view *v) {
    const game_player *p = &v->player;
    const bool wide = ui.view.w >= 90;
    int rank = game_combat_rank(p->combat_score);
    /* the rank ladder: passed in amber, yours lit, the rest grey */
    int lx = 1, lw = wide ? 30 : ui.view.w - 2, ly = 4;
    st_frame(lx - 1, ly, lw + 1, 12, HUD_DIM);
    put(lx + 1, ly, " COMBAT RANK ", HUD_HI);
    for (int r = 0; r < 9; r++) {
        int yy = ly + 10 - r;
        const char *name = game_rank(r);
        /* a rail up the left: climbed in amber, the rest dotted */
        lap_put_cell(ui.view.x + lx + 1, ui.view.y + yy, LAP_BRAILLE(r == rank ? 0xFF : r < rank ? 0x47 : 0x02),
                     LAP_ATTR(r == rank ? HUD_HI : r < rank ? HUD : HUD_SUB, 0));
        if (r == rank) {
            st_pill(lx + 3, yy, name, HUD);
            put(lx + 6 + (int)strlen(name), yy, "< YOU", HUD_HI);
        } else put(lx + 4, yy, name, r < rank ? HUD : HUD_SUB);
    }
    int x = wide ? 34 : 1, w = wide ? ui.view.w - 35 : ui.view.w - 2;
    int y = wide ? 4 : ly + 14;
    st_label(x, y, w, "COMMANDER");
    put(x, y + 2, "CREDITS", HUD);
    snprintf(line, sizeof line, "%u cr", (unsigned)p->credits);
    put(x + 14, y + 2, line, HUD_HI);
    put(x, y + 4, "RANK", HUD);
    put(x + 14, y + 4, game_rank(rank), HUD_HI);
    uint32_t lo = cg->systems.rank[rank], hi = rank < 8 ? cg->systems.rank[rank + 1] : lo;
    float adv = hi > lo ? cg_clamp((float)(p->combat_score - lo) / (hi - lo), 0, 1) : 1;
    put(x, y + 5, hi > lo ? "NEXT RANK" : "HIGHEST", HUD_SUB);
    int bw = w - 30 > 24 ? 24 : w - 30;
    if (bw < 8) bw = 8;
    st_bar(x + 14, y + 5, bw, adv, HUD_HI);
    if (hi > lo) snprintf(line, sizeof line, "%u / %u", (unsigned)p->combat_score, (unsigned)hi);
    else snprintf(line, sizeof line, "%u", (unsigned)p->combat_score);
    put(x + 15 + bw, y + 5, line, HUD_SUB);
    put(x, y + 7, "LEGAL", HUD);
    int lg = p->legal < 2 ? p->legal : 2;
    st_pill(x + 13, y + 7, legal_names[lg], lg ? HUD_WARN : HUD_OK);
    put(x, y + 9, "KILLS", HUD);
    snprintf(line, sizeof line, "%u", (unsigned)p->kills); put(x + 14, y + 9, line, HUD_HI);
    put(x + 22, y + 9, "SCORE", HUD);
    snprintf(line, sizeof line, "%u", (unsigned)p->combat_score); put(x + 30, y + 9, line, HUD_HI);
    put(x, y + 11, "EXPLORER", HUD);
    snprintf(line, sizeof line, "%s / %u found", game_explorer_rank(v->discoveries.rank), (unsigned)v->discoveries.count);
    put(x + 14, y + 11, line, HUD_HI);
    if (p->cheats_used) put(x, y + 12, "SAVE MARKED: CHEATS USED", HUD_WARN);
    int ly2 = wide ? 19 : y + 14;
    st_label(1, ly2, ui.view.w - 2, "LOCATION");
    const game_system *s = &v->systems[v->system];
    put(1, ly2 + 2, "SYSTEM", HUD);
    put(15, ly2 + 2, s->name, HUD_HI);
    put(1, ly2 + 3, "ECONOMY", HUD);
    put(15, ly2 + 3, eco_names[s->economy & 7], HUD);
    put(1, ly2 + 4, "GOVERNMENT", HUD);
    put(15, ly2 + 4, gov_names[s->government & 7], HUD);
    snprintf(line, sizeof line, "%u", s->tech); put(1, ly2 + 5, "TECH", HUD); put(15, ly2 + 5, line, HUD);
    put(1, ly2 + 6, "SHIP", HUD);
    put(15, ly2 + 6, game_player_ship()->name, HUD_HI);
    if (v->discoveries.count && ly2 + 8 < ui.view.h) {
        int dx = wide ? 60 : 1, dy = wide ? ly2 + 2 : ly2 + 8;
        put(dx, dy, "LATEST FINDS", HUD_SUB);
        int n = v->discoveries.count < 16 ? (int)v->discoveries.count : 16;
        for (int i = 0; i < 4 && i < n && dy + 1 + i < ui.view.h; i++)
            put(dx, dy + 1 + i, v->discoveries.names[(n - 1 - i) & 15], HUD_COOL);
    }
}

/* ---- STATUS: SETTINGS --- */
static void st_setting_do(int i) {
    const game_view *v = game_state();
    switch (i) {
    case S_LADDER: case S_TAPES: case S_NAV: case S_JUMP: case S_TBOX:
        ui.hud_off ^= (uint8_t)(1 << (i - S_LADDER)); ui_save_settings(); lap_sound(LAP_SOUND_KEY); break;
    case S_AIM: ui.aim = (ui.aim + 1) % 3; ui_apply_aim(); ui_save_settings(); break;
    case S_ZOOM: ui.autozoom = !ui.autozoom; ui_save_settings(); break;
    case S_TILT: {
        bool on = !v->player.motion_steer;
        lap_motion_t m;
        if (on && lap_motion(&m)) { ui.level_pitch = m.pitch; ui.level_roll = m.roll; }
        game_motion_steer(on);
        break;
    }
    case S_STEER: ui.steer = (ui.steer + 1) % 3; ui_save_settings(); lap_sound(LAP_SOUND_KEY); break;
    case S_TRAFFIC: ui.traffic = (ui.traffic + 1) % 3; game_set_traffic(ui.traffic); ui_save_settings(); break;
    case S_ENC: ui.encounters = (ui.encounters + 1) % 3; game_set_encounters(ui.encounters); ui_save_settings(); break;
    case S_MESH: ui.mesh_slot = (ui.mesh_slot + 1) % 4; ui_save_settings(); break;
    case S_SOUND: ui_options_open(1); break;
    case S_VOICE:
        ui.voice = !ui.voice;
        if (!ui.voice) lap_say_stop(); else lap_say("Voice on");
        ui_save_settings();
        break;
    default: break;
    }
}
/* A setting as a framed key: ON lit green, a choice lit amber, OFF dim. */
static void st_toggle(const ui_rect *r, const char *label, const char *hint, const char *value, int state, bool focus) {
    uint8_t bg = focus ? HUD_DIM : LAP_BLACK;
    lap_fill(r->x, r->y, r->w, r->h, ' ', LAP_ATTR(HUD, bg));
    st_frame(r->x - ui.view.x, r->y - ui.view.y, r->w, r->h, focus ? HUD_HI : state == 1 ? HUD_OK : state == 2 ? HUD : HUD_DIM);
    if (focus) {    /* the frame's black ground would cut the focus fill; repaint its middle */
        lap_fill(r->x + 1, r->y + 1, r->w - 2, r->h - 2, ' ', LAP_ATTR(HUD, bg));
    }
    int cx = r->x + r->w / 2;
    lap_put_str(cx - (int)strlen(label) / 2, r->y + 1, label, LAP_ATTR(state ? HUD_HI : HUD, bg));
    if (r->h >= 5 && hint) lap_put_str(cx - (int)strlen(hint) / 2, r->y + 2, hint, LAP_ATTR(HUD_SUB, bg));
    char t[24];
    snprintf(t, sizeof t, " %s ", value);
    uint8_t a = state == 1 ? LAP_ATTR(LAP_BLACK, HUD_OK) : state == 2 ? LAP_ATTR(LAP_BLACK, HUD) : LAP_ATTR(HUD_SUB, bg);
    lap_put_str(cx - (int)strlen(t) / 2, r->y + r->h - 2, t, a);
}

static void st_settings(const game_view *v) {
    const game_player *p = &v->player;
    const bool wide = ui.view.w >= 90;
    static const char *const group[4] = {"HUD", "FLIGHT", "WORLD", "SOUND"};
    static const int first[5] = {S_LADDER, S_AIM, S_TRAFFIC, S_SOUND, S_COUNT};
    static const char *const label[S_COUNT] = {"LADDER", "TAPES", "NAV MARK", "JUMP MARK", "TARGET BOX",
        "AIM", "ZOOM", "TILT", "STEER", "SHIPS FROM", "ENCOUNTERS", "MESH CH", "SOUND SET", "VOICE"};
    static const char *const hint[S_COUNT] = {"pitch ladder", "speed / alt", "nav marker", "route marker",
        "target bracket", "gun assist", "far targets", "tilt to steer", "stick speed", "traffic source",
        "how busy", "mesh channel", "tones, music", "ship computer"};
    static const char *const aim[3] = {"OFF", "NORMAL", "STRONG"};
    static const char *const steer[3] = {"GENTLE", "NORMAL", "QUICK"};
    static const char *const traffic[3] = {"LIVE+SIM", "SIM ONLY", "LIVE ONLY"};
    static const char *const busy[3] = {"CALM", "NORMAL", "BUSY"};
    char mesh[8], vol[12];
    snprintf(mesh, sizeof mesh, "CH %d", ui.mesh_slot);
    snprintf(vol, sizeof vol, "VOL %d%%", ui.volume * 20);
    const char *value[S_COUNT];
    int state[S_COUNT];
    for (int i = S_LADDER; i <= S_TBOX; i++) {
        bool on = !(ui.hud_off & (1 << (i - S_LADDER)));
        value[i] = on ? "ON" : "OFF"; state[i] = on;
    }
    value[S_AIM] = aim[ui.aim % 3]; state[S_AIM] = ui.aim > 0;
    value[S_ZOOM] = ui.autozoom ? "AUTO" : "OFF"; state[S_ZOOM] = ui.autozoom;
    value[S_TILT] = p->motion_steer ? "ON" : "OFF"; state[S_TILT] = p->motion_steer;
    value[S_STEER] = steer[ui.steer % 3]; state[S_STEER] = 2;
    value[S_TRAFFIC] = traffic[ui.traffic % 3]; state[S_TRAFFIC] = 2;
    value[S_ENC] = busy[ui.encounters % 3]; state[S_ENC] = 2;
    value[S_MESH] = mesh; state[S_MESH] = 2;
    value[S_SOUND] = vol; state[S_SOUND] = ui.sound;
    value[S_VOICE] = ui.voice ? "ON" : "OFF"; state[S_VOICE] = ui.voice;
    int cols = wide ? 5 : 3, bh = wide ? 5 : 6, gap = wide ? 0 : 1, gw = ui.view.w - 2;
    int y = 4;
    for (int g = 0; g < 4; g++) {
        st_label(1, y, gw, group[g]);
        int n = first[g + 1] - first[g], rows = (n + cols - 1) / cols;
        for (int j = 0; j < n; j++) {
            int i = first[g] + j, c = j % cols, r = j / cols;
            int x = 1 + gw * c / cols, w = 1 + gw * (c + 1) / cols - x - 1;
            set_rect[i] = (ui_rect){ui.view.x + x, ui.view.y + y + 1 + r * (bh + gap), w, bh};
            st_toggle(&set_rect[i], label[i], hint[i], value[i], state[i], set_focus == i);
        }
        y += 1 + rows * (bh + gap) - gap + 1;
    }
    if (y < ui.view.h - 1)
        put(1, ui.view.h - 2, wide ? "Tap a key to change it.   Keys: LEFT / RIGHT tabs, UP / DOWN pick, ENTER change, BACK close."
                                   : "Tap to change. UP/DOWN pick, ENTER", HUD_SUB);
}

static void status(const game_view *v) {
    st_tabs();
    switch (status_tab) {
    case ST_SHIP: st_ship(v); break;
    case ST_CARGO: st_cargo(v); break;
    case ST_MISSIONS: st_missions(v); break;
    case ST_CMDR: st_commander(v); break;
    default: st_settings(v); break;
    }
}

static bool status_tap(int col, int row) {
    for (int i = 0; i < ST_TABS; i++) {
        ui_rect r = st_tab_rect(i);
        if (ui_in(&r, col, row)) { ui_status_tab(i); lap_sound(LAP_SOUND_KEY); return true; }
    }
    if (status_tab == ST_SETTINGS)
        for (int i = 0; i < S_COUNT; i++)
            if (set_rect[i].w && ui_in(&set_rect[i], col, row)) { set_focus = -1; st_setting_do(i); return true; }
    return true;
}

bool ui_status_key(lap_key_t k, char ch) {
    if (game_state()->panel != PANEL_STATUS) return false;
    if (k == LAP_KEY_LEFT) { ui_status_tab((status_tab + ST_TABS - 1) % ST_TABS); return true; }
    if (k == LAP_KEY_RIGHT || k == LAP_KEY_TAB) { ui_status_tab((status_tab + 1) % ST_TABS); return true; }
    if (status_tab != ST_SETTINGS) return false;
    if (k == LAP_KEY_DOWN) { set_focus = (set_focus + 1) % S_COUNT; return true; }
    if (k == LAP_KEY_UP) { set_focus = set_focus <= 0 ? S_COUNT - 1 : set_focus - 1; return true; }
    if ((k == LAP_KEY_ENTER || (k == LAP_KEY_CHAR && ch == ' ')) && set_focus >= 0) { st_setting_do(set_focus); return true; }
    return false;
}

/* ---- MARKET ---------------------------------------------------------- */
static void market(const game_view *v) {
    const game_player *p = &v->player;
    /* GOODS | OUTFIT */
    static const char *const subtabs[5] = {"GOODS", "OUTFIT", "ARMS", "BOARD", "SHIPS"};
    for (int i = 0; i < 5; i++) {
        ui_rect tr = market_tab_rect(i);
        int x = tr.x, w = tr.w;
        bool on = ui.market_tab == i;
        lap_fill(x, ui.view.y, w, 3, ' ', LAP_ATTR(on ? LAP_BLACK : HUD, on ? HUD : LAP_BLACK));
        const char *s = subtabs[i];
        lap_put_str(x + (w - (int)strlen(s)) / 2, ui.view.y + 1, s,
                    LAP_ATTR(on ? LAP_BLACK : HUD, on ? HUD : LAP_BLACK));
    }
    if (!p->docked) {
        put(2, 6, "Dock at a station to trade.", HUD);
        put(2, 8, "Prices here today:", HUD_SUB);
    }
    if (ui.market_tab == 0) {
        int columns = ui.view.w >= 90 ? 2 : 1;
        int width = ui.view.w / columns;
        int first = p->docked ? 5 : 10;
        int per = (GAME_GOODS + columns - 1) / columns;
        market_stride = market_row_stride(per, first, 7, 1, 4);
        for (int c = 0; c < columns; c++) {
            put(1 + c * width, 4, "GOOD", HUD_SUB);
            put(c * width + width - 27, 4, "PRICE   STOCK   HOLD", HUD_SUB);
        }
        for (int i = 0; i < GAME_GOODS; i++) {
            int col = i / per, row = i % per;
            int x = col * width, y = first + row * market_stride;
            if (y >= ui.view.h - 7) break;
            const game_market_item *m = &v->market[i];
            bool sel = ui.market_pick == i;
            uint8_t bg = sel ? HUD_DIM : LAP_BLACK;
            uint8_t fg = m->illegal ? HUD_WARN : sel ? HUD_HI : HUD;
            lap_fill(ui.view.x + x, ui.view.y + y, width, market_stride, ' ', LAP_ATTR(fg, bg));
            lap_put_str(ui.view.x + x + 1, ui.view.y + y, m->name, LAP_ATTR(fg, bg));
            snprintf(line, sizeof line, "%5d %7d %6d", m->price, m->stock, p->cargo[i]);
            lap_put_str(ui.view.x + x + width - 27, ui.view.y + y, line, LAP_ATTR(fg, bg));
            if (market_stride > 2) {
                uint32_t paid = 0;
                if (p->cargo[i] && ui_cargo_cost(i, &paid)) snprintf(line, sizeof line, "Hold %u cr / P&L %+d", p->cargo[i] * m->price, (int)(p->cargo[i] * m->price) - (int)paid);
                else snprintf(line, sizeof line, "%s", m->illegal ? "Restricted cargo" : game_good_note(i));
                lap_put_str(ui.view.x + x + 2, ui.view.y + y + 1, line, LAP_ATTR(HUD_SUB, bg));
            }
        }
        const char *note = game_good_note(ui.market_pick);
        if (note && note[0]) put(1, ui.view.h - 6, note, HUD_SUB);
        const game_market_item *m = &v->market[ui.market_pick];
        bool can_buy = p->docked && m->stock > 0 && p->credits >= (unsigned)m->price &&
                       game_cargo_used() < game_cargo_capacity();
        bool can_sell = p->docked && p->cargo[ui.market_pick] > 0;
        const char *labels[4] = {"BUY 1", "BUY 10", "SELL 1", "SELL ALL"};
        bool lit[4] = {0}, use[4] = {can_buy, can_buy, can_sell, can_sell};
        foot_keys(4, labels, lit, use);
    } else if (ui.market_tab == 2) {
        /* one row per mount: what is fitted, its group, its ammo; below, the choice and its price */
        int mounts = p->mounts ? p->mounts : 2;
        if (ui.market_pick >= mounts) ui.market_pick = 0;
        arms_stride = market_row_stride(mounts, 6, 22, 2, 8);
        put(1, 4, "MOUNT   WEAPON      GROUP   AMMO   MODE", HUD_SUB);
        int y = 6;
        for (int m = 0; m < mounts && m < GAME_MOUNTS; m++) {
            const game_mount *w = &p->weapons[m];
            bool sel = ui.market_pick == m;
            uint8_t bg = sel ? HUD_DIM : LAP_BLACK, fg = sel ? HUD_HI : HUD;
            lap_fill(ui.view.x, ui.view.y + y, ui.view.w, arms_stride, ' ', LAP_ATTR(fg, LAP_BLACK));
            lap_fill(ui.view.x, ui.view.y + y, ui.view.w, 2, ' ', LAP_ATTR(fg, bg));
            char ammo[12] = "-";
            if (w->kind == WEAPON_RATTLE) snprintf(ammo, sizeof ammo, "%d", w->ammo);
            static const char *const mode[3] = {"FIXED", "GIMBAL", "TURRET"};
            snprintf(line, sizeof line, "  %d     %-10s  %-6s  %-5s  %s", m + 1, ui_weapon_name(w->kind),
                     w->kind < WEAPON_COUNT ? (w->group ? "ALT" : "FIRE") : "-", ammo,
                     w->kind < WEAPON_COUNT ? mode[w->mode % 3] : "-");
            lap_put_str(ui.view.x + 1, ui.view.y + y, line, LAP_ATTR(fg, bg));
            if (arms_stride > 2 && w->kind < WEAPON_COUNT) {
                snprintf(line, sizeof line, "Heat %.0f%% / charge %.0f%% / %s", w->heat * 100, w->charge * 100, w->group ? "secondary trigger" : "primary trigger");
                lap_put_str(ui.view.x + 3, ui.view.y + y + 1, line, LAP_ATTR(HUD_SUB, bg));
            }
            y += arms_stride;
        }
        y++;
        if (arms_kind < 0) arms_kind = p->weapons[ui.market_pick].kind < WEAPON_COUNT ? p->weapons[ui.market_pick].kind : 0;
        game_equipment_offer o;
        bool known = game_weapon_info((game_weapon_kind)arms_kind, &o);
        static const char *const what[WEAPON_COUNT] = {
            "Pulse laser: quick, cool, good on shields.", "Beam laser: a steady burn, shields melt.",
            "Rattle gun: kinetic rounds, needs ammo, chews hull.", "Slug gun: charge, then one heavy hit.",
            "Seeker missiles: lock on, then they find it.", "Dumbfire missiles: aim and hope."};
        snprintf(line, sizeof line, "FIT %s", ui_weapon_name(arms_kind));
        put(1, y++, line, HUD_HI);
        put(1, y++, what[arms_kind % WEAPON_COUNT], HUD_SUB);
        if (known) { snprintf(line, sizeof line, "price %u cr", (unsigned)o.price); put(1, y++, line, HUD); }
        snprintf(line, sizeof line, "rearm all: %u cr", (unsigned)game_rearm_cost());
        put(1, y++, line, HUD_SUB);
        /* the mount's mode: fixed on the nose, gimballed a little, a turret that tracks */
        const game_mount *pw = &p->weapons[ui.market_pick];
        static const char *const mode_says[3] = {"FIXED: straight ahead, full damage.",
                                                 "GIMBAL: leans onto the target, a little less damage.",
                                                 "TURRET: tracks the target round the front, least damage."};
        if (pw->kind < WEAPON_COUNT) {
            int next = (pw->mode + 1) % 3;
            game_equipment_offer mo;
            put(1, y++, mode_says[pw->mode % 3], HUD_SUB);
            if (game_mount_info(ui.market_pick, &mo)) {
                static const char *const mode_name[3] = {"FIXED", "GIMBAL", "TURRET"};
                snprintf(line, sizeof line, "MOUNT key: to %s", mode_name[next]);
                put(1, y++, line, HUD);
            }
        }
        if (y + WEAPON_COUNT + 2 < ui.view.h - 4) {
            heading(y++, "WEAPON GUIDE");
            static const char *const guide[] = {"Quick / cool / shields", "Sustained / shield damage", "Ammo / kinetic / hull", "Charge / heavy single shot", "Lock required / guided", "Aim directly / unguided"};
            int gap = ui.view.w < 90 && y + WEAPON_COUNT * 2 < ui.view.h - 4 ? 2 : 1;
            for (int i = 0; i < WEAPON_COUNT; i++) {
                snprintf(line, sizeof line, "%-8s %s", ui_weapon_name(i), guide[i]); put(1, y, line, i == arms_kind ? HUD_HI : HUD_SUB); y += gap;
            }
        }
        const char *labels[5] = {"NEXT TYPE", "FIT", "GROUP", "MOUNT", "REARM"};
        bool lit[5] = {0}, use[5] = {true, p->docked && known && o.available, pw->kind < WEAPON_COUNT,
                                     p->docked && pw->kind < WEAPON_COUNT, p->docked && game_rearm_cost() > 0};
        foot_keys(5, labels, lit, use);
    } else if (ui.market_tab == 4) {
        nyard = game_shipyard(yard, 12);
        if (ui.market_pick >= nyard) ui.market_pick = 0;
        const game_yard_offer *sel = nyard ? &yard[ui.market_pick] : NULL;
        /* the chosen hull turning in the top third */
        bool side = ui.view.w >= 90;
        int card = side ? ui.view.h - 10 : ui.view.h * 2 / 5;
        int card_w = side ? ui.view.w / 3 : ui.view.w;
        yard_list_x = side ? card_w + 2 : 0;
        int list_w = ui.view.w - yard_list_x;
        if (sel && ui.scene) {
            render_hull_card(ui.scene, sel->ship_class, ui.t, 0);
            uint8_t *d = l3_dots(ui.scene);
            int dw = 0, dh = 0;
            l3_size(ui.scene, &dw, &dh);
            /* the middle band of the scene, shown under the tabs */
            int skip = (dh / 3 - card * 3) / 2 + dh / 3;
            if (skip < 0) skip = 0;
            if (side) {
                int sy = (dh - card * 3) / 2, sx = (dw - card_w * 2) / 2;
                if (sy < 0) sy = 0;
                for (int r = 0; r < card * 3; r++) memcpy(ui.dots + r * card_w * 2, d + (sy + r) * dw + sx, card_w * 2);
                lap_dots(ui.view.x, ui.view.y + 3, card_w, card, ui.dots);
            } else lap_dots(ui.view.x, ui.view.y + 3, ui.view.w, card, d + (size_t)skip * dw);
            const game_ship_stats *st = game_ship(sel->ship_class);
            put(1, 4, st ? st->name : "", HUD_HI);
            if (sel->ship_class == game_player_class()) put(1, 5, "YOURS", HUD_HI);
        }
        yard_first = side ? 5 : 3 + card;
        if (nyard > (ui.view.h - 4 - 3 - yard_first) / 2) put(ui.view.w - 19, yard_first - 1, "tap picture: more", HUD_SUB);
        int rows = (ui.view.h - 4 - 3 - yard_first) / 2;
        if (yard_scroll > nyard - rows) yard_scroll = nyard - rows;
        if (yard_scroll < 0) yard_scroll = 0;
        int y = yard_first;
        for (int k2 = 0; k2 < rows && yard_scroll + k2 < nyard; k2++) {
            int i = yard_scroll + k2;
            const game_yard_offer *o = &yard[i];
            const game_ship_stats *st = game_ship(o->ship_class);
            bool picked = ui.market_pick == i, mine = o->ship_class == game_player_class();
            uint8_t bg = picked ? HUD_DIM : LAP_BLACK;
            uint8_t fg = mine ? HUD_HI : !o->available ? HUD_SUB : picked ? HUD_HI : HUD;
            lap_fill(ui.view.x + yard_list_x, ui.view.y + y, list_w, 2, ' ', LAP_ATTR(fg, bg));
            lap_put_str(ui.view.x + yard_list_x + 1, ui.view.y + y, st ? st->name : "?", LAP_ATTR(fg, bg));
            if (mine) snprintf(line, sizeof line, "      FLYING");
            else if (!o->available) snprintf(line, sizeof line, "  not stocked");
            else if (o->price < 0) snprintf(line, sizeof line, "+%d back", (int)-o->price);
            else snprintf(line, sizeof line, "%10d", (int)o->price);
            lap_put_str(ui.view.x + ui.view.w - 1 - (int)strlen(line), ui.view.y + y, line, LAP_ATTR(fg, bg));
            if (st) {
                snprintf(line, sizeof line, "cargo %d  speed %.0f  %.0f ly  seats %d", st->cargo, st->speed,
                         o->fuel_max, o->seats);
                lap_put_str(ui.view.x + yard_list_x + 3, ui.view.y + y + 1, line, LAP_ATTR(picked ? HUD_HI : HUD_SUB, bg));
            }
            y += 2;
        }
        if (sel && sel->pitch) put(1, ui.view.h - 6, sel->pitch, HUD_SUB);
        bool can = p->docked && sel && sel->available && sel->ship_class != game_player_class() &&
                   (sel->price < 0 || p->credits >= (uint32_t)sel->price);
        const char *labels[1] = {sel && sel->ship_class == game_player_class() ? "YOUR SHIP" : "BUY THIS HULL"};
        bool lit[1] = {false}, use[1] = {can};
        foot_keys(1, labels, lit, use);
    } else if (ui.market_tab == 3) {
        static const char *const kinds[9] = {"DELIVERY", "COURIER", "BOUNTY", "RESCUE", "PASSENGER", "SURVEY", "SALVAGE", "SMUGGLING", "ESCORT"};
        board_stride = market_row_stride(6, 6, 6, 3, 8);
        snprintf(line, sizeof line, "STATION BOARD  day %u", (unsigned)v->market_day);
        put(1, 4, line, HUD_SUB);
        if (v->event_text[0]) put(ui.view.w - 1 - (int)strlen(v->event_text), 4, v->event_text, HUD_COOL);
        int y = 6, accepted = 0;
        for (int i = 0; i < 6; i++) accepted += v->missions[i].accepted && !v->missions[i].done && !v->missions[i].failed;
        for (int i = 0; i < 6; i++) {
            const game_mission *m = &v->missions[i];
            if (!m->title[0]) continue;
            bool sel = ui.market_pick == i;
            uint8_t bg = sel ? HUD_DIM : LAP_BLACK;
            uint8_t fg = m->failed ? HUD_WARN : m->done ? HUD_OK : m->accepted ? HUD_COOL : sel ? HUD_HI : HUD;
            lap_fill(ui.view.x, ui.view.y + y, ui.view.w, 3, ' ', LAP_ATTR(fg, bg));
            snprintf(line, sizeof line, "%-8s %s", kinds[m->kind & 3], m->title);
            lap_put_str(ui.view.x + 1, ui.view.y + y, line, LAP_ATTR(fg, bg));
            int left = (int)m->deadline - (int)v->market_day;
            snprintf(line, sizeof line, "to %s  %u cr  %d days%s",
                     m->destination >= 0 && m->destination < GAME_SYSTEMS ? v->systems[m->destination].name : "-",
                     (unsigned)m->reward, left < 0 ? 0 : left,
                     m->done ? "  DONE" : m->failed ? "  FAILED" : m->accepted ? "  TAKEN" : "");
            lap_put_str(ui.view.x + 3, ui.view.y + y + 1, line, LAP_ATTR(sel ? HUD_HI : HUD_SUB, bg));
            if (m->brief[0]) {
                snprintf(line, sizeof line, "%.*s", ui.view.w - 4, m->brief);
                lap_put_str(ui.view.x + 3, ui.view.y + y + 2, line, LAP_ATTR(HUD_SUB, bg));
            }
            y += board_stride;
        }
        const game_mission *m = &v->missions[ui.market_pick < 6 ? ui.market_pick : 0];
        bool can = p->docked && m->title[0] && !m->accepted && accepted < 3;
        const char *labels[1] = {m->accepted ? "TAKEN" : accepted >= 3 ? "3 TAKEN: FULL" : "ACCEPT"};
        bool lit[1] = {m->accepted}, use[1] = {can};
        foot_keys(1, labels, lit, use);
    } else {
        put(1, 4, "EQUIPMENT", HUD_SUB);
        put(ui.view.w - 10, 4, "PRICE", HUD_SUB);
        int y = 6;
        market_stride = market_row_stride(EQ_COUNT, y, 10, 2, 5);
        for (int e = 0; e < EQ_COUNT; e++) {
            game_equipment_offer o;
            if (!game_equipment_info(e, &o)) continue;
            bool sel = ui.market_pick == e;
            uint8_t bg = sel ? HUD_DIM : LAP_BLACK;
            uint8_t fg = o.owned && e != EQ_MISSILE ? HUD_HI : o.available ? (sel ? HUD_HI : HUD) : HUD_SUB;
            lap_fill(ui.view.x, ui.view.y + y, ui.view.w, market_stride, ' ', LAP_ATTR(fg, LAP_BLACK));
            lap_fill(ui.view.x, ui.view.y + y, ui.view.w, 2, ' ', LAP_ATTR(fg, bg));
            lap_put_str(ui.view.x + 1, ui.view.y + y, o.name, LAP_ATTR(fg, bg));
            if (o.owned && e != EQ_MISSILE) snprintf(line, sizeof line, "  FITTED");
            else snprintf(line, sizeof line, "%8u", (unsigned)o.price);
            lap_put_str(ui.view.x + ui.view.w - 10, ui.view.y + y, line, LAP_ATTR(fg, bg));
            if (market_stride > 2) {
                snprintf(line, sizeof line, "Tech %u / %s", o.min_tech, o.owned ? "installed" : o.available ? "available here" : "requires credits / local tech");
                lap_put_str(ui.view.x + 3, ui.view.y + y + 1, line, LAP_ATTR(HUD_SUB, bg));
            }
            y += market_stride;
        }
        y++;
        snprintf(line, sizeof line, "Fuel %.1f/%.1f ly   refuel %u cr", p->fuel, p->fuel_max, (unsigned)game_refuel_cost());
        put(1, y++, line, HUD);
        snprintf(line, sizeof line, "Hull %.0f%%      repair %u cr", p->hull * 100 / game_player_ship()->hull,
                 (unsigned)game_repair_cost());
        put(1, y++, line, HUD);
        game_equipment_offer o;
        bool can = game_equipment_info(ui.market_pick, &o) && o.available;
        const char *labels[3] = {"BUY", "REFUEL", "REPAIR"};
        bool lit[3] = {0}, use[3] = {can, p->docked && game_refuel_cost() > 0, p->docked && game_repair_cost() > 0};
        foot_keys(3, labels, lit, use);
    }
}

/* ---- GALAXY ---------------------------------------------------------- */
/* A map you move about: drag to pan, the wheel or up/down to zoom, HOME to come
   back. Systems are coloured by economy, safety or tech (the COLOUR key), sized
   by population; names show wherever they fit. A system past your tank's range
   gets a route of hops; plotting it plots the first, and each arrival plots the
   next. A second tap on the chosen system plots it too. */
static const uint8_t ECO_INK[8] = {14, 6, 4, 12, 10, 11, 2, 3};
static const uint8_t GOV_INK[8] = {9, 1, 3, 11, 13, 6, 2, 10};
static const char *const COLOUR_BY[3] = {"ECONOMY", "SAFETY", "TECH"};
static int gx_map_w, gx_map_h;          /* the map's area in dots; the card takes the rest */
static int route_for = -2, route_from = -2;

/* One ly has the same pixel length on both axes. Dots are 1/2 x 1/3 cell. */
static float gx_aspect(void) {
    int cw = 10, ch = 17; lap_cell_px(&cw, &ch);
    return cw > 0 && ch > 0 ? cw * 1.5f / ch : 1;
}
static float gx_scale(void) {
    return fmaxf(1, (gx_map_w - 4)) / (2 * fmaxf(4, ui.gx_half));
}
static void gx_xy(const game_system *s, int *x, int *y) {
    float k = gx_scale();
    *x = gx_map_w / 2 + (int)floorf((s->x - ui.gx_x) * k + .5f);
    *y = 9 + (gx_map_h - 9) / 2 + (int)floorf((s->y - ui.gx_y) * k * gx_aspect() + .5f);
}
static uint8_t sys_ink(const game_system *s) {
    if (ui.gx_colour == 1) return GOV_INK[s->government & 7];
    if (ui.gx_colour == 2) return s->tech >= 12 ? 15 : s->tech >= 8 ? 14 : s->tech >= 4 ? HUD_SUB : HUD_DIM;
    return ECO_INK[s->economy & 7];
}
static void gx_dot(int x, int y, uint8_t ink) {
    if (x >= 0 && y >= 0 && x < gx_map_w && y < gx_map_h) ui.dots[y * ui.dw + x] = ink;
}
/* The fewest hops of at most the tank's range from here to `goal` (shortest of those). */
static void route_find(const game_view *v, int goal) {
    route_for = goal;
    route_from = v->system;
    ui.route_n = 0;
    if (goal < 0 || goal >= GAME_SYSTEMS) return;
    float reach = v->player.fuel_max > 0 ? v->player.fuel_max : 7;
    static int16_t prev[GAME_SYSTEMS];
    static float dist[GAME_SYSTEMS];
    static int16_t queue[GAME_SYSTEMS];
    for (int i = 0; i < GAME_SYSTEMS; i++) { prev[i] = -1; dist[i] = -1; }
    int qh = 0, qt = 0;
    queue[qt++] = (int16_t)v->system;
    dist[v->system] = 0;
    while (qh < qt) {
        int a = queue[qh++];
        if (a == goal) break;
        for (int b = 0; b < GAME_SYSTEMS; b++) {
            if (dist[b] >= 0) continue;
            float d = game_distance(&v->systems[a], &v->systems[b]);
            if (d > reach) continue;
            dist[b] = dist[a] + d;
            prev[b] = (int16_t)a;
            queue[qt++] = (int16_t)b;
        }
    }
    if (dist[goal] < 0) return;
    int n = 0, at = goal;
    int16_t rev[24];
    while (at >= 0 && n < 24) { rev[n++] = (int16_t)at; at = prev[at]; }
    for (int i = 0; i < n; i++) ui.route[i] = rev[n - 1 - i];
    ui.route_n = n;
}
/* After each arrival, the next hop of the route plots itself. */
void ui_route_update(void) {
    const game_view *v = game_state();
    if (!v || ui.route_goal < 0) return;
    if (v->system == ui.route_goal) { ui.route_goal = -1; return; }
    if (v->plotted >= 0 || v->jump != JUMP_IDLE) return;
    if (route_from != v->system || route_for != ui.route_goal) route_find(v, ui.route_goal);
    if (ui.route_n >= 2 && game_plot_jump(ui.route[1])) {
        char t[48];
        snprintf(t, sizeof t, "Route: next hop %s", v->systems[ui.route[1]].name);
        if (ui.voice) lap_say(t);
    }
}
void ui_galaxy_open(void) {
    const game_view *v = game_state();
    if (!v) return;
    if (ui.gx_half <= 0) ui.gx_half = 15;
    ui.gx_x = v->systems[v->system].x;
    ui.gx_y = v->systems[v->system].y;
}
void ui_galaxy_drag(float dx_dots, float dy_dots) {
    float k = gx_scale();
    ui.gx_x -= dx_dots / k;
    ui.gx_y -= dy_dots / (k * gx_aspect());
}
static void gx_zoom(float f) { ui.gx_half = cg_clamp(ui.gx_half * f, 4, 140); }
bool ui_galaxy_key(lap_key_t k, char ch) {
    const game_view *v = game_state();
    if (v->panel == PANEL_LOG) {
        if (k == LAP_KEY_UP || k == LAP_KEY_WHEEL_UP || k == LAP_KEY_LEFT) { ui.log_scroll -= 1; return true; }
        if (k == LAP_KEY_DOWN || k == LAP_KEY_WHEEL_DOWN || k == LAP_KEY_RIGHT) { ui.log_scroll += 1; return true; }
        return false;
    }
    if (k == LAP_KEY_UP || k == LAP_KEY_WHEEL_UP) { gx_zoom(.8f); return true; }
    if (k == LAP_KEY_DOWN || k == LAP_KEY_WHEEL_DOWN) { gx_zoom(1.25f); return true; }
    if (k == LAP_KEY_LEFT || k == LAP_KEY_RIGHT) {          /* step to the next system by distance from the chosen one */
        int from = ui.galaxy_pick >= 0 ? ui.galaxy_pick : v->system, best = -1;
        float fd = ui.galaxy_pick >= 0 ? 0 : -1, bd = 1e9f;
        for (int i = 0; i < GAME_SYSTEMS; i++) {
            float d = game_distance(&v->systems[v->system], &v->systems[i]);
            float cur = game_distance(&v->systems[v->system], &v->systems[from]);
            if (fd >= 0 && (k == LAP_KEY_RIGHT ? d <= cur : d >= cur)) continue;
            float gap = fabsf(d - cur);
            if (i != from && gap < bd) { bd = gap; best = i; }
        }
        if (best >= 0) { ui.galaxy_pick = best; ui.gx_x = v->systems[best].x; ui.gx_y = v->systems[best].y; }
        return true;
    }
    if (k == LAP_KEY_CHAR && (ch == '+' || ch == '=')) { gx_zoom(.8f); return true; }
    if (k == LAP_KEY_CHAR && (ch == '-' || ch == '_')) { gx_zoom(1.25f); return true; }
    if (k == LAP_KEY_CHAR && (ch | 0x20) == 'h') { ui_galaxy_open(); return true; }
    return false;
}

static void galaxy(const game_view *v) {
    const game_system *home = &v->systems[v->system];
    const game_player *p = &v->player;
    uint8_t *d = ui.dots;
    if (ui.gx_half <= 0) ui_galaxy_open();
    memset(d, LAP_DOT_CLEAR, (size_t)ui.dw * ui.dh);
    gx_map_w = ui.dw;
    gx_map_h = ui.dh - (ui.view.w >= 90 ? 30 : 39);
    float k = gx_scale();
    int pick = ui.galaxy_pick >= 0 ? ui.galaxy_pick : v->plotted;
    if (pick >= 0 && (route_for != pick || route_from != v->system)) route_find(v, pick);
    /* a 5 ly grid, faint, and the scale */
    float step = ui.gx_half > 60 ? 25 : ui.gx_half > 25 ? 10 : 5;
    float halfx = gx_map_w / (2 * k), halfy = gx_map_h / (2 * k * gx_aspect());
    for (float gx = floorf((ui.gx_x - halfx) / step) * step; gx <= ui.gx_x + halfx; gx += step) {
        int x = gx_map_w / 2 + (int)((gx - ui.gx_x) * k);
        for (float gy = floorf((ui.gx_y - halfy) / step) * step; gy <= ui.gx_y + halfy; gy += step) {
            int y = 9 + (gx_map_h - 9) / 2 + (int)((gy - ui.gx_y) * k * gx_aspect());
            if (y > 8) gx_dot(x, y, HUD_DIM);
        }
    }
    /* your reach: what the fuel in the tank does now, and a full tank */
    int hx, hy;
    gx_xy(home, &hx, &hy);
    for (int i = 0; i < 200; i++) {
        float a = i * 2 * CG_PI / 200;
        gx_dot(hx + (int)floorf(cosf(a) * p->fuel * k + .5f), hy + (int)floorf(sinf(a) * p->fuel * k * gx_aspect() + .5f), i % 2 ? HUD : HUD_DIM);
        if (i % 4 == 0)
            gx_dot(hx + (int)floorf(cosf(a) * p->fuel_max * k + .5f), hy + (int)floorf(sinf(a) * p->fuel_max * k * gx_aspect() + .5f), HUD_DIM);
    }
    /* the route, hop by hop */
    for (int i = 0; i + 1 < ui.route_n; i++) {
        int x0, y0, x1, y1;
        gx_xy(&v->systems[ui.route[i]], &x0, &y0);
        gx_xy(&v->systems[ui.route[i + 1]], &x1, &y1);
        int n = (int)fmaxf(abs(x1 - x0), abs(y1 - y0));
        for (int t = 0; t <= n; t++) if (t % 2 == 0) gx_dot(x0 + (x1 - x0) * t / (n ? n : 1), y0 + (y1 - y0) * t / (n ? n : 1), HUD_HI);
    }
    /* the systems */
    for (int i = 0; i < GAME_SYSTEMS; i++) {
        int x, y;
        gx_xy(&v->systems[i], &x, &y);
        if (x < 1 || y < 9 || x >= gx_map_w - 1 || y >= gx_map_h - 1) continue;
        uint8_t ink = sys_ink(&v->systems[i]);
        /* Keep real coordinates and every selectable star. A round density
           envelope fades the outer sparse region instead of drawing a hard square. */
        float radius = hypotf(v->systems[i].x - 32, v->systems[i].y - 32);
        bool edge = ui.gx_half > 30 && radius > 25 && i != pick && i != v->system;
        if (edge && (radius - 25) * 4 > (float)(v->systems[i].seed % 64)) ink = HUD_DIM;
        int s = edge ? 1 : 1 + (v->systems[i].population > 3) + (ui.gx_half < 12);
        for (int a = 0; a < s; a++) for (int b = 0; b < s; b++) gx_dot(x + a, y + b, ink);
        if (i == pick || i == v->system) {
            uint8_t mk = i == v->system ? 15 : HUD_COOL;
            for (int q = -5; q <= 5; q++) {
                if (abs(q) < 3) continue;
                gx_dot(x + q, y, mk);
                gx_dot(x, y + q, mk);
            }
        }
    }
    /* jobs: an amber ring; commanders on the mesh: a cyan diamond */
    for (int m = 0; m < 6; m++) {
        game_mission_marker marker;
        if(!game_mission_target(m,&marker)) continue;
        int x, y;
        gx_xy(&v->systems[marker.system], &x, &y);
        for (int q = 0; q < 24; q++) gx_dot(x + (int)floorf(cosf(q * .2618f) * 5 + .5f), y + (int)floorf(sinf(q * .2618f) * 4.5f + .5f), HUD_HI);
    }
    for (int q = 0; q < ui_net_count(); q++) {
        const ui_peer *pe = ui_net_peer(q);
        if (!pe->same_galaxy) continue;
        for (int i = 0; i < GAME_SYSTEMS; i++) {
            if (strcmp(v->systems[i].name, pe->system)) continue;
            int x, y;
            gx_xy(&v->systems[i], &x, &y);
            for (int r = 0; r <= 3; r++) {
                gx_dot(x - r, y - 3 + r, HUD_COOL); gx_dot(x + r, y - 3 + r, HUD_COOL);
                gx_dot(x - r, y + 3 - r, HUD_COOL); gx_dot(x + r, y + 3 - r, HUD_COOL);
            }
        }
    }
    lap_dots(ui.view.x, ui.view.y, ui.view.w, ui.view.h, d);
    /* names wherever they fit: the chosen, home and the route first, then by population */
    static uint8_t taken[64][32];
    memset(taken, 0, sizeof taken);
    int order[GAME_SYSTEMS], n = 0;
    for (int i = 0; i < GAME_SYSTEMS; i++) order[n++] = i;
    for (int a = 0; a < n; a++) {                      /* priority to the front */
        int i = order[a];
        bool first = i == pick || i == v->system;
        for (int r = 0; r < ui.route_n; r++) first |= ui.route[r] == i;
        if (first) { order[a] = order[0]; order[0] = i; }
    }
    int shown = 0;
    for (int a = 0; a < n && shown < 60; a++) {
        const int i = order[a];
        int x, y;
        gx_xy(&v->systems[i], &x, &y);
        if (x < 1 || y < 9 || x >= gx_map_w - 1 || y >= gx_map_h - 1) continue;
        bool must = i == pick || i == v->system;
        if (!must && ui.gx_half > 30 && (v->systems[i].population < 4 || hypotf(v->systems[i].x - 32, v->systems[i].y - 32) > 29)) continue;
        const char *nm = v->systems[i].name;
        int len = (int)strlen(nm), col = x / 2 + 2, row = y / 3;
        if (col + len >= gx_map_w / 2) col = x / 2 - len - 1;
        if (col < 0 || row < 3 || row >= 64) continue;
        bool clear = true;
        for (int c = col - 1; c <= col + len && clear; c++) if (c >= 0 && c < 256 && taken[row][c >> 3] >> (c & 7) & 1) clear = false;
        if (!clear) continue;
        for (int c = col - 1; c <= col + len; c++) if (c >= 0 && c < 256) taken[row][c >> 3] |= (uint8_t)(1 << (c & 7));
        put(col, row, nm, i == pick ? HUD_COOL : i == v->system ? 15 : sys_ink(&v->systems[i]) == 8 ? HUD_SUB : HUD);
        shown++;
    }
    heading(0, "GALACTIC CHART");
    snprintf(line, sizeof line, "%.0f ly across   colour: %s", gx_map_w / k, COLOUR_BY[ui.gx_colour % 3]);
    put(1, 1, line, HUD_SUB);
    /* Full-width detail strip; an unselected chart never reserves an empty card. */
    int y = gx_map_h / 3;
    heading(y++, "SYSTEM / ROUTE");
    if (pick >= 0) {
        const game_system *s = &v->systems[pick];
        float dist = game_distance(home, s);
        snprintf(line, sizeof line, "%s  %.1f ly  Tech %u  Pop %.1f bn", s->name, dist, s->tech, s->population);
        put(1, y++, line, HUD_HI);
        snprintf(line, sizeof line, "%s / %s", eco_names[s->economy & 7], gov_names[s->government & 7]); put(1, y++, line, HUD);
        if (pick == v->system) put(1, y++, "You are here.", HUD);
        else if (ui.route_n >= 2) {
            snprintf(line, sizeof line, "%d jump%s / first %s / %s", ui.route_n - 1, ui.route_n == 2 ? "" : "s",
                v->systems[ui.route[1]].name, dist <= p->fuel ? "in range" : "refuel / route"); put(1, y++, line, HUD);
        } else put(1, y++, "No route within full tank range.", HUD_WARN);
        /* Two lines of the description, broken between words. */
        const bool side = ui.view.w >= 90;
        const int w = side ? ui.view.w / 2 - 2 : ui.view.w - 2;
        const int dx = side ? ui.view.w / 2 + 1 : 1, dy = side ? gx_map_h / 3 + 2 : y;
        const char *d = s->description; int n = (int)strlen(d), cut = n;
        if (n > w) { cut = w; while (cut > 0 && d[cut] != ' ') cut--; if (!cut) cut = w; }
        snprintf(line, sizeof line, "%.*s", cut, d); put(dx, dy, line, HUD_SUB);
        const char *rest = d + cut; while (*rest == ' ') rest++;
        if ((int)strlen(rest) > w) snprintf(line, sizeof line, "%.*s...", w - 3, rest);
        else snprintf(line, sizeof line, "%s", rest);
        put(dx, dy + 1, line, HUD_SUB);
    } else {
        put(1, y++, "Tap a system. Drag to move. Tap again to plot.", HUD);
        put(1, y, "Ring: current fuel / dotted ring: full tank", HUD_SUB);
    }
    bool can = pick >= 0 && pick != v->system && ui.route_n >= 2;
    const char *plot = pick >= 0 && (pick == v->plotted || pick == ui.route_goal) ? "PLOTTED" : ui.route_n > 2 ? "ROUTE" : "PLOT";
    const char *labels[5] = {"ZOOM -", "ZOOM +", "HOME", COLOUR_BY[(ui.gx_colour + 1) % 3], plot};
    bool lit[5] = {false, false, false, false, pick >= 0 && (pick == v->plotted || pick == ui.route_goal)};
    bool use[5] = {true, true, true, true, can};
    foot_keys(5, labels, lit, use);
}
/* The system nearest a tap, within a few cells; -1 for none. */
static int galaxy_at(const game_view *v, int col, int row) {
    int tx = (col - ui.view.x) * 2 + 1, ty = (row - ui.view.y) * 3 + 1, best = -1, bd = 14 * 14;
    if (tx >= gx_map_w || ty < 9 || ty >= gx_map_h) return -2;
    for (int i = 0; i < GAME_SYSTEMS; i++) {
        int x, y;
        gx_xy(&v->systems[i], &x, &y);
        int dd = (x - tx) * (x - tx) + (y - ty) * (y - ty);
        if (dd < bd) { bd = dd; best = i; }
    }
    return best;
}
static void galaxy_plot(const game_view *v, int pick) {
    if (pick < 0 || pick == v->system) return;
    if (route_for != pick || route_from != v->system) route_find(v, pick);
    if (ui.route_n < 2) { lap_sound(LAP_SOUND_ERROR); return; }
    if (game_plot_jump(ui.route[1])) {
        ui.route_goal = ui.route_n > 2 ? pick : -1;
        lap_sound(LAP_SOUND_ROGER);
    } else lap_sound(LAP_SOUND_ERROR);
}

/* ---- LOG ------------------------------------------------------------- */
static const char *const journal_tags[] = {"TRADE", "COMBAT", "MISSION", "NAV", "COMMS"};
static const uint8_t journal_ink[] = {HUD, HUD_WARN, HUD_HI, HUD_SUB, HUD_COOL};
static ui_rect log_filters[6];
static int log_filter, log_total, log_visible, log_hits[96];
static void logbook(const game_view *v) {
    (void)v;
    heading(0, "COMMANDER JOURNAL");
    put(1, 1, "Newest first / tap a place to open GALAXY", HUD_SUB);
    for (int i = 0; i < 6; i++) {
        int x = ui.view.x + ui.view.w * i / 6;
        log_filters[i] = (ui_rect){x, ui.view.y + 2, ui.view.x + ui.view.w * (i + 1) / 6 - x, 3};
        ui_toggle_box(&log_filters[i], i ? journal_tags[i - 1] : "ALL", "", HUD, log_filter == i);
    }
    int order[56]; log_total = 0;
    for (int i = 0; i < ui_journal_count(); i++) {
        const ui_journal_entry *e = ui_journal_get(i);
        if (!log_filter || e->kind == log_filter - 1) order[log_total++] = i;
    }
    if (ui.log_scroll >= log_total) ui.log_scroll = log_total - 1;
    if (ui.log_scroll < 0) ui.log_scroll = 0;
    for (int i = 0; i < 96; i++) log_hits[i] = -1;
    int y = 6; uint32_t day = UINT32_MAX, jump = UINT32_MAX;
    log_visible = 0;
    /* One line an entry, like a ship's log: time, tag, words, and the place at
       the right when it fits; a narrow screen wraps the words and the place. */
    for (int k = ui.log_scroll; k < log_total; k++) {
        int i = order[k]; const ui_journal_entry *e = ui_journal_get(i);
        const bool placed = e->system >= 0 && e->system < GAME_SYSTEMS;
        char where[40] = "";
        if (placed) snprintf(where, sizeof where, "> %s", game_state()->systems[e->system].name);
        int room = ui.view.w - 17 - (placed ? (int)strlen(where) + 2 : 0);
        const bool one = (int)strlen(e->words) <= room;
        int width = one ? room : ui.view.w - 17;
        int lines = one ? 1 : 1 + ((int)strlen(e->words) > width) + placed;
        int needed = lines + (e->day != day || e->jump != jump);
        if (y + needed > ui.view.h - 5) break;
        if (e->day != day || e->jump != jump) {
            snprintf(line, sizeof line, "DAY %u / JUMP %u", (unsigned)e->day, (unsigned)e->jump);
            heading(y++, line); day = e->day; jump = e->jump;
        }
        int first = y;
        snprintf(line, sizeof line, "%02u:%02u", e->minute / 60, e->minute % 60); put(1, y, line, HUD_SUB);
        put(7, y, journal_tags[e->kind], journal_ink[e->kind]);
        snprintf(line, sizeof line, "%.*s", width, e->words); put(15, y, line, HUD);
        if (one && placed) put(ui.view.w - 1 - (int)strlen(where), y, where, HUD_SUB);
        y++;
        if (!one) {
            if ((int)strlen(e->words) > width) {
                snprintf(line, sizeof line, "%.*s", ui.view.w - 17, e->words + width); put(15, y++, line, HUD);
            }
            if (placed) put(15, y++, where, HUD_SUB);
        }
        for (int r = first; r < y && r < 96; r++) log_hits[r] = i;
        log_visible++;
    }
    if (!log_total) {
        put(1, 7, "No entries in this category yet.", HUD);
        put(1, 9, "Trades, journeys and radio calls appear here.", HUD_SUB);
    }
    snprintf(line, sizeof line, "%d-%d / %d  swipe or arrows", log_total ? ui.log_scroll + 1 : 0,
        ui.log_scroll + log_visible, log_total); put(1, ui.view.h - 5, line, HUD_SUB);
    const char *labels[3] = {"NEWER", "OLDER", "CLOSE"};
    bool lit[3] = {0}, use[3] = {ui.log_scroll > 0, ui.log_scroll + log_visible < log_total, true};
    foot_keys(3, labels, lit, use);
}

/* Panels use the dashboard's room while open. Kept out of ui_dash.c so the
 * flight-control changes can merge independently. Called before scene sizing. */
void ui_panel_layout(void) {
    ui_journal_update();
    if (game_state()->panel == PANEL_NONE) return;
    if (ui.wide) {
        int right = ui.keys[0].x - 2;
        ui.view.x = ui.tab_keys[0].w + 2;
        ui.view.w = right - ui.view.x;
    } else if (!ui.landscape) {
        ui.panel_full = true;
        ui.view = (ui_rect){0, 0, ui.cols, ui.rows - 3};
        ui.tabs = (ui_rect){0, ui.rows - 3, ui.cols, 3};
        for (int i = 0; i < 4; i++) ui.tab_keys[i] = (ui_rect){ui.cols * i / 4, ui.rows - 3, ui.cols * (i + 1) / 4 - ui.cols * i / 4, 3};
        ui.left = ui.right = ui.scope = ui.middle = ui.inst = ui.ctl_box = (ui_rect){0};
        ui.throttle = ui.throttle_grab = ui.modebar = ui.mode_info = (ui_rect){0};
        for (int i = 0; i < UI_KEYS; i++) ui.keys[i] = (ui_rect){ui.cols + 30, ui.rows + 10, 0, 0};
        for (int i = 0; i < UI_MODES; i++) ui.mode_keys[i] = (ui_rect){0};
        ui.nlamps = ui.nctls = 0;
    }
}

void ui_panel_draw(void) {
    if(ui_options_draw()) return;
    const game_view *v = game_state();
    lap_fill(ui.view.x, ui.view.y, ui.view.w, ui.view.h, ' ', LAP_ATTR(HUD, 0));
    npkeys = 0;
    switch (v->panel) {
    case PANEL_STATUS: status(v); break;
    case PANEL_MARKET: market(v); break;
    case PANEL_GALAXY: galaxy(v); break;
    case PANEL_LOG: logbook(v); break;
    default: break;
    }
}

bool ui_panel_tap(int col, int row) {
    if(ui_options_tap(col,row)) return true;
    const game_view *v = game_state();
    if (!ui_in(&ui.view, col, row) || v->panel == PANEL_NONE) return false;
    int k = foot_hit(col, row), y = row - ui.view.y;
    switch (v->panel) {
    case PANEL_STATUS:
        return status_tap(col, row);
    case PANEL_MARKET:
        if (y < 3) {
            for (int i = 0; i < 5; i++)
                if (ui_in(&(ui_rect){market_tab_rect(i).x, ui.view.y, market_tab_rect(i).w, 3}, col, row)) {
                    ui.market_tab = i; ui.market_pick = 0; arms_kind = -1;
                }
            break;
        }
        if (ui.market_tab == 2) {
            int mounts = p_mounts();
            if (k == 0) arms_kind = (arms_kind + 1) % WEAPON_COUNT;
            else if (k == 1) {
                if (game_fit_weapon(ui.market_pick, (game_weapon_kind)arms_kind)) lap_sound(LAP_SOUND_ROGER);
                else lap_sound(LAP_SOUND_ERROR);
            } else if (k == 2) game_weapon_group(ui.market_pick, v->player.weapons[ui.market_pick].group ? 0 : 1);
            else if (k == 3) {
                if (game_mount_mode(ui.market_pick, (v->player.weapons[ui.market_pick].mode + 1) % 3)) lap_sound(LAP_SOUND_ROGER);
                else lap_sound(LAP_SOUND_ERROR);
            } else if (k == 4) { if (game_rearm()) lap_sound(LAP_SOUND_ROGER); else lap_sound(LAP_SOUND_ERROR); }
            else if (y >= 6) {
                int m = (y - 6) / (arms_stride > 0 ? arms_stride : 2);
                if (m >= 0 && m < mounts) { ui.market_pick = m; arms_kind = -1; }
            }
            break;
        }
        if (ui.market_tab == 4) {
            if (k == 0) {
                if (game_buy_ship((game_class)yard[ui.market_pick].ship_class)) {
                    lap_sound(LAP_SOUND_ROGER);
                    if (ui.voice) lap_say("New ship delivered to the pad");
                } else lap_sound(LAP_SOUND_ERROR);
            } else if (y >= yard_first && col >= ui.view.x + yard_list_x) {
                int i = (y - yard_first) / 2 + yard_scroll;
                if (i >= 0 && i < nyard) ui.market_pick = i;
            } else if (k == -1 && y >= 3 && (y < yard_first || col < ui.view.x + yard_list_x)) {
                yard_scroll = yard_scroll + 3 >= nyard ? 0 : yard_scroll + 3;
            }
            break;
        }
        if (ui.market_tab == 3) {
            if (k == 0) {
                if (game_mission_accept(ui.market_pick)) lap_sound(LAP_SOUND_ROGER);
                else lap_sound(LAP_SOUND_ERROR);
            } else if (y >= 6) {
                int row_i = (y - 6) / (board_stride > 0 ? board_stride : 3), n = 0;
                for (int i = 0; i < 6; i++)
                    if (v->missions[i].title[0] && n++ == row_i) { ui.market_pick = i; break; }
            }
        } else if (ui.market_tab == 0) {
            if (k == 0) ui_cargo_trade(ui.market_pick, 1, true);
            else if (k == 1) ui_cargo_trade(ui.market_pick, 10, true);
            else if (k == 2) ui_cargo_trade(ui.market_pick, 1, false);
            else if (k == 3) ui_cargo_trade(ui.market_pick, v->player.cargo[ui.market_pick], false);
            else {
                int columns = ui.view.w >= 90 ? 2 : 1, width = ui.view.w / columns;
                int first = v->player.docked ? 5 : 10;
                int i = (y - first) / (market_stride > 0 ? market_stride : 1) + (col - ui.view.x) / width * ((GAME_GOODS + columns - 1) / columns);
                if (y >= first && y < ui.view.h - 7 && i >= 0 && i < GAME_GOODS) ui.market_pick = i;
            }
        } else {
            if (k == 0) game_equip(ui.market_pick);
            else if (k == 1) game_refuel();
            else if (k == 2) game_repair();
            else {
                int i = (y - 6) / (market_stride > 0 ? market_stride : 2);
                if (y >= 6 && i >= 0 && i < EQ_COUNT) ui.market_pick = i;
            }
        }
        lap_sound(LAP_SOUND_KEY);
        break;
    case PANEL_GALAXY: {
        int pick = ui.galaxy_pick >= 0 ? ui.galaxy_pick : v->plotted;
        if (k == 0) gx_zoom(1.25f);
        else if (k == 1) gx_zoom(.8f);
        else if (k == 2) ui_galaxy_open();
        else if (k == 3) ui.gx_colour = (ui.gx_colour + 1) % 3;
        else if (k == 4) galaxy_plot(v, pick);
        else {
            int hit = galaxy_at(v, col, row);
            if (hit == -2) break;                       /* the card */
            if (hit >= 0 && hit == ui.galaxy_pick) galaxy_plot(v, hit);   /* a second tap plots */
            else ui.galaxy_pick = hit;
        }
        break;
    }
    case PANEL_LOG:
        for (int f = 0; f < 6; f++) if (ui_in(&log_filters[f], col, row)) { log_filter = f; ui.log_scroll = 0; return true; }
        if (k < 0 && y >= 0 && y < 96 && log_hits[y] >= 0) {
            const ui_journal_entry *e = ui_journal_get(log_hits[y]);
            if (e && e->system >= 0 && e->system < GAME_SYSTEMS) {
                game_panel_open(PANEL_GALAXY); ui_galaxy_open(); ui.galaxy_pick = e->system;
                ui.gx_x = v->systems[e->system].x; ui.gx_y = v->systems[e->system].y;
            }
        }
        if (k == 0) ui.log_scroll -= log_visible > 0 ? log_visible : 1;
        else if (k == 1 && ui.log_scroll + log_visible < log_total) ui.log_scroll += log_visible > 0 ? log_visible : 1;
        else if (k == 2) game_panel_open(PANEL_NONE);
        break;
    default: break;
    }
    return true;
}
