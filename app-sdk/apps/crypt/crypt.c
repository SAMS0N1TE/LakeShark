/* CRYPT for LakeShark: walk a dark crypt drawn with lap3d's grid walls,
   lit only by the torches you pass, and gather the gems floating in its
   rooms. Hold the left or right edge of the glass to turn, the middle to
   walk; arrows work too. Tap a gem in reach to take it.
   Uses API 1.2 (lap3d): l3_walls, lamps, particles, l3_pick. */
#include "lakeshark_app.h"
#include "lap3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 16
#define D 16
#define PI 3.14159265f

/* # stone, B brick, . floor, T a torch on the wall beside it, G a gem. */
static const char *const MAP[D] = {
    "################",
    "#..T...#...G...#",
    "#.##.#.#.###.#.#",
    "#.#G.#...#T..#.#",
    "#.####B###.###.#",
    "#....T.....#..G#",
    "####.#####.#.###",
    "#G...#...#.#...#",
    "#.#B##.#.#.##B.#",
    "#.#..T.#...#...#",
    "#.#.####B###.#.#",
    "#...#G.....T.#.#",
    "###.#.######.#.#",
    "#T..#....G...#.#",
    "#..........#..T#",
    "################",
};

static l3_scene *sc;
static uint8_t grid[W * D];
static int m_stone, m_brick, m_floor, m_ceiling, m_gem, m_flame;
static l3_vec torch[16], flame[16];
static int ntorch;
static l3_vec gem[16];
static bool taken[16];
static int ngem, got;
static float px = 1.5f, pz = 1.5f, yaw = 180;
static uint32_t last_ms;
static char hud[40];

static const uint8_t STONE[4]   = { LAP_BLACK, 8, LAP_WHITE, 15 };
static const uint8_t BRICK[4]   = { LAP_BLACK, LAP_RED, 9, 11 };
static const uint8_t FLOOR_[3]  = { LAP_BLACK, 8, LAP_WHITE };
static const uint8_t CEIL[2]    = { LAP_BLACK, 8 };
static const uint8_t GEM[4]     = { LAP_BLUE, LAP_CYAN, 14, 15 };

static bool solid(float x, float z)
{
    const int ix = (int)floorf(x), iz = (int)floorf(z);
    return ix < 0 || iz < 0 || ix >= W || iz >= D || grid[iz * W + ix];
}

static void setup(void)
{
    m_stone   = l3_material(sc, STONE, 4, 0.05f, L3_GRAIN);
    m_brick   = l3_material(sc, BRICK, 4, 0.05f, L3_GRAIN);
    m_floor   = l3_material(sc, FLOOR_, 3, 0, L3_CHECKER);
    m_ceiling = l3_material(sc, CEIL, 2, 0, 0);
    m_gem     = l3_material(sc, GEM, 4, 0.9f, 0);
    m_flame   = l3_material(sc, L3_RAMP_FIRE, 4, 0, L3_UNLIT);
    for (int z = 0; z < D; z++)
        for (int x = 0; x < W; x++) {
            const char c = MAP[z][x];
            grid[z * W + x] = (uint8_t)(c == '#' ? m_stone + 1 : c == 'B' ? m_brick + 1 : 0);
            if (c == 'G' && ngem < 16) gem[ngem++] = l3v(x + 0.5f, 0.45f, z + 0.5f);
            if (c == 'T' && ntorch < 16) torch[ntorch++] = l3v(x + 0.5f, 0.7f, z + 0.5f);
        }
    l3_sun(sc, l3v(0, -1, 0), 0, 0.14f);            /* no sun down here: torches and a little light */
    l3_sky(sc, LAP_BLACK, LAP_BLACK);
    l3_fog(sc, LAP_BLACK, 5.0f, 10.0f);
}

/* The four torches nearest the player light the walls; each flickers. */
static void torches(uint32_t now)
{
    int best[4] = { -1, -1, -1, -1 };
    float bd[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
    for (int i = 0; i < ntorch; i++) {
        const float dx = torch[i].x - px, dz = torch[i].z - pz, d = dx * dx + dz * dz;
        for (int k = 0; k < 4; k++)
            if (d < bd[k]) {
                for (int j = 3; j > k; j--) { bd[j] = bd[j - 1]; best[j] = best[j - 1]; }
                bd[k] = d; best[k] = i;
                break;
            }
    }
    for (int k = 0; k < 4; k++) {
        if (best[k] < 0) { l3_lamp(sc, k, l3v(0, 0, 0), 0, 1); continue; }
        const float flick = 0.85f + 0.15f * sinf(now * 0.013f + best[k] * 1.7f) * sinf(now * 0.007f + best[k]);
        l3_lamp(sc, k, torch[best[k]], 2.4f * flick, 6.0f);
    }
    for (int i = 0; i < ntorch; i++)       /* the flame itself, dancing a little */
        flame[i] = l3v(torch[i].x + 0.03f * sinf(now * 0.02f + i), torch[i].y + 0.04f * sinf(now * 0.031f + i * 2),
                       torch[i].z);
}

static void walk(float dt)
{
    lap_touch_t f;
    int cw, ch;
    lap_cell_px(&cw, &ch);
    float turn = 0, go = 0;
    if (lap_touch(&f) && f.down) {
        const int w = lap_cols() * cw;
        if (f.x < w / 3) turn = 1;
        else if (f.x > w * 2 / 3) turn = -1;
        else go = 1;
    }
    yaw += turn * 110 * dt;
    if (go) {
        const float y = yaw * PI / 180, step = 1.6f * dt;
        const float nx = px - sinf(y) * step, nz = pz - cosf(y) * step;
        if (!solid(nx + (nx > px ? 0.2f : -0.2f), pz)) px = nx;     /* slide along walls */
        if (!solid(px, nz + (nz > pz ? 0.2f : -0.2f))) pz = nz;
    }
}

static void enter(void)
{
    lap_fullscreen(true);
    lap_keep_awake(true);
    last_ms = lap_millis();
}

static void leave(void) { l3_destroy(sc); sc = NULL; }

static void draw(void)
{
    if (!sc) {
        sc = l3_create(lap_cols(), lap_rows());
        if (!sc) { lap_put_str(1, 1, "not enough memory", LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK)); return; }
        setup();
    }
    l3_resize(sc, lap_cols(), lap_rows());
    const uint32_t now = lap_millis();
    float dt = (now - last_ms) / 1000.0f;
    last_ms = now;
    if (dt > 0.1f) dt = 0.1f;
    walk(dt);
    torches(now);

    l3_begin(sc);
    l3_camera_look(sc, l3v(px, 0.5f, pz), yaw, 0, 70);
    l3_flags(sc, L3_STATIC);
    l3_tag(sc, 0);
    l3_floor(sc, 0, m_floor);
    l3_flags(sc, L3_STATIC | L3_TWO_SIDED);
    l3_quad(sc, l3v(0, 1, 0), l3v(W, 1, 0), l3v(W, 1, D), l3v(0, 1, D), m_ceiling);
    l3_flags(sc, L3_STATIC);
    l3_walls(sc, grid, W, D, 1, 1, l3v(0, 0, 0));
    l3_flags(sc, 0);
    for (int i = 0; i < ngem; i++) {
        if (taken[i]) continue;
        l3_tag(sc, 1 + i);
        const float bob = 0.06f * sinf(now * 0.003f + i);
        l3_xform t = l3_pose(l3v(gem[i].x, gem[i].y + bob, gem[i].z), l3_quat_euler(now * 0.09f + i * 40, 45, 35), 1);
        l3_box(sc, &t, l3v(0.09f, 0.09f, 0.09f), m_gem);
    }
    l3_tag(sc, 0);
    l3_particles(sc, flame, ntorch, 0.12f, m_flame);
    l3_end(sc);
    l3_show(sc, 0, 0);

    l3_stats_t st;
    l3_stats(sc, &st);
    snprintf(hud, sizeof(hud), " GEMS %d/%d  %2u ms ", got, ngem, (unsigned)(st.us_total / 1000));
    lap_put_str(0, 0, hud, LAP_ATTR(got == ngem ? LAP_YELLOW | LAP_BRIGHT : LAP_CYAN | LAP_BRIGHT, LAP_BLACK));
    if (got == ngem) lap_put_str(0, 1, " ALL GEMS FOUND ", LAP_ATTR(LAP_BLACK, LAP_YELLOW));
}

/* A tap on a gem within reach takes it. */
static bool touch(int col, int row)
{
    if (!sc) return false;
    const int tag = l3_pick(sc, col * 2 + 1, row * 3 + 1);
    if (tag < 1 || tag > ngem || taken[tag - 1]) return false;
    const l3_vec g = gem[tag - 1];
    if ((g.x - px) * (g.x - px) + (g.z - pz) * (g.z - pz) > 2.5f * 2.5f) return false;
    taken[tag - 1] = true;
    got++;
    lap_tone(0, 1, 0, LAP_WAVE_SINE);
    lap_tone(880, 60, 40, LAP_WAVE_TRIANGLE);
    lap_tone(1320, 90, 35, LAP_WAVE_TRIANGLE);
    lap_haptic(LAP_HAPTIC_CONFIRM);
    return true;
}

static bool key(lap_key_t k, char ch)
{
    (void)ch;
    const float y = yaw * PI / 180;
    if (k == LAP_KEY_LEFT)  { yaw += 15; return true; }
    if (k == LAP_KEY_RIGHT) { yaw -= 15; return true; }
    if (k == LAP_KEY_UP || k == LAP_KEY_DOWN) {
        const float s = k == LAP_KEY_UP ? 0.35f : -0.35f, nx = px - sinf(y) * s, nz = pz - cosf(y) * s;
        if (!solid(nx, nz)) { px = nx; pz = nz; }
        return true;
    }
    return false;
}

LAP_ICON_PX(
    "88888888888888888888",
    "8000000000000000000.",
    "8077770000000077770.",
    "8077770000000077770.",
    "80777700bb000077770.",
    "8077770b99b00077770.",
    "80777700b9000077770.",
    "8077770009000077770.",
    "80777700770000777708",
    "80777700000000777708",
    "80777700000000777708",
    "8077770000ee00777708",
    "807777000eeee0777708",
    "8077770000ee00777708",
    "80777700000000777708",
    "88888888888888888888",
    "8888888888888888888.",
    "....................");

LAP_APP(.id = "crypt", .name = "CRYPT", .sub = "gems in the dark", .version = "1.0",
        .author = "LakeShark", .color = LAP_RED,
        .icon_art = { "##########", "#  ,  #  #", "# ### # ##", "#  *  ,  #", "##########", "  CRYPT   " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
