/* Standalone lap3d host bridge: same geometry and palette as ls_lap_api.c. */
#include "../../app-sdk/include/lakeshark_app.h"
int l3_test_cw = 10, l3_test_ch = 17;
void lap_cell_px(int *w, int *h) {
    if (w)
        *w = l3_test_cw;
    if (h)
        *h = l3_test_ch;
}
void lap_dots(int x, int y, int w, int h, const uint8_t *dots) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)dots;
}
uint16_t lap_palette(int c) {
    static const uint16_t p[16] = {0x0000, 0xB249, 0x4C6B, 0xB447, 0x3B74, 0x8AD4, 0x3C73, 0xAD97,
                                   0x3A09, 0xFB4D, 0x6F0F, 0xFE4A, 0x6DBF, 0xDC5F, 0x5F1C, 0xF7BF};
    return p[c & 15];
}
