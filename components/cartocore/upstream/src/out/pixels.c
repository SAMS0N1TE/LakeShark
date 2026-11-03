#include "cartocore/out.h"
#include <string.h>

/* Rastered planes as a picture: one dot is one pixel, so a renderer set up
   with cols = width/2 and rows = height/4 paints a width x height image. Lines
   take their ink, polygon edges their exact coverage mask, everything else its
   ground. Hillshade is interpolated between cell centres so relief is smooth
   rather than in 2x4 blocks, and it lifts as well as darkens so it reads on a
   near-black ground. */

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}

static int land_ink(uint8_t ink)
{
    return ink == CC_LAND || ink == CC_WOOD || ink == CC_PARK || ink == CC_FARM ||
           ink == CC_RESIDENTIAL || ink == CC_INDUSTRIAL;
}

static uint32_t shaded(uint32_t c, int s16)
{
    /* s16: shade in sixteenths around flat (0), roughly -128..+64 */
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    const int lift = s16 / 4;
    r += r * s16 / 256 + lift; g += g * s16 / 256 + lift; b += b * s16 / 256 + lift;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

void cc_pixels_rgb565_prepare(const cc_pixel_style *s,uint16_t shades[CC_INK_COUNT][256]) {
    for(unsigned ink=0;ink<CC_INK_COUNT;ink++)for(int q=0;q<256;q++)
        shades[ink][q]=rgb565(shaded(s->rgb[ink],q-128));
}

/* Shade at a dot, in sixteenths of a step around flat, bilinear between
   the four nearest cell centres (2c+1, 4r+2). Absent cells count as flat. */
static int shade_at(const cc_planes *p, int x, int y)
{
    int cx = (x - 1) >> 1, cy = (y - 2) >> 2;
    int fx = (x - 1) - cx * 2, fy = (y - 2) - cy * 4;  /* 0..1, 0..3 */
    if (x < 1) { cx = 0; fx = 0; }
    if (y < 2) { cy = 0; fy = 0; }
    int acc = 0;
    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
        int sx = cx + i, sy = cy + j;
        if (sx >= p->cols) sx = p->cols - 1;
        if (sy >= p->rows) sy = p->rows - 1;
        const uint8_t q = p->shade[(size_t)sy * p->cols + sx];
        const int v = q > 15 ? 0 : ((int)q - 8) * 16;
        const int w = (i ? fx : 2 - fx) * (j ? fy : 4 - fy);    /* of 8 */
        acc += v * w;
    }
    return acc / 8;
}

static int unchanged(const cc_planes *p,const cc_cell *state,int x,int y,int relief) {
    size_t i=(size_t)y*p->cols+x;const uint8_t *v=(const uint8_t*)&state[i];
    if(!v[10] || v[0]!=p->dots[i] || v[1]!=p->ink[i] || v[2]!=p->dot_priority[i] ||
       v[3]!=p->fill[i] || v[4]!=p->fill_priority[i] || v[6]!=p->coverage_ink[i])return 0;
    if(p->coverage_ink[i] && (v[5]!=p->coverage[i] || v[7]!=p->coverage_bg[i] || v[8]!=p->coverage_priority[i]))return 0;
    if(relief) for(int j=-1;j<=1;j++) for(int k=-1;k<=1;k++) {
        int sx=x+k,sy=y+j;
        /* Clamped picture borders have a different interpolation neighbour
           after a pan. Recompute a one-cell halo instead of borrowing it. */
        if(sx<0 || sy<0 || sx>=p->cols || sy>=p->rows)return 0;
        size_t at=(size_t)sy*p->cols+sx;const uint8_t *q=(const uint8_t*)&state[at];
        if(!q[10] || q[9]!=p->shade[at])return 0;
    }
    return 1;
}
/* Specialize away the previous-picture branch for ordinary full output. */
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
static inline void pixels_render(const cc_planes *p,const cc_pixel_style *s,uint16_t *out,
                          const uint16_t *previous,cc_cell *state,int dx,int dy)
{
    const int w = p->cols * 2, h = p->rows * 4;
    const int relief = p->terrain_active && s->relief > 0 ? s->relief : 0;
    uint16_t solid[CC_INK_COUNT];
    for (unsigned k = 0; k < CC_INK_COUNT; k++) solid[k] = rgb565(s->rgb[k]);
    const uint16_t river = rgb565(s->river);
    for (int y = 0; y < h; y++) {
        uint16_t *row = out + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            if(previous && !(x&1)) {
                uint8_t *entry=(uint8_t*)&state[(size_t)(y/4)*p->cols+x/2];
                if(!(y&3))entry[11]=(uint8_t)unchanged(p,state,x/2,y/4,relief);
                if(entry[11]) {
                const uint16_t *in=previous+(size_t)(y+dy)*w+x+dx;
                row[x]=in[0];row[x+1]=in[1];x++;continue;
                }
            }
            const size_t i = (size_t)(y >> 2) * p->cols + (size_t)(x >> 1);
            const uint8_t bit = cc_dot_bit(x, y);
            if ((p->dots[i] & bit) && p->dot_priority[i] >= p->fill_priority[i]) {
                row[x] = p->ink[i] == CC_WATER ? river : solid[p->ink[i]];
                continue;
            }
            uint8_t ink = p->fill[i];
            if (p->edges_mode != CC_EDGES_CRISP && p->coverage_ink[i] && p->dot_priority[i] < p->coverage_priority[i])
                ink = (p->coverage[i] & bit) ? p->coverage_ink[i] : p->coverage_bg[i];
            if (relief && land_ink(ink)) {
                const int s16 = shade_at(p, x, y) * relief / 100;
                row[x] = s->shades && s16>=-128 && s16<=127?s->shades[ink][s16+128]:
                    s16 ? rgb565(shaded(s->rgb[ink], s16)) : solid[ink];
            } else row[x] = solid[ink];
        }
    }
}

void cc_pixels_rgb565_pan(const cc_planes *p,const cc_pixel_style *s,uint16_t *out,
                          const uint16_t *previous,cc_cell *state,int dx,int dy) {
    pixels_render(p,s,out,previous,state,dx,dy);
}
void cc_pixels_rgb565(const cc_planes *p,const cc_pixel_style *s,uint16_t *out) {
    pixels_render(p,s,out,NULL,NULL,0,0);
}
