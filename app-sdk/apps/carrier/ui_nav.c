/* The dash's switchable middle: FLIGHT (scanner and gauges, ui_dash.c), NAV
 * (where to go, and an autopilot that flies there) and COMMS (what has been
 * said, who is near, the mesh). The throttle stays put in every mode. */
#include "internal.h"
#include "art.h"
#include "ui.h"

static char line[80];
static const char *const roman[6] = {"I", "II", "III", "IV", "V", "VI"};

/* ---- destinations ------------------------------------------------------ */

int ui_nav_list(ui_dest *out, int max) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    const char *sys = v->systems[v->system].name;
    int n = 0;
    if (n < max) {
        out[n] = (ui_dest){UI_NAV_STATION, 0, v->contents.station, 60, ""};
        snprintf(out[n].name, sizeof out[n].name, "%s",
                 v->contents.station_name[0] ? v->contents.station_name : "Station");
        n++;
    }
    for (int i = 0; i < v->contents.planet_count && n < max; i++) {
        const game_planet *g = &v->contents.planets[i];
        out[n] = (ui_dest){UI_NAV_PLANET, i, g->position, g->radius, ""};
        snprintf(out[n].name, sizeof out[n].name, "%s %s", sys, roman[i % 6]);
        n++;
    }
    if (n < max) {
        out[n] = (ui_dest){UI_NAV_STAR, 0, v->contents.star, v->contents.star_radius, ""};
        snprintf(out[n].name, sizeof out[n].name, "%s star", sys);
        n++;
    }
    for (int i = 0; i < v->poi_count && n < max; i++) {    /* places, once found or in range */
        const game_poi *q = &v->pois[i];
        if (!q->discovered && !game_poi_detectable(v->player.position, q->position)) continue;
        out[n] = (ui_dest){UI_NAV_POI, i, q->position, q->kind == POI_NEBULA ? 0 : q->size * .5f, ""};
        snprintf(out[n].name, sizeof out[n].name, "%s%s", q->discovered ? q->name : "Unknown source",
                 q->scanned ? "" : " ?");
        n++;
    }
    for (int i = 0; i < ui_net_count() && n < max; i++) {
        const ui_peer *c = ui_net_peer(i);
        out[n] = (ui_dest){UI_NAV_PEER, i, c->position, 12, ""};
        snprintf(out[n].name, sizeof out[n].name, "CMDR %s", c->name);
        n++;
    }
    /* ships, nearest first */
    int order[GAME_CONTACTS], k = 0;
    for (int i = 0; i < GAME_CONTACTS; i++)
        if (v->contacts[i].active) order[k++] = i;
    for (int a = 1; a < k; a++)
        for (int b = a; b > 0 && v->contacts[order[b]].range < v->contacts[order[b - 1]].range; b--) {
            int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    for (int j = 0; j < k && n < max; j++) {
        const game_contact *c = &v->contacts[order[j]];
        const game_ship_stats *st = game_ship(c->ship_class);
        out[n] = (ui_dest){UI_NAV_CONTACT, order[j], c->position, st ? st->radius : 10, ""};
        snprintf(out[n].name, sizeof out[n].name, "%s", c->name[0] ? c->name : st ? st->name : "ship");
        n++;
    }
    (void)p;
    return n;
}

/* The chosen destination now (contacts and peers move). */
bool ui_nav_target(ui_dest *out) {
    const game_view *v = game_state();
    switch (ui.nav_kind) {
    case UI_NAV_STATION:
        *out = (ui_dest){UI_NAV_STATION, 0, v->contents.station, 60, ""};
        snprintf(out->name, sizeof out->name, "%s", v->contents.station_name[0] ? v->contents.station_name : "Station");
        return true;
    case UI_NAV_PLANET:
        if (ui.nav_index >= v->contents.planet_count) return false;
        *out = (ui_dest){UI_NAV_PLANET, ui.nav_index, v->contents.planets[ui.nav_index].position,
                         v->contents.planets[ui.nav_index].radius, ""};
        snprintf(out->name, sizeof out->name, "%s %s", v->systems[v->system].name, roman[ui.nav_index % 6]);
        return true;
    case UI_NAV_STAR:
        *out = (ui_dest){UI_NAV_STAR, 0, v->contents.star, v->contents.star_radius, "Star"};
        return true;
    case UI_NAV_POI: {
        if (ui.nav_index < 0 || ui.nav_index >= v->poi_count) return false;
        const game_poi *q = &v->pois[ui.nav_index];
        *out = (ui_dest){UI_NAV_POI, ui.nav_index, q->position, q->kind == POI_NEBULA ? 0 : q->size * .5f, ""};
        snprintf(out->name, sizeof out->name, "%s", q->discovered ? q->name : "Unknown source");
        return true;
    }
    case UI_NAV_PEER:
        ui.nav_index = -1;
        for (int k = 0; k < ui_net_count(); k++)
            if (!strcmp(ui_net_peer(k)->id, ui.nav_peer)) ui.nav_index = k;
        if (ui.nav_index < 0) return false;
        *out = (ui_dest){UI_NAV_PEER, ui.nav_index, ui_net_peer(ui.nav_index)->position, 12, ""};
        snprintf(out->name, sizeof out->name, "CMDR %s", ui_net_peer(ui.nav_index)->name);
        return true;
    case UI_NAV_CONTACT: {
        const game_contact *c = &v->contacts[ui.nav_index];
        if (ui.nav_index < 0 || ui.nav_index >= GAME_CONTACTS || !c->active) return false;
        const game_ship_stats *st = game_ship(c->ship_class);
        *out = (ui_dest){UI_NAV_CONTACT, ui.nav_index, c->position, st ? st->radius : 10, ""};
        snprintf(out->name, sizeof out->name, "%s", c->name[0] ? c->name : st ? st->name : "ship");
        return true;
    }
    default:
        return false;
    }
}

/* Where to stop short of it. */
static float stand_off(const ui_dest *d) {
    switch (d->kind) {
    case UI_NAV_PLANET: return d->radius + 450;
    case UI_NAV_STAR: return d->radius * 3.2f;
    case UI_NAV_STATION: return 700;
    case UI_NAV_POI: {      /* inside its scan range, outside anything that bites */
        const game_poi *q = &game_state()->pois[d->index];
        if (q->kind == POI_DARK_STAR || q->kind == POI_PULSAR) return q->radius + 120;
        if (q->kind == POI_NEBULA) return 0;
        return q->scan_range * .6f;
    }
    default: return 140;
    }
}

static void set_autopilot(bool on, const char *why) {
    if (ui.autopilot == on) return;
    ui.autopilot = on;
    if (ui.voice && why) lap_say(why);
    if (!on) game_yaw(0);
}

/* ---- the autopilot: turn to it, run, slow, stop short, dock if it is home ---- */

void ui_autopilot(float dt) {
    (void)dt;
    const game_view *v = game_state();
    const game_player *p = &v->player;
    if (!ui.autopilot) return;
    ui_dest d;
    if (p->docked || p->dead || v->jump != JUMP_IDLE || p->docking || !ui_nav_target(&d)) {
        set_autopilot(false, NULL);
        return;
    }
    /* any hand on the stick takes it back */
    if ((ui.grab == UI_G_VIEW && ui.dragging) || (ui.down && ui.grab == UI_G_THROTTLE) || ui.throttle_touched ||
        fabsf(ui.key_pitch) > .3f || fabsf(ui.key_roll) > .3f || fabsf(ui.key_yaw) > .3f) {
        ui.throttle_touched = false;
        set_autopilot(false, "Autopilot off");
        return;
    }
    l3_vec f = l3_quat_rotate(p->attitude, l3v(0, 0, -1)), up = l3_quat_rotate(p->attitude, l3v(0, 1, 0));
    l3_vec rt = l3_cross(f, up), to = l3_sub(d.position, p->position);
    float dist = cg_len(to);
    float stop = stand_off(&d);
    /* steer around a world that is not the destination */
    for (int i = 0; i < v->contents.planet_count; i++) {
        if (d.kind == UI_NAV_PLANET && d.index == i) continue;
        const game_planet *g = &v->contents.planets[i];
        l3_vec og = l3_sub(g->position, p->position);
        float along = l3_dot(og, l3_norm(to));
        if (along <= 0 || along > dist) continue;
        l3_vec closest = l3_sub(og, l3_scale(l3_norm(to), along));
        float miss = cg_len(closest);
        if (miss < g->radius + 500) {    /* aim past its limb instead */
            l3_vec side = miss > 1 ? l3_scale(closest, -1 / miss) : up;
            to = l3_add(og, l3_scale(side, g->radius + 900));
            dist = cg_len(to) + stop;
            break;
        }
    }
    float lx = l3_dot(to, rt), ly = l3_dot(to, up), lz = l3_dot(to, f);
    float yaw = -atan2f(lx, lz), pitch = atan2f(ly, sqrtf(lx * lx + lz * lz) * (lz < 0 ? -1 : 1));
    game_stick(cg_clamp(pitch * 1.8f, -1, 1), 0);
    game_yaw(cg_clamp(yaw * 1.8f, -1, 1));
    float aligned = lz / fmaxf(1, sqrtf(lx * lx + ly * ly + lz * lz));
    const game_ship_stats *me = game_player_ship();
    float vmax = me ? me->speed : 100;
    float want = cg_clamp((dist - stop) / (vmax * 3.5f), 0, 1);
    if (aligned < .85f) want = fminf(want, .2f);
    if (game_state()->interdiction.active) {      /* fight the tug: aim at the escape marker */
        l3_vec e = game_state()->interdiction.escape;
        float ex = l3_dot(e, rt), ey = l3_dot(e, up), ez = l3_dot(e, f);
        game_stick(cg_clamp(atan2f(ey, ez) * 2, -1, 1), 0);
        game_yaw(cg_clamp(-atan2f(ex, ez) * 2, -1, 1));
        game_throttle(1);
        return;
    }
    /* long legs in cruise; the drive drops out by itself near anything solid */
    if (!p->cruising && !p->mass_locked && dist - stop > 4000 && aligned > .97f) game_cruise(true);
    if (p->cruising && (dist - stop < 2500 || aligned < .6f)) game_cruise(false);
    if (p->cruising) want = cg_clamp((dist - stop) / 12000.f, .15f, 1);
    game_throttle(want);
    if (dist - stop < 25) {
        game_throttle(0);
        set_autopilot(false, d.kind == UI_NAV_STATION ? NULL : "Destination reached");
        if (d.kind == UI_NAV_STATION && v->action == ACT_DOCK && !p->docking) game_context();
    }
}

/* ---- the mode bar ------------------------------------------------------- */

void ui_modebar_draw(void) {
    const game_view *v = game_state();
    static const char *const names[UI_MODES] = {"FLIGHT", "NAV", "COMMS", "SYS"};
    for (int i = 0; i < UI_MODES; i++) {
        const char *s = names[i];
        if (i == UI_MODE_NAV && ui.autopilot) s = ui.mode_keys[i].w >= 11 ? "NAV AUTO" : "AUTO";
        if (i == UI_MODE_COMMS && ui_net_unread()) s = "COMMS *";
        if (ui.mode_keys[i].w <= 0) continue;
        ui_mode_key(&ui.mode_keys[i], s, ui.mode == i, i && ui.mode_keys[i].y == ui.mode_keys[i - 1].y);
    }
    /* the corner: what the radio gives you (only when there is room for it) */
    const ui_rect *r = &ui.mode_info;
    if (r->w <= 0) return;
    lap_fill(r->x, r->y, r->w, r->h, ' ', LAP_ATTR(HUD, 0));
    int live = 0;
    for (int i = 0; i < GAME_CONTACTS; i++) live += v->contacts[i].active && v->contacts[i].live;
    if (live) {
        snprintf(line, sizeof line, "LIVE %d", live);
        lap_put_cell(r->x + 1, r->y + 1, LAP_BRAILLE(0x36), LAP_ATTR(HUD_OK, 0));
        lap_put_str(r->x + 3, r->y + 1, line, LAP_ATTR(HUD_OK, 0));
    } else
        lap_put_str(r->x + 1, r->y + 1, "SIM", LAP_ATTR(HUD_SUB, 0));
    if (ui_net_count()) {
        snprintf(line, sizeof line, "MESH %d", ui_net_count());
        lap_put_str(r->x + 1, r->y + 2, line, LAP_ATTR(HUD_COOL, 0));
    } else if (ui.mesh_on)
        lap_put_str(r->x + 1, r->y + 2, "MESH ON", LAP_ATTR(HUD_SUB, 0));
}

/* ---- NAV --------------------------------------------------------------- */

static ui_dest dest[16];
static int ndest;
static ui_rect nav_rows[8], nav_keys[2];
static int nav_rows_n, nav_first;

void ui_nav_draw(const ui_rect *a) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    ndest = ui_nav_list(dest, 16);
    int rows = (a->h - 4) / 2;
    if (rows > 8) rows = 8;
    if (ui.nav_scroll > ndest - rows) ui.nav_scroll = ndest - rows;
    if (ui.nav_scroll < 0) ui.nav_scroll = 0;
    nav_first = ui.nav_scroll;
    nav_rows_n = 0;
    lap_fill(a->x, a->y, a->w, a->h, ' ', LAP_ATTR(HUD, 0));
    for (int k = 0; k < rows && nav_first + k < ndest; k++) {
        const ui_dest *d = &dest[nav_first + k];
        bool sel = d->kind == ui.nav_kind && d->index == ui.nav_index;
        uint8_t bg = sel ? HUD_DIM : LAP_BLACK;
        uint8_t fg = d->kind == UI_NAV_CONTACT ? (v->contacts[d->index].hostile ? HUD_WARN
                     : v->contacts[d->index].live ? HUD_COOL : HUD)
                   : d->kind == UI_NAV_PEER ? HUD_COOL : sel ? HUD_HI : HUD;
        ui_rect r = {a->x, a->y + k * 2, a->w, 2};
        nav_rows[nav_rows_n++] = r;
        lap_fill(r.x, r.y, r.w, 2, ' ', LAP_ATTR(fg, bg));
        static const char *const icon[] = {"", "#", "o", "*", "+", "@", "%"};
        snprintf(line, sizeof line, "%s %s", icon[d->kind], d->name);
        lap_put_str(r.x + 1, r.y, line, LAP_ATTR(fg, bg));
        float dist = cg_len(l3_sub(d->position, p->position)) - (d->kind == UI_NAV_PLANET ? d->radius : 0);
        float eta = p->cruising || dist > 4000 ? game_cruise_eta(d->position) : p->speed > 1 ? dist / p->speed : -1;
        char eta_s[16] = "";
        if (eta >= 0 && eta < 6000) snprintf(eta_s, sizeof eta_s, "  %d:%02d", (int)eta / 60, (int)eta % 60);
        if (dist >= 1000) snprintf(line, sizeof line, "%.1f km%s", dist / 1000, eta_s);
        else snprintf(line, sizeof line, "%.0f m%s", dist, eta_s);
        lap_put_str(r.x + 3, r.y + 1, line, LAP_ATTR(sel ? HUD_HI : HUD_SUB, bg));
    }
    ui_dest t;
    bool have = ui_nav_target(&t);
    nav_keys[0] = (ui_rect){a->x, a->y + a->h - 4, a->w * 2 / 3, 4};
    nav_keys[1] = (ui_rect){a->x + a->w * 2 / 3, a->y + a->h - 4, a->w - a->w * 2 / 3, 4};
    ui_key_box(&nav_keys[0], ui.autopilot ? "AUTOPILOT ON" : "AUTOPILOT", have ? t.name : "pick a place",
               have && !p->docked ? HUD_HI : HUD_SUB, LAP_BLACK, ui.autopilot);
    ui_key_box(&nav_keys[1], ndest > rows ? "MORE" : "-", "", ndest > rows ? HUD : HUD_SUB, LAP_BLACK, false);
}

bool ui_nav_tap(int col, int row) {
    const game_view *v = game_state();
    for (int k = 0; k < nav_rows_n; k++)
        if (ui_in(&nav_rows[k], col, row)) {
            const ui_dest *d = &dest[nav_first + k];
            ui.nav_kind = d->kind;
            ui.nav_index = d->index;
            if (d->kind == UI_NAV_PEER && ui_net_peer(d->index))
                snprintf(ui.nav_peer, sizeof ui.nav_peer, "%s", ui_net_peer(d->index)->id);
            if (d->kind == UI_NAV_CONTACT)
                for (int n = 0; n < GAME_CONTACTS && v->selected != d->index; n++) game_target_next();
            lap_sound(LAP_SOUND_KEY);
            return true;
        }
    if (ui_in(&nav_keys[0], col, row)) {
        ui_dest t;
        if (ui.autopilot) set_autopilot(false, "Autopilot off");
        else {
            if (!ui_nav_target(&t)) { ui.nav_kind = UI_NAV_STATION; ui.nav_index = 0; }   /* home by default */
            if (ui_nav_target(&t) && !v->player.docked) set_autopilot(true, "Autopilot engaged");
            else lap_sound(LAP_SOUND_ERROR);
        }
        return true;
    }
    if (ui_in(&nav_keys[1], col, row)) {
        ui.nav_scroll += 3;
        if (ui.nav_scroll >= ndest) ui.nav_scroll = 0;
        return true;
    }
    return false;
}

/* ---- COMMS -------------------------------------------------------------- */

static ui_rect comms_keys[4], reply_keys[UI_REPLIES], peer_rows[4];
static int peer_rows_n;
static const char *const replies[UI_REPLIES] = {"o7", "Need fuel", "Pirates here!", "Trade?", "Meet at station"};

void ui_comms_draw(const ui_rect *a) {
    const game_view *v = game_state();
    lap_fill(a->x, a->y, a->w, a->h, ' ', LAP_ATTR(HUD, 0));
    int keys_h = 4, y = a->y;
    if (ui.comms_tab == 2) {       /* the local news feed */
        lap_put_str(a->x + 1, y, v->systems[v->system].name, LAP_ATTR(HUD_HI, 0));
        lap_put_str(a->x + 2 + (int)strlen(v->systems[v->system].name), y, "NEWS", LAP_ATTR(HUD_SUB, 0));
        int r = 1;
        for (int i = 0; i < 4 && r < a->h - keys_h; i++) {
            if (!v->news[i][0]) continue;
            snprintf(line, sizeof line, "%.*s", a->w - 2, v->news[i]);
            lap_put_str(a->x + 1, y + r, line, LAP_ATTR(i == 0 ? HUD : HUD_SUB, 0));
            r += 1;
            if ((int)strlen(v->news[i]) > a->w - 2 && r < a->h - keys_h) {
                snprintf(line, sizeof line, "%.*s", a->w - 3, v->news[i] + a->w - 2);
                lap_put_str(a->x + 2, y + r, line, LAP_ATTR(i == 0 ? HUD : HUD_SUB, 0));
                r++;
            }
        }
        if (v->event_text[0] && r < a->h - keys_h) lap_put_str(a->x + 1, y + r, v->event_text, LAP_ATTR(HUD_COOL, 0));
    } else if (ui.comms_tab == 0) {       /* the log: newest at the top */
        int rows = a->h - keys_h;
        for (int i = 0; i < v->message_count && i < rows; i++) {
            snprintf(line, sizeof line, "%.*s", a->w - 1, v->messages[i]);
            lap_put_str(a->x + 1, y + i, line, LAP_ATTR(i == 0 ? HUD_HI : i < 3 ? HUD : HUD_SUB, 0));
        }
        if (!v->message_count) lap_put_str(a->x + 1, y, "Quiet. No traffic yet.", LAP_ATTR(HUD_SUB, 0));
    } else {                        /* the mesh: who is out there, what they said */
        ui_net_mark_read();
        peer_rows_n = 0;
        if (!ui.mesh_on) {
            lap_put_str(a->x + 1, y, "Mesh is off. Other CARRIER", LAP_ATTR(HUD, 0));
            lap_put_str(a->x + 1, y + 1, "players nearby show up as", LAP_ATTR(HUD, 0));
            lap_put_str(a->x + 1, y + 2, "ships when it is on.", LAP_ATTR(HUD, 0));
            snprintf(line, sizeof line, "Channel slot %d. %s", ui.mesh_slot,
                     ui_net_tx_ok() ? "Transmit is armed." : "Arm TRANSMIT in MESH.");
            lap_put_str(a->x + 1, y + 4, line, LAP_ATTR(ui_net_tx_ok() ? HUD_OK : HUD_WARN, 0));
            if (ui.mesh_slot) {         /* a shared channel needs no key typed: everyone types the same name */
                snprintf(line, sizeof line, "MESH CHANNEL %d: type #carrier", ui.mesh_slot);
                lap_put_str(a->x + 1, y + 5, line, LAP_ATTR(HUD_SUB, 0));
            }
        } else {
            int rows = a->h - keys_h - (ui_net_count() ? 4 : 0);
            for (int i = 0; i < ui_net_lines() && i < rows; i++) {
                const ui_line *l = ui_net_line(i);
                snprintf(line, sizeof line, "%.*s", a->w - 1, l->text);
                lap_put_str(a->x + 1, y + i, line, LAP_ATTR(l->mine ? HUD_SUB : l->carrier ? HUD_COOL : HUD, 0));
            }
            if (!ui_net_lines()) lap_put_str(a->x + 1, y, "Listening on the mesh...", LAP_ATTR(HUD_SUB, 0));
            /* quick replies: one tap sends */
            if (ui_net_count() || 1) {
                int ry = a->y + a->h - keys_h - 3, x = a->x;
                for (int i = 0; i < UI_REPLIES; i++) {
                    int w = (int)strlen(replies[i]) + 2;
                    if (x + w > a->x + a->w) break;
                    reply_keys[i] = (ui_rect){x, ry, w, 3};
                    ui_key_box(&reply_keys[i], replies[i], "", HUD_COOL, LAP_BLACK, false);
                    x += w + 1;
                }
            }
        }
    }
    static const char *const tabs[3] = {"LOG", "MESH", "NEWS"};
    for (int i = 0; i < 3; i++) {
        comms_keys[i] = (ui_rect){a->x + a->w * i / 4, a->y + a->h - keys_h, a->w / 4, keys_h};
        ui_key_box(&comms_keys[i], tabs[i], "", HUD, LAP_BLACK, ui.comms_tab == i);
    }
    comms_keys[3] = (ui_rect){a->x + a->w * 3 / 4, a->y + a->h - keys_h, a->w - a->w * 3 / 4, keys_h};
    if (ui.comms_tab != 1) {
        bool can = v->action == ACT_HAIL;
        ui_key_box(&comms_keys[3], "HAIL", can ? "" : "no one near", can ? HUD_HI : HUD_SUB, LAP_BLACK, false);
    } else
        ui_key_box(&comms_keys[3], ui.mesh_on ? "ON" : "OFF", "", ui.mesh_on ? HUD_COOL : HUD_SUB,
                   LAP_BLACK, ui.mesh_on);
    (void)peer_rows;
}

bool ui_comms_tap(int col, int row) {
    const game_view *v = game_state();
    for (int i = 0; i < 3; i++)
        if (ui_in(&comms_keys[i], col, row)) { ui.comms_tab = i; lap_sound(LAP_SOUND_KEY); return true; }
    if (ui_in(&comms_keys[3], col, row)) {
        if (ui.comms_tab != 1) {
            if (v->action == ACT_HAIL) game_context(); else lap_sound(LAP_SOUND_ERROR);
        } else {
            ui.mesh_on = !ui.mesh_on;
            ui_save_settings();
            if (ui.mesh_on) ui_net_announce();
            lap_sound(LAP_SOUND_KEY);
        }
        return true;
    }
    if (ui.comms_tab == 1 && ui.mesh_on)
        for (int i = 0; i < UI_REPLIES; i++)
            if (reply_keys[i].w && ui_in(&reply_keys[i], col, row)) {
                if (ui_net_say(replies[i])) lap_sound(LAP_SOUND_ROGER); else lap_sound(LAP_SOUND_ERROR);
                return true;
            }
    return false;
}
