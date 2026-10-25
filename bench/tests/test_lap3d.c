/* Targeted engine contract tests; no firmware or hardware dependencies. */
#include "l3_private.h"
#include "lap3d.h"
#include "ls_test.h"
#include <stdlib.h>
#include <string.h>
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t k);
void *__real_realloc(void *p, size_t n);
static bool count_alloc;
static unsigned allocs;
void *__wrap_malloc(size_t n) {
    if (count_alloc)
        allocs++;
    return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t k) {
    if (count_alloc)
        allocs++;
    return __real_calloc(n, k);
}
void *__wrap_realloc(void *p, size_t n) {
    if (count_alloc)
        allocs++;
    return __real_realloc(p, n);
}
extern int l3_test_cw, l3_test_ch;
static l3_scene *scene(int w, int h) {
    l3_scene *s = l3_create(w, h);
    LS_CHECK(s);
    LS_EQ_INT(l3_material(s, L3_RAMP_METAL, 4, 0.6f, 0), 0);
    l3_camera(s, l3v(0, 0, 6), l3v(0, 0, 0), 40);
    return s;
}
static int coverage(l3_scene *s) {
    int n = 0;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if (l3_depth(s, x, y) > 0)
                n++;
    return n;
}
LS_CASE(math_round_trips) {
    l3_vec p = l3v(1.2f, -0.7f, 3.1f);
    for (int i = 0; i < 20; i++) {
        l3_quat q = l3_quat_euler(i * 13.0f, i * -7.0f, i * 3.0f), inv = {-q.x, -q.y, -q.z, q.w};
        l3_vec v = l3_quat_rotate(inv, l3_quat_rotate(q, p));
        LS_NEAR(v.x, p.x, 0.00002f);
        LS_NEAR(v.y, p.y, 0.00002f);
        LS_NEAR(v.z, p.z, 0.00002f);
        l3_xform t = l3_pose(l3v(-1, 2, 4), q, 1.7f), it;
        LS_CHECK(l3_inverse(&it, &t));
        l3_xform identity = l3_mul(&it, &t);
        v = l3_apply(&identity, p);
        LS_NEAR(v.x, p.x, 0.00002f);
        LS_NEAR(v.y, p.y, 0.00002f);
        LS_NEAR(v.z, p.z, 0.00002f);
    }
    l3_quat a = l3_quat_axis(l3v(0, 1, 0), 0), b = l3_quat_axis(l3v(0, 1, 0), 90);
    l3_vec v = l3_quat_rotate(l3_quat_slerp(a, b, 0.5f), l3v(1, 0, 0));
    LS_NEAR(v.x, 0.7071068f, 0.00001f);
    LS_NEAR(v.z, -0.7071068f, 0.00001f);
    v = l3_quat_rotate(l3_quat_spin(a, l3v(0, L3_PI / 2, 0), 1), l3v(1, 0, 0));
    LS_NEAR(v.z, -1, 0.00001f);
    LS_EQ_INT(l3_norm(l3v(0, 0, 0)).x, 0);
}
LS_CASE(project_ray_consistency) {
    l3_scene *s = scene(54, 66);
    for (int ortho = 0; ortho < 2; ortho++) {
        if (ortho)
            l3_camera_ortho(s, l3v(1, 2, 6), l3v(0, 0, 0), 5);
        for (int y = 0; y < s->h; y += 11)
            for (int x = 0; x < s->w; x += 9) {
                l3_vec o, d;
                float px, py;
                l3_ray(s, (float)x, (float)y, &o, &d);
                LS_CHECK(l3_project(s, l3_add(o, l3_scale(d, 5)), &px, &py));
                LS_NEAR(px, x, 0.0001f);
                LS_NEAR(py, y, 0.0001f);
            }
    }
    l3_destroy(s);
}
LS_CASE(sphere_aspect_silhouette) {
    const int cw[] = {10, 8, 12, 10}, ch[] = {17, 16, 12, 17};
    for (int i = 0; i < 4; i++) {
        l3_test_cw = cw[i];
        l3_test_ch = ch[i];
        l3_scene *s = scene(i == 3 ? 116 : 54, i == 3 ? 30 : 66);
        l3_camera_ortho(s, l3v(0, 0, 6), l3v(0, 0, 0), 5);
        l3_begin(s);
        l3_sphere(s, l3v(0, 0, 0), 1, 0);
        l3_end(s);
        float cx = (s->w - 1) * 0.5f, cy = (s->h - 1) * 0.5f;
        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++) {
                float a = (x - cx) / s->camera.fx, b = (y - cy) / s->camera.fy,
                      r = sqrtf(a * a + b * b), tol = 1.0f / fminf(s->camera.fx, s->camera.fy);
                if (r < 1 - tol)
                    LS_CHECK(l3_depth(s, x, y) > 0);
                if (r > 1 + tol)
                    LS_CHECK(l3_depth(s, x, y) < 0);
            }
        l3_destroy(s);
    }
    l3_test_cw = 10;
    l3_test_ch = 17;
}
LS_CASE(depth_mesh_and_picking) {
    l3_scene *s = scene(30, 20);
    l3_xform t = l3_identity();
    const l3_vec v[] = {{-2, -2, 1}, {2, -2, 1}, {0, 2, 1}};
    const uint16_t idx[] = {0, 1, 2};
    l3_mesh m = {v, idx, NULL, 3, 1};
    l3_begin(s);
    l3_tag(s, 10);
    l3_sphere(s, l3v(0, 0, 0), 1, 0);
    l3_tag(s, 20);
    l3_draw_mesh(s, &m, &t, 0);
    l3_tag(s, 30);
    l3_sphere(s, l3v(0, 0, 2), 0.5f, 0);
    l3_end(s);
    LS_EQ_INT(l3_pick(s, 30, 30), 30);
    LS_NEAR(l3_depth(s, 30, 30), 3.5f, 0.015f);
    LS_EQ_INT(l3_pick(s, 44, 33), 20);
    LS_EQ_INT(l3_pick(s, -1, 0), 0);
    LS_CHECK(l3_depth(s, 1000, 0) < 0);
    l3_destroy(s);
}
LS_CASE(near_plane_clipping) {
    l3_scene *s = scene(30, 20);
    l3_camera(s, l3v(0, 0, 0), l3v(0, 0, -1), 60);
    l3_clip(s, 0.1f, 10);
    const l3_vec v[] = {{-0.3f, -0.3f, -1}, {0.3f, -0.3f, -1}, {0, 0.3f, 0.03f}};
    const uint16_t idx[] = {0, 1, 2};
    l3_mesh m = {v, idx, NULL, 3, 1};
    l3_xform t = l3_identity();
    l3_begin(s);
    l3_tag(s, 8);
    l3_draw_mesh(s, &m, &t, 0);
    l3_end(s);
    LS_CHECK(coverage(s) > 100);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if (l3_depth(s, x, y) > 0) {
                l3_vec o, d;
                l3_ray(s, (float)x, (float)y, &o, &d);
                float z = l3_depth(s, x, y) * l3_dot(d, s->camera.forward);
                LS_CHECK(z >= 0.0999f && z <= 1.0001f);
            }
    l3_destroy(s);
}
static void cached_frame(l3_scene *s, float x, bool ball) {
    l3_begin(s);
    l3_flags(s, L3_STATIC | L3_CAST);
    l3_tag(s, 2);
    l3_floor(s, -1, 0);
    l3_flags(s, 0);
    if (ball) {
        l3_tag(s, 9);
        l3_sphere(s, l3v(x, 0, 0), 0.7f, 0);
    }
    l3_end(s);
}
LS_CASE(reuse_bit_identical_and_removal) {
    l3_scene *s = scene(54, 66);
    uint8_t *expected = malloc((size_t)s->w * s->h);
    LS_CHECK(expected);
    l3_outline(s, 0);
    l3_fog(s, 4, 4, 12);
    for (int i = 0; i < 7; i++) {
        cached_frame(s, i * 0.1f, i != 6);
        if (i)
            LS_CHECK(s->stats.dots_reused > 0);
        memcpy(expected, l3_dots(s), (size_t)s->w * s->h);
        s->valid = false;
        cached_frame(s, i * 0.1f, i != 6);
        LS_EQ_INT(memcmp(expected, l3_dots(s), (size_t)s->w * s->h), 0);
    }
    free(expected);
    l3_destroy(s);
}
LS_CASE(determinism_no_frame_allocations) {
    l3_scene *s = scene(30, 20);
    l3_mesh m;
    LS_CHECK(l3_mesh_icosphere(&m, 2));
    l3_xform t = l3_identity();
    uint8_t *expected = malloc((size_t)s->w * s->h);
    LS_CHECK(expected);
    for (int frame = 0; frame < 5; frame++) {
        allocs = 0;
        count_alloc = true;
        l3_begin(s);
        l3_flags(s, L3_SMOOTH | L3_CAST);
        l3_draw_mesh(s, &m, &t, 0);
        l3_end(s);
        count_alloc = false;
        LS_EQ_UINT(allocs, 0);
        LS_EQ_UINT(s->stats.dropped, 0);
        if (!frame)
            memcpy(expected, l3_dots(s), (size_t)s->w * s->h);
        else
            LS_EQ_INT(memcmp(expected, l3_dots(s), (size_t)s->w * s->h), 0);
    }
    free(expected);
    l3_mesh_free(&m);
    l3_destroy(s);
}
LS_CASE(primitives_and_generators) {
    l3_scene *s = scene(30, 20);
    l3_xform t = l3_pose(l3v(0, 0, 0), l3_quat_euler(23, 17, 6), 1);
    for (int kind = 0; kind < 12; kind++) {
        l3_begin(s);
        switch (kind) {
        case 0:
            l3_ellipsoid(s, &t, l3v(1, 0.7f, 0.4f), 0);
            break;
        case 1:
            l3_box(s, &t, l3v(1, 0.7f, 0.4f), 0);
            break;
        case 2:
            l3_rbox(s, &t, l3v(1, 0.7f, 0.4f), 0.2f, 0);
            break;
        case 3:
            l3_cylinder(s, l3v(-0.6f, -0.7f, 0), l3v(0.6f, 0.7f, 0), 0.4f, 0);
            break;
        case 4:
            l3_capsule(s, l3v(-0.6f, -0.7f, 0), l3v(0.6f, 0.7f, 0), 0.4f, 0);
            break;
        case 5:
            l3_cone(s, l3v(0, -0.7f, 0), l3v(0, 0.7f, 0), 0.7f, 0.1f, 0);
            break;
        case 6:
            l3_torus(s, &t, 0.7f, 0.2f, 0);
            break;
        case 7:
            l3_disc(s, l3v(0, 0, 0), l3v(0, 0, 1), 1, 0);
            break;
        case 8:
            l3_quad(s, l3v(-1, -1, 0), l3v(1, -1, 0), l3v(1, 1, 0), l3v(-1, 1, 0), 0);
            break;
        case 9:
            l3_line(s, l3v(-1, -1, 0), l3v(1, 1, 0), 7);
            break;
        case 10:
            l3_point(s, l3v(0, 0, 0), 8, 7);
            break;
        case 11:
            l3_label(s, l3v(0, 0, 0), "3D", 2, 15, 0);
            break;
        }
        l3_end(s);
        LS_CHECK_MSG(coverage(s) > 5, "primitive %d has no coverage", kind);
    }
    const float xy[] = {-1, -1, 1, -1, 1, 0, 0, 0, 0, 1, -1, 1}, rz[] = {0, -1, 1, 0, 0, 1},
                h[] = {0, 0, 0, 1};
    l3_mesh m;
    LS_CHECK(l3_mesh_extrude(&m, xy, 6, 0.5f));
    LS_EQ_INT(m.ntri, 20);
    l3_mesh_free(&m);
    LS_CHECK(l3_mesh_lathe(&m, rz, 3, 12));
    LS_EQ_INT(m.ntri, 48);
    l3_mesh_free(&m);
    LS_CHECK(l3_mesh_heightmap(&m, h, 2, 2, 1));
    LS_EQ_INT(m.ntri, 2);
    l3_mesh_free(&m);
    LS_CHECK(l3_mesh_text(&m, "LAKE", 0.3f));
    LS_CHECK(m.ntri > 100);
    l3_mesh_free(&m);
    for (int i = 0; i <= 3; i++) {
        LS_CHECK(l3_mesh_icosphere(&m, i));
        LS_EQ_INT(m.ntri, 20 * (1 << (2 * i)));
        LS_EQ_INT(m.nv, 10 * (1 << (2 * i)) + 2);
        for (int v = 0; v < m.nv; v++)
            LS_NEAR(l3_length(m.v[v]), 1, 0.00001f);
        l3_mesh_free(&m);
    }
    l3_destroy(s);
}
LS_CASE(material_texture_shadow_controls) {
    l3_scene *s = scene(30, 20);
    uint8_t red[] = {1, 9}, tex[] = {1, 255, 255, 1};
    int mat = l3_material(s, red, 2, 0, L3_CHECKER | L3_GRAIN);
    LS_EQ_INT(mat, 1);
    int texture = l3_texture(s, 2, 2, tex);
    LS_CHECK(texture >= 0);
    l3_material_texture(s, 0, texture);
    l3_lamp(s, 0, l3v(2, 3, 2), 0.4f, 8);
    l3_shadows(s, true);
    l3_outline(s, 0);
    l3_camera(s, l3v(3, 3, 6), l3v(0, 0, 0), 45);
    cached_frame(s, 0, true);
    uint8_t *before = malloc((size_t)s->w * s->h);
    memcpy(before, s->dots, (size_t)s->w * s->h);
    s->valid = false;
    cached_frame(s, 0, true);
    LS_EQ_INT(memcmp(before, s->dots, (size_t)s->w * s->h), 0);
    LS_EQ_UINT(s->stats.dropped, 0);
    free(before);
    l3_destroy(s);
}
LS_CASE(resize_reserve_and_overflow) {
    LS_CHECK(!l3_create(0, 1));
    l3_scene *s = scene(10, 10);
    LS_CHECK(l3_reserve(s, 512, 8192, 16384));
    l3_begin(s);
    LS_CHECK(!l3_reserve(s, 1024, 8192, 16384));
    for (int i = 0; i < 513; i++)
        l3_sphere(s, l3v(0, 0, 0), 1, 0);
    l3_end(s);
    LS_EQ_UINT(s->stats.dropped, 1);
    LS_CHECK(l3_resize(s, 20, 30));
    int w, h;
    l3_size(s, &w, &h);
    LS_EQ_INT(w, 40);
    LS_EQ_INT(h, 90);
    LS_CHECK(!l3_resize(s, -1, 30));
    l3_destroy(s);
}
LS_CASE(row_band_order_is_identical) {
    l3_scene *s = scene(40, 35);
    l3_mesh m;
    LS_CHECK(l3_mesh_icosphere(&m, 2));
    l3_xform t = l3_identity();
    l3_begin(s);
    l3_flags(s, L3_SMOOTH);
    l3_tag(s, 4);
    l3_draw_mesh(s, &m, &t, 0);
    l3_sphere(s, l3v(1, 0, 0), 0.6f, 0);
    l3_end(s);
    size_t n = (size_t)s->w * s->h;
    uint8_t *reference = malloc(n);
    l3_pixel *gb = malloc(n * sizeof(*gb));
    LS_CHECK(reference && gb);
    memcpy(reference, s->dots, n);
    memcpy(gb, s->pixels, n * sizeof(*gb));
    s->pass = 3;
    for (int band = s->bands - 1; band >= 0; band--)
        l3_raster_band(s, band, band & 1);
    l3_resolve(s);
    LS_EQ_INT(memcmp(reference, s->dots, n), 0);
    LS_EQ_INT(memcmp(gb, s->pixels, n * sizeof(*gb)), 0);
    free(reference);
    free(gb);
    l3_mesh_free(&m);
    l3_destroy(s);
}
LS_CASE(clipped_line_and_sprite) {
    l3_scene *s = scene(30, 20);
    l3_camera(s, l3v(0, 0, 0), l3v(0, 0, -1), 60);
    l3_clip(s, 0.1f, 10);
    l3_begin(s);
    l3_tag(s, 7);
    l3_line(s, l3v(-0.3f, 0, -1), l3v(0.3f, 0, 0.1f), 15);
    l3_end(s);
    LS_CHECK(coverage(s) > 10);
    const char *rows[] = {"x.x", ".x.", "x.x"};
    const uint8_t inks[] = {10};
    l3_begin(s);
    l3_tag(s, 6);
    l3_sprite(s, l3v(0, -0.5f, -3), 1, rows, 3, "x", inks);
    l3_end(s);
    LS_CHECK(coverage(s) > 30);
    LS_EQ_UINT(s->stats.dropped, 0);
    l3_destroy(s);
}
LS_CASE(shadow_cast_flags_change_receivers) {
    l3_scene *s = scene(40, 35);
    l3_camera(s, l3v(3, 3, 6), l3v(0, 0, 0), 45);
    l3_shadows(s, true);
    size_t n = (size_t)s->w * s->h;
    uint8_t *reference = malloc(n);
    LS_CHECK(reference);
    for (int cast = 0; cast < 2; cast++) {
        l3_begin(s);
        l3_tag(s, 1);
        l3_floor(s, -1, 0);
        l3_flags(s, cast ? L3_CAST : 0);
        l3_tag(s, 2);
        l3_sphere(s, l3v(0, 0, 0), 0.7f, 0);
        l3_end(s);
        if (!cast)
            memcpy(reference, s->dots, n);
    }
    int changed = 0;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if (l3_pick(s, x, y) == 1 && reference[y * s->w + x] != s->dots[y * s->w + x])
                changed++;
    LS_CHECK(changed > 5);
    free(reference);
    l3_destroy(s);
}
LS_CASE(generated_text_winding_is_outward) {
    l3_mesh mesh;
    LS_CHECK(l3_mesh_text(&mesh, "A", 0.5f));
    for (int i = 0; i < mesh.ntri; i++) {
        int base = mesh.idx[i * 3] / 8 * 8;
        l3_vec center = l3v(0, 0, 0);
        for (int j = 0; j < 8; j++)
            center = l3_add(center, mesh.v[base + j]);
        center = l3_scale(center, 0.125f);
        l3_vec a = mesh.v[mesh.idx[i * 3]], b = mesh.v[mesh.idx[i * 3 + 1]],
               c = mesh.v[mesh.idx[i * 3 + 2]];
        LS_CHECK(l3_dot(l3_cross(l3_sub(b, a), l3_sub(c, a)), l3_sub(a, center)) > 0);
    }
    l3_mesh_free(&mesh);
}
LS_CASE(disjoint_dynamic_bounds_count_union) {
    l3_scene *s = scene(54, 66);
    for (int f = 0; f < 2; f++) {
        l3_begin(s);
        l3_flags(s, L3_STATIC);
        l3_floor(s, -1, 0);
        l3_flags(s, 0);
        l3_sphere(s, l3v(-1.4f, 0, 0), 0.2f, 0);
        l3_sphere(s, l3v(1.4f, 0, 0), 0.2f, 0);
        l3_end(s);
    }
    unsigned count = 0;
    for (int i = 0; i < s->w * s->h; i++)
        count += s->dirty_mask[i] != 0;
    LS_EQ_UINT(s->stats.dots_shaded, count);
    LS_CHECK(s->stats.dots_reused > (unsigned)(s->w * s->h) * 9 / 10);
    l3_destroy(s);
}

/* Independent dots/tags captured from the original forward renderer. */
#include "../tools/l3bench_scene.h"
#include "../tools/l3bench_reference.h"
static uint8_t *decode_reference(const uint8_t *in, size_t size) {
    uint8_t *out = malloc(size);
    LS_CHECK(out);
    size_t at = 0;
    while (at < size) {
        unsigned token = *in++, n;
        if (token < 128) {
            n = token + 1;
            LS_CHECK(at + n <= size);
            memcpy(out + at, in, n);
            in += n; at += n;
        } else {
            n = token - 128 + 3;
            unsigned back = in[0] | ((unsigned)in[1] << 8);
            in += 2;
            LS_CHECK(back && back <= at && at + n <= size);
            while (n--) {
                out[at] = out[at - back];
                at++;
            }
        }
    }
    return out;
}
LS_CASE(deferred_matches_original_l3bench) {
    float height[33 * 33];
    for (int y = 0; y < 33; y++)
        for (int x = 0; x < 33; x++) {
            float a = (x - 16) * .18f, b = (y - 16) * .18f;
            height[y * 33 + x] = .45f * sinf(a * 2) * cosf(b * 2);
        }
    l3_mesh mesh;
    l3bench_orbit_assets orbit = {0};
    LS_CHECK(l3_mesh_heightmap(&mesh, height, 33, 33, .16f));
    LS_CHECK(l3bench_orbit_init(&orbit));
    for (int orientation = 0; orientation < 2; orientation++)
        for (int which = 0; which < 7; which++) {
            l3_scene *s = l3_create(orientation ? 116 : 54,
                                   orientation ? 30 : which >= 5 ? 70 : 66);
            LS_CHECK(s && l3_reserve(s, 256, 4096, 8192));
            if (which >= 5) l3bench_orbit_setup(s);
            else l3bench_setup(s, which);
            allocs = 0;
            count_alloc = true;
            for (int frame = 0; frame <= 20; frame++) {
                if (which >= 5) l3bench_orbit_frame(s, frame, which == 6, &orbit);
                else l3bench_frame(s, which, frame, &mesh);
            }
            count_alloc = false;
            LS_EQ_UINT(allocs, 0);
            size_t n = (size_t)s->w * s->h;
            uint8_t *reference = decode_reference(l3_reference[orientation][which], n * 2);
            unsigned dots = 0, tags = 0;
            for (size_t i = 0; i < n; i++) {
                dots += s->dots[i] != reference[i];
                tags += s->pixels[i].tag != reference[n + i];
            }
            ls_note("oracle orientation=%d scene=%d: %u/%zu dots, %u tags (budget 8 dots, 0 tags)",
                    orientation, which, dots, n, tags);
            LS_EQ_UINT(tags, 0);
            LS_CHECK(dots <= 8); /* <0.04%: packed normal / sub-dot FP rounding only. */
            free(reference); l3_destroy(s);
        }
    l3bench_orbit_free(&orbit); l3_mesh_free(&mesh);
}
LS_CASE(shadow_cache_tracks_sun_casters_and_mesh_contents) {
    l3_scene *s = l3_create(40, 35);
    l3bench_orbit_assets orbit = {0};
    LS_CHECK(s && l3bench_orbit_init(&orbit));
    l3bench_orbit_setup(s);
    size_t n = (size_t)s->w * s->h;
    uint8_t *dots = malloc(n);
    LS_CHECK(dots);
    l3bench_orbit_frame(s, 20, true, &orbit);
    l3bench_orbit_frame(s, 20, true, &orbit);
    LS_CHECK(s->reuse && !s->shadow_changed);
    LS_EQ_UINT(s->stats.dots_shaded, 0);
    LS_EQ_UINT(s->stats.us_raster, 0);
    for (int change = 0; change < 3; change++) {
        if (change == 0) l3_sun(s, l3v(.8f, -1, -.35f), .9f, .25f);
        if (change == 1) ((l3_vec *)orbit.vase.v)[3].x += .2f;
        l3bench_orbit_frame(s, change == 2 ? 23 : 20, change != 2, &orbit);
        LS_CHECK(s->shadow_changed && !s->reuse);
        memcpy(dots, s->dots, n);
        s->shadow_valid = s->valid = false;
        l3bench_orbit_frame(s, change == 2 ? 23 : 20, change != 2, &orbit);
        LS_EQ_INT(memcmp(dots, s->dots, n), 0);
    }
    free(dots); l3bench_orbit_free(&orbit); l3_destroy(s);
}
LS_CASE(screen_clipping_bounds_integer_edges) {
    l3_scene *s = scene(40, 35);
    const l3_vec vertices[] = {{-10000, -10000, 0}, {10000, -10000, 0}, {0, 10000, 0}};
    const uint16_t indices[] = {0, 1, 2};
    const l3_mesh mesh = {.v = vertices, .nv = 3, .idx = indices, .ntri = 1};
    l3_xform t = l3_identity();
    l3_begin(s); l3_tag(s, 9); l3_draw_mesh(s, &mesh, &t, 0); l3_end(s);
    LS_EQ_INT(coverage(s), s->w * s->h);
    for (int i = 0; i < s->ntri; i++)
        for (int j = 0; j < 3; j++) {
            LS_CHECK(s->tri[i].x[j] >= -16 && s->tri[i].x[j] <= s->w * 16);
            LS_CHECK(s->tri[i].y[j] >= -16 && s->tri[i].y[j] <= s->h * 16);
        }
    LS_EQ_UINT(sizeof(l3_pixel), 8);
    LS_EQ_UINT(sizeof(l3_surface), 16);
    l3_destroy(s);
}
LS_CASE(incremental_floor_with_camera_roll) {
    l3_scene *s = scene(32, 24);
    for (int ortho = 0; ortho < 2; ortho++) {
        if (ortho) l3_camera_ortho(s, l3v(2, 3, 6), l3v(0, 0, 0), 6);
        else l3_camera(s, l3v(2, 3, 6), l3v(0, 0, 0), 55);
        l3_camera_up(s, l3v(.4f, 1, .2f));
        l3_clip(s, .1f, 100);
        l3_begin(s); l3_tag(s, 9); l3_floor(s, -1, 0); l3_end(s);
        for (int y = 0; y < s->h; y += 2)
            for (int x = 0; x < s->w; x += 3) {
                l3_vec o, d;
                l3_ray(s, (float)x, (float)y, &o, &d);
                float t = fabsf(d.y) > 1.0e-8f ? (-1 - o.y) / d.y : -1;
                l3_vec p = l3_add(o, l3_scale(d, t));
                float z = l3_dot(l3_sub(p, s->camera.eye), s->camera.forward);
                if (t > 0 && z >= .1f && z <= 100) {
                    LS_EQ_INT(l3_pick(s, x, y), 9);
                    LS_NEAR(l3_depth(s, x, y), l3_length(l3_sub(p, s->camera.eye)), .0002f);
                } else LS_CHECK(l3_depth(s, x, y) < 0);
            }
    }
    l3_destroy(s);
}

/* Round 2: additions only; the original 19 cases and oracle are unchanged. */
LS_CASE(band_scratch_budget_fallback_and_visibility) {
    const int widths[] = {54, 116, 160};
    for (unsigned i = 0; i < sizeof(widths)/sizeof(widths[0]); i++) {
        l3_scene *s = scene(widths[i], 17);
        LS_CHECK(s->scratch[0] && s->scratch[1]);
        LS_CHECK(s->internal_bytes <= 24 * 1024);
        if (widths[i] == 116) LS_EQ_UINT(s->internal_bytes, 22272);
        if (widths[i] == 160) {
            LS_EQ_UINT(s->internal_bytes, 0);
            LS_CHECK(!s->scratch_internal[0] && !s->scratch_internal[1]);
        }
        LS_EQ_UINT(sizeof(l3_key), 4);
        cached_frame(s, 0, true);
        cached_frame(s, .1f, true);
        size_t n = (size_t)s->w*s->h;
        uint8_t *dots = malloc(n);
        l3_pixel *pixels = malloc(n*sizeof(*pixels));
        LS_CHECK(dots && pixels);
        memcpy(dots, s->dots, n);
        memcpy(pixels, s->pixels, n*sizeof(*pixels));
        s->valid = false;
        cached_frame(s, .1f, true);
        LS_EQ_INT(memcmp(dots, s->dots, n), 0);
        LS_EQ_INT(memcmp(pixels, s->pixels, n*sizeof(*pixels)), 0);
        for (size_t j = 0; j < n; j++) {
            LS_EQ_UINT(s->keys[j].tag, pixels[j].tag);
            LS_EQ_UINT(s->keys[j].mat, pixels[j].mat);
        }
        free(dots); free(pixels);
        if (widths[i] == 116) {
            LS_CHECK(l3_resize(s, 116, 19));
            LS_EQ_UINT(s->internal_bytes, 22272);
        }
        l3_destroy(s);
    }
}
LS_CASE(dynamic_shadow_caster_never_writes_static_cache) {
    l3_scene *s = l3_create(54, 70);
    l3bench_orbit_assets orbit = {0};
    LS_CHECK(s && l3bench_orbit_init(&orbit));
    l3bench_orbit_setup(s);
    size_t n = (size_t)s->w*s->h*sizeof(l3_pixel);
    memset(s->cache, 0x5a, n);
    for (int i = 0; i < 2; i++) {
        l3bench_orbit_frame(s, 20, false, &orbit);
        LS_CHECK(!s->cacheable && !s->valid && s->reuse == s->partial); /* partial redraw, never the lit static cache */
        if (i) {
            LS_CHECK(!s->shadow_changed);
            LS_EQ_UINT(s->stats.dots_shaded, 0);
            LS_EQ_UINT(s->stats.us_raster, 0);
        }
        for (size_t j = 0; j < n; j++) LS_EQ_UINT(((uint8_t *)s->cache)[j], 0x5a);
    }
    l3bench_orbit_frame(s, 20, true, &orbit);
    LS_CHECK(s->cacheable && s->valid);
    l3bench_orbit_frame(s, 20, true, &orbit);
    LS_CHECK(s->reuse);
    LS_EQ_UINT(s->stats.dots_shaded, 0);
    LS_EQ_UINT(s->stats.us_raster, 0);
    l3bench_orbit_free(&orbit); l3_destroy(s);
}
/* DICE's case: a fixed camera, a static tray, a die flying over it with sun
   shadows. Partial redraw must give exactly the picture a fresh scene gives,
   frame after frame, wherever the shadow was and went. */
static void tray_frame(l3_scene *s, int frame) {
    static const uint8_t felt[4] = {LAP_BLACK, LAP_GREEN, LAP_GREEN, 10};
    static const uint8_t wood[4] = {LAP_BLACK, LAP_RED, LAP_YELLOW, 11};
    if (s->nmat == 0) {
        l3_material(s, felt, 4, 0, L3_GRAIN);
        l3_material(s, wood, 4, 0.1f, 0);
        l3_material(s, L3_RAMP_IVORY, 4, 0.35f, 0);
        l3_sun(s, l3v(0.45f, -0.82f, 0.35f), 0.55f, 0.18f);
        l3_shadows(s, true);
        l3_sky(s, LAP_BLACK, LAP_BLACK);
    }
    l3_begin(s);
    l3_camera_ortho(s, l3v(0, 6.8f, 4.2f), l3v(0, 0, 0), 3.4f);
    l3_flags(s, L3_STATIC);
    l3_quad(s, l3v(-1, 0, 1.3f), l3v(1, 0, 1.3f), l3v(1, 0, -1.3f), l3v(-1, 0, -1.3f), 0);
    l3_xform w = l3_pose(l3v(0, 0.06f, -1.35f), (l3_quat){0, 0, 0, 1}, 1);
    l3_box(s, &w, l3v(1.1f, 0.06f, 0.05f), 1);
    l3_flags(s, L3_CAST);
    const float t = frame * 0.11f;
    l3_xform d = l3_pose(l3v(-0.7f + t * 0.9f, 0.2f + fabsf(sinf(t * 3)) * 0.6f, 0.9f - t * 1.1f),
                         l3_quat_euler(t * 300, t * 170, t * 90), 1);
    l3_rbox(s, &d, l3v(0.2f, 0.2f, 0.2f), 0.03f, 2);
    l3_end(s);
}
/* Map buildings: an L in one winding and a square in the other; roofs face up
   and walls face out whichever way the footprint came. */
LS_CASE(prisms_roofs_up_walls_out) {
    static const float xz[] = {0, 0, 4, 0, 4, 1, 1, 1, 1, 3, 0, 3,    /* an L */
                               6, 0, 6, 2, 8, 2, 8, 0};              /* a square, the other way */
    static const uint16_t counts[] = {6, 4};
    static const float base[] = {0, 0.5f}, top[] = {3, 2.5f};
    l3_mesh m = {0};
    LS_CHECK(l3_mesh_prisms(&m, xz, counts, 2, base, top, 1, 2));
    LS_EQ_INT(m.ntri, 6 * 2 + 4 + 4 * 2 + 2);
    l3_scene *s = scene(40, 40);
    l3_material(s, L3_RAMP_GREY, 4, 0, 0);
    l3_material(s, L3_RAMP_GREY, 4, 0, 0);
    l3_begin(s);
    l3_camera(s, l3v(4, 10, 12), l3v(4, 0, 1), 40);
    l3_xform id = l3_identity();
    l3_tag(s, 7);
    l3_draw_mesh(s, &m, &id, 0);
    l3_end(s);
    l3_hit hit;
    LS_CHECK(l3_raycast(s, l3v(0.5f, 10, 2.5f), l3v(0, -1, 0), 20, &hit));     /* down onto the L's arm */
    LS_CHECK(fabsf(hit.p.y - 3) < 0.01f && hit.n.y > 0.99f);
    LS_CHECK(l3_raycast(s, l3v(7, 10, 1), l3v(0, -1, 0), 20, &hit));           /* down onto the square */
    LS_CHECK(fabsf(hit.p.y - 2.5f) < 0.01f && hit.n.y > 0.99f);
    LS_CHECK(l3_raycast(s, l3v(10, 1.5f, 1), l3v(-1, 0, 0), 20, &hit));        /* the square's east wall */
    LS_CHECK(fabsf(hit.p.x - 8) < 0.01f && hit.n.x > 0.99f);
    LS_CHECK(l3_raycast(s, l3v(2, 1.5f, -5), l3v(0, 0, 1), 20, &hit));         /* the L's north wall */
    LS_CHECK(fabsf(hit.p.z) < 0.01f && hit.n.z < -0.99f);
    l3_destroy(s);
    l3_mesh_free(&m);
}
LS_CASE(partial_redraw_matches_full_render) {
    l3_scene *s = l3_create(54, 50);
    LS_CHECK(s);
    unsigned worst = 0, partial = 0;
    for (int frame = 0; frame < 16; frame++) {
        tray_frame(s, frame);
        partial += s->partial;
        l3_scene *fresh = l3_create(54, 50);
        LS_CHECK(fresh);
        tray_frame(fresh, frame);
        unsigned diff = 0;
        int bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1;
        for (int i = 0; i < s->w * s->h; i++)
            if (s->dots[i] != fresh->dots[i]) {
                diff++;
                int x = i % s->w, y = i / s->w;
                bx0 = x < bx0 ? x : bx0;
                by0 = y < by0 ? y : by0;
                bx1 = x > bx1 ? x : bx1;
                by1 = y > by1 ? y : by1;
            }
        if (diff)
            ls_note("frame %d: %u differ in x%d-%d y%d-%d; dirty x%d-%d y%d-%d prev x%d-%d y%d-%d", frame, diff,
                    bx0, bx1, by0, by1, s->dirty.x0, s->dirty.x1, s->dirty.y0, s->dirty.y1,
                    s->previous_dynamic.x0, s->previous_dynamic.x1, s->previous_dynamic.y0, s->previous_dynamic.y1);
        if (diff > worst) worst = diff;
        l3_destroy(fresh);
    }
    ls_note("partial frames %u of 16, worst %u dots differ", partial, worst);
    LS_CHECK(partial >= 14);
    LS_EQ_UINT(worst, 0);
    l3_destroy(s);
}
LS_CASE(interleaved_static_and_no_depth_keep_recording_order) {
    l3_scene *s = scene(40, 35);
    size_t n = (size_t)s->w*s->h;
    uint8_t *dots = malloc(n);
    l3_pixel *pixels = malloc(n*sizeof(*pixels));
    LS_CHECK(dots && pixels);
    for (int flags = 0; flags < 2; flags++) {
        l3_begin(s); l3_tag(s, 1);
        l3_sphere(s, l3v(0, 0, .3f), .7f, 0);
        l3_flags(s, flags ? L3_STATIC : 0); l3_tag(s, 2);
        l3_sphere(s, l3v(.1f, 0, 0), .9f, 0);
        l3_flags(s, L3_NO_DEPTH | (flags ? L3_STATIC : 0)); l3_tag(s, 3);
        l3_point(s, l3v(0, 0, -1), 4, 15);
        l3_flags(s, flags ? L3_STATIC : 0); l3_tag(s, 4);
        l3_sphere(s, l3v(-.8f, .3f, 0), .5f, 0);
        l3_end(s);
        LS_CHECK(!s->cacheable && !s->reuse);
        if (!flags) {
            memcpy(dots, s->dots, n);
            memcpy(pixels, s->pixels, n*sizeof(*pixels));
        } else {
            LS_EQ_INT(memcmp(dots, s->dots, n), 0);
            LS_EQ_INT(memcmp(pixels, s->pixels, n*sizeof(*pixels)), 0);
        }
    }
    free(dots); free(pixels); l3_destroy(s);
}
LS_CASE(round2_oracle_is_exact_in_both_orientations) {
    float *height = malloc(33*33*sizeof(*height));
    LS_CHECK(height);
    for (int y = 0; y < 33; y++) for (int x = 0; x < 33; x++) {
        float a = (x-16)*.18f, b = (y-16)*.18f;
        height[y*33+x] = .45f*sinf(a*2)*cosf(b*2);
    }
    l3_mesh mesh;
    l3bench_orbit_assets orbit = {0};
    LS_CHECK(l3_mesh_heightmap(&mesh, height, 33, 33, .16f));
    LS_CHECK(l3bench_orbit_init(&orbit));
    free(height);
    for (int orientation = 0; orientation < 2; orientation++) for (int which = 0; which < 7; which++) {
        l3_scene *s = l3_create(orientation ? 116 : 54, orientation ? 30 : which >= 5 ? 70 : 66);
        LS_CHECK(s && l3_reserve(s, 256, 4096, 8192));
        if (which >= 5) l3bench_orbit_setup(s); else l3bench_setup(s, which);
        for (int frame = 0; frame <= 20; frame++) {
            if (which >= 5) l3bench_orbit_frame(s, frame, which == 6, &orbit);
            else l3bench_frame(s, which, frame, &mesh);
        }
        size_t n = (size_t)s->w*s->h;
        uint8_t *reference = decode_reference(l3_reference[orientation][which], n*2);
        LS_EQ_INT(memcmp(s->dots, reference, n), 0);
        for (size_t j = 0; j < n; j++) LS_EQ_UINT(s->keys[j].tag, reference[n+j]);
        free(reference); l3_destroy(s);
    }
    l3bench_orbit_free(&orbit); l3_mesh_free(&mesh);
}

/* Round 3: additions only. The existing assertions above remain unchanged. */
LS_CASE(round_texels_are_circular_within_one_dot) {
    l3_scene *s = l3_create(54,70);
    LS_CHECK(s);
    const uint8_t white[] = {15}, black[] = {0}, tex[] = {1};
    LS_EQ_INT(l3_material(s,white,1,0,L3_UNLIT|L3_ROUND),0);
    LS_EQ_INT(l3_material(s,black,1,0,L3_UNLIT),1);
    l3_material_texture(s,0,l3_texture(s,1,1,tex));
    l3_camera_ortho(s,l3v(0,0,5),l3v(0,0,0),3);
    l3_begin(s); l3_tag(s,7);
    l3_xform t = l3_identity();
    l3_box(s,&t,l3v(1,1,.1f),0); l3_end(s);
    for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) {
        if (l3_pick(s,x,y) != 7) continue;
        float px = (x+.5f-s->w*.5f)*s->camera.inv_fx;
        float py = (s->h*.5f-y-.5f)*s->camera.inv_fy;
        float r = sqrtf(px*px+py*py), tol = fmaxf(s->camera.inv_fx,s->camera.inv_fy);
        if (r < .84f-tol) LS_EQ_UINT(s->dots[y*s->w+x],0);
        if (r > .84f+tol) LS_EQ_UINT(s->dots[y*s->w+x],15);
    }
    l3_destroy(s);
}
LS_CASE(wire_edges_are_unique_bright_depth_tested_and_culled) {
    l3_scene *s = scene(30,30);
    const l3_vec v[] = {{-.7f,-.7f,0},{.7f,-.7f,0},{.7f,.7f,0},{-.7f,.7f,0}};
    const uint16_t idx[] = {0,1,2,0,2,3}, back[] = {2,1,0,3,2,0};
    l3_mesh mesh = {v,idx,NULL,4,2};
    l3_xform t = l3_identity();
    l3_begin(s); l3_flags(s,L3_WIRE); l3_tag(s,5);
    l3_draw_mesh(s,&mesh,&t,0); l3_end(s);
    int wire_count = coverage(s), edges = 0;
    LS_CHECK(wire_count > 20 && wire_count < 200);
    for (int i = 0; i < s->ntri; i++) for (int e = 0; e < 3; e++) edges += (s->tri[i].edges>>e)&1;
    LS_EQ_INT(edges,5);
    for (int i = 0; i < s->w*s->h; i++) if (s->pixels[i].object) LS_EQ_UINT(s->dots[i],15);
    mesh.idx = back;
    l3_begin(s); l3_flags(s,L3_WIRE); l3_draw_mesh(s,&mesh,&t,0); l3_end(s);
    LS_EQ_INT(coverage(s),0);
    l3_begin(s); l3_flags(s,L3_WIRE|L3_TWO_SIDED); l3_draw_mesh(s,&mesh,&t,0); l3_end(s);
    LS_EQ_INT(coverage(s),wire_count);
    l3_begin(s); l3_tag(s,8); l3_sphere(s,l3v(0,0,1),.8f,0);
    l3_flags(s,L3_WIRE|L3_TWO_SIDED); l3_tag(s,5); l3_draw_mesh(s,&mesh,&t,0); l3_end(s);
    LS_EQ_INT(l3_pick(s,s->w/2,s->h/2),8);
    l3_destroy(s);
}
LS_CASE(worlds_flat_terrain_and_single_wall_match_surfaces) {
    uint8_t h[4] = {0,0,0,0}, map[] = {1};
    l3_scene *a = scene(54,70), *b = scene(54,70);
    for (int ortho = 0; ortho < 2; ortho++) for (int pitch = 0; pitch < 3; pitch++) {
        l3_vec eye = l3v(1.7f,1+pitch,5), target = l3v(0,.3f,0);
        if (ortho) { l3_camera_ortho(a,eye,target,5); l3_camera_ortho(b,eye,target,5); }
        else { l3_camera(a,eye,target,50); l3_camera(b,eye,target,50); }
        for (int wall = 0; wall < 2; wall++) {
            l3_begin(a); l3_tag(a,9);
            if (wall) l3_walls(a,map,1,1,2,2,l3v(-1,0,-1));
            else l3_terrain(a,h,NULL,2,2,2,1,l3v(-1,0,-1),0);
            l3_end(a);
            l3_begin(b); l3_tag(b,9);
            if (wall) { l3_xform t = l3_identity(); t.m[10] = 1; l3_box(b,&t,l3v(1,1,1),0); }
            else l3_quad(b,l3v(-1,0,1),l3v(1,0,1),l3v(1,0,-1),l3v(-1,0,-1),0);
            l3_end(b);
            unsigned differences = 0;
            for (int y = 1; y+1 < a->h; y++) for (int x = 1; x+1 < a->w; x++) {
                bool aa = l3_pick(a,x,y) != 0, bb = l3_pick(b,x,y) != 0;
                if (aa == bb) {
                    if (aa) LS_NEAR(l3_depth(a,x,y),l3_depth(b,x,y),.01f);
                    continue;
                }
                differences++;
                bool edge = false;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
                    edge |= (l3_pick(a,x+dx,y+dy)!=0) == bb;
                LS_CHECK(edge);
            }
            LS_CHECK(differences < 150);
            l3_hit hit;
            LS_CHECK(l3_raycast(a,l3v(0,4,0),l3v(0,-2,0),3,&hit));
            LS_NEAR(hit.t,wall ? 1 : 2,.0001f); LS_EQ_INT(hit.tag,9);
        }
    }
    l3_destroy(a); l3_destroy(b);
}
LS_CASE(terrain_ridge_occludes_sphere_and_casts_shadows) {
    uint8_t h[25] = {0};
    for (int x = 0; x < 5; x++) h[3*5+x] = 3;
    l3_scene *s = scene(54,70);
    l3_camera(s,l3v(0,1.5f,5),l3v(0,1.5f,0),50);
    l3_shadows(s,true);
    l3_begin(s); l3_tag(s,3); l3_flags(s,L3_CAST);
    l3_terrain(s,h,NULL,5,5,1,1,l3v(-2,0,-2),0);
    l3_tag(s,7); l3_sphere(s,l3v(0,1,-1),.5f,0); l3_end(s);
    LS_EQ_INT(l3_pick(s,s->w/2,s->h/2),3);
    l3_hit hit;
    LS_CHECK(l3_raycast(s,l3v(0,1.5f,5),l3v(0,0,-1),10,&hit));
    LS_EQ_INT(hit.tag,3);
    unsigned filled = 0;
    for (int i = 0; i < L3_SHADOW*L3_SHADOW; i++) filled += s->shadow[i] != 0;
    LS_CHECK(filled > 100);
    l3_destroy(s);
}
LS_CASE(camera_helpers_follow_degree_convention) {
    l3_scene *s = scene(20,20);
    l3_camera_look(s,l3v(1,2,3),0,0,50);
    LS_NEAR(s->camera.forward.z,-1,.00001f);
    l3_camera_look(s,l3v(1,2,3),90,30,50);
    LS_NEAR(s->camera.forward.x,.8660254f,.00001f);
    LS_NEAR(s->camera.forward.y,.5f,.00001f);
    l3_camera_orbit(s,l3v(1,2,3),4,0,30,50);
    LS_NEAR(s->eye.y,4,.00001f); LS_NEAR(s->eye.z,6.4641016f,.00001f);
    LS_NEAR(s->camera.forward.y,-.5f,.00001f);
    l3_destroy(s);
}
LS_CASE(particles_are_binned_lit_depth_tested_and_allocation_free) {
    l3_scene *s = scene(54,70);
    l3_vec *p = malloc(2000*sizeof(*p)); LS_CHECK(p);
    for (int i = 0; i < 2000; i++) p[i] = l3v((i%50-25)*.04f,(i/50-20)*.04f,0);
    allocs = 0; count_alloc = true;
    l3_begin(s); l3_tag(s,6); l3_particles(s,p,2000,.04f,0); l3_end(s);
    count_alloc = false;
    LS_EQ_UINT(allocs,0); LS_EQ_UINT(s->stats.dropped,0);
    LS_CHECK(s->nparticle > 1000 && coverage(s) > 100);
    l3_hit hit;
    LS_CHECK(l3_raycast(s,l3v(0,0,6),l3v(0,0,-2),4,&hit));
    LS_EQ_INT(hit.tag,6); LS_NEAR(hit.t,2.99f,.0002f);
    l3_begin(s); l3_tag(s,8); l3_sphere(s,l3v(0,0,1),.8f,0);
    l3_tag(s,6); l3_particles(s,p,2000,.04f,0); l3_end(s);
    LS_EQ_INT(l3_pick(s,s->w/2,s->h/2),8);
    free(p); l3_destroy(s);
}
LS_CASE(raycast_every_analytic_kind_mesh_and_last_frame_lifetime) {
    l3_scene *s = scene(20,20);
    l3_xform t = l3_identity();
    const l3_vec v[] = {{-2,-2,0},{2,-2,0},{0,2,0}};
    const uint16_t idx[] = {0,1,2};
    l3_mesh mesh = {v,idx,NULL,3,1};
    l3_hit hit;
    for (int kind = S_SPHERE; kind <= S_MESH; kind++) {
        l3_begin(s); l3_tag(s,kind+1); l3_flags(s,L3_TWO_SIDED);
        switch (kind) {
        case S_SPHERE: l3_sphere(s,l3v(0,0,0),1,0); break;
        case S_ELLIPSOID: l3_ellipsoid(s,&t,l3v(1,1,1),0); break;
        case S_BOX: l3_box(s,&t,l3v(1,1,1),0); break;
        case S_RBOX: l3_rbox(s,&t,l3v(1,1,1),.2f,0); break;
        case S_CYLINDER: l3_cylinder(s,l3v(0,-1,0),l3v(0,1,0),1,0); break;
        case S_CAPSULE: l3_capsule(s,l3v(0,-1,0),l3v(0,1,0),1,0); break;
        case S_CONE: l3_cone(s,l3v(0,-1,0),l3v(0,1,0),1,.5f,0); break;
        case S_TORUS: l3_torus(s,&t,.75f,.25f,0); break;
        case S_DISC: l3_disc(s,l3v(0,0,0),l3v(0,0,1),1,0); break;
        case S_FLOOR: l3_floor(s,0,0); break;
        case S_QUAD: l3_quad(s,v[0],v[1],v[2],l3v(-2,2,0),0); break;
        case S_MESH: l3_draw_mesh(s,&mesh,&t,0); break;
        }
        l3_end(s);
        l3_vec o = kind == S_FLOOR ? l3v(0,6,0) : l3v(0,0,6);
        l3_vec d = kind == S_FLOOR ? l3v(0,-2,0) : l3v(0,0,-2);
        LS_CHECK(l3_raycast(s,o,d,4,&hit)); LS_EQ_INT(hit.tag,kind+1);
        LS_CHECK(hit.t > 2 && hit.t <= 3.001f);
        LS_NEAR(l3_length(hit.n),1,.0001f);
        LS_CHECK(!l3_raycast(s,o,d,1,&hit));
    }
    l3_begin(s);
    LS_CHECK(!l3_raycast(s,l3v(0,0,6),l3v(0,0,-1),10,&hit));
    l3_end(s);
    LS_CHECK(!l3_raycast(s,l3v(0,0,6),l3v(0,0,-1),10,&hit));
    l3_destroy(s);
}

LS_CASE(wall_column_cache_matches_box_and_inside_queries_exit) {
    l3_scene *a = scene(54,70), *b = scene(54,70);
    uint8_t map[] = {1};
    for (int ortho = 0; ortho < 2; ortho++) for (int height = 0; height < 3; height++) {
        l3_vec eye = l3v(1.2f,(float)height,5), target = l3v(0,(float)height,0);
        if (ortho) { l3_camera_ortho(a,eye,target,4); l3_camera_ortho(b,eye,target,4); }
        else { l3_camera(a,eye,target,50); l3_camera(b,eye,target,50); }
        l3_begin(a); l3_tag(a,4); l3_walls(a,map,1,1,2,1,l3v(-1,0,-1)); l3_end(a);
        l3_begin(b); l3_tag(b,4);
        l3_xform t = l3_identity(); t.m[10] = .5f;
        l3_box(b,&t,l3v(1,.5f,1),0); l3_end(b);
        LS_EQ_INT(a->ncolumn,a->w);
        unsigned mismatch = 0;
        for (int y = 0; y < a->h; y++) for (int x = 0; x < a->w; x++)
            mismatch += l3_pick(a,x,y) != l3_pick(b,x,y);
        ls_note("wall cache ortho=%d height=%d mismatch=%u",ortho,height,mismatch);
        LS_CHECK(mismatch < 60);
        l3_hit hit;
        LS_CHECK(l3_raycast(a,l3v(0,.5f,0),l3v(0,0,2),2,&hit));
        LS_NEAR(hit.t,.5f,.00001f); LS_NEAR(hit.n.z,1,.00001f);
    }
    l3_destroy(a); l3_destroy(b);
}
LS_CASE(fused_resolve_and_separate_resolve_are_identical) {
    l3_scene *s = scene(54,70);
    size_t n = (size_t)s->w*s->h;
    uint8_t *dots = malloc(n); LS_CHECK(dots);
    l3_outline(s,0); l3_fog(s,4,4,12);
    for (int f = 0; f < 8; f++) {
        cached_frame(s,f*.11f,true);
        LS_EQ_UINT(s->stats.us_shade,s->counts[0].us_shade+s->counts[1].us_shade);
        LS_EQ_UINT(s->stats.us_resolve,s->counts[0].us_resolve+s->counts[1].us_resolve);
        memcpy(dots,s->dots,n);
        l3_resolve(s);
        LS_EQ_INT(memcmp(dots,s->dots,n),0);
    }
    LS_EQ_UINT(sizeof(*s->shadow),2);
    free(dots); l3_destroy(s);
}
LS_CASE(world_queries_survive_a_completely_reused_frame) {
    l3_scene *s = scene(30,30);
    uint8_t h[] = {2,2,2,2}, map[] = {1};
    for (int wall = 0; wall < 2; wall++) {
        for (int frame = 0; frame < 2; frame++) {
            l3_begin(s); l3_flags(s,L3_STATIC); l3_tag(s,8);
            if (wall) l3_walls(s,map,1,1,2,2,l3v(-1,1,-1));
            else l3_terrain(s,h,NULL,2,2,2,1,l3v(-1,1,-1),0);
            l3_end(s);
            if (frame) LS_EQ_UINT(s->stats.dots_shaded,0);
            l3_hit hit;
            LS_CHECK(l3_raycast(s,l3v(0,5,0),l3v(0,-1,0),10,&hit));
            LS_EQ_INT(hit.tag,8); LS_NEAR(hit.t,2,.0001f);
        }
    }
    l3_destroy(s);
}
LS_CASE(flat_closed_shapes_keep_exit_surfaces_when_near_clipped) {
    l3_scene *s = scene(20,20);
    l3_xform t = l3_identity();
    for (int round = 0; round < 2; round++) for (int inside = 0; inside < 2; inside++) {
        l3_camera(s,l3v(0,0,inside ? 0 : 2),l3v(0,0,-1),60);
        l3_clip(s,inside ? .1f : 1.5f,10);
        l3_begin(s); l3_tag(s,7);
        if (round) l3_rbox(s,&t,l3v(1,1,1),.2f,0);
        else l3_box(s,&t,l3v(1,1,1),0);
        l3_end(s);
        LS_CHECK(coverage(s) > s->w*s->h*9/10);
        LS_EQ_INT(l3_pick(s,s->w/2,s->h/2),7);
        LS_NEAR(l3_depth(s,s->w/2,s->h/2),inside ? 1 : 3,.01f);
    }
    l3_destroy(s);
}

/* Round 4 additions: old query paths remain independent visibility oracles. */
LS_CASE(terrain_columns_match_traced_silhouette_within_one_dot) {
    uint8_t heights[9*9];
    for (int z = 0; z < 9; z++) for (int x = 0; x < 9; x++)
        heights[z*9+x] = (uint8_t)(8+(z < 5 ? z : 8-z)*3+x);
    for (int orientation = 0; orientation < 2; orientation++) {
        l3_scene *s = scene(orientation ? 58 : 27,orientation ? 30 : 70);
        for (int pitch = 0; pitch < 3; pitch++) {
            l3_camera_look(s,l3v(.4f,2,4),17,(float)(-8-pitch*12),60);
            allocs = 0; count_alloc = true;
            l3_begin(s); l3_tag(s,7);
            l3_terrain(s,heights,NULL,9,9,.5f,.04f,l3v(-2,0,-2),0); l3_end(s);
            count_alloc = false;
            LS_EQ_UINT(allocs,0); LS_CHECK(s->land_active);
            unsigned hits = 0, differences = 0;
            for (int y = 1; y+1 < s->h; y++) for (int x = 1; x+1 < s->w; x++) {
                l3_work w = {0};
                l3_zray(&s->camera,(float)x,(float)y,&w.o,&w.dir);
                bool traced = l3_world_hit(&s->cmd[0],&w,s->camera.near_z,s->camera.far_z);
                bool column = l3_pick(s,x,y) == 7;
                hits += traced;
                if (traced == column) continue;
                differences++;
                bool neighbour = false;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
                    neighbour |= (l3_pick(s,x+dx,y+dy) == 7) == traced;
                LS_CHECK(neighbour);
            }
            ls_note("terrain columns orientation=%d pitch=%d silhouette differences=%u",orientation,pitch,differences);
            LS_CHECK(hits > 50);
        }
        l3_destroy(s);
    }
}
LS_CASE(pitched_wall_columns_match_traced_boxes) {
    const uint8_t map[25] = {1,1,1,1,1, 1,0,0,0,1, 1,0,1,0,1, 1,0,0,0,1, 1,1,1,1,1};
    l3_scene *s = scene(30,30);
    for (int pitch = -30; pitch <= 30; pitch += 15) {
        l3_camera_look(s,l3v(1.5f,.5f,3.5f),-17,(float)pitch,70);
        l3_begin(s); l3_tag(s,4); l3_walls(s,map,5,5,1,1,l3v(0,0,0)); l3_end(s);
        LS_EQ_INT(s->ncolumn,s->w);
        for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) {
            l3_work w = {0};
            l3_zray(&s->camera,(float)x,(float)y,&w.o,&w.dir);
            float nearest = s->camera.far_z; bool hit = false;
            for (int z = 0; z < 5; z++) for (int xx = 0; xx < 5; xx++) if (map[z*5+xx]) {
                l3_cmd box = {0}; box.kind = S_BOX; box.half = l3v(.5f,.5f,.5f);
                box.t = l3_identity(); box.t.m[9] = xx+.5f; box.t.m[10] = .5f; box.t.m[11] = z+.5f;
                LS_CHECK(l3_inverse(&box.inv,&box.t));
                if (l3_intersect(&box,&w,s->camera.near_z,nearest)) { nearest = w.hit; hit = true; }
            }
            LS_EQ_INT(l3_pick(s,x,y) == 4,hit);
            if (hit) LS_NEAR(s->pixels[y*s->w+x].depth,nearest,.0002f);
        }
    }
    l3_destroy(s);
}
LS_CASE(terrain_sun_cache_ignores_camera_and_noncasters_but_tracks_height_and_sun) {
    l3_scene *s = scene(27,35); uint8_t heights[81]; memset(heights,8,sizeof heights);
    l3_vec particle = l3v(0,2,0); l3_shadows(s,true);
    for (int frame = 0; frame < 4; frame++) {
        l3_camera_look(s,l3v(0,3,5-frame*.1f),0,-14,60);
        if (frame == 2) heights[40]++;
        if (frame == 3) l3_sun(s,l3v(.4f,-1,-.3f),.8f,.2f);
        particle.x += .1f;
        l3_begin(s); l3_flags(s,L3_CAST|L3_STATIC);
        l3_terrain(s,heights,NULL,9,9,.5f,.1f,l3v(-2,0,-2),0);
        l3_flags(s,0); l3_particles(s,&particle,1,.1f,0); l3_end(s);
        LS_EQ_INT(s->shadow_changed,frame != 1);
    }
    l3_destroy(s);
}
LS_CASE(band_ranges_scale_with_entries_instead_of_screen_height) {
    l3_scene *s = scene(54,70); LS_CHECK(l3_reserve(s,64,4096,6144));
    size_t bytes = (size_t)s->bin_stride*sizeof(*s->bin);
    LS_CHECK(bytes < 150000);
    LS_CHECK(l3_resize(s,116,30));
    LS_EQ_UINT((size_t)s->bin_stride*sizeof(*s->bin),bytes);
    l3_destroy(s);
}

LS_CASE(static_and_dynamic_terrain_layers_preserve_partial_redraw) {
    l3_scene *s = scene(27,35); uint8_t low[81], high[81];
    memset(low,0,sizeof low); memset(high,8,sizeof high);
    l3_camera_look(s,l3v(0,3,5),0,-20,60);
    size_t n = (size_t)s->w*s->h;
    uint8_t *dots = malloc(n); l3_pixel *pixels = malloc(n*sizeof(*pixels));
    LS_CHECK(dots && pixels);
    for (int frame = 0; frame < 4; frame++) {
        l3_begin(s); l3_flags(s,L3_STATIC); l3_tag(s,1);
        l3_terrain(s,low,NULL,9,9,.5f,.1f,l3v(-2,0,-2),0);
        l3_flags(s,0); l3_tag(s,2);
        if (frame < 3) l3_terrain(s,high,NULL,9,9,.25f,.1f,l3v(-1+frame*.4f,0,-1),0);
        l3_end(s);
        LS_CHECK(s->land_active);
        memcpy(dots,s->dots,n); memcpy(pixels,s->pixels,n*sizeof(*pixels));
        s->valid = false; s->have_frame = false;
        l3_begin(s); l3_flags(s,L3_STATIC); l3_tag(s,1);
        l3_terrain(s,low,NULL,9,9,.5f,.1f,l3v(-2,0,-2),0);
        l3_flags(s,0); l3_tag(s,2);
        if (frame < 3) l3_terrain(s,high,NULL,9,9,.25f,.1f,l3v(-1+frame*.4f,0,-1),0);
        l3_end(s);
        LS_EQ_INT(memcmp(dots,s->dots,n),0);
        LS_EQ_INT(memcmp(pixels,s->pixels,n*sizeof(*pixels)),0);
    }
    free(dots); free(pixels); l3_destroy(s);
}
LS_CASE(lamp_reciprocal_square_root_has_bounded_error) {
    for (int i = 1; i < 10000; i++) {
        float d2 = i*.00317f;
        LS_NEAR(l3_rsqrt(d2)*sqrtf(d2),1,.000005f);
    }
}

LS_CASE(wall_prepass_matches_recording_order_including_floor_ceiling_ties) {
    l3_scene *s = scene(30,30); const uint8_t grid[] = {1,1,1,1,0,1,1,1,1};
    l3_camera_look(s,l3v(1.5f,.5f,1.5f),17,0,70);
    l3_begin(s); l3_tag(s,1); l3_floor(s,0,0);
    l3_flags(s,L3_TWO_SIDED); l3_tag(s,2);
    l3_quad(s,l3v(0,1,0),l3v(3,1,0),l3v(3,1,3),l3v(0,1,3),0);
    l3_flags(s,0); l3_tag(s,3); l3_walls(s,grid,3,3,1,1,l3v(0,0,0)); l3_end(s);
    LS_CHECK(s->walls_first);
    size_t n = (size_t)s->w*s->h;
    l3_pixel *pixels = malloc(n*sizeof(*pixels)); LS_CHECK(pixels);
    memcpy(pixels,s->pixels,n*sizeof(*pixels));
    s->walls_first = false; s->reuse = false; s->cacheable = false;
    s->dirty = (l3_rect){0,0,s->w,s->h}; l3_run(s,4);
    LS_EQ_INT(memcmp(pixels,s->pixels,n*sizeof(*pixels)),0);
    free(pixels); l3_destroy(s);
}

LS_CASE(vertical_terrain_camera_keeps_exact_query_fallback) {
    l3_scene *s = scene(20,20); uint8_t heights[81]; memset(heights,4,sizeof heights);
    l3_camera(s,l3v(0,5,0),l3v(0,0,0),60);
    l3_begin(s); l3_tag(s,9);
    l3_terrain(s,heights,NULL,9,9,.5f,.1f,l3v(-2,0,-2),0); l3_end(s);
    LS_CHECK(!s->land_active); LS_CHECK(coverage(s) > s->w*s->h/2);
    LS_EQ_INT(l3_pick(s,s->w/2,s->h/2),9);
    l3_destroy(s);
}

LS_CASE(partial_shadow_reach_includes_distant_noncasting_receivers) {
    l3_scene *a = scene(30,40), *b = scene(30,40);
    for (int frame = 0; frame < 5; frame++) {
        for (int which = 0; which < 2; which++) {
            l3_scene *s = which ? b : a;
            if (!frame) {
                l3_camera(s,l3v(4,3,7),l3v(0,1,-3),50);
                l3_sun(s,l3v(0,-.1f,-1),.8f,.2f); l3_shadows(s,true);
            }
            if (which) { s->valid = false; s->have_frame = false; }
            l3_begin(s); l3_flags(s,L3_STATIC|L3_TWO_SIDED); l3_tag(s,1);
            l3_quad(s,l3v(-5,-2,-8),l3v(5,-2,-8),l3v(5,5,-8),l3v(-5,5,-8),0);
            l3_flags(s,L3_CAST); l3_tag(s,2);
            l3_sphere(s,l3v(frame*.22f,.8f,0),.5f,0); l3_end(s);
        }
        LS_EQ_INT(memcmp(a->dots,b->dots,(size_t)a->w*a->h),0);
        if (frame) LS_CHECK(a->partial);
    }
    l3_destroy(a); l3_destroy(b);
}

LS_CASE(upside_down_terrain_camera_keeps_query_fallback) {
    l3_scene *s = scene(20,20); uint8_t heights[81]; memset(heights,4,sizeof heights);
    l3_camera_look(s,l3v(0,3,4),0,-20,60); l3_camera_up(s,l3v(0,-1,0));
    l3_begin(s); l3_tag(s,9);
    l3_terrain(s,heights,NULL,9,9,.5f,.1f,l3v(-2,0,-2),0); l3_end(s);
    LS_CHECK(!s->land_active); LS_CHECK(coverage(s) > 20);
    l3_destroy(s);
}
