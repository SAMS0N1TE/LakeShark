/* ON FOOT: out of the seat while docked. The whole glass is the view: walk
 * the ship and the hangar round it, look about, and use what is in reach -
 * the seat takes you back to the cockpit, the hold and the station desk open
 * their panels and come back here when closed.
 *
 * Fingers: the left third is a stick where it lands (forward, back, sideways);
 * drag anywhere else to look; a short tap there uses what is in reach. Keys:
 * W/S or up/down walk, A/D or left/right turn, Q/E step sideways, ENTER uses,
 * BACK sits down. */
#include "internal.h"
#include "art.h"
#include "ui.h"

static const char *const place_name[2] = {"HANGAR", "ABOARD"};
static float on_foot;           /* seconds since leaving the seat: the hint shows a while */
/* On foot the view is the whole glass, drawn at half resolution and doubled:
   a room covers every dot, and at full size the Nano took 120 ms a frame. */
static l3_scene *wsc;
static uint8_t *wdots;
static int wdots_w, wdots_h;

static void fill_ship(void) {
    const game_ship_stats *st = game_player_ship();
    ui.walk_ship.hull_radius = st ? st->radius : 6;
    ui.walk_ship.cargo_capacity = game_cargo_capacity();
    ui.walk_ship.cargo_used = game_cargo_used();
    int seats = game_passenger_seats();
    ui.walk_ship.seats = seats > 0 ? seats : 0;
    ui.walk_ship.has_engines_room = ui.walk_ship.hull_radius > 4;
}

void ui_walk_start(void) {
    const game_view *v = game_state();
    if (!v || !v->player.docked) return;
    fill_ship();
    walk_enter(&ui.walk, WALK_SHIP, &ui.walk_ship);
    game_panel_open(PANEL_NONE);
    ui.walking = true;
    ui.walk_resume = false;
    ui.walk_grab = 0;
    on_foot = 0;
    lap_sound(LAP_SOUND_KEY);
    if (ui.voice) lap_say("Out of the seat.");
}

l3_scene *walk_scene_now(void) { return wsc; }   /* for the host's engine-cost probe */

static void free_view(void) {
    if (wsc) render_walk_forget();
    l3_destroy(wsc);
    wsc = NULL;
    free(wdots);
    wdots = NULL;
    wdots_w = wdots_h = 0;
}

void ui_walk_stop(void) {
    ui.walking = false;
    ui.walk_resume = false;
    ui.walk_grab = 0;
    free_view();
}

/* A panel opened on foot; closing it brings you back where you stood. */
static void open_from_foot(game_panel p, int market_tab) {
    game_panel_open(p);
    if (p == PANEL_MARKET) { ui.market_tab = market_tab; ui.market_pick = 0; }
    ui.walking = false;
    ui.walk_resume = true;
    ui.walk_grab = 0;
    free_view();
}

static void use(void) {
    walk_place before = ui.walk.place;
    int k = walk_use(&ui.walk, &ui.walk_ship);
    if (k < 0) { lap_sound(LAP_SOUND_ERROR); return; }
    lap_sound(LAP_SOUND_ROGER);
    switch (k) {
    case WS_SEAT: ui_walk_stop(); if (ui.voice) lap_say("Back in the seat."); break;
    case WS_HOLD: case WS_DESK: open_from_foot(PANEL_MARKET, 0); break;
    case WS_ENGINES: open_from_foot(PANEL_MARKET, 1); break;
    case WS_ARMOURY: open_from_foot(PANEL_MARKET, 2); break;
    case WS_CABIN: open_from_foot(PANEL_MARKET, 3); break;
    case WS_YARD: open_from_foot(PANEL_MARKET, 4); break;
    case WS_CONSOLE: open_from_foot(PANEL_STATUS, 0); break;
    default:
        if (ui.walk.place != before && ui.voice)
            lap_say(ui.walk.place == WALK_HANGAR ? "Hangar deck." : "Aboard.");
        break;
    }
}

/* The finger: held on an arrow it walks or turns for as long as it stays;
   dragged anywhere else it looks about; the USE and SEAT keys take a tap. A
   tap comes back for main.c to hand to tap(), which keeps the board's own
   tap path from acting on it twice. Tapping the view itself does nothing. */
bool ui_walk_finger(float dt, int *tap_col, int *tap_row) {
    bool tapped = false;
    lap_touch_t t;
    int cw, ch;
    lap_cell_px(&cw, &ch);
    if (cw <= 0 || ch <= 0) return false;
    bool down = lap_touch(&t) && t.down;
    float turn = 0, look = 0, held_fwd = 0;
    if (down) {
        int c0 = t.x0 / cw, r0 = t.y0 / ch;
        if (!ui.walk_grab) {
            ui.walk_grab = 2;                   /* looking, unless it landed on a key */
            for (int k = 0; k < 4; k++)
                if (ui_in(&ui.walk_pad[k], c0, r0)) ui.walk_grab = 4 + k;
            if (ui_in(&ui.walk_use_key, c0, r0) || ui_in(&ui.walk_seat_key, c0, r0)) ui.walk_grab = 3;
            ui.walk_lx = t.x0;
            ui.walk_ly = t.y0;
            ui.down_ms = lap_millis();
            ui.dragging = false;
        }
        float mx = (float)(t.x - t.x0) / cw, my = (float)(t.y - t.y0) / ch;
        if (fabsf(mx) > 1.2f || fabsf(my) > .8f) ui.dragging = true;
        if (ui.walk_grab == 2) {           /* look: degrees per cell dragged */
            turn = (float)(t.x - ui.walk_lx) / cw * 2.4f;
            look = -(float)(t.y - ui.walk_ly) / ch * 3.2f;
            ui.walk_lx = t.x;
            ui.walk_ly = t.y;
        } else if (ui.walk_grab >= 4) {    /* an arrow, held */
            const int k = ui.walk_grab - 4;
            if (k == 0) held_fwd = 1;
            else if (k == 1) held_fwd = -1;
            else turn = (k == 2 ? -1 : 1) * 95 * dt;
        }
    } else if (ui.walk_grab) {
        if (ui.walk_grab == 3 && !ui.dragging && lap_millis() - ui.down_ms < 700) {
            *tap_col = t.x0 / cw;
            *tap_row = t.y0 / ch;
            tapped = true;
        }
        ui.walk_grab = 0;
        ui.dragging = false;
    }
    float fwd = cg_clamp(held_fwd + ui.walk_fwd, -1, 1);
    float strafe = cg_clamp(ui.walk_strafe, -1, 1);
    turn += ui.walk_turn * 95 * dt;
    look += ui.walk_look * 70 * dt;
    walk_step(&ui.walk, fwd, strafe, turn, look, dt);
    /* a key press holds for 0.6 s, longer than the keyboard's wait before it
       repeats, so a key held down walks without stopping and starting */
    float fade = dt / .6f;
    float *imp[4] = {&ui.walk_fwd, &ui.walk_strafe, &ui.walk_turn, &ui.walk_look};
    for (int i = 0; i < 4; i++)
        *imp[i] = *imp[i] > 0 ? fmaxf(0, *imp[i] - fade) : fminf(0, *imp[i] + fade);
    return tapped;
}

bool ui_walk_tap(int col, int row) {
    if (ui_in(&ui.walk_seat_key, col, row)) {
        ui_walk_stop();
        lap_sound(LAP_SOUND_KEY);
        return true;
    }
    if (ui_in(&ui.walk_use_key, col, row)) { use(); return true; }
    return true;                        /* the view itself: nothing */
}

bool ui_walk_key(lap_key_t k, char ch) {
    if (k == LAP_KEY_BACK) { ui_walk_stop(); return true; }
    if (k == LAP_KEY_ENTER) { use(); return true; }
    if (k == LAP_KEY_UP) { ui.walk_fwd = 1; return true; }
    if (k == LAP_KEY_DOWN) { ui.walk_fwd = -1; return true; }
    if (k == LAP_KEY_LEFT) { ui.walk_turn = -1; return true; }
    if (k == LAP_KEY_RIGHT) { ui.walk_turn = 1; return true; }
    if (k != LAP_KEY_CHAR) return true;
    switch (ch | 0x20) {
    case 'w': ui.walk_fwd = 1; break;
    case 's': ui.walk_fwd = -1; break;
    case 'a': ui.walk_turn = -1; break;
    case 'd': ui.walk_turn = 1; break;
    case 'q': ui.walk_strafe = -1; break;
    case 'e': ui.walk_strafe = 1; break;
    case 'r': ui.walk_look = 1; break;
    case 'f': ui.walk_look = -1; break;
    case 'c': case ' ': use(); break;
    case 'o': ui_walk_stop(); break;
    default: break;
    }
    return true;
}

void ui_walk_draw(void) {
    const game_view *v = game_state();
    if (!v->player.docked) { ui_walk_stop(); return; }
    const int C = ui.cols, R = ui.rows;
    const int hc = (C + 1) / 2, hr = (R + 1) / 2;
    if (!wsc) wsc = l3_create(hc, hr);
    else l3_resize(wsc, hc, hr);
    const int fw = C * 2, fh = R * 3;
    if (fw != wdots_w || fh != wdots_h || !wdots) {
        free(wdots);
        wdots = malloc((size_t)fw * fh);
        wdots_w = fw;
        wdots_h = fh;
    }
    if (!wsc || !wdots) {
        ui_walk_stop();
        lap_say("Not enough memory to walk.");
        return;
    }
    render_walk(wsc, &ui.walk, ui.t);
    int sw, sh;
    l3_size(wsc, &sw, &sh);
    const uint8_t *src = l3_dots(wsc);
    for (int y = 0; y < fh; y++) {
        const uint8_t *row = src + (size_t)(y / 2 < sh ? y / 2 : sh - 1) * sw;
        uint8_t *dst = wdots + (size_t)y * fw;
        for (int x = 0; x < fw; x++) dst[x] = row[x / 2 < sw ? x / 2 : sw - 1];
    }
    lap_dots(0, 0, C, R, wdots);
    /* where you are */
    char line[48];
    snprintf(line, sizeof line, "%s  %s", place_name[ui.walk.place == WALK_SHIP],
             ui.walk.place == WALK_SHIP ? (game_player_ship() ? game_player_ship()->name : "")
             : v->contents.station_name[0] ? v->contents.station_name : "STATION");
    ui_put(1, 1, line, LAP_ATTR(HUD_HI, 0));
    /* a small cross where you look */
    ui_cell(C / 2, R / 2, '+', LAP_ATTR(ui.walk.spot >= 0 ? 15 : HUD, 0));
    /* what is in reach, and the key to use it */
    const walk_spot *sp;
    int ns = walk_spots(&sp);
    int kw = 18, kh = 3;
    ui.walk_use_key = (ui_rect){C - kw - 1, R - kh - 1, kw, kh};
    if (ui.walk.spot >= 0 && ui.walk.spot < ns) {
        const char *label = sp[ui.walk.spot].label;
        ui_key_box(&ui.walk_use_key, label, "USE", HUD, LAP_BLACK, true);
        ui_put(C / 2 - (int)strlen(label) / 2, R / 2 + 2, label, LAP_ATTR(HUD_HI, 0));
    } else {
        ui.walk_use_key = (ui_rect){0, 0, 0, 0};
    }
    /* back to the seat from anywhere */
    ui.walk_seat_key = (ui_rect){C - 9, 0, 9, 3};
    ui_key_box(&ui.walk_seat_key, "SEAT", "", HUD, LAP_BLACK, false);
    /* every place to use near you, named where it stands */
    for (int i = 0; i < ns; i++) {
        if (i == ui.walk.spot) continue;            /* that one is named under the cross */
        l3_vec d = l3_sub(sp[i].p, ui.walk.feet);
        if (l3_dot(d, d) > 12 * 12) continue;
        float dx, dy;
        if (!l3_project(wsc, l3_add(sp[i].p, l3v(0, .45f, 0)), &dx, &dy)) continue;
        int col = (int)dx - (int)strlen(sp[i].label) / 2, row = (int)(dy * 2 / 3);   /* half-size dots to cells */
        if (row < 2 || row >= R - 5 || col < 0 || col + (int)strlen(sp[i].label) >= C) continue;
        ui_put(col, row, sp[i].label, LAP_ATTR(HUD_HI, 0));
    }
    /* hold an arrow to walk: forward, back, turn left, turn right */
    static const char *const arrow[4] = {"^", "v", "<", ">"};
    const int pw = 7, ph = 3, px = 1, py = R - 2 * ph - 1;
    ui.walk_pad[0] = (ui_rect){px + pw, py, pw, ph};
    ui.walk_pad[1] = (ui_rect){px + pw, py + ph, pw, ph};
    ui.walk_pad[2] = (ui_rect){px, py + ph, pw, ph};
    ui.walk_pad[3] = (ui_rect){px + 2 * pw, py + ph, pw, ph};
    for (int k = 0; k < 4; k++)
        ui_key_box(&ui.walk_pad[k], arrow[k], "", HUD, LAP_BLACK, ui.walk_grab == 4 + k);
    if ((on_foot += ui.dt) < 25)
        ui_put(px + 3 * pw + 2, R - 2, "hold arrows or W A S D to walk   drag to look   USE: Enter", LAP_ATTR(HUD_SUB, 0));
}
