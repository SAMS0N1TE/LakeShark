/* ORBIT for LakeShark: what lap3d can do, in one scene. A steel ball
   bouncing, a red die tumbling, a green ring spinning, a turned ivory vase
   and a solid 3D floating over them, on a checker floor under the sun, with
   shadows and outlines. Drag to turn the camera; tap a thing to make it
   jump. The top line is the engine's own time for the frame.
   Uses API 1.2 (lap3d) and lap_touch. */
#include "lakeshark_app.h"
#include "lap3d.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { T_BALL = 1, T_DIE, T_RING, T_VASE, T_WORD };

static l3_scene *sc;
static int m_floor, m_steel, m_die, m_pip, m_ring, m_vase, m_word;
static l3_mesh vase, word;
static bool have_vase, have_word;
static float yaw = 30, pitch = 18, dist = 14.0f;
static float hop[6];                    /* per tag: height of a jump, decaying */
static float hop_v[6];
static uint32_t last_ms;
static bool dragging;
static int drag_x, drag_y;
static float drag_yaw, drag_pitch;
static char hud[48];

static const uint8_t FLOOR_INKS[3] = { LAP_BLACK, 8, LAP_WHITE };
static const uint8_t DIE_INKS[3]   = { LAP_BLACK, LAP_RED, 9 };
static const uint8_t PIP_INKS[2]   = { LAP_BLACK, 8 };
static const uint8_t WORD_INKS[4]  = { LAP_BLUE, LAP_CYAN, 14, 15 };

/* Pips on the die: a 7 x 7 picture where 1 is a pip, once per face. */
#define X 0xFF
static const uint8_t DIE_TEX[49] = {
    X, X, X, X, X, X, X,
    X, 1, X, X, X, 1, X,
    X, X, X, X, X, X, X,
    X, X, X, 1, X, X, X,
    X, X, X, X, X, X, X,
    X, 1, X, X, X, 1, X,
    X, X, X, X, X, X, X,
};
#undef X

static void setup(void)
{
    m_floor = l3_material(sc, FLOOR_INKS, 3, 0, L3_CHECKER);
    m_steel = l3_material(sc, L3_RAMP_METAL, 4, 0.9f, 0);
    m_pip   = l3_material(sc, PIP_INKS, 2, 0, 0);
    m_die   = l3_material(sc, DIE_INKS, 3, 0.2f, 0);
    m_ring  = l3_material(sc, L3_RAMP_GREEN, 4, 0.6f, 0);
    m_vase  = l3_material(sc, L3_RAMP_IVORY, 4, 0.3f, 0);
    m_word  = l3_material(sc, WORD_INKS, 4, 0.4f, 0);
    /* the pip texture's 1 is the dark material; 0xFF keeps the die's own */
    uint8_t tex[49];
    for (int i = 0; i < 49; i++) tex[i] = DIE_TEX[i] == 1 ? (uint8_t)m_pip : 0xFF;
    l3_material_texture(sc, m_die, l3_texture(sc, 7, 7, tex));

    static const float profile[] = { 0.0f, 0.0f, 0.55f, 0.0f, 0.75f, 0.35f, 0.62f, 0.9f,
                                     0.35f, 1.3f, 0.32f, 1.6f, 0.45f, 1.8f };
    have_vase = l3_mesh_lathe(&vase, profile, 7, 16);
    have_word = l3_mesh_text(&word, "3D", 1.2f);
    l3_reserve(sc, 64, 4096, 6144);

    l3_sun(sc, l3v(-0.5f, -1.0f, -0.35f), 0.9f, 0.25f);
    l3_shadows(sc, true);
    l3_sky(sc, LAP_BLACK, LAP_BLUE);
    l3_outline(sc, LAP_BLACK);
    l3_fog(sc, LAP_BLACK, 16, 26);
}

static void camera(void)
{
    const float y = yaw * 3.14159265f / 180, p = pitch * 3.14159265f / 180;
    const l3_vec eye = l3v(sinf(y) * cosf(p) * dist, sinf(p) * dist + 0.6f, cosf(y) * cosf(p) * dist);
    l3_camera(sc, eye, l3v(0, 1.9f, 0), 40);
}

static void step(float dt)
{
    if (!dragging) yaw += dt * 9;                   /* a slow turn when left alone */
    for (int i = 1; i < 6; i++) {                   /* jumps: up, gravity, a small bounce */
        hop_v[i] -= 18 * dt;
        hop[i] += hop_v[i] * dt;
        if (hop[i] < 0) { hop[i] = 0; hop_v[i] = hop_v[i] < -2 ? -hop_v[i] * 0.35f : 0; }
    }
}

static void scene(float t)
{
    l3_begin(sc);
    camera();

    l3_flags(sc, L3_STATIC);
    l3_tag(sc, 0);
    l3_floor(sc, 0, m_floor);

    l3_flags(sc, L3_CAST);
    /* the steel ball keeps bouncing on its own */
    const float b = fabsf(sinf(t * 2.6f)) * 1.4f;
    l3_tag(sc, T_BALL);
    l3_sphere(sc, l3v(-2.2f, 0.7f + b + hop[T_BALL], 0.6f), 0.7f, m_steel);

    l3_tag(sc, T_DIE);
    l3_xform die = l3_pose(l3v(1.8f, 0.6f + hop[T_DIE], 1.2f),
                           l3_quat_euler(t * 40, t * 25, t * 15), 1);
    l3_rbox(sc, &die, l3v(0.6f, 0.6f, 0.6f), 0.14f, m_die);

    l3_tag(sc, T_RING);
    l3_xform ring = l3_pose(l3v(0.4f, 1.1f + hop[T_RING], -1.8f),
                            l3_quat_euler(t * 60, 35, 0), 1);
    l3_torus(sc, &ring, 0.75f, 0.25f, m_ring);

    if (have_vase) {
        l3_tag(sc, T_VASE);
        l3_flags(sc, L3_CAST | L3_SMOOTH);
        l3_xform v = l3_pose(l3v(-1.0f, hop[T_VASE], -0.6f), l3_quat_euler(t * 20, 0, 0), 1);
        l3_draw_mesh(sc, &vase, &v, m_vase);
    }
    if (have_word) {
        l3_tag(sc, T_WORD);
        l3_flags(sc, L3_CAST);
        l3_xform w = l3_pose(l3v(0, 3.6f + 0.15f * sinf(t * 1.7f) + hop[T_WORD], -0.5f),
                             l3_quat_euler(sinf(t * 0.6f) * 35, 0, 0), 0.32f);
        l3_draw_mesh(sc, &word, &w, m_word);
    }
    l3_end(sc);
}

static void enter(void)
{
    lap_fullscreen(true);
    lap_keep_awake(true);
    last_ms = lap_millis();
}

static void leave(void)
{
    if (have_vase) l3_mesh_free(&vase);
    if (have_word) l3_mesh_free(&word);
    have_vase = have_word = false;
    l3_destroy(sc);
    sc = NULL;
}

static void drag(void)
{
    lap_touch_t f;
    int cw, ch;
    lap_cell_px(&cw, &ch);
    if (!lap_touch(&f) || !f.down) { dragging = false; return; }
    if (!dragging) {
        dragging = true; drag_x = f.x; drag_y = f.y; drag_yaw = yaw; drag_pitch = pitch;
    }
    yaw = drag_yaw - (f.x - drag_x) * 0.4f;
    pitch = drag_pitch + (f.y - drag_y) * 0.25f;
    if (pitch < 6) pitch = 6;
    if (pitch > 70) pitch = 70;
}

static void draw(void)
{
    if (!sc) {
        sc = l3_create(lap_cols(), lap_rows());
        if (!sc) { lap_put_str(1, 1, "not enough memory for 3D", LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK)); return; }
        setup();
    }
    l3_resize(sc, lap_cols(), lap_rows());
    const uint32_t now = lap_millis();
    float dt = (now - last_ms) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    last_ms = now;
    drag();
    step(dt);
    scene(now / 1000.0f);
    l3_show(sc, 0, 0);

    l3_stats_t st;
    l3_stats(sc, &st);
    snprintf(hud, sizeof(hud), " %u ms sh%u ra%u re%u d%u t%u ", (unsigned)(st.us_total / 1000),
             (unsigned)(st.us_shadow / 1000), (unsigned)(st.us_raster / 1000), (unsigned)(st.us_shade / 1000),
             (unsigned)st.dots_shaded, (unsigned)st.triangles);
    lap_put_str(0, 0, hud, LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK));
    lap_put_str(0, lap_rows() - 1, " drag to turn - tap to poke ", LAP_ATTR(LAP_WHITE, LAP_BLACK));
}

/* A tap: whatever shows under the finger jumps. */
static bool touch(int col, int row)
{
    if (!sc) return false;
    const int tag = l3_pick(sc, col * 2 + 1, row * 3 + 1);
    if (tag > 0 && tag < 6 && hop[tag] < 0.05f) { hop_v[tag] = 6.5f; return true; }
    return false;
}

static bool key(lap_key_t k, char ch)
{
    (void)ch;
    if (k == LAP_KEY_LEFT)  { yaw -= 10; return true; }
    if (k == LAP_KEY_RIGHT) { yaw += 10; return true; }
    if (k == LAP_KEY_UP)    { dist = dist > 5 ? dist - 0.8f : dist; return true; }
    if (k == LAP_KEY_DOWN)  { dist = dist < 16 ? dist + 0.8f : dist; return true; }
    return false;
}

LAP_ICON_PX(
    "....................",
    "......ffffff........",
    "....ff777777ff......",
    "...f7777777777f.....",
    "..f777fff77777778...",
    "..f77fffff77777788..",
    ".f777ffff777777778..",
    ".f77777777777777788.",
    ".f77777777777777788.",
    ".f7777777777777788..",
    "..f77777777777778...",
    "..877777777777788...",
    "...8877777777788....",
    ".....8888888888.....",
    "...8.8.8.8.8.8.8....",
    "..8.8.8.8.8.8.8.8...",
    "....................",
    "....................");

LAP_APP(.id = "orbit", .name = "ORBIT", .sub = "3D showcase", .version = "1.0",
        .author = "LakeShark", .color = LAP_CYAN,
        .icon_art = { "   .--.   ", "  /    \\  ", " |  ()  | ", "  \\    /  ", "   '--'   ", "  ORBIT   " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
