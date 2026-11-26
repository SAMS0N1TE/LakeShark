#include "menu_art.h"
#include "display.h"
#include "ls_font.h"
#include "settings.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum { COLS = 56, ROWS = 26, CELL_W = 4, CELL_H = 6, TOP = 37 };
enum { WATER, GRID, TRAIL, LOW, MID, HIGH, FOAM, PING };
typedef struct { char glyph; uint8_t ink; } cell;
typedef struct { int x, y; } point;

static cell art[ROWS][COLS];
static uint8_t glyphs[95][CELL_H];
static const int8_t sine[] = {
     0,  6, 12, 18, 23, 27, 30, 31, 32, 31, 30, 27, 23, 18, 12,  6,
     0, -6,-12,-18,-23,-27,-30,-31,-32,-31,-30,-27,-23,-18,-12, -6,
};

static void dot(int x, int y, char glyph, uint8_t ink)
{
    if ((unsigned)x < COLS && (unsigned)y < ROWS) art[y][x] = (cell){ glyph, ink };
}

static void stamp(int x, int y, const char *text, uint8_t ink)
{
    for (; *text; ++text, ++x)
        if (*text != ' ') dot(x, y, *text, ink);
}

static void line(int x0, int y0, int x1, int y1, char glyph, uint8_t ink)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        dot(x0, y0, glyph, ink);
        if (x0 == x1 && y0 == y1) break;
        int next = 2 * error;
        if (next >= dy) { error += dy; x0 += sx; }
        if (next <= dx) { error += dx; y0 += sy; }
    }
}

static void water(unsigned phase)
{
    int sweep_x = sine[(phase + 8) & 31], sweep_y = sine[phase & 31];
    for (int y = 0; y < ROWS; ++y) {
        for (int x = 0; x < COLS; ++x) {
            unsigned grain = (unsigned)(x + 1) * 2654435761u ^ (unsigned)(y + 3) * 40507u;
            grain ^= grain >> 13;
            int px = x - 29, py = (y - 12) * 3 / 2;
            int depth = px * px + py * py;
            int along = px * sweep_x + py * sweep_y;
            int cross = px * sweep_y - py * sweep_x;
            bool scan = along > 0 && cross >= -38 && cross < along / 4 + 21;
            bool ring = abs(depth - 100) < 11 || abs(depth - 324) < 18 ||
                        abs(depth - 625) < 26;
            int floor = 23 + sine[(x / 2 + 8) & 31] / 13;
            if (y >= floor) {
                char sand = ".:;,/."[grain % 6];
                dot(x, y, sand, y == floor ? LOW : WATER);
            } else if (ring && ((x + y) & 1) == 0) {
                dot(x, y, scan ? '+' : '.', scan ? HIGH : GRID);
            } else if (scan && depth < 625 && (grain & 7) < 3) {
                dot(x, y, cross < 26 ? ':' : '.', cross < 26 ? MID : TRAIL);
            } else if ((grain & 31) == 3 || (x % 11 == 7 && y % 5 == 2)) {
                dot(x, y, x % 11 == 7 ? '+' : '.', WATER);
            }
        }
    }

    for (int i = 0; i < 11; ++i) {
        int x = (i * 17 + 3 + phase / 3) % COLS;
        int y = (i * 7 + 21 - (int)(phase / 2 % 24)) % 24;
        if (y < 0) y += 24;
        dot(x, y, i % 4 ? '.' : 'o', i % 4 ? GRID : LOW);
    }
}

static bool contains(const point *vertices, int count, int x, int y)
{
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        const point a = vertices[j], b = vertices[i];
        if ((a.y > y) == (b.y > y)) continue;
        int cross = (b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y);
        if ((cross > 0) == (b.y > a.y)) inside = !inside;
    }
    return inside;
}

static void skin(const point *shape, int count, int bob, unsigned phase, bool flank)
{
    for (int y = 0; y < ROWS; ++y) {
        for (int x = 0; x < COLS; ++x) {
            int sy = y - bob;
            if (!contains(shape, count, x, sy)) continue;
            bool rim = !contains(shape, count, x, sy - 1) || !contains(shape, count, x + 1, sy);
            int light = sine[(x + 2 * sy - phase) & 31];
            unsigned weave = (unsigned)(x * 17 + sy * 11) & 7;
            int belly = 13 + (x > 18 && x < 46);
            if (rim) dot(x, y, sy < belly ? '/' : '_', sy < belly ? HIGH : MID);
            else if (flank && sy >= belly) dot(x, y, ".:-=+*##"[weave], sy == belly ? FOAM : HIGH);
            else dot(x, y, ".:;-=+**"[weave], light > 13 ? HIGH : light > -8 ? MID : LOW);
        }
    }
}

#define SKIN(vertices, lift, phase, flank) skin(vertices, sizeof(vertices) / sizeof(*vertices), lift, phase, flank)

static void shark(unsigned phase, bool salvage)
{
    const point dorsal[] = {{18,12},{24,9},{30,3},{31,2},{31,7},{36,10}};
    const point flank[] = {{11,14},{18,12},{25,10},{34,8},{42,8},{47,9},{51,11},{53,13},
                          {50,15},{45,16},{38,17},{34,18},{25,18},{17,16},{11,16}};
    const point pectoral[] = {{33,14},{42,15},{39,18},{35,21},{30,24},{31,20}};
    int lift = sine[(phase / 2) & 31] / 22;
    int beat = sine[(phase * 2) & 31] / 15;
    const point tail[] = {{15,14},{9,11},{3,5+beat},{4,10+beat},{7,14},
                          {4,18-beat},{2,21-beat},{7,21-beat},{13,16}};
    SKIN(tail, lift, phase, false);
    SKIN(dorsal, lift, phase, false);
    SKIN(flank, lift, phase, true);
    SKIN(pectoral, lift, phase, false);
    line(13, 15 + lift, 27, 14 + lift, '.', MID);
    stamp(40, 10 + lift, ".-o-.", FOAM);
    stamp(38, 12 + lift, ")))", LOW);
    stamp(38, 13 + lift, ")))", LOW);
    stamp(45, 14 + lift, "/vv--", LOW);
    stamp(49, 12 + lift, ".:", HIGH);
    if (salvage) {
        int y = 19 + sine[phase & 31] / 22;
        stamp(13, y, "[<>]", PING);
        stamp(13, y + 1, "====", MID);
    } else {
        stamp(15, 7, "o", PING);
        stamp(20, 5, "o", PING);
        stamp(25, 4, "+", PING);
    }
}

static void signal(unsigned phase, bool cartridge)
{
    const int scroll = phase % 25;
    for (int x = 5; x < 52; ++x) {
        int amp = abs(x - 26) / 3 + 2;
        int y = 13 + (sine[(x * 3 + phase * 2) & 31] * amp) / 32;
        dot(x, y, (x + scroll) % 7 == 0 ? '+' : '.', x % 3 == 0 ? HIGH : LOW);
        if ((x & 1) == 0) line(x, 23, x, 23 - abs(sine[(x + phase) & 31]) / 8, ':', TRAIL);
    }
    if (!cartridge) {
        line(7, 4, 7, 19, '|', MID);
        line(50, 4, 50, 19, '|', MID);
        int step = phase % 54;
        stamp(8 + (step < 27 ? step : 54 - step) * 40 / 27,
              10 + sine[(phase / 2) & 31] / 7, "<*>", PING);
        stamp(9, 8 + sine[(phase * 2) & 31] / 10, "][", FOAM);
        stamp(46, 12, "[]", HIGH);
        return;
    }
    stamp(15, 5, ".----[ ROM ]--------.", HIGH);
    for (int y = 6; y < 19; ++y) {
        for (int x = 16; x <= 34; ++x) dot(x, y, ' ', WATER);
        dot(15, y, '|', FOAM);
        dot(35, y, '|', MID);
    }
    stamp(18, 8, ".------------.", MID);
    stamp(18, 9, "| GBA    [ ] |", HIGH);
    stamp(18, 10, "|------------|", MID);
    stamp(18, 11, "| ::  32-BIT |", FOAM);
    stamp(18, 12, "'------------'", HIGH);
    stamp(17, 16, "|:|:|:|:|:|:|:|:|", PING);
    stamp(15, 19, "'==================='", HIGH);
    int scan = 17 + (int)(phase % 18);
    dot(scan, 17, '+', FOAM);
}

static void passage(unsigned phase)
{
    for (int i = 0; i < 6; ++i) {
        int n = (i * 13 + phase) % 78;
        int w = 3 + n * n / 210, h = 2 + n * n / 520;
        int x = 28 - w, y = 12 - h;
        uint8_t color = i & 1 ? HIGH : LOW;
        line(x, y + 2, x + 4, y, '/', color);
        line(x + 4, y, x + 2 * w - 4, y, '-', color);
        line(x + 2 * w - 4, y, x + 2 * w, y + 2, '\\', color);
        line(x, y + 2, x, 12 + h, '|', color);
        line(x + 2 * w, y + 2, x + 2 * w, 12 + h, '|', color);
        line(x, 12 + h, x + 2 * w, 12 + h, '.', color);
    }
    for (int x = 2; x <= 54; x += 7) line(28, 13, x, 25, '.', TRAIL);
    stamp(27, 11, "-+-", PING);
    dot(28, 10, '|', HIGH);
    dot(28, 12, '|', HIGH);
}

static void controller(unsigned phase, uint8_t held)
{
    stamp(1, 3, ".-------------------------------.", HIGH);
    stamp(0, 4, "/  *     TK21 / CONTROLLER    *   \\", MID);
    stamp(0, 5, "|   .-----.          .-------.    |", FOAM);
    stamp(0, 6, "|  /   |   \\         |  (Y)  |    |", HIGH);
    stamp(0, 7, "|  | --+-- |         |(X) (B)|    |", HIGH);
    stamp(0, 8, "|  \\   |   /         |  (A)  |    |", HIGH);
    stamp(0, 9, "|   '-----'          |       |    |", HIGH);
    stamp(0,10, "|   [ KEY ]          '-------'    |", MID);
    stamp(0,11, "|   (MENU)  (ALT)                |", MID);
    stamp(0,12, "\\__       ______________       __/", FOAM);
    stamp(2,13, "\\_____/'              '\\_____/", MID);
    stamp(7,16, "X   Y  BTN  KEY", MID);
    stamp(7,17, "|   |   |    |", HIGH);
    stamp(7,18, ":   :   :    :", LOW);
    for (int i = 0; i < 4; ++i) {
        int y = 19 + i;
        int x = 6 + (int)((phase + i * 4) % 18);
        stamp(x, y, i & 1 ? ".:" : "..+", i & 1 ? TRAIL : MID);
    }
    static const point pads[] = {{6,11},{13,11},{25,8},{23,7},{25,6},{27,7},{6,10}};
    for (int i = 0; i < 7; ++i)
        if (held & (1u << i)) dot(pads[i].x, pads[i].y, '*', PING);
    dot(7, 7, '+', phase & 4 ? PING : FOAM);
}

static uint16_t dim(uint16_t color, unsigned gain)
{
    unsigned r = ((color >> 11) & 31) * gain / 16;
    unsigned g = ((color >> 5) & 63) * gain / 16;
    unsigned b = (color & 31) * gain / 16;
    return (r << 11) | (g << 5) | b;
}

/* Sample the same LakeShark face into a denser grid for illustration. */
static void build_glyphs(void)
{
    const ls_font_t *font = &ls_font_mono_13;
    for (unsigned i = 1; i < sizeof(glyphs) / sizeof(*glyphs); ++i) {
        const ls_font_glyph_t *g = ls_font_glyph(font, i + ' ');
        if (!g) continue;
        const uint8_t *bitmap = font->bitmap + g->bitmap_index;
        int top = font->cell_h - font->base_line - g->box_h - g->ofs_y;
        for (int y = 0; y < g->box_h; ++y) {
            for (int x = 0; x < g->box_w; ++x) {
                int k = y * g->box_w + x;
                unsigned coverage = k & 1 ? bitmap[k / 2] & 15 : bitmap[k / 2] >> 4;
                int gx = (x + g->ofs_x) * CELL_W / font->cell_w;
                int gy = (y + top) * CELL_H / font->cell_h;
                if (coverage >= 7 && (unsigned)gx < CELL_W && (unsigned)gy < CELL_H)
                    glyphs[i][gy] |= 1u << gx;
            }
        }
    }
}

void menu_art_draw(uint16_t *frame, int left, menu_art_kind kind, const uint16_t *p,
                   unsigned theme, int64_t now, uint8_t held)
{
    static bool ready;
    static unsigned last_phase;
    static menu_art_kind last_kind;
    static uint8_t last_held;
    unsigned phase = (unsigned)(now / 83000);
    if (!ready) build_glyphs();
    if (!ready || kind != last_kind || phase != last_phase || held != last_held) {
        memset(art, 0, sizeof(art));
        if (kind != ART_TK21) water(phase);
        switch (kind) {
        case ART_RELAY: case ART_SALVAGE: shark(phase, kind == ART_SALVAGE); break;
        case ART_SIGNAL: case ART_CARTRIDGE: signal(phase, kind == ART_CARTRIDGE); break;
        case ART_BSP: passage(phase); break;
        case ART_TK21: controller(phase, held); break;
        }
        ready = true;
        last_kind = kind;
        last_phase = phase;
        last_held = held;
    }
    uint16_t accent = p[theme == THEME_AMBER ? 14 : theme == THEME_GREEN ? 10 : 14];
    uint16_t colors[] = {dim(accent, 4), dim(accent, 6), dim(accent, 8), p[6],
                         dim(accent, 12), accent, p[15], p[11]};
    for (int y = 0; y < ROWS; ++y) {
        for (int x = 0; x < COLS && left + (x + 1) * CELL_W <= GAME_WIDTH; ++x) {
            const cell c = art[y][x];
            if (c.glyph <= ' ') continue;
            const uint8_t *rows = glyphs[(unsigned)c.glyph - ' '];
            uint16_t *out = frame + (TOP + y * CELL_H) * GAME_WIDTH + left + x * CELL_W;
            for (int dy = 0; dy < CELL_H; ++dy) {
                unsigned bits = rows[dy];
                for (int dx = 0; bits; ++dx, bits >>= 1)
                    if (bits & 1) out[dx] = colors[c.ink];
                out += GAME_WIDTH;
            }
        }
    }
}
