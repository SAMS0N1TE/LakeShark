/* CARRIER's screen: the space view through the canopy, the dash under it, the
 * four panels. Everything here reads the game through game.h (and the effect
 * pools in internal.h) and draws; no game rule lives on this side. */
#ifndef CARRIER_UI_H
#define CARRIER_UI_H
#include "game.h"
#include "lap_canvas.h"
#include "walk.h"

#define UI_FOV 62
enum { UI_VIEW_COCKPIT, UI_VIEW_CHASE, UI_VIEW_TACTICAL };
/* the HUD's parts STATUS can hide, and the flight HUD's own green, apart from the amber cockpit */
enum { UI_HUD_LADDER = 1, UI_HUD_TAPES = 2, UI_HUD_NAV = 4, UI_HUD_JUMP = 8, UI_HUD_TARGET = 16 };
#define FHUD    LAP_GREEN
#define FHUD_HI (LAP_GREEN | LAP_BRIGHT)
#define UI_LAUNCH_S 2.4f            /* seconds in the slot leaving */
#define UI_DOCK_S 2.2f              /* and arriving */
#define UI_ROUND(x) ((int)floorf((x) + .5f))
#define UI_BLASTS 6

typedef struct {
    l3_vec p;
    float life, size;               /* life 1 -> 0 */
} ui_blast;

typedef struct {
    int x, y, w, h;                 /* cells */
} ui_rect;

enum { UI_MODE_FLIGHT, UI_MODE_NAV, UI_MODE_COMMS, UI_MODE_SYS, UI_MODES };
enum { UI_NAV_NONE, UI_NAV_STATION, UI_NAV_PLANET, UI_NAV_STAR, UI_NAV_CONTACT, UI_NAV_PEER, UI_NAV_POI };
#define UI_REPLIES 5

typedef struct {                    /* a place NAV can fly to */
    int kind, index;
    l3_vec position;
    float radius;
    char name[28];
} ui_dest;

typedef struct {                    /* another commander heard on the mesh */
    char id[20], name[16], system[16], rank[16];
    l3_vec position;
    l3_quat attitude;
    uint32_t heard_ms;
    bool same_galaxy;
} ui_peer;

typedef struct {
    char text[60];
    bool mine, carrier;
} ui_line;

typedef struct {
    /* layout, in cells, from the screen each frame */
    int cols, rows;
    ui_rect view, dash, tabs, msg, scope, left, right, throttle, keys[5], close, viewkey, quit_stay, quit_leave;
    ui_rect tab_keys[4], inst;       /* the panel tabs; the block the instrument dots cover */
    bool quit_ask;              /* the LEAVE THE COCKPIT question is up */
    bool keys_help;             /* the keyboard card, on ? */
    ui_rect talk_keys[3], throttle_grab, dead_key, rebuy_key, steer_key;
    int steer;                  /* 0 gentle, 1 normal, 2 quick */
    bool voice;                 /* the ship's computer speaks */
    bool sound;                 /* tones for guns, hits, alarms */
    int mode;                   /* the dash's middle: UI_MODE_* */
    ui_rect modebar, mode_keys[UI_MODES], mode_info, middle;
    int nav_kind, nav_index, nav_scroll, comms_tab;
    bool autopilot, mesh_on;
    int mesh_slot;              /* MeshCore channel slot 0..3 for CARRIER lines */
    int traffic, encounters;    /* game_set_traffic / game_set_encounters, kept in ui.cfg */
    ui_rect set_keys[6];        /* STATUS settings: TRAFFIC, ENCOUNTERS, MESH CH, SOUND, AIM ASSIST, ZOOM */
    ui_rect hud_keys[5];        /* STATUS: the HUD's parts, each shown or hidden */
    uint8_t hud_off;            /* UI_HUD_* bits of the parts hidden */
    char nav_label[40];
    char nav_peer[20];          /* the chosen commander's id: slots move as peers expire */
    float fx, fy;
    float shake, dt;            /* camera shake from hits; this frame's seconds */
    float zoom;                 /* smart zoom on a far target near the nose, 1 = none */
    bool autozoom;              /* STATUS: ZOOM AUTO */
    int aim;                    /* STATUS: aim assist 0 off, 1 normal, 2 strong (touch) */
    int view_mode;              /* UI_VIEW_COCKPIT, _CHASE, _TACTICAL: the VIEW placard, key o */
    bool throttle_touched;
    int tips;                   /* first-flight tips shown so far (3 = done, saved) */
    float tip_t;      /* R/F or a throttle tap since the autopilot last looked */               /* the scene's focal lengths in dots, as lap3d sets them */
    float fuel_seen, scoop_t;  /* fuel last frame; seconds since it last rose */
    int nav_label_col, nav_label_row;
    int talk_n;
    bool landscape, wide;           /* wide: a desktop grid, the console low across the bottom */
    bool panel_full;                /* wide, a panel open: it has the full height */
    /* drawing */
    l3_scene *scene;
    uint8_t *dots;                  /* the view, copied out of the scene and drawn over */
    int dw, dh;                     /* its size in dots */
    lap_canvas_t dash_c;
    /* touch */
    bool down, dragging, consumed;
    int grab;                       /* what the finger went down on: UI_G_* */
    float stick_x, stick_y;         /* -1..1 while steering */
    int x0, y0, x, y;               /* pixels */
    uint32_t down_ms;
    /* state the screen keeps */
    bool fire_latch, fire2_latch, firing_shown;
    float key_pitch, key_roll, key_yaw, key_tx, key_ty;   /* keyboard impulses, decaying */
    int galaxy_pick, market_pick, log_scroll, market_tab;
    float gx_x, gx_y, gx_half;  /* GALAXY: the chart's centre and half width in ly */
    int gx_colour;              /* 0 economy, 1 safety, 2 tech */
    int route[24], route_n, route_goal;   /* hops to the chosen system; the goal a plotted route is going to, -1 none */
    float t, hit_flash, last_fore, last_aft, launch_t, dock_t;   /* the slot, out and in */
    int debris_seen, msg_rev;
    uint32_t msg_ms;
    float level_pitch, level_roll;  /* motion steering's level, taken when switched on */
    bool was_docked;
    /* on foot, docked (ui_walk.c) */
    bool walking, walk_resume;      /* walking now; back to walking when the panel opened on foot closes */
    walk_state walk;
    walk_ship walk_ship;
    int walk_grab;                  /* the finger: 0 none, 1 the stick, 2 looking, 3 a key */
    int walk_lx, walk_ly;           /* last finger position while looking, pixels */
    float walk_fwd, walk_strafe, walk_turn, walk_look;   /* keyboard impulses, decaying */
    float walk_stick_x, walk_stick_y;
    ui_rect walk_use_key, walk_seat_key, walkkey, walk_back_key;
    ui_rect walk_pad[4];            /* hold to walk: forward, back, turn left, turn right */
    /* the wide console: status lamps and push buttons (ui_dash.c) */
    ui_rect lamps[15], ctls[15], ctl_box;
    int nlamps, nctls;
    bool free_cam;                  /* chase view: dragging swings the camera, not the ship */
    int fsd_said;                   /* the drive state last spoken */
} ui_state;
extern ui_state ui;

enum { UI_G_NONE, UI_G_VIEW, UI_G_TAB, UI_G_KEY, UI_G_THROTTLE, UI_G_SCOPE, UI_G_PANEL };
enum { UI_K_FIRE, UI_K_FIRE2, UI_K_CRUISE, UI_K_TARGET, UI_K_ACTION, UI_K_ECM = 100, UI_K_MISSILE = 101 };

void ui_layout(void);
void ui_space_reset(uint32_t seed);
void ui_planets_reset(void);
void ui_planets_paint(void);
void ui_view_draw(void);
/* Text and cells over the view with a black ground (attr bg 0) take the scene's
   colour under them instead; an explicit ground is kept. */
uint8_t ui_ground(int col, int row, uint8_t fg);
void ui_put(int x, int y, const char *s, uint8_t attr);
void ui_cell(int x, int y, int16_t ch, uint8_t attr);
void ui_dash_draw(void);
void ui_panel_draw(void);
bool ui_panel_tap(int col, int row);
void ui_effects_update(float dt);
void ui_exit_draw(void);
void ui_keys_draw(void);
void ui_talk_draw(void);
void ui_save_settings(void);
void ui_route_update(void);   /* GALAXY: plots the next hop after each arrival */
void ui_galaxy_open(void);
void ui_galaxy_drag(float dx_dots, float dy_dots);
bool ui_galaxy_key(lap_key_t k, char ch);
void ui_apply_aim(void);      /* STATUS aim assist to the guns */
void ui_voice_reset(void);
void ui_voice_update(void);
void ui_dead_draw(void);
void ui_sys_draw(const ui_rect *a);
bool ui_sys_tap(int col, int row);
void ui_weapons_dots(void);
void ui_incoming_dots(void);
void ui_combat_words(void);
void ui_target_parts(int x, int y);
const char *ui_weapon_name(int kind);
void ui_sound_kit(int which);
bool ui_boost_context(void);
void ui_sys_draw(const ui_rect *a);
bool ui_sys_tap(int col, int row);
void ui_weapons_dots(void);
void ui_incoming_dots(void);
void ui_combat_words(void);
void ui_target_parts(int x, int y);
const char *ui_weapon_name(int kind);
void ui_sound_kit(int which);
bool ui_boost_context(void);
void ui_sys_draw(const ui_rect *a);
bool ui_sys_tap(int col, int row);
void ui_weapons_dots(void);
void ui_incoming_dots(void);
void ui_combat_words(void);
void ui_target_parts(int x, int y);
const char *ui_weapon_name(int kind);
void ui_sound_kit(int which);
bool ui_boost_context(void);
void ui_sound_update(void);
void ui_sound_reset(void);
void ui_sound_far_boom(void);
void ui_sound_boost(void);
void ui_halos(void);
float ui_fov(void);          /* the view's field of view, narrowed by the smart zoom */
void ui_zoom_update(float dt);
void ui_holo_paint(void);    /* its dots, into the view before lap_dots */
void ui_tactical_paint(void);  /* the TACTICAL plot: ui_tactical.c */
void ui_tactical_words(void);
bool ui_tactical_tap(int col, int row);
void ui_holo_draw(void);
void ui_holo_free(void);
void ui_damage_dots(float dt);
void ui_glitch_cells(void);
void ui_shake_update(float dt);
extern l3_vec render_shake;
extern int render_view;                    /* 0 cockpit, 1 chase camera (render.c) */
extern float render_cam_yaw, render_cam_pitch;   /* the chase camera swung round the ship, degrees */
extern l3_vec render_eye, render_fwd, render_up;
void ui_tips_draw(float dt);
void ui_modebar_draw(void);
void ui_nav_draw(const ui_rect *a);
bool ui_nav_tap(int col, int row);
void ui_comms_draw(const ui_rect *a);
bool ui_comms_tap(int col, int row);
int ui_nav_list(ui_dest *out, int max);
bool ui_nav_target(ui_dest *out);
void ui_autopilot(float dt);
int ui_net_count(void);
const ui_peer *ui_net_peer(int i);
int ui_net_lines(void);
const ui_line *ui_net_line(int i);
bool ui_net_unread(void);
void ui_net_mark_read(void);
bool ui_net_tx_ok(void);
void ui_net_announce(void);
bool ui_net_say(const char *words);
void ui_net_update(void);
void render_peers(l3_scene *sc);
void render_pois(l3_scene *sc);
void render_world_extras(l3_scene *sc);
void ui_poi_paint(void);
void ui_poi_labels(void);
const char *ui_poi_status(char *buf, int n);
int render_material(int which);
extern void (*render_extra)(l3_scene *sc);
int render_planet_kind(int i);
void render_hangar(l3_scene *sc, float t);
void render_tunnel(l3_scene *sc, float k, bool leaving);   /* the slot, 0..1 through it */
void render_walk(l3_scene *sc, const walk_state *w, float t);
void render_walk_forget(void);      /* the walk scene is about to be freed */
/* ui_walk.c: on foot, docked */
void ui_walk_start(void);
void ui_walk_stop(void);
void ui_walk_draw(void);
bool ui_walk_finger(float dt, int *tap_col, int *tap_row);   /* true: a tap to hand to tap() */
bool ui_walk_tap(int col, int row);
bool ui_walk_key(lap_key_t k, char ch);
void render_hull_card(l3_scene *sc, int ship, float t, float lift);
void render_blast(l3_vec p, float size);
void render_tick(float dt);
extern void (*render_new_system)(uint32_t seed);
bool ui_in(const ui_rect *r, int col, int row);
bool ui_project(l3_vec w, float *x, float *y, float *z);
void ui_key_box(const ui_rect *r, const char *label, const char *sub, uint8_t fg, uint8_t bg, bool lit);
void ui_tab_key(const ui_rect *r, const char *s, bool on, bool usable, bool divider);
void ui_mode_key(const ui_rect *r, const char *s, bool on, bool divider);   /* dash modes: cyan, marked not filled */
void ui_toggle_box(const ui_rect *r, const char *label, const char *sub, uint8_t fg, bool on);
/* ui_console.c: the wide console's lamps and buttons, the frame shift drive */
void ui_console_draw(void);
bool ui_console_tap(int col, int row);
void ui_fsd_voice(void);
const char *ui_fsd_words(uint8_t *ink, bool *flash);
bool ui_fsd_engage(void);
#endif
