/* Host-only lap3d timing and PNG evidence. Uses uncompressed PNG DEFLATE. */
#include "l3bench_scene.h"
#ifdef L3_PROFILE
#include "../../components/apps/tui/lap3d/l3_private.h"
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ ((0u - (c & 1)) & 0xedb88320u);
    }
    return ~c;
}
static void be(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}
static bool chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t *p = malloc(n + 12);
    if (!p)
        return false;
    be(p, (uint32_t)n);
    memcpy(p + 4, type, 4);
    if (n)
        memcpy(p + 8, data, n);
    be(p + 8 + n, crc(p + 4, n + 4));
    bool ok = fwrite(p, 1, n + 12, f) == n + 12;
    free(p);
    return ok;
}
static bool png(const char *path, l3_scene *s) {
    int dw, dh;
    l3_size(s, &dw, &dh);
    int w = dw * 5, h = dh * 6;
    size_t stride = (size_t)w * 3 + 1, n = stride * h;
    uint8_t *raw = malloc(n);
    if (!raw)
        return false;
    uint8_t *dots = l3_dots(s);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * stride] = 0;
        for (int x = 0; x < w; x++) {
            uint8_t ink = dots[(y / 6) * dw + x / 5];
            uint16_t c = lap_palette(ink == LAP_DOT_CLEAR ? 0 : ink);
            uint8_t *p = raw + (size_t)y * stride + 1 + x * 3;
            p[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            p[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            p[2] = (uint8_t)((c & 31) * 255 / 31);
        }
    }
    size_t blocks = (n + 65534) / 65535, zsize = 2 + n + blocks * 5 + 4;
    uint8_t *z = malloc(zsize);
    if (!z) {
        free(raw);
        return false;
    }
    z[0] = 0x78;
    z[1] = 1;
    size_t at = 2, pos = 0;
    while (pos < n) {
        unsigned count = n - pos > 65535 ? 65535 : (unsigned)(n - pos);
        z[at++] = (uint8_t)(pos + count == n);
        z[at++] = (uint8_t)count;
        z[at++] = (uint8_t)(count >> 8);
        z[at++] = (uint8_t)~count;
        z[at++] = (uint8_t)(~count >> 8);
        memcpy(z + at, raw + pos, count);
        at += count;
        pos += count;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    be(z + at, (b << 16) | a);
    free(raw);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return false;
    }
    uint8_t ihdr[13] = {0};
    be(ihdr, (uint32_t)w);
    be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    const uint8_t sig[] = {137, 80, 78, 71, 13, 10, 26, 10};
    bool ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, zsize) && chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    return ok;
}
int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "bench/build-lap3d";
    const char *names[] = {"shapes", "mesh2048", "shadows", "outline", "static_ball", "orbit", "orbit_static", "terrain", "walls", "particles2000", "dice", "floor", "skyride", "crypt"};
    float *height = malloc(33 * 33 * sizeof(*height));
    if (!height)
        return 1;
    for (int y = 0; y < 33; y++)
        for (int x = 0; x < 33; x++) {
            float a = (x - 16) * 0.18f, b = (y - 16) * 0.18f;
            height[y * 33 + x] = 0.45f * sinf(a * 2) * cosf(b * 2);
        }
    l3_mesh mesh;
    if (!l3_mesh_heightmap(&mesh, height, 33, 33, 0.16f)) {
        free(height);
        return 1;
    }
    free(height);
    l3bench_orbit_assets orbit = {0};
    if (!l3bench_orbit_init(&orbit))
        return 5;
    l3bench_world_assets *world = malloc(sizeof(*world));
    if (!world) return 1;
    l3bench_world_init(world);
    l3bench_app_assets *apps = malloc(sizeof(*apps));
    if (!apps) return 1;
    l3bench_app_init(apps);
    puts("orientation scene us/frame shadow raster shade resolve shaded reused tris "
         "shapes cores dropped");
    for (int orientation = 0; orientation < 2; orientation++)
        for (int scene = 0; scene < 14; scene++) {
#ifdef L3BENCH_BASELINE
            if (scene >= 7 && scene <= 9) continue;
#endif
            l3_scene *s = l3_create(orientation ? 116 : 54, orientation ? 30 : scene >= 5 ? 70 : 66);
            if (!s)
                return 1;
            l3_reserve(s, scene == 5 || scene == 6 ? 64 : 256, 4096, scene == 5 || scene == 6 ? 6144 : 8192);   /* the 2,048-triangle mesh, clipped, in two views */
            if (scene >= 12) {
                l3bench_app_setup(s,scene);
                l3bench_app_frame(s,scene,0,apps);
            } else if (scene >= 7) {
                l3bench_world_setup(s,scene);
                l3bench_world_frame(s,scene,0,world);
            } else if (scene >= 5) {
                l3bench_orbit_setup(s);
                l3bench_orbit_frame(s, 0, scene == 6, &orbit);
            } else {
                l3bench_setup(s, scene);
                l3bench_frame(s, scene, 0, &mesh);
            }
            uint64_t total = 0, shadow = 0, raster = 0, shade = 0, resolve = 0;
            l3_stats_t stats = {0};
            const char *frame_env = getenv("L3BENCH_FRAMES");
            const int frames = frame_env ? atoi(frame_env) : 20;
            if (frames < 1 || frames > 10000) return 6;
            for (int f = 1; f <= frames; f++) {
                if (scene >= 12) l3bench_app_frame(s,scene,f,apps);
                else if (scene >= 7) l3bench_world_frame(s,scene,f,world);
                else if (scene >= 5)
                    l3bench_orbit_frame(s, f, scene == 6, &orbit);
                else
                    l3bench_frame(s, scene, f, &mesh);
                l3_stats(s, &stats);
                if (stats.dropped)
                    return 2;
                total += stats.us_total;
                shadow += stats.us_shadow;
                raster += stats.us_raster;
                shade += stats.us_shade;
                resolve += stats.us_resolve;
            }
            printf("%s %s %llu %llu %llu %llu %llu %u %u %u %u %u %u\n",
                   orientation ? "landscape" : "portrait", names[scene],
                   (unsigned long long)(total / frames), (unsigned long long)(shadow / frames),
                   (unsigned long long)(raster / frames), (unsigned long long)(shade / frames), (unsigned long long)(resolve / frames),
                   stats.dots_shaded, stats.dots_reused, stats.triangles, stats.shapes, stats.cores,
                   stats.dropped);
#ifdef L3_PROFILE
            const l3_counts *c = &s->counts[0]; /* deterministic one-core host */
            printf("proxy %d %s: candidates=%u tri_bbox=%u intersections=%u sdf_steps=%u "
                   "writes=%u lights=%u shadow_lookups=%u resolves=%u tri_recip=%u plane_recip=%u\n",
                   orientation, names[scene], c->candidates, c->edges, c->intersections, c->steps,
                   c->writes, c->lights, c->shadows, c->resolves,
                   c->triangle_reciprocals, c->plane_reciprocals);
            printf("traffic %d %s: %llu bytes/frame internal=%zu psram=%zu\n", orientation, names[scene],
                   (unsigned long long)s->frame_bytes, s->internal_bytes+s->lighting_internal+
                       (s->shadow_internal ? L3_SHADOW*L3_SHADOW*sizeof(uint16_t) : 0), s->psram_bytes);
#endif
            char path[512];
            /* Optional raw dots/tags for an independent, pre-optimization oracle. */
            if (argc > 2) {
                snprintf(path, sizeof path, "%s/%d-%d.bin", argv[2], orientation, scene);
                FILE *raw = fopen(path, "wb");
                if (!raw)
                    return 4;
                int w, h;
                l3_size(s, &w, &h);
                fwrite(l3_dots(s), 1, (size_t)w * h, raw);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        fputc(l3_pick(s, x, y), raw);
                fclose(raw);
            }
            snprintf(path, sizeof path, "%s/%s-%s.png", out, orientation ? "landscape" : "portrait",
                     names[scene]);
            if (!png(path, s))
                return 3;
            l3_destroy(s);
        }
    free(apps); free(world);
    l3bench_orbit_free(&orbit);
    l3_mesh_free(&mesh);
    return 0;
}
