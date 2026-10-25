/* The wide console's two panels: status lamps on the left, push buttons on
 * the right, like a control desk. A lamp lights in its colour when it means
 * something (red when it is bad, flashing when it is urgent) and always shows
 * its value; tapping one opens where that is dealt with. A button has a small
 * light of its own: lit when it can be pressed, the whole key lit while what
 * it does is on. The frame shift drive's state lives here too, spoken as it
 * changes. */
#include "internal.h"
#include "art.h"
#include "ui.h"

enum { FSD_NONE, FSD_LOCKED, FSD_ALIGN, FSD_READY, FSD_CHARGE, FSD_JUMP };

/* The drive: no route, mass locked, turn to the destination, ready, charging, in the jump. */
static int fsd_state(void) {
    const game_view *v = game_state();
    if (v->jump == JUMP_TUNNEL) return FSD_JUMP;
    if (v->jump == JUMP_CHARGE) return FSD_CHARGE;
    if (v->plotted < 0 || v->player.docked) return FSD_NONE;
    if (v->player.mass_locked) return FSD_LOCKED;
    return game_jump_alignment() > GAME_JUMP_ALIGN_DEG ? FSD_ALIGN : FSD_READY;
}

const char *ui_fsd_words(uint8_t *ink, bool *flash) {
    const game_view *v = game_state();
    static char t[16];
    *flash = false;
    switch (fsd_state()) {
    case FSD_JUMP: *ink = 15; return "JUMPING";
    case FSD_CHARGE:
        *ink = HUD_COOL;
        *flash = true;
        snprintf(t, sizeof t, "CHARGE %.0f", fmaxf(1, ceilf(3 - v->jump_progress)));
        return t;
    case FSD_LOCKED: *ink = HUD_HI; return "MASS LOCK";
    case FSD_ALIGN:
        *ink = HUD_HI;
        snprintf(t, sizeof t, "ALIGN %.0f", game_jump_alignment());
        return t;
    case FSD_READY: *ink = HUD_OK; return "READY";
    default: *ink = HUD_SUB; return "NO ROUTE";
    }
}

/* Each frame: say what the drive just became. */
void ui_fsd_voice(void) {
    const game_view *v = game_state();
    static int plotted = -2, count = -1;
    if (v->plotted != plotted) {
        if (v->plotted >= 0 && plotted != -2 && ui.voice) {
            char t[48];
            snprintf(t, sizeof t, "Route plotted to %s.", v->systems[v->plotted].name);
            lap_say(t);
        }
        plotted = v->plotted;
    }
    int s = fsd_state();
    if (s == FSD_CHARGE) {                  /* a count down to the jump */
        int left = (int)ceilf(3 - v->jump_progress);
        if (left != count && left >= 1 && left <= 2 && ui.voice) lap_say(left == 2 ? "Two" : "One");
        count = left;
    } else count = -1;
    if (s == ui.fsd_said) return;
    int was = ui.fsd_said;
    ui.fsd_said = s;
    if (!ui.voice || was == 0 && s == FSD_NONE) return;
    switch (s) {
    case FSD_READY: lap_say("Aligned. Frame shift ready."); break;
    case FSD_CHARGE: lap_say("Frame shift drive charging. Three"); break;
    case FSD_JUMP: lap_say("Engage."); break;
    case FSD_LOCKED: if (was == FSD_READY || was == FSD_ALIGN) lap_say("Mass locked."); break;
    case FSD_ALIGN: if (was == FSD_CHARGE) lap_say("Jump aborted."); break;
    default: break;
    }
}

/* A lamp: lit, a block of its colour with black words; dark, its value in amber. */
static void lamp(const ui_rect *r, const char *label, const char *val, uint8_t ink, bool lit, bool flash) {
    bool on = lit && (!flash || ((int)(ui.t * 3) & 1) == 0);
    uint8_t bg = on ? ink : LAP_BLACK;
    lap_fill(r->x, r->y, r->w - 1, r->h - 1, ' ', LAP_ATTR(HUD, bg));
    lap_put_str(r->x + 1, r->y, label, LAP_ATTR(on ? LAP_BLACK : HUD_SUB, bg));
    lap_put_str(r->x + 1, r->y + 1, val, LAP_ATTR(on ? LAP_BLACK : lit ? ink : HUD, bg));
    lap_hline(r->x, r->y + r->h - 1, r->w - 1, LAP_BRAILLE(0x09), LAP_ATTR(HUD_DIM, LAP_BLACK));
}

/* A push button: its own light at the left, lit while it can be pressed, and its
   keyboard key at the right. */
static const char *key_cap;
static void button(const ui_rect *r, const char *label, const char *val, bool usable, bool active, uint8_t led) {
    uint8_t bg = active ? HUD : LAP_BLACK, fg = active ? LAP_BLACK : usable ? HUD : HUD_SUB;
    lap_fill(r->x, r->y, r->w - 1, r->h - 1, ' ', LAP_ATTR(fg, bg));
    lap_put_cell(r->x, r->y, LAP_BRAILLE(0xFF), LAP_ATTR(active ? LAP_BLACK : usable ? led : HUD_DIM, bg));
    lap_put_str(r->x + 2, r->y, label, LAP_ATTR(fg, bg));
    if (key_cap && r->w >= (int)strlen(label) + (int)strlen(key_cap) + 5)
        lap_put_str(r->x + r->w - 2 - (int)strlen(key_cap), r->y, key_cap, LAP_ATTR(active ? LAP_BLACK : HUD_DIM, bg));
    if (val && *val) lap_put_str(r->x + 2, r->y + 1, val, LAP_ATTR(active ? LAP_BLACK : HUD_SUB, bg));
    lap_hline(r->x, r->y + r->h - 1, r->w - 1, LAP_BRAILLE(0x09), LAP_ATTR(usable ? led : HUD_DIM, LAP_BLACK));
}

enum { L_SHIELD, L_HULL, L_FSD, L_FUEL, L_HEAT, L_DANGER, L_TARGET, L_CRUISE, L_MASS, L_SPEED, L_DOCK, L_CARGO,
       L_MISSILE, L_AUTO, L_LIVE, L_COUNT };
enum { B_BOOST, B_CHAFF, B_SINK, B_CELL, B_FOE, B_AUTO, B_JUMP, B_VIEW, B_CAM, B_ECM, B_MISSILE, B_RESET,
       B_SYS, B_ENG, B_WEP, B_COUNT };

void ui_console_draw(void) {
    if (!ui.nlamps) return;
    const game_view *v = game_state();
    const game_player *p = &v->player;
    const game_ship_stats *me = game_player_ship();
    char t[16];
    for (int k = 0; k < ui.nlamps && k < L_COUNT; k++) {
        const ui_rect *r = &ui.lamps[k];
        switch (k) {
        case L_SHIELD: {
            float lo = fminf(p->shield_fore, p->shield_aft);
            snprintf(t, sizeof t, "%3.0f %3.0f", p->shield_fore, p->shield_aft);
            lamp(r, "SHIELDS", t, lo > 50 ? HUD_COOL : lo > 0 ? HUD_HI : HUD_WARN, true, lo <= 0);
            break;
        }
        case L_HULL: {
            float f = me ? p->hull / me->hull : 1;
            snprintf(t, sizeof t, "%3.0f%%", f * 100);
            lamp(r, "HULL", t, f > .6f ? HUD_OK : f > .3f ? HUD_HI : HUD_WARN, true, f <= .3f);
            break;
        }
        case L_FSD: {
            uint8_t ink;
            bool flash;
            const char *w = ui_fsd_words(&ink, &flash);
            lamp(r, "FSD", w, ink, v->plotted >= 0 || v->jump != JUMP_IDLE, flash);
            break;
        }
        case L_FUEL:
            snprintf(t, sizeof t, "%.1f LY", p->fuel);
            lamp(r, "FUEL", t, HUD_HI, p->fuel < 1 || (v->plotted >= 0 && p->fuel < v->plotted_ly), false);
            break;
        case L_HEAT:
            snprintf(t, sizeof t, "%3.0f%%", p->heat);
            lamp(r, "HEAT", t, p->heat > 85 ? HUD_WARN : HUD_HI, p->heat > 65, p->heat > 85);
            break;
        case L_DANGER: {
            int n = 0;
            for (int i = 0; i < GAME_CONTACTS; i++) n += v->contacts[i].active && v->contacts[i].hostile && v->contacts[i].range < 2500;
            snprintf(t, sizeof t, n ? "HOSTILE %d" : "CLEAR", n);
            lamp(r, "DANGER", t, HUD_WARN, n > 0, n > 0);
            break;
        }
        case L_TARGET: {
            const game_contact *c = v->selected >= 0 && v->contacts[v->selected].active ? &v->contacts[v->selected] : NULL;
            if (c) {
                if (c->range >= 1000) snprintf(t, sizeof t, "%.1fkm", c->range / 1000);
                else snprintf(t, sizeof t, "%.0fm", c->range);
            }
            lamp(r, "TARGET", c ? t : "NONE", c && c->hostile ? HUD_WARN : HUD_HI, c != NULL, false);
            break;
        }
        case L_CRUISE:
            snprintf(t, sizeof t, p->cruising ? "x%.0f" : p->cruise_charge > 0 ? "SPOOL" : "OFF", p->cruise_factor);
            lamp(r, "CRUISE", t, HUD_COOL, p->cruising || p->cruise_charge > 0, p->cruise_charge > 0 && !p->cruising);
            break;
        case L_MASS: lamp(r, "MASS LOCK", p->mass_locked ? "ON" : "OFF", HUD_HI, p->mass_locked, false); break;
        case L_SPEED:
            snprintf(t, sizeof t, "%.0f", cg_len(p->velocity));
            lamp(r, "SPEED", t, HUD, false, false);
            break;
        case L_DOCK:
            lamp(r, "DOCK", p->docked ? "DOCKED" : p->docking ? "COMPUTER" : "--", HUD_OK, p->docked || p->docking, p->docking);
            break;
        case L_CARGO: {
            int used = game_cargo_used(), cap = game_cargo_capacity();
            snprintf(t, sizeof t, "%d/%dt", used, cap);
            lamp(r, "CARGO", t, HUD_HI, used >= cap && cap > 0, false);
            break;
        }
        case L_MISSILE:
            snprintf(t, sizeof t, "x%d", p->missiles);
            lamp(r, "MISSILES", t, HUD_WARN, p->missiles > 0 && v->selected >= 0 && v->contacts[v->selected].hostile, false);
            break;
        case L_AUTO: lamp(r, "AUTOPILOT", ui.autopilot ? "ON" : "OFF", HUD_COOL, ui.autopilot, false); break;
        case L_LIVE: {
            int live = 0;
            for (int i = 0; i < GAME_CONTACTS; i++) live += v->contacts[i].active && v->contacts[i].live;
            snprintf(t, sizeof t, live ? "%d SHIPS" : "SIM", live);
            lamp(r, "RADIO", t, HUD_OK, live > 0, false);
            break;
        }
        }
    }
    bool hot = !p->docked;
    static const char *const cap[B_COUNT] = {"TAB", "5", "V", "6", "H", "P", "J", "O", "\\", "7", "M", "v", "<", "^", ">"};
    for (int k = 0; k < ui.nctls && k < B_COUNT; k++) {
        const ui_rect *r = &ui.ctls[k];
        key_cap = cap[k];
        switch (k) {
        case B_BOOST:
            snprintf(t, sizeof t, p->boost_cooldown > 0 ? "%.0fs" : "", p->boost_cooldown);
            button(r, "BOOST", t, hot && p->boost_cooldown <= 0, p->boost_left > 0, HUD_OK);
            break;
        case B_CHAFF: snprintf(t, sizeof t, "x%d", p->chaff); button(r, "CHAFF", t, hot && p->chaff, p->chaff_left > 0, HUD_OK); break;
        case B_SINK: snprintf(t, sizeof t, "x%d", p->heat_sinks); button(r, "HEAT SINK", t, hot && p->heat_sinks, false, HUD_OK); break;
        case B_CELL: snprintf(t, sizeof t, "x%d", p->shield_cells); button(r, "SHLD CELL", t, hot && p->shield_cells, false, HUD_COOL); break;
        case B_FOE: button(r, "NEAR FOE", "target", hot, false, HUD_WARN); break;
        case B_AUTO: button(r, "AUTOPILOT", ui.autopilot ? "on" : "to NAV", hot, ui.autopilot, HUD_COOL); break;
        case B_JUMP: {
            int s = fsd_state();
            button(r, "JUMP", s == FSD_READY ? "ready" : s == FSD_ALIGN ? "align" : s == FSD_CHARGE ? "charging" : "",
                   s == FSD_READY, s == FSD_CHARGE || s == FSD_JUMP, HUD_OK);
            break;
        }
        case B_VIEW: {
            static const char *const name[3] = {"cockpit", "chase", "tactical"};
            if (p->docked) button(r, "VIEW", "on foot", true, false, HUD_OK);
            else button(r, "VIEW", name[ui.view_mode % 3], hot, false, HUD);
            break;
        }
        case B_CAM: button(r, "FREE CAM", ui.free_cam ? "drag to look" : "chase view", hot, ui.free_cam, HUD_COOL); break;
        case B_ECM: button(r, "ECM", "", hot && (p->equipment & (1u << EQ_ECM)), false, HUD_OK); break;
        case B_MISSILE: snprintf(t, sizeof t, "x%d", p->missiles); button(r, "MISSILE", t, hot && p->missiles, false, HUD_WARN); break;
        case B_RESET: button(r, "PIPS", "even", hot, false, HUD); break;
        case B_SYS: case B_ENG: case B_WEP: {
            static const char *const pn[3] = {"SYS", "ENG", "WEP"};
            static const uint8_t pk[3] = {HUD_COOL, HUD_OK, HUD_WARN};
            int i = k - B_SYS, n = (int)(p->pips[i] + .5f);
            char bars[8];
            for (int j = 0; j < 4; j++) bars[j] = j * 2 < n ? '#' : '.';
            bars[4] = 0;
            button(r, pn[i], bars, hot, false, pk[i]);
            break;
        }
        }
    }
}

bool ui_console_tap(int col, int row) {
    const game_view *v = game_state();
    for (int k = 0; k < ui.nlamps && k < L_COUNT; k++) {
        if (!ui_in(&ui.lamps[k], col, row)) continue;
        switch (k) {
        case L_SHIELD: case L_HULL: case L_HEAT: game_panel_open(PANEL_STATUS); break;
        case L_FSD: case L_FUEL: game_panel_open(PANEL_GALAXY); ui_galaxy_open(); break;
        case L_CARGO: if (v->player.docked) { game_panel_open(PANEL_MARKET); ui.market_tab = 0; } else game_panel_open(PANEL_STATUS); break;
        case L_TARGET: case L_DANGER: game_target_nearest(k == L_DANGER); break;
        case L_AUTO: case L_DOCK: ui.mode = UI_MODE_NAV; break;
        case L_LIVE: ui.mode = UI_MODE_COMMS; break;
        default: return true;
        }
        lap_sound(LAP_SOUND_KEY);
        return true;
    }
    for (int k = 0; k < ui.nctls && k < B_COUNT; k++) {
        if (!ui_in(&ui.ctls[k], col, row)) continue;
        bool ok = true;
        switch (k) {
        case B_BOOST: ok = game_boost(); if (ok) ui_sound_boost(); break;
        case B_CHAFF: ok = game_chaff(); if (ok) ui_sound_kit(1); break;
        case B_SINK: ok = game_heat_sink(); if (ok) ui_sound_kit(2); break;
        case B_CELL: ok = game_shield_cell(); if (ok) ui_sound_kit(0); break;
        case B_FOE: game_target_nearest(true); break;
        case B_AUTO: {
            ui_dest d;
            ui.autopilot = !ui.autopilot && ui_nav_target(&d);
            if (ui.voice) lap_say(ui.autopilot ? "Autopilot engaged" : "Autopilot off");
            break;
        }
        case B_JUMP: ok = ui_fsd_engage(); break;
        case B_VIEW:            /* docked, out of the seat; else cockpit, chase, tactical */
            if (v->player.docked) ui_walk_start();
            else ui.view_mode = (ui.view_mode + 1) % 3;
            break;
        case B_CAM:
            ui.free_cam = !ui.free_cam;
            if (ui.free_cam) ui.view_mode = UI_VIEW_CHASE;
            else render_cam_yaw = render_cam_pitch = 0;
            break;
        case B_ECM: ok = game_ecm(); break;
        case B_MISSILE: ok = game_missile(); break;
        case B_RESET: game_pips_preset(0); break;
        case B_SYS: case B_ENG: case B_WEP: game_pip(k - B_SYS); break;
        }
        lap_sound(ok ? LAP_SOUND_KEY : LAP_SOUND_ERROR);
        return true;
    }
    return false;
}

/* J, or the JUMP button: engage when the drive is ready, else say why not. */
bool ui_fsd_engage(void) {
    const game_view *v = game_state();
    if (v->plotted < 0) { if (ui.voice) lap_say("No route plotted."); return false; }
    if (v->jump != JUMP_IDLE) return false;
    if (v->action == ACT_JUMP) { game_context(); return true; }   /* the rules say what stops it */
    if (v->player.mass_locked && ui.voice) lap_say("Mass locked.");
    return false;
}
