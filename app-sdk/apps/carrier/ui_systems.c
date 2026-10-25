/* The fight's controls and read-outs past the guns: the SYS dash mode (power to
 * shields, engines or weapons; shield cells, chaff, heat sinks, ECM; which part of
 * the target to shoot), each weapon's own look on the glass, the red arcs that show
 * where fire comes from, the missile warning, scans, and the cockpit's air and heat. */
#include "internal.h"
#include "art.h"
#include "ui.h"

static char line[64];
static const char *const module_names[GAME_MODULES] = {"POWER", "DRIVES", "WEAPONS", "SHIELDS", "SENSORS", "LIFE", "CARGO"};
static const char *const weapon_names[WEAPON_COUNT] = {"PULSE", "BEAM", "RATTLE", "SLUG", "SEEKER", "DUMBFIRE"};

const char *ui_weapon_name(int kind) { return kind >= 0 && kind < WEAPON_COUNT ? weapon_names[kind] : "-"; }

static inline void dot(int x, int y, uint8_t ink) {
    if (x >= 0 && y >= 0 && x < ui.dw && y < ui.dh) ui.dots[y * ui.dw + x] = ink;
}
static void dline(int x0, int y0, int x1, int y1, uint8_t ink, int skip, int phase) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (int n = 0; n < 2000; n++) {
        if (!skip || ((n + phase) % skip) < skip / 2) dot(x0, y0, ink);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

/* ---- SYS: the dash's fourth mode ------------------------------------------- */

static ui_rect pip_cols[3], preset_keys[4], kit_keys[4], sub_key;

void ui_sys_draw(const ui_rect *a) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    lap_fill(a->x, a->y, a->w, a->h, ' ', LAP_ATTR(HUD, 0));
    bool roomy = a->h >= 14;
    int ph = roomy ? 5 : 4;
    static const char *const bars[3] = {"SYS", "ENG", "WEP"};
    static const uint8_t ink[3] = {HUD_COOL, HUD_OK, HUD_WARN};
    for (int i = 0; i < 3; i++) {
        int x = a->x + a->w * i / 3, w = a->x + a->w * (i + 1) / 3 - x;
        pip_cols[i] = (ui_rect){x, a->y, w, ph};
        float pips = p->pips[i];
        /* four pips, each two halves, as stacked blocks */
        for (int k = 0; k < 4; k++) {
            float f = pips - k;
            int16_t cell = f >= 1 ? LAP_BLOCK : f >= .5f ? LAP_SHADE_50 : LAP_BRAILLE(0x24);
            for (int c = 0; c < 3; c++)
                lap_put_cell(x + (w - 3) / 2 + c, a->y + ph - 2 - k * (ph - 1) / 4, cell, LAP_ATTR(f > 0 ? ink[i] : HUD_DIM, 0));
        }
        snprintf(line, sizeof line, "%s %.1f", bars[i], pips);
        lap_put_str(x + (w - (int)strlen(line)) / 2, a->y + ph - 1, line, LAP_ATTR(ink[i], 0));
    }
    int y = a->y + ph;
    if (roomy) {
        static const char *const pres[4] = {"EVEN", "SHIELD", "ATTACK", "RUN"};
        for (int i = 0; i < 4; i++) {
            int x = a->x + a->w * i / 4, w = a->x + a->w * (i + 1) / 4 - x;
            preset_keys[i] = (ui_rect){x, y, w, 3};
            ui_key_box(&preset_keys[i], pres[i], "", HUD, LAP_BLACK, false);
        }
        y += 3;
    } else
        for (int i = 0; i < 4; i++) preset_keys[i] = (ui_rect){0, 0, 0, 0};
    char sub[4][16];
    snprintf(sub[0], 16, "x%d", p->shield_cells);
    snprintf(sub[1], 16, "x%d", p->chaff);
    snprintf(sub[2], 16, "x%d", p->heat_sinks);
    snprintf(sub[3], 16, "%s", p->equipment & (1u << EQ_ECM) ? "" : "none");
    static const char *const kit[4] = {"CELL", "CHAFF", "SINK", "ECM"};
    bool has[4] = {p->shield_cells > 0, p->chaff > 0, p->heat_sinks > 0, (p->equipment & (1u << EQ_ECM)) != 0};
    for (int i = 0; i < 4; i++) {
        int x = a->x + a->w * i / 4, w = a->x + a->w * (i + 1) / 4 - x;
        kit_keys[i] = (ui_rect){x, y, w, 3};
        ui_key_box(&kit_keys[i], kit[i], sub[i], has[i] ? HUD_HI : HUD_SUB, LAP_BLACK, i == 1 && p->chaff_left > 0);
    }
    y += 3;
    if (y + 3 <= a->y + a->h) {
        sub_key = (ui_rect){a->x, y, a->w, a->y + a->h - y};
        const game_contact *c = v->selected >= 0 && v->contacts[v->selected].active ? &v->contacts[v->selected] : NULL;
        if (c && c->scanned && v->subtarget >= 0 && v->subtarget < GAME_MODULES)
            snprintf(line, sizeof line, "AIM: %s %d%%", module_names[v->subtarget], (int)(c->modules[v->subtarget] * 100));
        else if (c && c->scanned) snprintf(line, sizeof line, "AIM: HULL");
        else if (c) snprintf(line, sizeof line, "SCANNING %d%%", (int)(c->scan_progress * 100));
        else snprintf(line, sizeof line, "NO TARGET");
        ui_key_box(&sub_key, line, c && c->scanned ? "tap: next part" : "", c && c->scanned ? HUD_HI : HUD_SUB, LAP_BLACK, false);
    } else
        sub_key = (ui_rect){0, 0, 0, 0};
}

bool ui_sys_tap(int col, int row) {
    for (int i = 0; i < 3; i++)
        if (ui_in(&pip_cols[i], col, row)) { game_pip(i); lap_sound(LAP_SOUND_KEY); return true; }
    for (int i = 0; i < 4; i++)
        if (preset_keys[i].w && ui_in(&preset_keys[i], col, row)) { game_pips_preset(i); lap_sound(LAP_SOUND_KEY); return true; }
    for (int i = 0; i < 4; i++)
        if (ui_in(&kit_keys[i], col, row)) {
            bool ok = i == 0 ? game_shield_cell() : i == 1 ? game_chaff() : i == 2 ? game_heat_sink() : game_ecm();
            if (ok) ui_sound_kit(i); else lap_sound(LAP_SOUND_ERROR);
            return true;
        }
    if (sub_key.w && ui_in(&sub_key, col, row)) { game_subtarget_next(); lap_sound(LAP_SOUND_KEY); return true; }
    return false;
}

/* ---- your guns, each with its own look ----------------------------------------- */

void ui_weapons_dots(void) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    if (p->docked || v->jump != JUMP_IDLE) return;
    int cx = ui.dw / 2, cy = ui.dh / 2;
    /* mounts: two under the canopy's sill, two higher at the struts */
    static const float mx[GAME_MOUNTS] = {-.46f, .46f, -.4f, .4f}, my[GAME_MOUNTS] = {.48f, .48f, .2f, .2f};
    int phase = (int)(ui.t * 60);
    for (int m = 0; m < GAME_MOUNTS && m < (int)(p->mounts ? p->mounts : 2); m++) {
        const game_mount *w = &p->weapons[m];
        if (w->kind >= WEAPON_COUNT) continue;
        int x0 = cx + (int)(mx[m] * ui.dw), y0 = cy + (int)(my[m] * ui.dh);
        if (w->kind == WEAPON_SLUG && w->charge > 0) {   /* the slug gun winds up: a growing glow */
            int r = 1 + (int)(w->charge * 3);
            for (int yy = -r; yy <= r; yy++)
                for (int xx = -r; xx <= r; xx++)
                    if (xx * xx + yy * yy <= r * r) dot(x0 + xx, y0 + yy, w->charge > .7f ? 15 : 12);
        }
        /* a shot is one frame in the game; its light lingers a little on the glass */
        static float vis[GAME_MOUNTS], heat_was[GAME_MOUNTS];
        bool shot = w->firing || w->heat > heat_was[m] + .004f;   /* each shot warms the mount */
        heat_was[m] = w->heat;
        if (shot) vis[m] = w->kind == WEAPON_SLUG ? .12f : w->kind == WEAPON_BEAM ? .08f : .12f;
        else vis[m] = fmaxf(0, vis[m] - ui.dt);
        if (vis[m] <= 0) continue;
        switch (w->kind) {
        case WEAPON_PULSE:      /* bolts racing to the point of aim */
            dline(x0, y0, cx, cy + 2, 9, 6, phase);
            dline(x0 + 1, y0, cx + 1, cy + 2, 15, 6, phase + 1);
            break;
        case WEAPON_BEAM:       /* a solid beam, its core flickering */
            dline(x0, y0, cx, cy + 2, (phase & 2) ? 9 : LAP_RED, 0, 0);
            dline(x0 + 1, y0, cx + 1, cy + 2, (phase & 1) ? 15 : 11, 0, 0);
            dline(x0 - 1, y0, cx - 1, cy + 2, LAP_RED, 0, 0);
            break;
        case WEAPON_RATTLE:     /* a muzzle flash; the tracers are in the scene */
            if (phase & 2) for (int k = -1; k <= 1; k++) { dot(x0 + k, y0, 11); dot(x0, y0 + k, 11); }
            break;
        case WEAPON_SLUG:       /* the shot: a white line, then gone */
            dline(x0, y0, cx, cy + 2, 15, 0, 0);
            dline(x0 + 1, y0, cx + 1, cy + 2, 12, 0, 0);
            break;
        default: break;
        }
    }
}

/* ---- incoming fire: red arcs round the reticle toward the shooter -------------- */

void ui_incoming_dots(void) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    l3_vec f = l3_quat_rotate(p->attitude, l3v(0, 0, -1)), up = l3_quat_rotate(p->attitude, l3v(0, 1, 0));
    l3_vec rt = l3_cross(f, up);
    int cx = ui.dw / 2, cy = ui.dh / 2;
    for (int k = 0; k < p->hit_count && k < GAME_HIT_DIRECTIONS; k++) {
        const game_hit_direction *h = &p->hit_directions[k];
        if (h->age > 1.5f) continue;
        float x = l3_dot(h->direction, rt), y = l3_dot(h->direction, up), z = l3_dot(h->direction, f);
        float a = atan2f(-y, x);
        float r = z < 0 ? 22 : 15;                 /* from behind: a wider arc */
        uint8_t ink = h->age < .5f ? HUD_WARN : LAP_RED;
        for (int s = -10; s <= 10; s++) {
            float b = a + s * .035f;
            dot(cx + UI_ROUND(cosf(b) * r * 1.2f), cy + UI_ROUND(sinf(b) * r), ink);
            if (h->age < .5f) dot(cx + UI_ROUND(cosf(b) * (r + 1) * 1.2f), cy + UI_ROUND(sinf(b) * (r + 1)), ink);
        }
    }
    /* breathing frost at the edges once the canopy is gone */
    if (p->canopy <= 0 && !p->docked)
        for (int k = 0; k < 40; k++) {
            uint32_t h = cg_hash((uint32_t)k * 2654435761u + (uint32_t)(ui.t * 7));
            int edge = h % 4, t = (int)((h >> 3) % 1000);
            int x = edge == 0 ? (int)(h >> 13) % 6 : edge == 1 ? ui.dw - 1 - (int)(h >> 13) % 6 : t * ui.dw / 1000;
            int y = edge >= 2 ? (edge == 2 ? (int)(h >> 13) % 5 : ui.dh - 1 - (int)(h >> 13) % 5) : t * ui.dh / 1000;
            dot(x, y, (h >> 20) & 1 ? 15 : LAP_WHITE);
        }
}

/* ---- words over the view: missile, air, heat, shields, scans ------------------- */

void ui_combat_words(void) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    if (p->docked) return;
    int blink = ((int)(ui.t * 4)) & 1;
    int y = ui.view.y + 4;
    if (v->missile_warning && blink) {
        snprintf(line, sizeof line, " MISSILE %.0fm ", v->missile_range);
        lap_put_str(ui.view.x + (ui.view.w - (int)strlen(line)) / 2, y++, line, LAP_ATTR(LAP_BLACK, HUD_WARN));
    }
    if (p->canopy <= 0 && p->oxygen > 0) {
        int s = (int)p->oxygen;
        snprintf(line, sizeof line, " CANOPY BREACHED  OXYGEN %d:%02d ", s / 60, s % 60);
        lap_put_str(ui.view.x + (ui.view.w - (int)strlen(line)) / 2, y++, line,
                    LAP_ATTR(s < 60 && blink ? LAP_BLACK : HUD_WARN, s < 60 && blink ? HUD_WARN : 0));
    }
    if (p->heat_level > 1) {
        snprintf(line, sizeof line, "HEAT %d%%", (int)(p->heat_level * 100));
        ui_put(ui.view.x + (ui.view.w - (int)strlen(line)) / 2, y++, line, LAP_ATTR(blink ? HUD_WARN : HUD_HI, 0));
    }
    float reboot = fmaxf(p->shield_reboot[0], p->shield_reboot[1]);
    if (reboot > 0) {
        snprintf(line, sizeof line, "SHIELDS REBOOT %.0fs", reboot);
        ui_put(ui.view.x + (ui.view.w - (int)strlen(line)) / 2, y++, line, LAP_ATTR(HUD_COOL, 0));
    }
    if (p->seeker_lock > 0 && p->seeker_lock < 1) {
        snprintf(line, sizeof line, "LOCKING %d%%", (int)(p->seeker_lock * 100));
        ui_put(ui.view.x + (ui.view.w - (int)strlen(line)) / 2, y++, line, LAP_ATTR(HUD_HI, 0));
    } else if (p->seeker_lock >= 1 && blink)
        ui_put(ui.view.x + (ui.view.w - 6) / 2, y++, "LOCKED", LAP_ATTR(HUD_WARN, 0));
    if (p->weapons_hot > 0) {          /* guns fired: the drives wait for them to cool */
        snprintf(line, sizeof line, "DRIVES IN %.1fs", p->weapons_hot);
        ui_put(ui.view.x + (ui.view.w - (int)strlen(line)) / 2, y++, line, LAP_ATTR(HUD_SUB, 0));
    }
    if (ui.zoom > 1.1f) {
        snprintf(line, sizeof line, "ZOOM %.1fx", ui.zoom);
        ui_put(ui.view.x + (ui.view.w - (int)strlen(line)) / 2, y++, line, LAP_ATTR(HUD_COOL, 0));
    }
}

/* Beside the hologram once the target is scanned: its parts, the aimed one marked. */
void ui_target_parts(int x, int y) {
    const game_view *v = game_state();
    if (v->selected < 0 || !v->contacts[v->selected].active) return;
    const game_contact *c = &v->contacts[v->selected];
    if (!c->scanned) {
        snprintf(line, sizeof line, "SCAN %d%%", (int)(c->scan_progress * 100));
        ui_put(x, y, line, LAP_ATTR(HUD_SUB, 0));
        return;
    }
    if (c->wanted) {
        snprintf(line, sizeof line, "WANTED %ucr", (unsigned)c->bounty);
        ui_put(x, y++, line, LAP_ATTR(HUD_WARN, 0));
    } else if (c->rank_text[0]) ui_put(x, y++, c->rank_text, LAP_ATTR(HUD_SUB, 0));
    static const int shown[6] = {MODULE_POWER, MODULE_DRIVES, MODULE_WEAPONS, MODULE_SHIELDS, MODULE_SENSORS, MODULE_CARGO};
    for (int i = 0; i < 6; i++) {
        int m = shown[i];
        bool aim = v->subtarget == m;
        float h = c->modules[m];
        snprintf(line, sizeof line, "%c%-7s", aim ? '>' : ' ', module_names[m]);
        ui_put(x, y + i, line, LAP_ATTR(aim ? HUD_HI : HUD_SUB, 0));
        for (int k = 0; k < 4; k++) {
            bool on = k < (int)(h * 4 + .5f);
            lap_put_cell(x + 8 + k, y + i, on ? LAP_BRAILLE(0x1B) : LAP_BRAILLE(0x12),
                         LAP_ATTR(on ? (h < .3f ? HUD_WARN : aim ? HUD_HI : HUD) : HUD_DIM, 0));
        }
    }
}
