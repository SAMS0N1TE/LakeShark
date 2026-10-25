/* CARRIER: fly a ship among the aircraft your radio hears, turned into ships.
 * This file wires the screen to the game: each frame read the finger, step the
 * game, draw the view (or a panel) and the dash. */
#include "internal.h"
#include "art.h"
#include "ui.h"

ui_state ui;
static uint32_t last_ms;
static bool tilt;
/* Where on the lever, 0 bottom to 1 top, as a throttle: the bottom part is
   reverse, with a small detent at zero so stopping is easy to find. */
static float throttle_at(float frac) {
    float rev = game_reverse_max(), t = cg_clamp(frac, 0, 1) * (1 + rev) - rev;
    return fabsf(t) < .04f ? 0 : t;
}            /* steering by tilting the board; off unless chosen in STATUS */

static void enter(void) {
    memset(&ui, 0, sizeof ui);
    ui.galaxy_pick = -1;
    ui.scoop_t = 10;
    ui.steer = 1;
    ui.voice = true;
    ui.sound = true;
    ui.mesh_slot = 1;
    ui.encounters = 1;
    ui.zoom = 1;
    ui.autozoom = true;
    ui.route_goal = -1;
    ui.aim = 1;
    int fd = lap_file_open("ui.cfg", "r");     /* the screen's own settings: steer, voice */
    if (fd >= 0) {
        char b[11] = {0};
        int n = lap_file_read(fd, b, 11);
        if (n >= 1 && b[0] >= '0' && b[0] <= '2') ui.steer = b[0] - '0';
        if (n >= 2) ui.voice = b[1] != '0';
        if (n >= 3) ui.mesh_on = b[2] == '1';
        if (n >= 4 && b[3] >= '0' && b[3] <= '3') ui.mesh_slot = b[3] - '0';
        if (n >= 5 && b[4] >= '0' && b[4] <= '2') ui.traffic = b[4] - '0';
        if (n >= 6 && b[5] >= '0' && b[5] <= '2') ui.encounters = b[5] - '0';
        if (n >= 7 && b[6] >= '0' && b[6] <= '3') ui.tips = b[6] - '0';
        if (n >= 8) ui.sound = b[7] != '0';
        if (n >= 9 && b[8] >= '0' && b[8] <= '2') ui.aim = b[8] - '0';
        if (n >= 10) ui.autozoom = b[9] != '0';
        if (n >= 11 && b[10] >= '0' && b[10] < '0' + 32) ui.hud_off = (uint8_t)(b[10] - '0');
        lap_file_close(fd);
    }
    ui.msg_rev = -1;
    render_new_system = ui_space_reset;
    render_extra = render_world_extras;
    game_init();
    game_set_traffic(ui.traffic);
    game_set_encounters(ui.encounters);
    ui_apply_aim();
    last_ms = lap_millis();
    lap_immersive(true);       /* the whole glass; EXIT lives on the canopy strut */
    lap_keep_awake(true);
    const game_view *v = game_state();
    if (v) ui.was_docked = v->player.docked;
    ui_voice_reset();
    ui_sound_reset();
}

static void leave(void) {
    ui_walk_stop();             /* frees the walk view's own scene */
    l3_destroy(ui.scene);
    ui_holo_free();
    free(ui.dots);
    lap_canvas_free(&ui.dash_c);
    game_shutdown();
    lap_keep_awake(false);
    lap_hold_rotation(false);
    lap_say_stop();
    memset(&ui, 0, sizeof ui);
}

static bool ensure_scene(void) {
    if (!ui.scene) ui.scene = l3_create(ui.view.w, ui.view.h);
    else if (!l3_resize(ui.scene, ui.view.w, ui.view.h)) return false;
    if (!ui.scene) return false;
    int w, h;
    l3_size(ui.scene, &w, &h);
    int cw, ch;
    lap_cell_px(&cw, &ch);
    float aspect = cw > 0 && ch > 0 ? (cw * 1.5f) / ch : 1;
    ui.fy = .5f * h / tanf(ui_fov() * CG_RAD * .5f);
    ui.fx = ui.fy / aspect;
    if (w != ui.dw || h != ui.dh || !ui.dots) {
        free(ui.dots);
        ui.dots = malloc((size_t)w * h);
        ui.dw = w;
        ui.dh = h;
    }
    return ui.dots != NULL;
}

/* Tap on the view: the ship under the finger, or the nearest one to it. */
static void pick_target(int col, int row) {
    const game_view *v = game_state();
    int dx = (col - ui.view.x) * 2 + 1, dy = (row - ui.view.y) * 3 + 1;
    int tag = l3_pick(ui.scene, dx, dy);
    if (tag >= 1 && tag <= GAME_CONTACTS && v->contacts[tag - 1].active) {
        while (v->selected != tag - 1) {
            int before = v->selected;
            game_target_next();
            if (v->selected == before) break;
        }
        lap_sound(LAP_SOUND_KEY);
        return;
    }
    /* nothing under it: the nearest ship drawn within a few cells */
    for (int r = 2; r <= 12; r += 2)
        for (int y = dy - r; y <= dy + r; y += 2)
            for (int x = dx - r; x <= dx + r; x += 2) {
                tag = l3_pick(ui.scene, x, y);
                if (tag >= 1 && tag <= GAME_CONTACTS && v->contacts[tag - 1].active) {
                    for (int n = 0; n < GAME_CONTACTS && v->selected != tag - 1; n++) game_target_next();
                    lap_sound(LAP_SOUND_KEY);
                    return;
                }
            }
}

static bool tap(int col, int row);
static uint32_t tap_ms;     /* the last tap acted on, from either path */
static int tap_col = -1, tap_row = -1;
/* The board may send a press to touch() as well as the poll. touch_early: it came
   while the poll held the finger, so the poll's tap is the one. touch_owed: the poll
   tapped first and that touch() is still coming. Each skips one touch, not a window,
   so a quick second click on the same button counts. */
static bool touch_early, touch_owed;
static void poll_tapped(int col, int row) {
    tap_col = col;
    tap_row = row;
    tap_ms = lap_millis();
    touch_owed = !touch_early;
    touch_early = false;
}

static int key_at(int col, int row) {
    for (int i = 0; i < 5; i++)
        if (ui_in(&ui.keys[i], col, row)) return i;
    return -1;
}

static void finger(float dt) {
    const game_view *v = game_state();
    if (ui.walking) {           /* on foot the finger walks and looks; a tap comes back here */
        int c, r;
        if (ui_walk_finger(dt, &c, &r)) {
            tap(c, r);
            poll_tapped(c, r);
        }
        game_stick(0, 0);
        game_yaw(0);
        return;
    }
    lap_touch_t t;
    int cw, ch;
    lap_cell_px(&cw, &ch);
    bool down = lap_touch(&t) && t.down;
    if (down) {
        int c0 = t.x0 / cw, r0 = t.y0 / ch;
        if (!ui.down) {     /* just went down: decide what it holds */
            ui.down = true;
            touch_early = false;
            ui.dragging = false;
            ui.consumed = false;
            ui.down_ms = lap_millis();
            ui.grab = ui.quit_ask ? UI_G_NONE
                    : v->panel != PANEL_NONE && ui_in(&ui.view, c0, r0) ? UI_G_PANEL
                    : ui_in(&ui.view, c0, r0) && !ui_in(&ui.close, c0, r0) && !ui_in(&ui.viewkey, c0, r0) ? UI_G_VIEW
                    : ui_in(&ui.throttle_grab, c0, r0) ? UI_G_THROTTLE
                    : key_at(c0, r0) >= 0 ? UI_G_KEY : UI_G_NONE;
        }
        if (ui.grab == UI_G_PANEL && v->panel == PANEL_GALAXY && ui.dragging) {   /* the chart moves with the finger */
            ui_galaxy_drag((t.x - ui.x) * 2.f / cw, (t.y - ui.y) * 3.f / ch);
            ui.consumed = true;
        }
        const int last_x = ui.x, last_y = ui.y;
        ui.x0 = t.x0; ui.y0 = t.y0; ui.x = t.x; ui.y = t.y;
        float mx = (float)(t.x - t.x0) / cw, my = (float)(t.y - t.y0) / ch;
        if (fabsf(mx) > 1.2f || fabsf(my) > .8f) ui.dragging = true;
        int key = key_at(c0, r0);
        switch (ui.grab) {
        case UI_G_VIEW:
            if (ui.free_cam && ui.view_mode == UI_VIEW_CHASE) {   /* swing the camera round the ship */
                if (ui.dragging && last_x) {
                    render_cam_yaw += (float)(t.x - last_x) / cw * 3;
                    render_cam_pitch = cg_clamp(render_cam_pitch - (float)(t.y - last_y) / ch * 4, -70, 70);
                }
                break;
            }
            if (ui.dragging) {
                /* cells of drag for full deflection, by the STEER setting; a dead zone and a
                   curve, so small moves aim finely and big ones still turn hard */
                static const float span_x[3] = {13, 9, 5}, span_y[3] = {9, 6, 3};
                float roll = cg_clamp(mx / span_x[ui.steer], -1, 1), pitch = cg_clamp(my / span_y[ui.steer], -1, 1);
                roll = fabsf(roll) < .06f ? 0 : roll;
                pitch = fabsf(pitch) < .06f ? 0 : pitch;
                ui.stick_x = roll * (.35f + .65f * fabsf(roll));
                ui.stick_y = pitch * (.35f + .65f * fabsf(pitch));
            }
            break;
        case UI_G_THROTTLE: {
            float top = (ui.throttle.y + 2) * ch, bottom = (ui.throttle.y + ui.throttle.h) * ch;
            game_throttle(throttle_at((bottom - t.y) / (bottom - top)));
            ui.throttle_touched = true;
            ui.consumed = true;
            break;
        }
        case UI_G_KEY:
            if (key == UI_K_FIRE && t.held_ms > 350) { game_fire(true); ui.consumed = true; }
            if (key == UI_K_FIRE2 && t.held_ms > 350) { game_fire2(true); ui.consumed = true; }
            if (key == UI_K_TARGET && t.held_ms > 650 && !ui.consumed) {
                game_target_nearest(true);
                if (v->selected < 0) game_target_nearest(false);
                lap_haptic(LAP_HAPTIC_CLICK);
                ui.consumed = true;
            }
            break;
        default: break;
        }
        if (ui.dragging && ui.grab != UI_G_PANEL) ui.consumed = true;
    } else if (ui.down) {
        ui.down = false;
        if (ui.grab == UI_G_KEY && ui.consumed) { game_fire(ui.fire_latch); game_fire2(ui.fire2_latch); }
        /* a short touch that did not drag is a tap; the board may also send it to
           touch(), so whichever path sees it first acts and the other skips */
        if (!ui.consumed && lap_millis() - ui.down_ms < 700) {
            tap(ui.x0 / cw, ui.y0 / ch);
            poll_tapped(ui.x0 / cw, ui.y0 / ch);
        } else if (ui.consumed) {
            poll_tapped(ui.x0 / cw, ui.y0 / ch);
        } else {
            touch_early = false;    /* a long press: neither path taps */
        }
        ui.consumed = false;
        ui.stick_x = ui.stick_y = 0;
        ui.dragging = false;
    }
    /* keyboard impulses fade; tilt steering when chosen */
    float decay = fmaxf(0, 1 - dt * 5);
    ui.key_pitch *= decay;
    ui.key_roll *= decay;
    ui.key_yaw *= decay;
    ui.key_tx *= decay;
    ui.key_ty *= decay;
    game_thrust(fabsf(ui.key_tx) < .05f ? 0 : ui.key_tx, fabsf(ui.key_ty) < .05f ? 0 : ui.key_ty);
    float sp = ui.stick_y + ui.key_pitch, sr = ui.stick_x + ui.key_roll;
    if (tilt) {
        lap_motion_t m;
        if (lap_motion(&m) && m.age_ms < 500) {
            float p = (m.pitch - ui.level_pitch) / 25, r = (m.roll - ui.level_roll) / 25;
            if (!ui.down || ui.grab != UI_G_VIEW) { sp = p; sr = r; }
        }
    }
    game_stick(cg_clamp(sp, -1, 1), cg_clamp(sr, -1, 1));
    game_yaw(cg_clamp(ui.key_yaw, -1, 1));
    if (!ui.down && v->player.firing != ui.fire_latch) game_fire(ui.fire_latch);
    if (!ui.down && v->player.firing2 != ui.fire2_latch) game_fire2(ui.fire2_latch);
    int s = lap_swipe();
    if (v->panel == PANEL_LOG && s == LAP_SWIPE_UP) ui.log_scroll += 4;
    if (v->panel == PANEL_LOG && s == LAP_SWIPE_DOWN) ui.log_scroll -= 4;
}


static void draw(void) {
    const game_view *v = game_state();
    if (!v) {
        lap_put_str(1, 2, "CARRIER: not enough memory", LAP_ATTR(HUD_WARN, 0));
        return;
    }
    uint32_t ms = lap_millis();
    float dt = (ms - last_ms) * .001f;
    last_ms = ms;
    if (dt > .2f) dt = .2f;
    ui.t += dt;
    ui.dt = dt;
    ui_layout();
    if (ui.walk_resume && v->panel == PANEL_NONE) {     /* the panel opened on foot closed: back where you stood */
        ui.walking = v->player.docked;
        ui.walk_resume = false;
    }
    if (!ensure_scene()) {
        lap_put_str(1, 2, "CARRIER: not enough memory", LAP_ATTR(HUD_WARN, 0));
        return;
    }
    finger(dt);
    if (tilt != v->player.motion_steer) {      /* switched in STATUS */
        tilt = v->player.motion_steer;
        lap_hold_rotation(tilt);
    }
    ui_autopilot(dt);
    ui_net_update();
    game_update(dt);
    ui_fsd_voice();
    ui_route_update();
    ui_effects_update(dt);
    ui_voice_update();
    ui_sound_update();
    lap_clear(LAP_ATTR(HUD, LAP_BLACK));
    if (ui.walking) {
        ui_walk_draw();
    } else {
        if (v->panel == PANEL_NONE) ui_view_draw();
        else ui_panel_draw();
        ui_dash_draw();
        ui_glitch_cells();
        ui_talk_draw();
        ui_tips_draw(dt);
        ui_dead_draw();
        ui_exit_draw();
        ui_keys_draw();
        if (ui.walk_resume && v->panel != PANEL_NONE) {   /* opened on foot: the way back, over the flight keys */
            int x0 = ui.keys[0].x, y0 = ui.keys[0].y, x1 = x0, y1 = y0;   /* all five keys, in a row or a column */
            for (int i = 0; i < 5; i++) {
                x0 = ui.keys[i].x < x0 ? ui.keys[i].x : x0;
                y0 = ui.keys[i].y < y0 ? ui.keys[i].y : y0;
                x1 = ui.keys[i].x + ui.keys[i].w > x1 ? ui.keys[i].x + ui.keys[i].w : x1;
                y1 = ui.keys[i].y + ui.keys[i].h > y1 ? ui.keys[i].y + ui.keys[i].h : y1;
            }
            ui.walk_back_key = (ui_rect){x0, y0, x1 - x0, y1 - y0};
            ui_key_box(&ui.walk_back_key, "BACK TO WALKING", "or press BACK", HUD_OK, LAP_BLACK, true);
        }
    }
    if (lap_frame() % 100 == 0) {
        l3_stats_t s;
        l3_stats(ui.scene, &s);
        lap_log("frame %u us, %u tris, %u dropped", (unsigned)s.us_total, (unsigned)s.triangles, (unsigned)s.dropped);
    }
}

/* The STATUS aim assist setting, handed to the game's guns. */
void ui_apply_aim(void) {
    game_aim_assist(ui.aim);
}

void ui_save_settings(void) {
    int fd = lap_file_open("ui.cfg", "w");
    if (fd >= 0) {
        char b[11] = {(char)('0' + ui.steer), ui.voice ? '1' : '0', ui.mesh_on ? '1' : '0',
                      (char)('0' + ui.mesh_slot), (char)('0' + ui.traffic), (char)('0' + ui.encounters),
                      (char)('0' + (ui.tips > 3 ? 3 : ui.tips)), ui.sound ? '1' : '0',
                      (char)('0' + ui.aim), ui.autozoom ? '1' : '0', (char)('0' + (ui.hud_off & 31))};
        lap_file_write(fd, b, 11);
        lap_file_close(fd);
    }
}

static void open_panel(game_panel p) {
    const game_view *v = game_state();
    game_panel_open(v->panel == p ? PANEL_NONE : p);
    if (p == PANEL_GALAXY) ui_galaxy_open();
    ui.market_pick = 0;
    ui.galaxy_pick = -1;
    lap_sound(LAP_SOUND_KEY);
}

static void press(int k) {
    const game_view *v = game_state();
    switch (k) {
    case UI_K_FIRE: ui.fire_latch = !ui.fire_latch; game_fire(ui.fire_latch); break;
    case UI_K_MISSILE: if (!game_missile()) lap_sound(LAP_SOUND_ERROR); break;
    case UI_K_FIRE2: ui.fire2_latch = !ui.fire2_latch; game_fire2(ui.fire2_latch); break;
    case UI_K_ECM: if (!game_ecm()) lap_sound(LAP_SOUND_ERROR); break;
    case UI_K_CRUISE:
        if (ui_boost_context()) {
            if (game_boost()) ui_sound_boost(); else lap_sound(LAP_SOUND_ERROR);
            break;
        }
        if (!game_cruise(!v->player.cruising)) {
            lap_sound(LAP_SOUND_ERROR);
            if (ui.voice && v->player.cruise_reason) lap_say(v->player.cruise_reason);
        }
        break;
    case UI_K_TARGET: game_target_next(); lap_sound(LAP_SOUND_KEY); break;
    case UI_K_ACTION:
        if (v->action == ACT_NONE) { lap_sound(LAP_SOUND_ERROR); break; }
        if (v->action == ACT_LAUNCH) game_panel_open(PANEL_NONE);
        game_context();
        lap_sound(LAP_SOUND_ROGER);
        break;
    }
}

static bool touch(int col, int row) {
    lap_touch_t t;
    if (ui.down || (lap_touch(&t) && t.down)) {   /* the poll holds the finger: it taps on release */
        touch_early = true;
        return true;
    }
    const bool owed = touch_owed;
    touch_owed = false;
    if (owed && lap_millis() - tap_ms < 400 && col == tap_col && row == tap_row) return true;
    tap_ms = lap_millis();
    tap_col = col;
    tap_row = row;
    return tap(col, row);
}

static bool tap(int col, int row) {
    const game_view *v = game_state();
    if (!v) return false;
    if (ui.walking) return ui_walk_tap(col, row);
    if (ui.walk_resume && v->panel != PANEL_NONE && ui_in(&ui.walk_back_key, col, row)) {
        game_panel_open(PANEL_NONE);    /* draw() puts you back on foot */
        lap_sound(LAP_SOUND_KEY);
        return true;
    }
    if (ui.tips < 3 && ui.tip_t > .6f && ui_in(&ui.view, col, row) && !v->player.docked) {   /* a tip goes on a tap */
        ui.tips++;
        ui.tip_t = 0;
        ui_save_settings();
        return true;
    }
    if (ui.keys_help) { ui.keys_help = false; return true; }
    if (ui.quit_ask) {          /* the question takes every tap until answered */
        if (ui_in(&ui.quit_leave, col, row)) { game_save(); lap_exit(); }
        else if (ui_in(&ui.quit_stay, col, row)) ui.quit_ask = false;
        return true;
    }
    if (v->player.dead && ui_in(&ui.rebuy_key, col, row)) {
        if (game_rebuy()) { game_panel_open(PANEL_NONE); lap_sound(LAP_SOUND_ROGER); }
        else lap_sound(LAP_SOUND_ERROR);
        return true;
    }
    if (v->player.dead && ui_in(&ui.dead_key, col, row)) {
        game_load();
        game_panel_open(PANEL_NONE);
        lap_sound(LAP_SOUND_ROGER);
        return true;
    }
    if (ui_in(&ui.close, col, row) && v->panel == PANEL_NONE) { ui.quit_ask = true; lap_sound(LAP_SOUND_KEY); return true; }
    if (ui_in(&ui.viewkey, col, row) && v->panel == PANEL_NONE && !ui.quit_ask) {
        if (v->player.docked) ui_walk_start();          /* docked, the placard says ON FOOT */
        else ui.view_mode = (ui.view_mode + 1) % 3;
        lap_sound(LAP_SOUND_KEY);
        return true;
    }
    for (int i = 0; i < ui.talk_n; i++)
        if (ui_in(&ui.talk_keys[i], col, row)) {
            if (v->talk.choice_count) game_talk_choose(i); else game_talk_close();
            lap_sound(LAP_SOUND_ROGER);
            return true;
        }
    if (!v) return false;
    for (int i = 0; i < 4; i++)
        if (ui_in(&ui.tab_keys[i], col, row)) { open_panel((game_panel)(PANEL_STATUS + i)); return true; }
    for (int i = 0; i < UI_MODES; i++)
        if (ui_in(&ui.mode_keys[i], col, row)) { ui.mode = i; lap_sound(LAP_SOUND_KEY); return true; }
    if (ui_in(&ui.throttle_grab, col, row)) {
        float top = (float)ui.throttle.y + 2, bottom = (float)(ui.throttle.y + ui.throttle.h);
        game_throttle(throttle_at((bottom - (row + .5f)) / (bottom - top)));
        ui.throttle_touched = true;
        return true;
    }
    if (ui.mode == UI_MODE_NAV && ui_in(&ui.middle, col, row)) return ui_nav_tap(col, row) || true;
    if (ui.mode == UI_MODE_COMMS && ui_in(&ui.middle, col, row)) return ui_comms_tap(col, row) || true;
    if (ui.mode == UI_MODE_SYS && ui_in(&ui.middle, col, row)) return ui_sys_tap(col, row) || true;
    if (ui_console_tap(col, row)) return true;
    int k = key_at(col, row);
    if (k >= 0) { press(k); return true; }
    if (v->panel != PANEL_NONE && ui_panel_tap(col, row)) return true;
    if (ui_in(&ui.view, col, row)) {
        if (ui.view_mode == UI_VIEW_TACTICAL) ui_tactical_tap(col, row);
        else pick_target(col, row);
        return true;
    }
    return ui_in(&ui.dash, col, row);
}

static bool key(lap_key_t k, char ch) {
    const game_view *v = game_state();
    if (!v) return false;
    if (ui.walking) return ui_walk_key(k, ch);
    if (ui.keys_help) { ui.keys_help = false; return true; }
    if (k == LAP_KEY_CHAR && ch == '?') { ui.keys_help = true; return true; }
    if (k == LAP_KEY_BACK) {
        if (ui.quit_ask) { ui.quit_ask = false; return true; }
        if (v->panel != PANEL_NONE) { game_panel_open(PANEL_NONE); return true; }
        return false;
    }
    if (v->panel == PANEL_GALAXY && ui_galaxy_key(k, ch)) return true;
    /* Elite Dangerous's own keyboard defaults (KeyboardMouseOnly): the mouse pitches
       and rolls (drag in the view), A D yaw, W S throttle, X zero, Q E and R F the side
       and up thrusters, TAB boost, J frame shift, arrows the power pips, 1-4 panels.
       What Elite leaves unbound has a spare key, shown on its console button. */
    if (k == LAP_KEY_WHEEL_UP || k == LAP_KEY_WHEEL_DOWN) {   /* the mouse wheel is the throttle, a notch at a time */
        game_throttle(v->player.throttle + (k == LAP_KEY_WHEEL_UP ? .05f : -.05f));
        ui.throttle_touched = true;
        return true;
    }
    if (k == LAP_KEY_UP) { game_pip(1); return true; }
    if (k == LAP_KEY_LEFT) { game_pip(0); return true; }
    if (k == LAP_KEY_RIGHT) { game_pip(2); return true; }
    if (k == LAP_KEY_DOWN) { game_pips_preset(0); return true; }
    if (k == LAP_KEY_TAB) { if (game_boost()) ui_sound_boost(); else lap_sound(LAP_SOUND_ERROR); return true; }
    if (k == LAP_KEY_BACKSPACE) {
        if (v->panel != PANEL_NONE) { game_panel_open(PANEL_NONE); return true; }
        return false;
    }
    if (k != LAP_KEY_CHAR) return false;
    /* the chase camera, swung round the ship: [ ] round, - = up and down, \ free cam
       on (drag to look) and off (back behind) */
    if (ch == '[' || ch == ']' || ch == '-' || ch == '=' || ch == '\\') {
        if (ch == '\\') {
            ui.free_cam = !ui.free_cam;
            if (ui.free_cam) ui.view_mode = UI_VIEW_CHASE;
            else render_cam_yaw = render_cam_pitch = 0;
        } else {
            ui.view_mode = UI_VIEW_CHASE;
            if (ch == '[') render_cam_yaw -= 12;
            if (ch == ']') render_cam_yaw += 12;
            if (ch == '-') render_cam_pitch = cg_clamp(render_cam_pitch - 8, -70, 70);
            if (ch == '=') render_cam_pitch = cg_clamp(render_cam_pitch + 8, -70, 70);
        }
        return true;
    }
    switch (ch | 0x20) {
    case 'w': case 's': {   /* throttle at a steady rate however fast the key repeats */
        static uint32_t last_ms, zero_ms;
        const uint32_t now = lap_millis(), gap = now - last_ms;
        last_ms = now;
        float step = gap > 300 ? .05f : fminf(.05f, gap * .00035f), th = v->player.throttle;
        if ((ch | 0x20) == 'w') th += step;
        else if (th > 0 && th - step <= 0) { th = 0; zero_ms = now; }      /* a held S rests at zero before reverse */
        else if (th > 0 || now - zero_ms > 600) th -= step;
        game_throttle(th);
        ui.throttle_touched = true;
        return true;
    }
    case 'x': game_throttle(0); ui.throttle_touched = true; return true;
    case 'a': ui.key_yaw = -1; return true;
    case 'd': ui.key_yaw = 1; return true;
    case 'q': ui.key_tx = -1; return true;
    case 'e': ui.key_tx = 1; return true;
    case 'r': ui.key_ty = 1; return true;
    case 'f': ui.key_ty = -1; return true;
    case 'j':               /* frame shift: the jump with a route plotted, else supercruise */
        if (v->plotted >= 0) ui_fsd_engage();
        else press(UI_K_CRUISE);
        return true;
    case 't': game_target_ahead(); lap_sound(LAP_SOUND_KEY); return true;
    case 'g': press(UI_K_TARGET); return true;
    case 'h':
        game_target_nearest(true);
        if (v->selected < 0) game_target_nearest(false);
        lap_sound(LAP_SOUND_KEY);
        return true;
    case 'y': game_subtarget_next(); return true;
    case 'v': if (game_heat_sink()) ui_sound_kit(2); return true;
    case 'l': press(UI_K_ACTION); return true;      /* dock, land, launch: the action on offer */
    case 'c': ui.mode = (ui.mode + 1) % UI_MODES; return true;   /* the dash's next page */
    case 'b': press(UI_K_FIRE2); return true;
    case 'm': press(UI_K_MISSILE); return true;
    case '5': if (game_chaff()) ui_sound_kit(1); return true;
    case '6': if (game_shield_cell()) ui_sound_kit(0); return true;
    case '7': press(UI_K_ECM); return true;
    case 'o':           /* VIEW: cockpit, chase, tactical; docked, out of the seat */
        if (v->player.docked) ui_walk_start();
        else ui.view_mode = (ui.view_mode + 1) % 3;
        return true;
    case 'p': {     /* autopilot to the NAV choice */
        ui_dest t;
        ui.autopilot = !ui.autopilot && ui_nav_target(&t);
        lap_say(ui.autopilot ? "Autopilot engaged" : "Autopilot off");
        return true;
    }
    default: break;
    }
    if (ch == ' ') { press(UI_K_FIRE); return true; }
    if (ch >= '1' && ch <= '4') { open_panel((game_panel)(PANEL_STATUS + ch - '1')); return true; }
    return false;
}

LAP_ICON_PX("..f.................",
            ".........ff.......f.",
            "........f77f........",
            "........7ee7........",
            ".......87ee78.......",
            ".......877778.......",
            "......87777778......",
            ".....8777777778.....",
            "....877777777778....",
            "...87777777777778...",
            "..8777777777777778..",
            ".877777777777777778.",
            "87777777777777777778",
            "88888888888888888888",
            ".....bb......bb.....",
            ".....33......33.....",
            "...f................",
            "................f...");
LAP_APP(.id = "carrier", .name = "CARRIER", .sub = "the sky's traffic, in space", .version = "0.3",
        .author = "LakeShark", .icon = LAP_ICON_PLANE, .color = LAP_YELLOW, .radio = LAP_RADIO_ADSB,
        .enter = enter, .leave = leave, .draw = draw, .touch = touch, .key = key);
