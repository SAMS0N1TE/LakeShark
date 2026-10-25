/* The four panels over the view, one tap each from the dash tabs, one tap back:
 * STATUS (you and your ship), MARKET (goods and outfitting, docked), GALAXY (the
 * chart; tap a system, plot the jump), LOG (every ship you have met). */
#include "internal.h"
#include "art.h"
#include "ui.h"

static char line[96];
static const char *const gov_names[8] = {"Anarchy", "Feudal", "Multi-gov", "Dictatorship",
                                         "Communist", "Confederacy", "Democracy", "Corporate"};
static const char *const eco_names[8] = {"Rich industrial", "Industrial", "Poor industrial",
                                         "Mainly industrial", "Mainly agricultural", "Rich agricultural",
                                         "Agricultural", "Poor agricultural"};
static ui_rect pkeys[6];
static game_yard_offer yard[12];
static int nyard, yard_first = 99, yard_scroll;
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

void render_showcase(l3_scene *sc, int ship, float t, float shift, float back);

static void put(int x, int y, const char *s, uint8_t ink) {
    lap_put_str(ui.view.x + x, ui.view.y + y, s, LAP_ATTR(ink, 0));
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
static void status(const game_view *v) {
    const game_player *p = &v->player;
    if (ui.scene) {     /* your ship, turning on the right */
        /* a tall view (a desktop's full-height panel) would draw the hull past its
           right edge: back the camera off so it keeps the same share of the width */
        int cw = 10, ch = 17;
        lap_cell_px(&cw, &ch);
        float shape = (float)(ui.view.w * cw) / (float)(ui.view.h * ch > 0 ? ui.view.h * ch : 1);
        float back = shape < 3.2f ? 3.2f / shape : 1;
        render_showcase(ui.scene, game_player_class(), ui.t, -2.6f * back, back);
        lap_dots(ui.view.x, ui.view.y, ui.view.w, ui.view.h, l3_dots(ui.scene));
    }
    static const char *legal[] = {"Clean", "Offender", "Fugitive"};
    heading(1, "COMMANDER");
    uint8_t ink;
    const char *cond = condition(v, &ink);
    int y = 3;
    snprintf(line, sizeof line, "System   %s", v->systems[v->system].name); put(1, y++, line, HUD);
    snprintf(line, sizeof line, "Jump to  %s", v->plotted >= 0 ? v->systems[v->plotted].name : "-");
    put(1, y++, line, HUD);
    put(1, y, "Status", HUD); put(10, y++, cond, ink);
    snprintf(line, sizeof line, "Fuel     %.1f ly", p->fuel); put(1, y++, line, HUD);
    snprintf(line, sizeof line, "Credits  %u", (unsigned)p->credits); put(1, y++, line, HUD_HI);
    snprintf(line, sizeof line, "Legal    %s", legal[p->legal < 3 ? p->legal : 0]);
    put(1, y++, line, p->legal ? HUD_WARN : HUD);
    snprintf(line, sizeof line, "Rank     %s", game_rank(p->rank)); put(1, y++, line, HUD);
    snprintf(line, sizeof line, "Kills    %u", (unsigned)p->kills); put(1, y++, line, HUD);
    snprintf(line, sizeof line, "Explorer %s, %u found", game_explorer_rank(v->discoveries.rank),
             (unsigned)v->discoveries.count);
    put(1, y++, line, HUD);
    if (v->event_text[0]) { put(1, y, "News", HUD); put(10, y++, v->event_text, HUD_COOL); }
    for (int i = 0; i < 6; i++) {
        const game_mission *m = &v->missions[i];
        if (!m->accepted || m->done || m->failed) continue;
        snprintf(line, sizeof line, "Job      %s", m->title);
        put(1, y++, line, HUD_COOL);
    }
    y++;
    if (game_passenger_seats() >= 0) {
        snprintf(line, sizeof line, "Seats    %d free", game_passenger_seats());
        put(1, y++, line, HUD);
    }
    /* these rules stop short of your ship turning on the right */
    const int rule_end = ui.scene ? ui.view.w * 2 / 5 : ui.view.w;
    snprintf(line, sizeof line, "CARGO %d/%dt", game_cargo_used(), game_cargo_capacity());
    heading_to(y++, line, rule_end);
    int shown = 0;
    for (int i = 0; i < GAME_GOODS && y < ui.view.h - 18; i++)
        if (p->cargo[i]) {
            snprintf(line, sizeof line, "%3d  %s", p->cargo[i], v->market[i].name);
            put(1, y++, line, v->market[i].illegal ? HUD_WARN : HUD);
            shown++;
        }
    if (!shown) put(1, y++, "empty hold", HUD_SUB);
    y++;
    heading_to(y++, "FITTED", rule_end);
    for (int e = 0; e < EQ_COUNT && y < ui.view.h - 18; e++) {
        game_equipment_offer o;
        if (game_equipment_info(e, &o) && o.owned) {
            snprintf(line, sizeof line, "%s%s", o.name, e == EQ_MISSILE ? "" : "");
            if (e == EQ_MISSILE) snprintf(line, sizeof line, "%s x%d", o.name, p->missiles);
            put(1, y++, line, HUD);
        }
    }
    static const char *const steer[3] = {"STEER GENTLE", "STEER NORMAL", "STEER QUICK"};
    static const char *const steer_short[3] = {"STEER LOW", "STEER MID", "STEER HIGH"};
    (void)steer;
    const char *labels[4] = {p->motion_steer ? "TILT ON" : "TILT OFF", steer_short[ui.steer],
                             ui.voice ? "VOICE ON" : "VOICE OFF", "CLOSE"};
    bool lit[4] = {p->motion_steer, false, ui.voice, false}, use[4] = {true, true, true, true};
    foot_keys(4, labels, lit, use);
    /* the world: how busy space is, and whether the radio's aircraft are in it */
    static const char *const traffic[3] = {"LIVE+SIM", "SIM ONLY", "LIVE ONLY"};
    static const char *const busy[3] = {"CALM", "NORMAL", "BUSY"};
    char slot[16];
    snprintf(slot, sizeof slot, "MESH CH %d", ui.mesh_slot);
    const char *set[4] = {traffic[ui.traffic % 3], busy[ui.encounters % 3], slot, ui.sound ? "SOUND ON" : "SOUND OFF"};
    const char *sub[4] = {"ships from", "encounters", "channel", "tones"};
    for (int i = 0; i < 4; i++) {
        int x = ui.view.x + ui.view.w * i / 4;
        ui.set_keys[i] = (ui_rect){x, ui.view.y + ui.view.h - 9, ui.view.x + ui.view.w * (i + 1) / 4 - x, 4};
        ui_toggle_box(&ui.set_keys[i], set[i], sub[i], HUD, i == 3 && ui.sound);
    }
    /* flying help: aim assist bends the guns onto a target near the nose
       (STRONG for a touchscreen), zoom closes in on a far one */
    static const char *const aim[3] = {"AIM OFF", "AIM NORMAL", "AIM STRONG"};
    const char *set2[2] = {aim[ui.aim % 3], ui.autozoom ? "ZOOM AUTO" : "ZOOM OFF"};
    const char *sub2[2] = {"gun assist", "far targets"};
    for (int i = 0; i < 2; i++) {
        int x = ui.view.x + ui.view.w * i / 2;
        ui.set_keys[4 + i] = (ui_rect){x, ui.view.y + ui.view.h - 13, ui.view.x + ui.view.w * (i + 1) / 2 - x, 4};
        ui_toggle_box(&ui.set_keys[4 + i], set2[i], sub2[i], HUD, i ? ui.autozoom : ui.aim > 0);
    }
    /* the HUD: each part shown or hidden */
    static const char *const part[5] = {"LADDER", "TAPES", "NAV MARK", "JUMP MARK", "TARGET BOX"};
    static const char *const what[5] = {"pitch rungs", "speed, height", "nav choice", "route target", "bracket"};
    for (int i = 0; i < 5; i++) {
        int x = ui.view.x + ui.view.w * i / 5;
        ui.hud_keys[i] = (ui_rect){x, ui.view.y + ui.view.h - 17, ui.view.x + ui.view.w * (i + 1) / 5 - x, 4};
        ui_toggle_box(&ui.hud_keys[i], part[i], what[i], FHUD_HI, !(ui.hud_off & (1 << i)));
    }
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
        put(1, 4, "GOOD", HUD_SUB);
        put(ui.view.w - 27, 4, "PRICE   STOCK   HOLD", HUD_SUB);
        int y = p->docked ? 5 : 10;
        int rows = (ui.view.h - 4 - y) / 2;
        for (int i = 0; i < GAME_GOODS && i < rows; i++) {
            const game_market_item *m = &v->market[i];
            bool sel = ui.market_pick == i;
            uint8_t bg = sel ? HUD_DIM : LAP_BLACK;
            uint8_t fg = m->illegal ? HUD_WARN : sel ? HUD_HI : HUD;
            lap_fill(ui.view.x, ui.view.y + y, ui.view.w, 2, ' ', LAP_ATTR(fg, bg));
            lap_put_str(ui.view.x + 1, ui.view.y + y, m->name, LAP_ATTR(fg, bg));
            snprintf(line, sizeof line, "%5d %7d %6d", m->price, m->stock, p->cargo[i]);
            lap_put_str(ui.view.x + ui.view.w - 27, ui.view.y + y, line, LAP_ATTR(fg, bg));
            y += 2;
        }
        const char *note = game_good_note(ui.market_pick);
        if (note && note[0]) put(1, ui.view.h - 6, note, HUD_COOL);
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
        put(1, 4, "MOUNT   WEAPON      GROUP   AMMO   MODE", HUD_SUB);
        int y = 6;
        for (int m = 0; m < mounts && m < GAME_MOUNTS; m++) {
            const game_mount *w = &p->weapons[m];
            bool sel = ui.market_pick == m;
            uint8_t bg = sel ? HUD_DIM : LAP_BLACK, fg = sel ? HUD_HI : HUD;
            lap_fill(ui.view.x, ui.view.y + y, ui.view.w, 2, ' ', LAP_ATTR(fg, bg));
            char ammo[12] = "-";
            if (w->kind == WEAPON_RATTLE) snprintf(ammo, sizeof ammo, "%d", w->ammo);
            static const char *const mode[3] = {"FIXED", "GIMBAL", "TURRET"};
            snprintf(line, sizeof line, "  %d     %-10s  %-6s  %-5s  %s", m + 1, ui_weapon_name(w->kind),
                     w->kind < WEAPON_COUNT ? (w->group ? "ALT" : "FIRE") : "-", ammo,
                     w->kind < WEAPON_COUNT ? mode[w->mode % 3] : "-");
            lap_put_str(ui.view.x + 1, ui.view.y + y, line, LAP_ATTR(fg, bg));
            y += 2;
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
        put(1, y++, what[arms_kind % WEAPON_COUNT], HUD_COOL);
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
        const char *labels[5] = {"NEXT TYPE", "FIT", "GROUP", "MOUNT", "REARM"};
        bool lit[5] = {0}, use[5] = {true, p->docked && known && o.available, pw->kind < WEAPON_COUNT,
                                     p->docked && pw->kind < WEAPON_COUNT, p->docked && game_rearm_cost() > 0};
        foot_keys(5, labels, lit, use);
    } else if (ui.market_tab == 4) {
        nyard = game_shipyard(yard, 12);
        if (ui.market_pick >= nyard) ui.market_pick = 0;
        const game_yard_offer *sel = nyard ? &yard[ui.market_pick] : NULL;
        /* the chosen hull turning in the top third */
        int card = ui.view.h / 3;
        if (sel && ui.scene) {
            render_hull_card(ui.scene, sel->ship_class, ui.t, 0);
            uint8_t *d = l3_dots(ui.scene);
            int dw = 0, dh = 0;
            l3_size(ui.scene, &dw, &dh);
            /* the middle band of the scene, shown under the tabs */
            int skip = (dh / 3 - card * 3) / 2 + dh / 3;
            if (skip < 0) skip = 0;
            lap_dots(ui.view.x, ui.view.y + 3, ui.view.w, card, d + (size_t)skip * dw);
            const game_ship_stats *st = game_ship(sel->ship_class);
            put(1, 4, st ? st->name : "", HUD_HI);
            if (sel->ship_class == (int)game_player_class()) put(1, 5, "YOURS", HUD_COOL);
        }
        yard_first = 3 + card;
        if (nyard > (ui.view.h - 4 - 3 - yard_first) / 2) put(ui.view.w - 19, yard_first - 1, "tap picture: more", HUD_SUB);
        int rows = (ui.view.h - 4 - 3 - yard_first) / 2;
        if (yard_scroll > nyard - rows) yard_scroll = nyard - rows;
        if (yard_scroll < 0) yard_scroll = 0;
        int y = yard_first;
        for (int k2 = 0; k2 < rows && yard_scroll + k2 < nyard; k2++) {
            int i = yard_scroll + k2;
            const game_yard_offer *o = &yard[i];
            const game_ship_stats *st = game_ship(o->ship_class);
            bool picked = ui.market_pick == i, mine = o->ship_class == (int)game_player_class();
            uint8_t bg = picked ? HUD_DIM : LAP_BLACK;
            uint8_t fg = mine ? HUD_COOL : !o->available ? HUD_SUB : picked ? HUD_HI : HUD;
            lap_fill(ui.view.x, ui.view.y + y, ui.view.w, 2, ' ', LAP_ATTR(fg, bg));
            lap_put_str(ui.view.x + 1, ui.view.y + y, st ? st->name : "?", LAP_ATTR(fg, bg));
            if (mine) snprintf(line, sizeof line, "      FLYING");
            else if (!o->available) snprintf(line, sizeof line, "  not stocked");
            else if (o->price < 0) snprintf(line, sizeof line, "+%d back", (int)-o->price);
            else snprintf(line, sizeof line, "%10d", (int)o->price);
            lap_put_str(ui.view.x + ui.view.w - 1 - (int)strlen(line), ui.view.y + y, line, LAP_ATTR(fg, bg));
            if (st) {
                snprintf(line, sizeof line, "cargo %d  speed %.0f  %.0f ly  seats %d", st->cargo, st->speed,
                         o->fuel_max, o->seats);
                lap_put_str(ui.view.x + 3, ui.view.y + y + 1, line, LAP_ATTR(picked ? HUD_HI : HUD_SUB, bg));
            }
            y += 2;
        }
        if (sel && sel->pitch) put(1, ui.view.h - 6, sel->pitch, HUD_COOL);
        bool can = p->docked && sel && sel->available && sel->ship_class != (int)game_player_class() &&
                   (sel->price < 0 || p->credits >= (uint32_t)sel->price);
        const char *labels[1] = {sel && sel->ship_class == (int)game_player_class() ? "YOUR SHIP" : "BUY THIS HULL"};
        bool lit[1] = {false}, use[1] = {can};
        foot_keys(1, labels, lit, use);
    } else if (ui.market_tab == 3) {
        static const char *const kinds[4] = {"DELIVERY", "COURIER", "BOUNTY", "RESCUE"};
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
            if (sel && m->brief[0]) {
                snprintf(line, sizeof line, "%.*s", ui.view.w - 4, m->brief);
                lap_put_str(ui.view.x + 3, ui.view.y + y + 2, line, LAP_ATTR(HUD_COOL, bg));
            }
            y += 3;
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
        for (int e = 0; e < EQ_COUNT; e++) {
            game_equipment_offer o;
            if (!game_equipment_info(e, &o)) continue;
            bool sel = ui.market_pick == e;
            uint8_t bg = sel ? HUD_DIM : LAP_BLACK;
            uint8_t fg = o.owned && e != EQ_MISSILE ? HUD_OK : o.available ? (sel ? HUD_HI : HUD) : HUD_SUB;
            lap_fill(ui.view.x, ui.view.y + y, ui.view.w, 2, ' ', LAP_ATTR(fg, bg));
            lap_put_str(ui.view.x + 1, ui.view.y + y, o.name, LAP_ATTR(fg, bg));
            if (o.owned && e != EQ_MISSILE) snprintf(line, sizeof line, "  FITTED");
            else snprintf(line, sizeof line, "%8u", (unsigned)o.price);
            lap_put_str(ui.view.x + ui.view.w - 10, ui.view.y + y, line, LAP_ATTR(fg, bg));
            y += 2;
        }
        y++;
        snprintf(line, sizeof line, "Fuel %.1f/7 ly   refuel %u cr", p->fuel, (unsigned)game_refuel_cost());
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

static float gx_scale(void) { return (gx_map_w * .5f - 4) / ui.gx_half; }
static void gx_xy(const game_system *s, int *x, int *y) {
    float k = gx_scale();
    *x = gx_map_w / 2 + (int)floorf((s->x - ui.gx_x) * k + .5f);
    *y = gx_map_h / 2 + (int)floorf((s->y - ui.gx_y) * k * .9f + .5f);
}
static uint8_t sys_ink(const game_system *s) {
    if (ui.gx_colour == 1) return GOV_INK[s->government & 7];
    if (ui.gx_colour == 2) return s->tech >= 12 ? 15 : s->tech >= 8 ? 14 : s->tech >= 4 ? 7 : 8;
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
    ui.gx_y -= dy_dots / (k * .9f);
}
static void gx_zoom(float f) { ui.gx_half = cg_clamp(ui.gx_half * f, 4, 140); }
bool ui_galaxy_key(lap_key_t k, char ch) {
    const game_view *v = game_state();
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
    const bool side = ui.view.w >= 90;                 /* wide: the card on the right; narrow: under the map */
    gx_map_w = side ? ui.dw * 64 / 100 : ui.dw;
    gx_map_h = side ? ui.dh - 15 : ui.dh - 45;
    float k = gx_scale();
    int pick = ui.galaxy_pick >= 0 ? ui.galaxy_pick : v->plotted;
    if (pick >= 0 && (route_for != pick || route_from != v->system)) route_find(v, pick);
    /* a 5 ly grid, faint, and the scale */
    float step = ui.gx_half > 60 ? 25 : ui.gx_half > 25 ? 10 : 5;
    for (float gx = floorf((ui.gx_x - ui.gx_half) / step) * step; gx <= ui.gx_x + ui.gx_half; gx += step) {
        int x = gx_map_w / 2 + (int)((gx - ui.gx_x) * k);
        for (int y = 9; y < gx_map_h; y += 4) gx_dot(x, y, 8);
    }
    for (float gy = floorf((ui.gx_y - ui.gx_half) / step) * step; gy <= ui.gx_y + ui.gx_half; gy += step) {
        int y = gx_map_h / 2 + (int)((gy - ui.gx_y) * k * .9f);
        if (y > 8) for (int x = 0; x < gx_map_w; x += 4) gx_dot(x, y, 8);
    }
    /* your reach: what the fuel in the tank does now, and a full tank */
    int hx, hy;
    gx_xy(home, &hx, &hy);
    for (int i = 0; i < 200; i++) {
        float a = i * 2 * CG_PI / 200;
        gx_dot(hx + (int)floorf(cosf(a) * p->fuel * k + .5f), hy + (int)floorf(sinf(a) * p->fuel * k * .9f + .5f), i % 2 ? HUD : HUD_DIM);
        if (i % 4 == 0)
            gx_dot(hx + (int)floorf(cosf(a) * p->fuel_max * k + .5f), hy + (int)floorf(sinf(a) * p->fuel_max * k * .9f + .5f), HUD_DIM);
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
        int s = 1 + (v->systems[i].population > 3) + (ui.gx_half < 12);
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
        const game_mission *job = &v->missions[m];
        if (!job->accepted || job->done || job->failed || job->destination < 0 || job->destination >= GAME_SYSTEMS) continue;
        int x, y;
        gx_xy(&v->systems[job->destination], &x, &y);
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
        if (!must && ui.gx_half > 30 && v->systems[i].population < 4) continue;
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
    snprintf(line, sizeof line, "%.0f ly across   colour: %s", ui.gx_half * 2, COLOUR_BY[ui.gx_colour % 3]);
    put(1, 1, line, HUD_SUB);
    /* the card for the chosen system */
    int cx = side ? gx_map_w / 2 + 2 : 1, y = side ? 4 : (gx_map_h / 3) + 1;
    int cw = side ? ui.view.w - cx - 1 : ui.view.w - 2;
    if (side) for (int r = 3; r < ui.view.h - 5; r++) lap_put_cell(ui.view.x + cx - 2, ui.view.y + r, LAP_BRAILLE(0x47), LAP_ATTR(HUD_DIM, 0));
    if (pick >= 0) {
        const game_system *s = &v->systems[pick];
        float dist = game_distance(home, s);
        snprintf(line, sizeof line, "%s", s->name);
        put(cx, y++, line, HUD_COOL);
        snprintf(line, sizeof line, "%.1f ly from here", dist);
        put(cx, y++, line, HUD);
        snprintf(line, sizeof line, "%s", eco_names[s->economy & 7]);
        put(cx, y++, line, ECO_INK[s->economy & 7]);
        snprintf(line, sizeof line, "%s", gov_names[s->government & 7]);
        put(cx, y++, line, GOV_INK[s->government & 7]);
        snprintf(line, sizeof line, "Tech %u   Pop %.1f bn", s->tech, s->population);
        put(cx, y++, line, HUD);
        y++;
        char desc[96];
        snprintf(desc, sizeof desc, "%s", s->description);
        int len = (int)strlen(desc);
        for (int q = 0; q < len && y < ui.view.h - 8; q += cw) {
            snprintf(line, (size_t)(cw + 1) < sizeof line ? (size_t)cw + 1 : sizeof line, "%s", desc + q);
            put(cx, y++, line, HUD_SUB);
        }
        y++;
        if (pick == v->system) put(cx, y++, "You are here.", 15);
        else if (ui.route_n == 2) put(cx, y++, dist <= p->fuel ? "One jump: in range." : "One jump: refuel first.", dist <= p->fuel ? HUD_OK : HUD_WARN);
        else if (ui.route_n > 2) {
            float tot = 0;
            for (int r = 0; r + 1 < ui.route_n; r++) tot += game_distance(&v->systems[ui.route[r]], &v->systems[ui.route[r + 1]]);
            snprintf(line, sizeof line, "Route: %d jumps, %.1f ly", ui.route_n - 1, tot);
            put(cx, y++, line, HUD_HI);
            snprintf(line, sizeof line, "first: %s", v->systems[ui.route[1]].name);
            put(cx, y++, line, HUD_SUB);
        } else put(cx, y++, "Out of reach of your tank.", HUD_WARN);
    } else put(cx, y, "Tap a system. Drag to move.", HUD);
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
    if (tx >= gx_map_w || ty >= gx_map_h) return -2;
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
static void logbook(const game_view *v) {
    snprintf(line, sizeof line, "SHIPS MET %d", v->registry_count);
    heading(0, line);
    put(1, 2, "SHIP         CALLSIGN  FROM THE RADIO", HUD_SUB);
    int rows = (ui.view.h - 8) / 2;
    if (ui.log_scroll > v->registry_count - rows) ui.log_scroll = v->registry_count - rows;
    if (ui.log_scroll < 0) ui.log_scroll = 0;
    int y = 4;
    for (int k = 0; k < rows; k++) {
        int i = v->registry_count - 1 - (ui.log_scroll + k);   /* newest first */
        if (i < 0) break;
        const game_registry_entry *e = &v->registry[i];
        const game_ship_stats *st = game_ship((game_class)e->ship_class);
        uint8_t ink = e->live ? HUD_COOL : HUD;
        snprintf(line, sizeof line, "%-12.12s %-9s", st ? st->name : "?", e->callsign[0] ? e->callsign : "-");
        put(1, y, line, ink);
        if (e->live) {
            snprintf(line, sizeof line, "FL%03d %dkt", (int)(e->altitude_ft / 100), e->speed_kt);
            put(24, y, line, HUD_COOL);
        } else put(24, y, "simulated", HUD_SUB);
        snprintf(line, sizeof line, "  %s  seen %u", game_faction(e->faction), (unsigned)e->visits);
        put(1, y + 1, line, HUD_SUB);
        y += 2;
    }
    if (!v->registry_count) put(1, 5, "No ships met yet. Fly out and look around.", HUD);
    const char *labels[3] = {"NEWER", "OLDER", "CLOSE"};
    bool lit[3] = {0}, use[3] = {ui.log_scroll > 0, ui.log_scroll < v->registry_count - rows, true};
    foot_keys(3, labels, lit, use);
}

void ui_panel_draw(void) {
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
    const game_view *v = game_state();
    if (!ui_in(&ui.view, col, row) || v->panel == PANEL_NONE) return false;
    int k = foot_hit(col, row), y = row - ui.view.y;
    switch (v->panel) {
    case PANEL_STATUS:
        if (k == 0) {
            bool on = !v->player.motion_steer;
            lap_motion_t m;
            if (on && lap_motion(&m)) { ui.level_pitch = m.pitch; ui.level_roll = m.roll; }
            game_motion_steer(on);
        } else if (k == 1) {
            ui.steer = (ui.steer + 1) % 3;
            ui_save_settings();
            lap_sound(LAP_SOUND_KEY);
        } else if (k == 2) {
            ui.voice = !ui.voice;
            if (!ui.voice) lap_say_stop(); else lap_say("Voice on");
            ui_save_settings();
        } else if (k == 3) game_panel_open(PANEL_NONE);
        else if (ui_in(&ui.set_keys[0], col, row)) {
            ui.traffic = (ui.traffic + 1) % 3;
            game_set_traffic(ui.traffic);
            ui_save_settings();
        } else if (ui_in(&ui.set_keys[1], col, row)) {
            ui.encounters = (ui.encounters + 1) % 3;
            game_set_encounters(ui.encounters);
            ui_save_settings();
        } else if (ui_in(&ui.set_keys[2], col, row)) {
            ui.mesh_slot = (ui.mesh_slot + 1) % 4;
            ui_save_settings();
        } else if (ui_in(&ui.set_keys[3], col, row)) {
            ui.sound = !ui.sound;
            if (!ui.sound) lap_tone_stop();
            ui_save_settings();
        } else if (ui_in(&ui.set_keys[4], col, row)) {
            ui.aim = (ui.aim + 1) % 3;
            ui_apply_aim();
            ui_save_settings();
        } else if (ui_in(&ui.set_keys[5], col, row)) {
            ui.autozoom = !ui.autozoom;
            ui_save_settings();
        } else {
            for (int i = 0; i < 5; i++)
                if (ui_in(&ui.hud_keys[i], col, row)) {
                    ui.hud_off ^= (uint8_t)(1 << i);
                    ui_save_settings();
                    lap_sound(LAP_SOUND_KEY);
                }
        }
        break;
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
                int m = (y - 6) / 2;
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
            } else if (y >= yard_first) {
                int i = (y - yard_first) / 2 + yard_scroll;
                if (i >= 0 && i < nyard) ui.market_pick = i;
            } else if (k == -1 && y >= 3 && y < yard_first) {
                yard_scroll = yard_scroll + 3 >= nyard ? 0 : yard_scroll + 3;
            }
            break;
        }
        if (ui.market_tab == 3) {
            if (k == 0) {
                if (game_mission_accept(ui.market_pick)) lap_sound(LAP_SOUND_ROGER);
                else lap_sound(LAP_SOUND_ERROR);
            } else if (y >= 6) {
                int row_i = (y - 6) / 3, n = 0;
                for (int i = 0; i < 6; i++)
                    if (v->missions[i].title[0] && n++ == row_i) { ui.market_pick = i; break; }
            }
        } else if (ui.market_tab == 0) {
            if (k == 0) game_market_buy(ui.market_pick, 1);
            else if (k == 1) game_market_buy(ui.market_pick, 10);
            else if (k == 2) game_market_sell(ui.market_pick, 1);
            else if (k == 3) game_market_sell(ui.market_pick, v->player.cargo[ui.market_pick]);
            else {
                int first = v->player.docked ? 5 : 10, i = (y - first) / 2;
                if (y >= first && i >= 0 && i < GAME_GOODS) ui.market_pick = i;
            }
        } else {
            if (k == 0) game_equip(ui.market_pick);
            else if (k == 1) game_refuel();
            else if (k == 2) game_repair();
            else {
                int i = (y - 6) / 2;
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
        if (k == 0) ui.log_scroll -= 4;
        else if (k == 1) ui.log_scroll += 4;
        else if (k == 2) game_panel_open(PANEL_NONE);
        break;
    default: break;
    }
    return true;
}
