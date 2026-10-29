/* FM's PICTURE page: SSTV, drawn in braille as it arrives.

   Every cell is two of the picture's own sixteen colours (picked from the
   rows received so far) and the braille dots say which of its eight pixels
   take the lighter one, so a 320 x 256 picture reads at a glance on the
   character grid without one block in it. The row being received is the
   scan line under the picture, walking down at the mode's own pace; the
   tuning scale under that is the audio tone right now against the four
   SSTV tones, so a mistuned USB signal shows as a pointer off its marks. */
#include "fm_sstv_view.h"

#include <stdio.h>
#include <string.h>

#include "../../ls_braille.h"
#include "../../ls_tui_ui.h"
#include "apps/fm/fm_bcast.h"
#include "esp_attr.h"
#include "ls_textpack.h"

static EXT_RAM_BSS_ATTR fm_sstv_status_t s_st;
static uint16_t s_pal[16];
static uint8_t s_rgb[16][3];
static uint32_t s_pal_key = 0xFFFFFFFFu;
static bool s_based;

int fm_sstv_opt_get(const ls_opt_t *o) { (void)o; return fm_bcast_opt(FM_BC_SSTV); }
void fm_sstv_opt_set(const ls_opt_t *o, int v) { (void)o; fm_bcast_set_opt(FM_BC_SSTV, v != 0); }

void fm_sstv_view_idle(void)
{
    if (!s_based) return;
    s_based = false;
    fm_sstv_palette(NULL, tui_rect_make(0, 0, 0, 0), NULL);
}

static inline void unpack(uint16_t p, int *r, int *g, int *b)
{
    *r = (p >> 8) & 0xF8; *g = (p >> 3) & 0xFC; *b = (p << 3) & 0xF8;
}

static int nearest(int r, int g, int b)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; ++i) {
        const int dr = r - s_rgb[i][0], dg = g - s_rgb[i][1], db = b - s_rgb[i][2];
        const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Sixteen colours for the rows in so far: a few rounds of k-means over a
   sample of them, started from evenly spaced pixels. */
static void palette(const uint16_t *pix, int w, int rows)
{
    enum { N = 512 };
    static EXT_RAM_BSS_ATTR uint8_t smp[N][3];
    const long total = (long)w * rows;
    if (total <= 0) return;
    for (int k = 0; k < N; ++k) {
        int r, g, b;
        unpack(pix[(long)k * total / N], &r, &g, &b);
        smp[k][0] = (uint8_t)r; smp[k][1] = (uint8_t)g; smp[k][2] = (uint8_t)b;
    }
    /* Farthest-point seeds, so a small bright thing (a sun, a callsign in
       yellow) keeps a colour of its own instead of averaging away. */
    memcpy(s_rgb[0], smp[N / 2], 3);
    for (int i = 1; i < 16; ++i) {
        int far = 0, far_d = -1;
        for (int k = 0; k < N; ++k) {
            int dmin = 1 << 30;
            for (int j = 0; j < i; ++j) {
                const int dr = smp[k][0] - s_rgb[j][0], dg = smp[k][1] - s_rgb[j][1], db = smp[k][2] - s_rgb[j][2];
                const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                if (d < dmin) dmin = d;
            }
            if (dmin > far_d) { far_d = dmin; far = k; }
        }
        memcpy(s_rgb[i], smp[far], 3);
    }
    for (int round = 0; round < 5; ++round) {
        long acc[16][4];
        memset(acc, 0, sizeof(acc));
        for (int k = 0; k < N; ++k) {
            const int c = nearest(smp[k][0], smp[k][1], smp[k][2]);
            acc[c][0] += smp[k][0]; acc[c][1] += smp[k][1]; acc[c][2] += smp[k][2]; acc[c][3]++;
        }
        for (int i = 0; i < 16; ++i)
            if (acc[i][3]) for (int c = 0; c < 3; ++c) s_rgb[i][c] = (uint8_t)(acc[i][c] / acc[i][3]);
    }
    for (int i = 0; i < 16; ++i)
        s_pal[i] = (uint16_t)(((s_rgb[i][0] & 0xF8) << 8) | ((s_rgb[i][1] & 0xFC) << 3) | (s_rgb[i][2] >> 3));
}

/* The picture into cells; returns how many cell rows hold received rows. */
static int draw_picture(tui_surface *sf, tui_rect a, tui_rect p, const uint16_t *pix,
                        int w, int h, int rows, int *scan_y, int *img_x, int *img_w)
{
    const int dw = p.w * 2, dh = p.h * 4;
    float s = (float)dw / (float)w;
    if ((float)dh / (float)h < s) s = (float)dh / (float)h;
    const int ow = (int)(w * s), oh = (int)(h * s);
    const int ox = (dw - ow) / 2;
    const int cells_w = (ow + 1) / 2, cells_h = (oh + 3) / 4;
    const int cx0 = p.x + ox / 2;
    *img_x = cx0;
    *img_w = cells_w;
    static const uint8_t BIT[4][2] = { {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80} };
    int done = 0;
    for (int cy = 0; cy < cells_h; ++cy) {
        const int first_row = (int)((cy * 4) / s);
        if (first_row >= rows) break;
        for (int cx = 0; cx < cells_w; ++cx) {
            int pr[8], pg[8], pb[8], lum[8], sum = 0;
            for (int d = 0; d < 8; ++d) {
                const int dx = cx * 2 + (d & 1), dy = cy * 4 + (d >> 1);
                const int ix = (int)(dx / s), iy = (int)(dy / s);
                if (ix >= w || iy >= rows || iy >= h) { pr[d] = pg[d] = pb[d] = 0; }
                else unpack(pix[(long)iy * w + ix], &pr[d], &pg[d], &pb[d]);
                lum[d] = 3 * pr[d] + 6 * pg[d] + pb[d];
                sum += lum[d];
            }
            const int mean = sum / 8;
            int hr = 0, hg = 0, hb = 0, hn = 0, lr = 0, lg = 0, lb = 0, ln = 0;
            unsigned bits = 0;
            for (int d = 0; d < 8; ++d) {
                if (lum[d] > mean) { hr += pr[d]; hg += pg[d]; hb += pb[d]; hn++; bits |= BIT[d >> 1][d & 1]; }
                else { lr += pr[d]; lg += pg[d]; lb += pb[d]; ln++; }
            }
            const int bg = ln ? nearest(lr / ln, lg / ln, lb / ln) : nearest(hr / hn, hg / hn, hb / hn);
            const int fg = hn ? nearest(hr / hn, hg / hn, hb / hn) : bg;
            if (fg == bg) bits = 0;
            ls_braille_put(sf, a, cx0 + cx, p.y + cy, (int16_t)(0x2800 | bits), TUI_ATTR(fg, bg));
        }
        done = cy + 1;
    }
    *scan_y = p.y + (int)(rows * s / 4.0f);
    if (*scan_y > p.y + cells_h) *scan_y = p.y + cells_h;
    return done;
}

/* 1000 to 2400 Hz, the four tones marked, the audio's tone the pointer. */
static void draw_scale(tui_surface *sf, tui_rect a, int x, int y, int w, float hz, bool live)
{
    if (w < 24) return;
    static const struct { float hz; const char *name; uint8_t hue; } MARKS[] = {
        { 1200, "SYNC", TUI_RED }, { 1500, "BLACK", TUI_WHITE }, { 1900, "VIS", TUI_YELLOW }, { 2300, "WHITE", TUI_WHITE },
    };
    for (int i = 0; i < w; ++i) tui_put_char(sf, a, x + i, y + 1, '-', LS_ATTR_FAINT);
    for (unsigned m = 0; m < 4; ++m) {
        const int mx = x + (int)((MARKS[m].hz - 1000.0f) * (float)(w - 1) / 1400.0f);
        tui_put_char(sf, a, mx, y + 1, '+', TUI_ATTR(MARKS[m].hue, TUI_BLACK));
        tui_put_str(sf, a, mx - (int)strlen(MARKS[m].name) / 2, y + 2, MARKS[m].name, TUI_ATTR(MARKS[m].hue, TUI_BLACK));
    }
    if (live && hz > 1000.0f && hz < 2400.0f) {
        const int px = x + (int)((hz - 1000.0f) * (float)(w - 1) / 1400.0f);
        tui_put_char(sf, a, px, y, 'v', TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        tui_put_char(sf, a, px, y + 1, '|', TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }
}

void fm_sstv_draw(tui_surface *sf, tui_rect a)
{
    fm_sstv_status(&s_st);
    int w = 0, h = 0;
    const uint16_t *pix = fm_sstv_image(&w, &h);
    char title[40], buf[96];
    snprintf(title, sizeof(title), "SSTV / %s", s_st.mode[0] ? s_st.mode : "PICTURES");
    ls_panel_box(sf, a, title, TUI_MAGENTA);
    if (s_st.receiving)
        tui_put_str(sf, a, a.x + a.w - 9, a.y, " RX (o) ", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    if (a.w < 24 || a.h < 10) return;
    const int x0 = a.x + 2, iw = a.w - 4, last = a.y + a.h - 2;
    const bool wide = iw >= 90;
    /* Picture left in landscape with the readouts beside it; on top in
       portrait. */
    const int info_w = wide ? 40 : iw;
    const int pic_w = wide ? iw - info_w - 2 : iw;
    const int pic_h = wide ? a.h - 3 : (a.h - 12 > 4 ? a.h - 12 : 4);
    tui_rect pic = tui_rect_make(x0, a.y + 1, pic_w, pic_h);
    int scan_y = pic.y;
    bool have = pix && w > 0 && h > 0 && s_st.rows > 0;
    /* The frame the picture will fill, as faint dots: what is still to come. */
    {
        const int fw = have ? w : 320, fh = have ? h : 256;
        float s = (float)(pic.w * 2) / (float)fw;
        if ((float)(pic.h * 4) / (float)fh < s) s = (float)(pic.h * 4) / (float)fh;
        const int cw = ((int)(fw * s) + 1) / 2, ch = ((int)(fh * s) + 3) / 4;
        const int cx = pic.x + (pic.w - cw) / 2;
        for (int yy = 0; yy < ch; ++yy)
            for (int xx = 0; xx < cw; ++xx)
                ls_braille_put(sf, a, cx + xx, pic.y + yy, (int16_t)(0x2800 | ((xx + yy) & 1 ? 0x40 : 0x08)),
                               LS_ATTR_FAINT);
        pic.h = ch;
    }
    if (have) {
        const uint32_t key = (s_st.images << 16) ^ (uint32_t)(s_st.rows / 16) ^ (uint32_t)w;
        if (key != s_pal_key) { s_pal_key = key; palette(pix, w, s_st.rows); }
        int img_x = pic.x, img_w = pic.w;
        const int done = draw_picture(sf, a, pic, pix, w, h, s_st.rows, &scan_y, &img_x, &img_w);
        fm_sstv_palette(sf, tui_rect_make(img_x, pic.y, img_w, done), s_pal);
        s_based = true;
        if (s_st.receiving && scan_y < pic.y + pic.h)
            for (int x = 0; x < img_w; ++x)
                tui_put_char(sf, a, img_x + x, scan_y, '=', TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    } else {
        fm_sstv_view_idle();
        static const char *const ART[] = {
            "  .---------------------.  ",
            "  |  ::  waiting for  ::  |  ",
            "  |  ::  a VIS header ::  |  ",
            "  '---------------------'  ",
        };
        const int ay = pic.y + pic.h / 2 - 2;
        for (int i = 0; i < 4; ++i)
            tui_put_str(sf, a, pic.x + (pic.w - 27) / 2, ay + i, ART[i], TUI_ATTR(TUI_MAGENTA, TUI_BLACK));
    }

    int y = wide ? a.y + 2 : pic.y + pic.h + 1;
    const int ix = wide ? x0 + pic_w + 2 : x0;
    /* The readouts keep to their own column. */
    a = tui_rect_intersect(a, tui_rect_make(ix, a.y, info_w, a.h));
    if (!s_st.on) {
        tui_put_str(sf, a, ix, y, LS_TEXT(t_fm_sstv_view_sstv_is_off_options, "SSTV is off: OPTIONS > SSTV PICTURES"), LS_ATTR_DIM);
        return;
    }
    if (y <= last) {
        if (s_st.receiving)
            snprintf(buf, sizeof(buf), "%s  row %d/%d", s_st.mode, s_st.rows, s_st.h);
        else if (s_st.images)
            LS_SNPRINTF(t_fm_sstv_view_lu_picture_s_last, buf, sizeof(buf), "%lu picture%s  last %s", (unsigned long)s_st.images,
                     s_st.images == 1 ? "" : "s", s_st.mode);
        else LS_SNPUTS(t_fm_sstv_view_listening_for_a_vis, buf, sizeof(buf), "listening for a VIS header");
        tui_put_str(sf, a, ix, y++, buf, TUI_ATTR(s_st.receiving ? TUI_GREEN | TUI_BRIGHT : TUI_WHITE, TUI_BLACK));
    }
    if (y <= last && (s_st.receiving || s_st.images)) {
        LS_SNPRINTF(t_fm_sstv_view_slant_0f_ppm_syncs, buf, sizeof(buf), "slant %+.0f ppm  syncs %d/%d", (double)s_st.slant_ppm,
                 s_st.syncs_found, s_st.syncs_found + s_st.syncs_missed);
        tui_put_str(sf, a, ix, y++, buf, LS_ATTR_DIM);
    }
    if (y <= last && s_st.saved[0]) {
        snprintf(buf, sizeof(buf), "%s %s", s_st.saving ? "writing" : "saved", s_st.saved);
        tui_put_str(sf, a, ix, y++, buf, TUI_ATTR(s_st.saving ? TUI_YELLOW : TUI_CYAN, TUI_BLACK));
    }
    y++;
    if (y + 2 <= last) draw_scale(sf, a, ix, y, info_w, s_st.tone_hz, s_st.level > 0.01f);
}
