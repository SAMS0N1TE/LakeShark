/* Fuel-limited chart and simulation-driven charge/tunnel/arrival sequence. */
#include "internal.h"
bool game_jump_direction(l3_vec *out) {
    if (!cg || cg->v.plotted < 0 || cg->v.plotted >= GAME_SYSTEMS)
        return false;
    const game_system *from = &cg->v.systems[cg->v.system];
    const game_system *to = &cg->v.systems[cg->v.plotted];
    float dx = to->x - from->x, dy = to->y - from->y;
    float horizontal = hypotf(dx, dy);
    /* Coincident chart positions have a stable -Z bearing too. */
    if (horizontal == 0) {
        dy = horizontal = 1;
    }
    /* Seed-only slope bounds elevation to atan(.45), about +/-24.2 degrees. */
    float slope = .45f * (2.f * (float)(to->seed & 65535u) / 65535.f - 1.f);
    if (out)
        *out = l3_norm(l3v(dx, horizontal * slope, -dy));
    return true;
}
float game_jump_alignment(void) {
    l3_vec direction;
    if (!game_jump_direction(&direction))
        return 180.f;
    float dot = l3_dot(l3_norm(cg_forward()), direction);
    return acosf(cg_clamp(dot, -1.f, 1.f)) / CG_RAD;
}
bool game_plot_jump(int system) {
    if (!cg || system < 0 || system >= GAME_SYSTEMS || system == cg->v.system ||
        cg->v.jump != JUMP_IDLE)
        return false;
    float distance = game_distance(&cg->v.systems[cg->v.system], &cg->v.systems[system]);
    if (distance > (cg->v.player.fuel_max > 0 ? cg->v.player.fuel_max : 7) || distance > cg->v.player.fuel)
        return false;
    cg->v.plotted = system;
    cg->v.plotted_ly = distance;
    return true;
}
void cg_arrive(int system) {
    game_view *v = &cg->v;
    v->system = system;
    v->plotted = -1;
    v->plotted_ly = 0;
    v->selected = -1;
    cg_combat_reset();
    cg->rescue_target = -1;
    cg->rescue = 0;
    v->rescue_progress = 0;
    game_system_contents(v->systems[system].seed, &v->contents);
    memset(v->contacts, 0, sizeof(v->contacts));
    memset(cg->fighters, 0, sizeof(cg->fighters));
    memset(cg->dead_until, 0, sizeof(cg->dead_until));
    memset(cg->beams, 0, sizeof(cg->beams));
    memset(cg->missiles, 0, sizeof(cg->missiles));
    memset(cg->debris, 0, sizeof(cg->debris));
    memset(cg->cans, 0, sizeof(cg->cans));
    v->player.position = l3v(0, 0, 1100);
    v->player.velocity = l3v(0, 0, 0);
    v->player.attitude = (l3_quat){0, 0, 0, 1};
    v->player.docking = false;
    v->player.docked = false;
    v->player.throttle = .15f;
    v->jump = JUMP_IDLE;
    v->jump_progress = 0;
    cg->poll = 1;
    cg_market();
    cg_system_arrive();
    cg_poi_arrive();
    cg_render_reset();
    cg_message(v->systems[system].description);
}
void cg_jump(float dt) {
    game_view *v = &cg->v;
    game_player *p = &v->player;
    if (v->jump == JUMP_IDLE)
        return;
    if(v->jump==JUMP_CHARGE && game_drive_refusal()) {
        v->jump=JUMP_IDLE;v->jump_progress=0;cg_message(game_drive_refusal());return;
    }
    if (v->jump == JUMP_CHARGE && p->mass_locked) {
        v->jump = JUMP_IDLE;
        v->jump_progress = 0;
        cg_message("Jump aborted: mass lock.");
        return;
    }
    if (p->dead) {
        v->jump = JUMP_IDLE;
        return;
    }
    if (v->jump == JUMP_CHARGE && game_jump_alignment() > GAME_JUMP_HOLD_DEG) {
        v->jump = JUMP_IDLE;
        v->jump_progress = 0;
        cg_message("Jump aborted: lost alignment.");
        return;
    }
    v->jump_progress += dt;
    if (v->jump == JUMP_CHARGE && v->jump_progress >= 3) {
        p->fuel = fmaxf(0, p->fuel - v->plotted_ly);
        v->jump = JUMP_TUNNEL;
        v->jump_progress = 0;
        cg_message("Hyperspace transit.");
    } else if (v->jump == JUMP_TUNNEL && v->jump_progress >= 2) {
        int destination = v->plotted;
        cg_day();
        cg_poi_route(destination);
        cg_arrive(destination);
        cg_message("Arrival confirmed. New system charted.");
    }
}
