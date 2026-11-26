#include "flight.h"
#include "display.h"
#include "e3d.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { RELAY, SALVAGE };
enum { ACTIVE, DONE };
enum { TARGETS = 5, COURSE_LENGTH = 18, RING_SIDES = 16 };
static const float spacing = 28.0f;
static const char *const title[] = {"TIDAL RELAY", "SALVAGE RUN"};

static e3d_t *engine;
static e3d_mesh *ship, *gate, *pod, *reef, *water;
static int mode, phase, target, score, misses;
static bool feedback_success;
static float distance, elapsed, altitude, lateral, roll, boost, feedback;
static uint16_t sky[GAME_HEIGHT];
static int frame_geom_us, frame_raster_us, frame_tris;

static uint16_t rgb(int r, int g, int b)
{
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
}

static void background(uint16_t *row, int y, int w, void *unused)
{
    for (int x = 0; x < w; ++x) row[x] = sky[y];
    if (y < 115) {
        /* Deterministic scanline stars, including across both render cores. */
        unsigned hash = (unsigned)(y + 23) * 2654435761u;
        if (hash >> 31) row[(hash >> 8) % w] = 0x7476;
        if (hash & 1) row[(hash >> 19) % w] = 0x9cba;
    }
}

static void triangle(e3d_mesh *mesh, int i, int a, int b, int c, uint32_t color)
{
    mesh->tris[i] = (e3d_tri){ .v = {a, b, c}, .r = color >> 16, .g = color >> 8, .b = color };
}

static e3d_mesh *make_ship(void)
{
    const float vertices[][3] = {
        {1.65f, 0, 0}, {-0.9f, 0, -1.6f}, {-0.65f, .3f, -.38f},
        {-1.3f, 0, 0}, {-0.65f, .3f, .38f}, {-0.9f, 0, 1.6f},
        {-0.65f,-.23f, 0}, {0.55f, .16f, 0}, {-0.82f, .08f, 0},
    };
    e3d_mesh *mesh = e3d_mesh_create(9, 12);
    if (!mesh) return NULL;
    memcpy(mesh->pos, vertices, sizeof(vertices));
    triangle(mesh, 0, 0, 1, 2, 0x95dbe4);
    triangle(mesh, 1, 0, 4, 5, 0x95dbe4);
    triangle(mesh, 2, 1, 3, 2, 0x4691b5);
    triangle(mesh, 3, 3, 5, 4, 0x4691b5);
    triangle(mesh, 4, 0, 2, 7, 0xf4c68e);
    triangle(mesh, 5, 0, 7, 4, 0xf4c68e);
    triangle(mesh, 6, 7, 2, 4, 0x54edf8);
    triangle(mesh, 7, 0, 6, 1, 0x265b7c);
    triangle(mesh, 8, 0, 5, 6, 0x265b7c);
    triangle(mesh, 9, 1, 6, 3, 0x387da0);
    triangle(mesh, 10, 3, 6, 5, 0x387da0);
    triangle(mesh, 11, 2, 8, 4, 0xff833b);
    e3d_mesh_finish(mesh, 0);
    return mesh;
}

static e3d_mesh *make_gate(void)
{
    e3d_mesh *mesh = e3d_mesh_create(RING_SIDES * 2, RING_SIDES * 2);
    if (!mesh) return NULL;
    for (int i = 0; i < RING_SIDES; ++i) {
        const float angle = i * (6.28318530718f / RING_SIDES);
        int v = i * 2, next = ((i + 1) % RING_SIDES) * 2;
        mesh->pos[v][1] = cosf(angle) * 3.0f;
        mesh->pos[v][2] = sinf(angle) * 3.0f;
        mesh->pos[v + 1][1] = cosf(angle) * 3.35f;
        mesh->pos[v + 1][2] = sinf(angle) * 3.35f;
        uint32_t c = (i % 4 == 0) ? 0xc4fff0 : 0x2dc5af;
        triangle(mesh, v, v, next, v + 1, c);
        triangle(mesh, v + 1, v + 1, next, next + 1, c);
    }
    e3d_mesh_finish(mesh, 0);
    return mesh;
}

static e3d_mesh *make_octahedron(uint32_t a, uint32_t b)
{
    const float vertices[][3] = {{1,0,0}, {-1,0,0}, {0,1,0}, {0,0,1}, {0,-1,0}, {0,0,-1}};
    e3d_mesh *mesh = e3d_mesh_create(6, 8);
    if (!mesh) return NULL;
    memcpy(mesh->pos, vertices, sizeof(vertices));
    for (int i = 0; i < 4; ++i) {
        int here = 2 + i, next = 2 + (i + 1) % 4;
        triangle(mesh, i * 2, 0, here, next, i & 1 ? a : b);
        triangle(mesh, i * 2 + 1, 1, next, here, i & 1 ? b : a);
    }
    e3d_mesh_finish(mesh, 0);
    return mesh;
}

static void reset(void)
{
    phase = ACTIVE;
    distance = elapsed = lateral = roll = boost = feedback = 0;
    altitude = 4;
    score = misses = target = 0;
}

esp_err_t flight_init(void)
{
    engine = e3d_create(GAME_WIDTH, GAME_HEIGHT, 2);
    ship = make_ship();
    gate = make_gate();
    pod = make_octahedron(0xffca67, 0xff8b4c);
    reef = make_octahedron(0xa33d65, 0x512951);
    water = e3d_mesh_floor(-80, -112, 200, 168, 0, 12, 0x102b46, 0x153751);
    if (!engine || !ship || !gate || !pod || !reef || !water) return ESP_ERR_NO_MEM;

    for (int y = 0; y < GAME_HEIGHT; ++y) {
        float t = fminf(y / 123.0f, 1);
        sky[y] = rgb(9 + 24 * t, 15 + 42 * t, 34 + 49 * t);
    }
    e3d_set_background_rows(engine, background, NULL);
    e3d_set_focal(engine, 167);
    e3d_params *p = e3d_params_get(engine);
    p->ambient = .58f;
    p->diffuse = .5f;
    p->specular = .2f;
    p->fog_keep_bg = true;
    p->fog_density = .014f;
    p->fog_rgb565 = sky[135];
    reset();
    return ESP_OK;
}

static void target_position(int index, float *y, float *z)
{
    static const int heights[] = {4, 5, 2, 4, 7, 5, 3, 6, 4};
    static const int lanes[] = {0, -3, -5, 1, 6, 2, -3, -6, 3};
    *y = heights[index % 9];
    *z = mode == RELAY ? lanes[index % 9] : -lanes[index % 9] * 1.1f;
}

static float near_target(int index)
{
    float y, z;
    target_position(index, &y, &z);
    float dy = altitude - y, dz = lateral - z;
    return dy * dy + dz * dz;
}

void flight_update(tk21_state in, float dt)
{
    if (phase != ACTIVE) return;

    float previous = distance;
    boost += (((in.held & TK21_U11) != 0) - boost) * fminf(1.0f, dt * 5.0f);
    distance += (17.0f + 13.0f * boost) * dt;
    elapsed += dt;
    roll += (in.x * 34 - roll) * fminf(1.0f, dt * 6.0f);
    lateral = fminf(9, fmaxf(-9, lateral + in.x * 9 * dt));
    altitude = fminf(9, fmaxf(1.3f, altitude + in.y * 6 * dt));
    feedback = fmaxf(0, feedback - dt);

    float crossing = (target + 1) * spacing;
    if (previous < crossing && distance >= crossing) {
        float radius = mode == RELAY ? 2.55f : 1.65f;
        if (near_target(target) < radius * radius) {
            ++score;
            feedback_success = true;
            feedback = .7f;
        } else if (mode == SALVAGE && ((target % 3 == 1 && lateral < -3) ||
                                       (target % 3 == 2 && lateral > 3))) {
            ++misses;
            feedback_success = false;
            feedback = 1.1f;
        } else if (mode == RELAY) {
            ++misses;
            feedback_success = false;
            feedback = 1.1f;
        }
        if (++target == COURSE_LENGTH) phase = DONE;
    }
}

void flight_start(bool salvage)
{
    mode = salvage ? SALVAGE : RELAY;
    reset();
}

static void draw_mesh(const e3d_mesh *mesh, float x, float y, float z,
                      float sx, float sy, float sz, float spin, uint32_t flags)
{
    e3d_object obj;
    e3d_object_init(&obj, mesh);
    e3d_scale(&obj.m, sx, sy, sz);
    if (spin) e3d_rotate(&obj.m, spin, 0, 0);
    e3d_move(&obj.m, x, y, z);
    obj.flags = flags | E3D_NO_SHADOW;
    e3d_draw(engine, &obj);
}

static void meter(uint16_t *pixels, int x, int y, int width, uint16_t color)
{
    for (int row = y; row < y + 3; ++row) {
        for (int i = x; i < x + width; ++i) pixels[row * GAME_WIDTH + i] = color;
    }
}

static void text_at(uint16_t *pixels, int x, int y, const char *s, uint16_t color, int scale)
{
    e3d_text(pixels, GAME_WIDTH, GAME_WIDTH, GAME_HEIGHT, x + 1, y + 1, s, 0x0000, scale);
    e3d_text(pixels, GAME_WIDTH, GAME_WIDTH, GAME_HEIGHT, x, y, s, color, scale);
}

void flight_draw(uint16_t *pixels)
{
    e3d_camera *cam = e3d_cam(engine);
    cam->pos[0] = -6.6f;
    cam->pos[1] = altitude + 2.25f;
    cam->pos[2] = lateral * .86f;
    cam->pitch = -10;
    cam->tilt = roll * .19f;
    e3d_light *light = e3d_light_get(engine);
    light->pos[0] = -5;
    light->pos[1] = 13;
    light->pos[2] = -7;

    e3d_begin(engine, pixels, GAME_WIDTH);
    draw_mesh(water, -fmodf(distance, 28), 0, 0, 1, 1, 1, 0,
              E3D_FLOOR | E3D_UNLIT | E3D_DOUBLE_SIDED);
    for (int i = target; i < target + TARGETS && i < COURSE_LENGTH; ++i) {
        float y, z, x = (i + 1) * spacing - distance;
        target_position(i, &y, &z);
        if (mode == RELAY) {
            draw_mesh(gate, x, y, z, 1, 1, 1, 0, E3D_UNLIT | E3D_DOUBLE_SIDED);
        } else {
            draw_mesh(pod, x, y, z, .7f, .75f, .75f, elapsed * 85 + i * 20, E3D_FLAT);
            if (i % 3)
                draw_mesh(reef, x, 2.5f, i % 3 == 1 ? -7 : 7, 5, 4, 3.5f, 0, E3D_FLAT);
        }
    }
    draw_mesh(ship, 0, altitude, lateral, 1, 1, 1, roll, E3D_UNLIT | E3D_DOUBLE_SIDED);
    e3d_end(engine);
    const e3d_stats *s = e3d_get_stats(engine);
    frame_geom_us = s->t_geom_us;
    frame_raster_us = s->t_raster_us;
    frame_tris = s->tris_drawn;

    const uint16_t paper = 0xe77e, mint = 0x7ff6, amber = 0xff07;
    char label[48];
    text_at(pixels, 12, 11, "LAKESHARK", mint, 1);
    text_at(pixels, 12, 24, title[mode], paper, 2);
    snprintf(label, sizeof(label), "%02d / %02d", score, COURSE_LENGTH);
    text_at(pixels, 322, 12, label, paper, 1);
    meter(pixels, 12, 47, 88, 0x294d);
    meter(pixels, 12, 47, (target * 88) / COURSE_LENGTH, mint);
    if (phase == DONE) {
        snprintf(label, sizeof(label), "FINISH   %02d POINTS   %.1f S", score, (double)elapsed);
        text_at(pixels, 12, 204, label, paper, 1);
        text_at(pixels, 12, 222, "MENU PAUSE", mint, 1);
    } else {
        text_at(pixels, 12, 222, "STICK STEER   A BOOST   MENU PAUSE", paper, 1);
        if (feedback > 0) {
            text_at(pixels, 322, 26, feedback_success ? "SYNC" : "MISS", amber, 1);
        }
        meter(pixels, 317, 222, 65, 0x294d);
        meter(pixels, 317, 222, (int)(boost * 65), amber);
    }
}

void flight_log(void)
{
    ESP_LOGI("flight", "%s state=%d target=%d score=%d misses=%d x=%.1f alt=%.1f z=%.1f tris=%d geom=%dus raster=%dus",
             title[mode], phase, target, score, misses, (double)distance, (double)altitude,
             (double)lateral, frame_tris, frame_geom_us, frame_raster_us);
}
