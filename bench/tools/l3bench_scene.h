/* Shared benchmark scene recipe, also callable from lssim's host fixture. */
#ifndef L3BENCH_SCENE_H
#define L3BENCH_SCENE_H
#include "lap3d.h"
#include <math.h>
/* ORBIT's scene class, kept here so timing and numeric regression share it. */
typedef struct { l3_mesh vase, word; } l3bench_orbit_assets;
static inline bool l3bench_orbit_init(l3bench_orbit_assets *a) {
    const float profile[] = {0, 0, .55f, 0, .75f, .35f, .62f, .9f,
                             .35f, 1.3f, .32f, 1.6f, .45f, 1.8f};
    return l3_mesh_lathe(&a->vase, profile, 7, 16) && l3_mesh_text(&a->word, "3D", 1.2f);
}
static inline void l3bench_orbit_free(l3bench_orbit_assets *a) {
    l3_mesh_free(&a->vase); l3_mesh_free(&a->word);
}
static inline void l3bench_orbit_setup(l3_scene *s) {
    const uint8_t floor[] = {0, 8, 15}, pip[] = {0, 8}, die[] = {0, 1, 9},
                  word[] = {4, 6, 14, 15};
    l3_material(s, floor, 3, 0, L3_CHECKER);
    l3_material(s, L3_RAMP_METAL, 4, .9f, 0);
    l3_material(s, pip, 2, 0, 0);
    l3_material(s, die, 3, .2f, 0);
    l3_material(s, L3_RAMP_GREEN, 4, .6f, 0);
    l3_material(s, L3_RAMP_IVORY, 4, .3f, 0);
    l3_material(s, word, 4, .4f, 0);
    uint8_t tex[49];
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 7; x++)
            tex[y*7+x] = ((x == 1 || x == 5) && (y == 1 || y == 5)) || (x == 3 && y == 3) ? 2 : 255;
    l3_material_texture(s, 3, l3_texture(s, 7, 7, tex));
    l3_camera(s, l3v(6.657396f, 4.926238f, 11.530947f), l3v(0, 1.9f, 0), 40);
    l3_sun(s, l3v(-.5f, -1, -.35f), .9f, .25f);
    l3_shadows(s, true); l3_sky(s, 0, 4); l3_outline(s, 0); l3_fog(s, 0, 16, 26);
}
static inline void l3bench_orbit_frame(l3_scene *s, int frame, bool frozen,
                                      const l3bench_orbit_assets *a) {
    float t = (frozen ? 20 : frame) * .04f;
    uint32_t flags = L3_CAST | (frozen ? L3_STATIC : 0);
    l3_begin(s); l3_flags(s, L3_STATIC); l3_tag(s, 0); l3_floor(s, 0, 0);
    l3_flags(s, flags); l3_tag(s, 1);
    l3_sphere(s, l3v(-2.2f, .7f + fabsf(sinf(t*2.6f))*1.4f, .6f), .7f, 1);
    l3_xform x = l3_pose(l3v(1.8f, .6f, 1.2f), l3_quat_euler(t*40, t*25, t*15), 1);
    l3_tag(s, 2); l3_rbox(s, &x, l3v(.6f, .6f, .6f), .14f, 3);
    x = l3_pose(l3v(.4f, 1.1f, -1.8f), l3_quat_euler(t*60, 35, 0), 1);
    l3_tag(s, 3); l3_torus(s, &x, .75f, .25f, 4);
    x = l3_pose(l3v(-1, 0, -.6f), l3_quat_euler(t*20, 0, 0), 1);
    l3_flags(s, flags | L3_SMOOTH); l3_tag(s, 4); l3_draw_mesh(s, &a->vase, &x, 5);
    x = l3_pose(l3v(0, 3.6f + .15f*sinf(t*1.7f), -.5f),
                l3_quat_euler(sinf(t*.6f)*35, 0, 0), .32f);
    l3_flags(s, flags); l3_tag(s, 5); l3_draw_mesh(s, &a->word, &x, 6);
    l3_end(s);
}
static inline void l3bench_frame(l3_scene *s, int which, int frame, const l3_mesh *mesh) {
    l3_xform t = l3_pose(l3v(-1.7f, 0, -0.7f), l3_quat_euler(30, 15, 10), 0.7f);
    l3_begin(s);
    l3_flags(s, L3_STATIC);
    l3_tag(s, 1);
    l3_floor(s, -1, 1);
    if (which == 1) {
        l3_flags(s, L3_SMOOTH);
        l3_tag(s, 5);
        l3_draw_mesh(s, mesh, &t, 0);
    } else {
        l3_flags(s, which == 4 ? L3_STATIC | L3_CAST : L3_CAST);
        l3_tag(s, 2);
        l3_rbox(s, &t, l3v(0.7f, 0.7f, 0.7f), 0.15f, 0);
        l3_tag(s, 3);
        l3_sphere(s, l3v(0, 0, 0), 0.8f, 2);
        t = l3_pose(l3v(1.6f, -0.3f, -0.8f), l3_quat_euler(0, 22, 0), 0.8f);
        l3_tag(s, 4);
        l3_torus(s, &t, 0.7f, 0.22f, 0);
        l3_tag(s, 5);
        l3_capsule(s, l3v(-1.5f, -0.7f, 1), l3v(-0.8f, 0.1f, 1.3f), 0.22f, 2);
        l3_tag(s, 6);
        l3_cone(s, l3v(1.4f, -1, 0.9f), l3v(1.4f, 0.3f, 0.9f), 0.45f, 0.12f, 3);
    }
    if (which == 4) {
        l3_flags(s, 0);
        l3_tag(s, 9);
        l3_sphere(s, l3v(-0.3f + frame * 0.025f, 0.7f, 1.3f), 0.22f, 3);
    }
    l3_end(s);
}
static inline void l3bench_setup(l3_scene *s, int which) {
    l3_material(s, L3_RAMP_METAL, 4, 0.65f, 0);
    l3_material(s, L3_RAMP_GREY, 4, 0, L3_CHECKER | L3_GRAIN);
    l3_material(s, L3_RAMP_GREEN, 4, 0.3f, 0);
    l3_material(s, L3_RAMP_FIRE, 4, 0.4f, 0);
    l3_camera(s, l3v(4, 3.5f, 7), l3v(0, -0.1f, 0), 43);
    l3_sun(s, l3v(0.5f, -1, -0.4f), 0.8f, 0.23f);
    l3_sky(s, 4, 8);
    l3_fog(s, 4, 10, 24);
    l3_shadows(s, which == 2);
    l3_outline(s, which == 3 ? 0 : LAP_DOT_CLEAR);
}
typedef struct { uint8_t heights[33*33], walls[16*16]; l3_vec p[2000]; } l3bench_world_assets;
static inline void l3bench_world_init(l3bench_world_assets *a) {
    for (int z = 0; z < 33; z++) for (int x = 0; x < 33; x++)
        a->heights[z*33+x] = (uint8_t)(32+24*sinf(x*.3f)*cosf(z*.25f));
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++)
        a->walls[z*16+x] = x == 0 || z == 0 || x == 15 || z == 15 || (x%4 == 0 && z%4 != 1) ? 1 : 0;
    for (int i = 0; i < 2000; i++)
        a->p[i] = l3v((i%50-25)*.07f, (i/50-20)*.07f, ((i*71)%101)*.01f);
}
static inline void l3bench_world_setup(l3_scene *s, int which) {
    l3_material(s,L3_RAMP_GREEN,4,0,0);
    l3_material(s,L3_RAMP_IVORY,4,.35f,L3_ROUND);
    l3_material(s,L3_RAMP_WOOD,4,.1f,0);
    if (which == 7) l3_camera(s,l3v(0,3.2f,5),l3v(0,.4f,0),55);
#ifndef L3BENCH_BASELINE
    else if (which == 8) l3_camera_look(s,l3v(1.5f,.8f,3.5f),0,-12,60);
#endif
    else if (which == 10) l3_camera_ortho(s,l3v(0,6.8f,4.2f),l3v(0,0,0),3.4f);
    else l3_camera(s,l3v(0,0,6),l3v(0,0,0),40);
    l3_sky(s,4,0);
    if (which == 10) {
        l3_shadows(s,true);
        uint8_t tex[6*7*7];
        for (int y = 0; y < 7; y++) for (int x = 0; x < 42; x++)
            tex[y*42+x] = x%7 == 3 && y == 3 ? 2 : 255;
        l3_material_texture(s,1,l3_texture(s,42,7,tex));
    }
}
static inline void l3bench_world_frame(l3_scene *s, int which, int frame, const l3bench_world_assets *a) {
    l3_begin(s); l3_tag(s,1);
#ifndef L3BENCH_BASELINE
    if (which == 7) l3_terrain(s,a->heights,NULL,33,33,.25f,.025f,l3v(-4,0,-4),0);
    else if (which == 8) l3_walls(s,a->walls,16,16,.5f,1.5f,l3v(0,0,-6));
    else if (which == 9) l3_particles(s,a->p,2000,.018f,0);
    else
#endif
    if (which == 11) l3_floor(s,-1,0);
    else {
        l3_flags(s,L3_STATIC);
        l3_quad(s,l3v(-1,0,1.3f),l3v(1,0,1.3f),l3v(1,0,-1.3f),l3v(-1,0,-1.3f),0);
        for (int wall = 0; wall < 4; wall++) {
            l3_xform t = l3_identity();
            t.m[9] = wall < 2 ? (wall ? 1.05f : -1.05f) : 0;
            t.m[10] = .06f; t.m[11] = wall >= 2 ? (wall == 2 ? -1.35f : 1.35f) : 0;
            l3_box(s,&t,wall < 2 ? l3v(.05f,.06f,1.4f) : l3v(1.1f,.06f,.05f),2);
        }
        l3_flags(s,L3_CAST);
        for (int die = 0; die < 2; die++) {
            float t = frame*.045f+die;
            l3_xform x = l3_pose(l3v((die ? .4f : -.4f)+.15f*sinf(t),.25f+fabsf(sinf(t*3))*.5f,.5f*cosf(t)),
                                 l3_quat_euler(t*300,t*170,t*90),1);
            l3_tag(s,die+2); l3_rbox(s,&x,l3v(.2f,.2f,.2f),.03f,1);
        }
    }
    l3_end(s);
}

/* Round 4: deterministic copies of the shipped SKYRIDE/CRYPT workloads. */
typedef struct {
    uint8_t height[128*128], mats[128*128], grid[256];
    l3_vec clouds[48], torch[16], gem[16], flame[16];
    int ntorch, ngem;
} l3bench_app_assets;
static float lattice(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 1442695041u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
static float smooth_noise(float x, float y, uint32_t seed)
{
    const int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    const float a = lattice(x0, y0, seed), b = lattice(x0 + 1, y0, seed);
    const float c = lattice(x0, y0 + 1, seed), d = lattice(x0 + 1, y0 + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static const char *const l3bench_crypt_map[16] = {
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


static inline void l3bench_app_init(l3bench_app_assets *a) {
    a->ntorch = a->ngem = 0;
    for (int z = 0; z < 128; z++) for (int x = 0; x < 128; x++) {
        float h = 0, amp = 1, f = 1.0f/32;
        for (int o = 0; o < 5; o++, amp *= .5f, f *= 2) h += amp*smooth_noise(x*f,z*f,31415+o);
        float dx = (x-64)/64.0f, dz = (z-64)/64.0f;
        h = h/1.9375f-(dx*dx+dz*dz)*.9f-.04f;
        a->height[z*128+x] = (uint8_t)(fmaxf(.08f,h)*255);
        a->mats[z*128+x] = h < .08f ? 0 : h < .12f ? 1 : h < .48f ? 2 : h < .62f ? 3 : 4;
    }
    for (int i = 0; i < 48; i++) a->clouds[i] = l3v((i*37%128)*.5f,12+(i%4)*.4f,(i*71%128)*.5f);
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
        char c = l3bench_crypt_map[z][x];
        a->grid[z*16+x] = c == '#' ? 1 : c == 'B' ? 2 : 0;
        if (c == 'G') a->gem[a->ngem++] = l3v(x+.5f,.45f,z+.5f);
        if (c == 'T') a->torch[a->ntorch++] = l3v(x+.5f,.7f,z+.5f);
    }
}
static inline void l3bench_app_setup(l3_scene *s, int which) {
    if (which == 12) {
        const uint8_t sea[] = {0,4,12,14}, sand[] = {0,3,11,11}, grass[] = {0,2,2,10},
                      rock[] = {0,8,8,7}, snow[] = {7,15,15}, cloud[] = {7,15};
        l3_material(s,sea,4,.6f,0); l3_material(s,sand,4,0,L3_GRAIN);
        l3_material(s,grass,4,0,L3_GRAIN); l3_material(s,rock,4,.1f,L3_GRAIN);
        l3_material(s,snow,3,.2f,0); l3_material(s,cloud,2,0,0);
        l3_sun(s,l3v(-.5f,-.7f,-.3f),.6f,.2f); l3_shadows(s,true);
        l3_sky(s,4,14); l3_fog(s,14,18,30);
    } else {
        const uint8_t stone[] = {0,8,7,15}, brick[] = {0,1,9,11}, floor[] = {0,8,7},
                      ceil[] = {0,8}, gem[] = {4,6,14,15};
        l3_material(s,stone,4,.05f,L3_GRAIN); l3_material(s,brick,4,.05f,L3_GRAIN);
        l3_material(s,floor,3,0,L3_CHECKER); l3_material(s,ceil,2,0,0);
        l3_material(s,gem,4,.9f,0); l3_material(s,L3_RAMP_FIRE,4,0,L3_UNLIT);
        l3_sun(s,l3v(0,-1,0),0,.14f); l3_sky(s,0,0); l3_fog(s,0,5,10);
    }
}
static inline void l3bench_app_frame(l3_scene *s, int which, int frame, l3bench_app_assets *a) {
    float t = frame*.016f;
    if (which == 13) {
        int best[4] = {-1,-1,-1,-1}; float bd[4] = {1e9f,1e9f,1e9f,1e9f};
        for (int i = 0; i < a->ntorch; i++) {
            float dx = a->torch[i].x-1.5f, dz = a->torch[i].z-1.5f, d = dx*dx+dz*dz;
            for (int k = 0; k < 4; k++) if (d < bd[k]) {
                for (int j = 3; j > k; j--) { bd[j] = bd[j-1]; best[j] = best[j-1]; }
                bd[k] = d; best[k] = i; break;
            }
            a->flame[i] = l3v(a->torch[i].x+.03f*sinf(t*20+i),
                              a->torch[i].y+.04f*sinf(t*31+i*2),a->torch[i].z);
        }
        for (int k = 0; k < 4; k++) l3_lamp(s,k,a->torch[best[k]],
            2.4f*(.85f+.15f*sinf(t*13+best[k]*1.7f)*sinf(t*7+best[k])),6);
    }
    l3_begin(s);
    if (which == 12) {
        l3_camera_look(s,l3v(32,6.5f,62-t*6),0,-14,60);
        l3_flags(s,L3_STATIC|L3_CAST); l3_tag(s,1);
        l3_terrain(s,a->height,a->mats,128,128,.5f,6.0f/255,l3v(0,0,0),2);
        l3_flags(s,0); l3_particles(s,a->clouds,48,1.6f,5);
    } else {
        l3_camera_look(s,l3v(1.5f,.5f,1.5f),180,0,70);
        l3_flags(s,L3_STATIC); l3_floor(s,0,2);
        l3_flags(s,L3_STATIC|L3_TWO_SIDED);
        l3_quad(s,l3v(0,1,0),l3v(16,1,0),l3v(16,1,16),l3v(0,1,16),3);
        l3_flags(s,L3_STATIC); l3_walls(s,a->grid,16,16,1,1,l3v(0,0,0));
        l3_flags(s,0);
        for (int i = 0; i < a->ngem; i++) {
            l3_vec p = a->gem[i]; p.y += .06f*sinf(t*3+i);
            l3_xform pose = l3_pose(p,l3_quat_euler(t*90+i*40,45,35),1);
            l3_tag(s,i+1); l3_box(s,&pose,l3v(.09f,.09f,.09f),4);
        }
        l3_tag(s,0); l3_particles(s,a->flame,a->ntorch,.12f,5);
    }
    l3_end(s);
}
#endif
