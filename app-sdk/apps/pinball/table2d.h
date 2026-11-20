/* Original PINBALL 1.1 dot renderer, retained as a selectable view.
   REEF keeps the original artwork; both views use the same live physics. */
static bool on_table(float u, float v)
{
    if (v >= 50) return u > 4 && u < 96 && v < 178;
    return (u - 50) * (u - 50) + (v - 50) * (v - 50) < 46 * 46;
}

static void rail(float ax, float ay, float bx, float by, uint8_t hi, uint8_t lo)
{
    lc_line(&cv, X(ax) + 1, Y(ay) + 1, X(bx) + 1, Y(by) + 1, lo);
    lc_line(&cv, X(ax), Y(ay), X(bx), Y(by), hi);
}

static void fill_tri(v2 a, v2 b, v2 c, const uint8_t *ramp, float lo, float hi)
{
    const int x0 = X(fminf(a.x, fminf(b.x, c.x))), x1 = X(fmaxf(a.x, fmaxf(b.x, c.x)));
    const int y0 = Y(fminf(a.y, fminf(b.y, c.y))), y1 = Y(fmaxf(a.y, fmaxf(b.y, c.y)));
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float u = (x + .5f - OX) / S, v = (y + .5f - OY) / S;
            const float w0 = (b.x - a.x) * (v - a.y) - (b.y - a.y) * (u - a.x);
            const float w1 = (c.x - b.x) * (v - b.y) - (c.y - b.y) * (u - b.x);
            const float w2 = (a.x - c.x) * (v - c.y) - (a.y - c.y) * (u - c.x);
            if ((w0 < 0 || w1 < 0 || w2 < 0) && (w0 > 0 || w1 > 0 || w2 > 0)) continue;
            const float k = (v - fminf(a.y, fminf(b.y, c.y))) / fmaxf(1, fabsf(fmaxf(a.y, fmaxf(b.y, c.y)) - fminf(a.y, fminf(b.y, c.y))));
            lc_shade(&cv, x, y, hi + (lo - hi) * k, ramp);
        }
}

/* Everything that never moves, once per layout. */
static void build_bg_2d(void)
{
    lc_clear(&cv, LC_BLACK);
    for (int y = hud_h; y < cv.h; y++)
        for (int x = 0; x < cv.w; x++) {
            const float u = (x + .5f - OX) / S, v = (y + .5f - OY) / S;
            if (!on_table(u, v)) continue;
            if (u > 88 && v > 56) { lc_pset(&cv, x, y, lc_mix(LC_BLACK, LC_BLUE, 0.12f, x, y)); continue; }
            /* Blue felt, flat so the ball reads on it: a soft spotlight under
               the bumpers, and darker toward the flippers. */
            const float du = u - 46, dv = v - 62;
            const float spot = 0.45f * expf(-(du * du + dv * dv) / 400.0f), low = (v - 120) / 60;
            uint8_t c = spot > 0.06f ? lc_mix(LC_BLUE, LC_BR_BLUE, spot, x, y) : LC_BLUE;
            if (low > 0) c = lc_mix(LC_BLUE, LC_BLACK, low * 0.8f, x, y);
            lc_pset(&cv, x, y, c);
        }
    /* The aprons below the inlanes, in brushed metal. */
    for (int y = Y(136); y < Y(178) && y < cv.h; y++)
        for (int x = 0; x < cv.w; x++) {
            const float u = (x + .5f - OX) / S, v = (y + .5f - OY) / S;
            const bool left = u > 4 && u < 26 && v > 136 + (u - 4) + 2;
            const bool right = u > 66 && u < 88 && v > 136 + (88 - u) + 2;
            const bool foot = v > 170 && ((u > 4 && u < 34) || (u > 58 && u < 88));
            if (left || right || foot) lc_shade(&cv, x, y, 0.25f + 0.3f * (1 - (v - 136) / 42), LC_RAMP_METAL);
        }
    /* Playfield art. */
    lc_text(&cv, X(46) - lc_text_w("LAKE", 2) / 2, Y(89), "LAKE", 2, LC_BR_BLUE);
    lc_text(&cv, X(46) - lc_text_w("SHARK", 2) / 2, Y(89) + 12, "SHARK", 2, LC_BR_BLUE);
    /* Rails, gate and posts. */
    for (int i = 0; i < nseg; i++) {
        const seg_t *s = &segs[i];
        if (s->kind == K_WALL) rail(s->ax, s->ay, s->bx, s->by, LC_WHITE, LC_GREY);
        else if (s->kind == K_GATE) rail(s->ax, s->ay, s->bx, s->by, LC_GREY, LC_BLUE);
        else if (s->kind == K_RUBBER && s->ay == 18) {
            rail(s->ax, s->ay, s->bx, s->by, LC_WHITE, LC_GREY);
            lc_ball(&cv, X(s->ax), Y(s->ay), 1, LC_RAMP_METAL);
        }
    }
    /* Slingshots: white plastic, red rubber. */
    fill_tri((v2){ 8, 116 }, (v2){ 8, 139 }, (v2){ 19, 147 }, LC_RAMP_GREY, 0.35f, 0.85f);
    fill_tri((v2){ 84, 116 }, (v2){ 84, 139 }, (v2){ 73, 147 }, LC_RAMP_GREY, 0.35f, 0.85f);
    rail(8, 139, 19, 147, LC_RED, LC_RED);
    rail(84, 139, 73, 147, LC_RED, LC_RED);
    /* Lane guides: the arrows a ball rolls over. */
    for (int i = 0; i < 3; i++) lc_text(&cv, X(LANE_X[i]) - 1, Y(20), "V", 1, LC_BLUE);
}

static void draw_flipper_2d(const flipper_t *f)
{
    const float c = cosf(f->ang), sn = sinf(f->ang);
    for (int k = 0; k <= 12; k++) {
        const float t = k / 12.0f;
        const float u = f->px + f->side * c * FL_LEN * t, v = f->py + sn * FL_LEN * t;
        lc_fill_circle(&cv, X(u), Y(v), (int)((3.2f - 1.6f * t) * S + 0.5f), LC_RED);
    }
    for (int k = 0; k <= 12; k++) {
        const float t = k / 12.0f;
        const float u = f->px + f->side * c * FL_LEN * t, v = f->py + sn * FL_LEN * t;
        lc_fill_circle(&cv, X(u), Y(v), (int)((3.2f - 1.6f * t) * S - 0.5f), LC_BR_WHITE);
    }
    lc_ball(&cv, X(f->px), Y(f->py), 1, LC_RAMP_METAL);
}

static void draw_table_2d(uint32_t now)
{
    memcpy(cv.d, bg_2d, (size_t)cv.w * cv.h);
    /* Slings flash when they kick. */
    for (int i = 0; i < 2; i++) {
        const bool hot = sling_lit[i] && now - sling_lit[i] < 120;
        if (i == 0) rail(8, 116, 19, 147, hot ? LC_BR_YELLOW : LC_BR_RED, hot ? LC_BR_WHITE : LC_RED);
        else rail(84, 116, 73, 147, hot ? LC_BR_YELLOW : LC_BR_RED, hot ? LC_BR_WHITE : LC_RED);
    }
    /* Pop bumpers: a red skirt, a steel cap that blazes when struck. */
    for (int i = 0; i < bumper_count; i++) {
        const bumper_t *b = &bumpers[i];
        const bool hot = b->lit && now - b->lit < 110;
        const int r = (int)(b->r * S + 0.5f);
        lc_fill_circle(&cv, X(b->x), Y(b->y), r + 1, hot ? LC_BR_YELLOW : LC_RED);
        lc_circle(&cv, X(b->x), Y(b->y), r + 1, hot ? LC_BR_WHITE : LC_BR_RED);
        lc_ball(&cv, X(b->x), Y(b->y) - 1, r - 2, hot ? LC_RAMP_FIRE : LC_RAMP_METAL);
    }
    /* Drop targets: standing red, or sunk flat. */
    for (int i = 0; i < 3; i++) {
        const int y0 = Y(DROP_V[i] - 4), y1 = Y(DROP_V[i] + 4);
        if (drop_up[i]) {
            lc_fill_rect(&cv, X(4) + 1, y0, X(8) - X(4), y1 - y0, LC_BR_RED);
            lc_line(&cv, X(8), y0, X(8), y1 - 1, LC_BR_WHITE);
        } else
            lc_line(&cv, X(4) + 1, y0, X(4) + 1, y1 - 1, LC_RED);
    }
    /* The MESH standups and their letters. */
    for (int i = 0; i < 4; i++) {
        const int y0 = Y(MESH_V[i] - 4), y1 = Y(MESH_V[i] + 4);
        lc_fill_rect(&cv, X(85), y0, X(88) - X(85), y1 - y0, mesh_lit[i] ? LC_BR_CYAN : LC_CYAN);
        const char l[2] = { "MESH"[i], 0 };
        put(X(80) / 2, Y(MESH_V[i]) / 3, l, FELT(mesh_lit[i] ? LAP_CYAN | LAP_BRIGHT : LAP_BLACK));
    }
    /* Lane lights and the multiplier ladder. */
    for (int i = 0; i < 3; i++)
        lc_fill_circle(&cv, X(LANE_X[i]), Y(36), 2, lane_lit[i] ? LC_BR_YELLOW : LC_YELLOW);
    static const char *const LADDER[4] = { "2X", "3X", "4X", "5X" };
    for (int i = 0; i < 4; i++)
        put(X(46) / 2 - 5 + i * 3, Y(116) / 3, LADDER[i], FELT(mult >= i + 2 ? LAP_YELLOW | LAP_BRIGHT : LAP_BLACK));
    /* Ball save, between the flippers. */
    if (now < save_until && (now / 200) % 2)
        put(X(46) / 2 - 2, Y(169) / 3, "SAVE", INK(LAP_GREEN | LAP_BRIGHT));
    draw_flipper_2d(&flip[0]);
    draw_flipper_2d(&flip[1]);
    /* The plunger, pulled back as far as the finger has pulled it. */
    const float pull = in_lane ? power * 5 : 0;
    lc_fill_rect(&cv, X(90), Y(176 + pull), X(94) - X(90), Y(181) - Y(176 + pull) + 1, LC_GREY);
    lc_line(&cv, X(90), Y(176 + pull), X(94) - 1, Y(176 + pull), LC_BR_WHITE);
    /* The ball, with a short smear behind it when it is quick. */
    const v2 bp = in_lane ? (v2){ LANE_U, LANE_V + pull } : ball;
    const int br = (int)(BALL_R * S + 0.5f);
    for (int i = 3; i >= 1; i--)
        if (!in_lane) lc_fill_circle(&cv, X(trail[i].x), Y(trail[i].y), br - 1 - (i > 1), i == 1 ? LC_GREY : LC_BLACK);
    lc_circle(&cv, X(bp.x), Y(bp.y), br + 1, LC_BLACK);
    lc_ball(&cv, X(bp.x), Y(bp.y), br, LC_RAMP_GREY);
    if(machine==ION) {
        rail(39,101,39,109,LC_WHITE,LC_GREY);
        rail(53,101,53,109,LC_WHITE,LC_GREY);
        const float d=3.0f*cosf(spinner_angle);
        lc_fill_rect(&cv,X(41),Y(105-fabsf(d)),X(51)-X(41),
            (int)(2*fabsf(d)*S)+1,fabsf(spinner_speed)>3 ? LC_BR_CYAN:LC_CYAN);
    }
    /* Scores floating up off whatever was hit. */
    for (int i = 0; i < POPS; i++) {
        if (!pops[i].t0) continue;
        const uint32_t age = now - pops[i].t0;
        if (age > 700) { pops[i].t0 = 0; continue; }
        const int y = Y(pops[i].v) - (int)(age / 60);
        put(X(pops[i].u) / 2 - (int)strlen(pops[i].s) / 2, y / 3, pops[i].s,
            FELT(age < 350 ? LAP_WHITE | LAP_BRIGHT : LAP_WHITE));
    }
}
