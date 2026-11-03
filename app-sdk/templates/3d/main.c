/* @NAME@: @SUB@.
 *
 * Made by `lsdk new` from the 3d template: the firmware's 3D engine
 * (include/lap3d.h), drawn in dots, lit and shadowed in the theme's colours.
 * A ring on a pedestal; drag a finger to walk round it. Nothing moves until
 * a finger does, so a still scene costs almost nothing.
 *
 * Keys: arrows turn the view, o opens OPTIONS. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "lap3d.h"
#include <string.h>

LAP_ICON_PX("....................",
            "....................",
            "......ffffffff......",
            "....ff........ff....",
            "...f....bbbb....f...",
            "...f...b....b...f...",
            "....ff..bbbb..ff....",
            "......ffffffff......",
            ".........77.........",
            ".........77.........",
            "........7777........",
            "......88888888......",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................");

static struct {
    l3_scene *sc;
    int metal, gold, stone;
    float yaw, pitch;
    bool shadows;
    lap_btn_t keys[1];
} s;

static const char *const ONOFF[] = { "OFF", "ON" };
static double opt_get(int id) { (void)id; return s.shadows; }
static void opt_set(int id, double v)
{
    (void)id;
    s.shadows = v > 0.5;
    if (s.sc) l3_shadows(s.sc, s.shadows);
}
static const lap_opt_t OPT_ROWS[] = {
    { .label = "SHADOWS", .kind = LAP_OPT_TOGGLE, .id = 0, .names = ONOFF },
};
static const lap_opt_list_t OPTIONS = { "@NAME@", OPT_ROWS, 1, opt_get, opt_set };

static void draw(void)
{
    const int w = lap_cols(), h = lap_rows();
    const int kh = lap_ui_keys_rows(1), ky = h - kh;
    if (!s.sc) {
        s.sc = l3_create(w, ky);
        if (!s.sc) { lap_put_str(1, 1, "not enough memory for 3D", LAP_ATTR(LAP_UI_ALARM, LAP_BLACK)); return; }
        s.metal = l3_material(s.sc, L3_RAMP_METAL, 4, 0.6f, 0);
        s.gold = l3_material(s.sc, L3_RAMP_FIRE, 4, 0.8f, 0);
        s.stone = l3_material(s.sc, L3_RAMP_SEA, 4, 0.1f, 0);
        l3_shadows(s.sc, s.shadows);
    }
    l3_drag_turn(&s.yaw, &s.pitch, 0.5f);
    l3_begin(s.sc);
    l3_camera_orbit(s.sc, l3v(0, 0.9f, 0), 5, s.yaw, s.pitch, 40);
    l3_flags(s.sc, L3_STATIC);
    l3_floor(s.sc, 0, s.stone);
    l3_flags(s.sc, L3_STATIC | L3_CAST);
    l3_cylinder(s.sc, l3v(0, 0, 0), l3v(0, 0.6f, 0), 0.35f, s.metal);
    l3_xform ring = l3_pose(l3v(0, 1.3f, 0), l3_quat_euler(0, 90, 0), 1);
    l3_torus(s.sc, &ring, 0.6f, 0.15f, s.gold);
    l3_sphere(s.sc, l3v(0, 1.3f, 0), 0.25f, s.metal);
    l3_end(s.sc);
    l3_show(s.sc, 0, 0);

    lap_put_glass(1, 0, "@NAME@", LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK));
    lap_put_glass(1, ky - 1, "drag, or the arrow keys, to walk round", LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK));
    s.keys[0] = (lap_btn_t)LAP_BTN_OPTIONS;
    lap_ui_keys(0, ky, w, kh, s.keys, 1, -1);
}

static bool key(lap_key_t k, char ch)
{
    switch (k) {
    case LAP_KEY_LEFT: s.yaw += 10; return true;
    case LAP_KEY_RIGHT: s.yaw -= 10; return true;
    case LAP_KEY_UP: if (s.pitch < 80) s.pitch += 5; return true;
    case LAP_KEY_DOWN: if (s.pitch > -10) s.pitch -= 5; return true;
    case LAP_KEY_CHAR: return (ch | 0x20) == 'o' && lap_options_open(&OPTIONS);
    default: return false;
    }
}

static bool touch(int col, int row)
{
    return lap_ui_key_hit(col, row) == 0 && lap_options_open(&OPTIONS);
}

static void enter(void)
{
    if (s.sc) return;
    memset(&s, 0, sizeof(s));
    s.yaw = 30;
    s.pitch = 20;
    s.shadows = true;
}

static void leave(void)
{
    l3_destroy(s.sc);
    s.sc = NULL;
}

LAP_APP(.id = "@ID@", .name = "@NAME@", .sub = "@SUB@", .version = "0.1",
        .author = "@AUTHOR@", .color = LAP_YELLOW,
        .icon_art = { "  .----.  ", " ( (()) ) ", "  '----'  ", "    ||    ", "  ======  ", "@ART@" },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
