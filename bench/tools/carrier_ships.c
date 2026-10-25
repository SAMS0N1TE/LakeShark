/* CARRIER's hulls, each on its own at the board's size, for checking silhouettes.

       carrier_ships OUT_PREFIX [yaw]      writes OUT_PREFIX_NN.png per hull
*/
#include "l3png.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../app-sdk/apps/carrier/hulls.inc"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    float yaw = argc > 2 ? (float)atof(argv[2]) : 35;
    l3_scene *sc = l3_create(26, 22);
    l3_reserve(sc, 64, 2048, 2048);
    static const uint8_t engine[] = {LAP_RED, 9, 11, 15}, glass[] = {LAP_BLUE, 12, 14, 15},
                         dark[] = {LAP_BLACK, 8}, slot[] = {LAP_BLACK}, lamp[] = {11};
    l3_material(sc, engine, 4, 0, L3_UNLIT | L3_GLOW);
    l3_material(sc, glass, 4, .9f, 0);
    l3_material(sc, dark, 2, 0, 0);
    l3_material(sc, L3_RAMP_GREY, 4, .5f, 0);
    l3_material(sc, slot, 1, 0, L3_UNLIT);
    l3_material(sc, lamp, 1, 0, L3_UNLIT | L3_GLOW);
    uint8_t hull_ink[] = {LAP_BLACK, LAP_CYAN, 14, 15};
    int hull = l3_material(sc, hull_ink, 4, .2f, 0);
    l3_sun(sc, l3v(.4f, -.6f, -.5f), .9f, .25f);
    l3_outline(sc, LAP_BLACK);
    l3_sky(sc, LAP_BLACK, LAP_BLACK);
    int n = (int)(sizeof hulls / sizeof hulls[0]);
    for (int i = 0; i < n; i++) {
        float r = 0;
        for (int k = 0; k < hulls[i].nv; k++) {
            l3_vec v = hulls[i].v[k];
            r = fmaxf(r, sqrtf(l3_dot(v, v)));
        }
        l3_camera_orbit(sc, l3v(0, 0, 0), r * 3.3f, yaw, 24, 40);
        l3_begin(sc);
        l3_xform t = l3_identity();
        l3_draw_mesh(sc, &hulls[i], &t, hull);
        l3_end(sc);
        char path[512];
        snprintf(path, sizeof path, "%s_%02d.png", argv[1], i);
        l3png(path, sc);
    }
    l3_destroy(sc);
    return 0;
}
