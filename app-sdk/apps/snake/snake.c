/* SNAKE for LakeShark. The snake glides between tiles instead of jumping,
   looks where it is going and flicks its tongue; apples pulse, golden stars
   come and go, eating throws sparks and the score floats up. Steer with the
   big pad (it answers the moment a finger lands, anywhere in its quarter),
   a swipe anywhere, or the arrow keys. The best score lives in the app's
   own folder. Uses API 1.1: lap_touch, lap_swipe, lap_tone, lap_haptic. */
#include "lakeshark_app.h"
#include "lap_canvas.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLEN 400
#define T 8                             /* a tile, in dots */

typedef struct { int8_t x, y; } pt;
enum { RIGHT, DOWN, LEFT, UP };
static const int DX[4] = { 1, 0, -1, 0 };
static const int DY[4] = { 0, 1, 0, -1 };

static lap_canvas_t cv;
static pt body[MAXLEN], prev[MAXLEN];
static int len, dir, queued[2], nqueued, score, best, gw, gh, level;
static pt food, star;
static bool star_on, dead, playing, new_best;
static uint32_t last_step, step_ms, died_at, star_until, ate_at, last_ms, shake_until;
static bool dirty_best;

/* Layout in dots: the field and the pad. */
static int fx0, fy0, hud_h, pad_cx, pad_cy, pad_r;
static int pad_x0, pad_y0, pad_x1, pad_y1;
static int pressed = -1, finger_dir = -1;
static uint32_t pressed_ms;

typedef struct { float x, y, vx, vy; uint32_t until; uint8_t col; char text[6]; } spark_t;
#define SPARKS 40
static spark_t sparks[SPARKS];

/* ---- the best score ------------------------------------------------------- */

static void load_best(void)
{
    int fd = lap_file_open("best.txt", "r");
    if (fd < 0) return;
    char b[16] = { 0 };
    if (lap_file_read(fd, b, sizeof(b) - 1) > 0) best = atoi(b);
    lap_file_close(fd);
}
static void save_best(void)
{
    int fd = lap_file_open("best.txt", "w");
    if (fd < 0) return;
    char b[16];
    int n = snprintf(b, sizeof(b), "%d\n", best);
    lap_file_write(fd, b, (size_t)n);
    lap_file_close(fd);
    dirty_best = false;
}

/* ---- the game ------------------------------------------------------------- */

static bool on_snake(int x, int y, int from)
{
    for (int i = from; i < len; i++) if (body[i].x == x && body[i].y == y) return true;
    return false;
}
static pt free_tile(void)
{
    pt p = { 0, 0 };
    for (int tries = 0; tries < 500; tries++) {
        p.x = (int8_t)(lap_random() % (uint32_t)gw);
        p.y = (int8_t)(lap_random() % (uint32_t)gh);
        if (!on_snake(p.x, p.y, 0) && !(p.x == food.x && p.y == food.y)) break;
    }
    return p;
}

static void reset(void)
{
    len = 5; dir = RIGHT; nqueued = 0; score = 0; level = 1; dead = false; new_best = false;
    for (int i = 0; i < len; i++) { body[i].x = (int8_t)(gw / 2 - i); body[i].y = (int8_t)(gh / 2); prev[i] = body[i]; }
    step_ms = 150;
    last_step = lap_millis();
    star_on = false;
    food = free_tile();
}

static void spark(float x, float y, float vx, float vy, uint32_t life, uint8_t col, const char *text)
{
    for (int i = 0; i < SPARKS; i++)
        if (sparks[i].until <= lap_millis()) {
            sparks[i] = (spark_t){ x, y, vx, vy, lap_millis() + life, col, "" };
            if (text) snprintf(sparks[i].text, sizeof(sparks[i].text), "%s", text);
            return;
        }
}
static void burst(int tx, int ty, uint8_t col, int n)
{
    const float cx = fx0 + tx * T + T / 2.0f, cy = fy0 + ty * T + T / 2.0f;
    for (int i = 0; i < n; i++) {
        const float a = lc_frand() * 6.283f, sp = 20 + lc_frand() * 40;
        spark(cx, cy, cosf(a) * sp, sinf(a) * sp - 15, 350 + lap_random() % 300, col, NULL);
    }
}

static void steer(int d)
{
    if (!playing || dead) return;
    const int last = nqueued ? queued[nqueued - 1] : dir;
    if (d == last || (d + 2) % 4 == last) return;     /* no reversing into yourself */
    if (nqueued < 2) queued[nqueued++] = d;
}

static void die(void)
{
    dead = true;
    died_at = lap_millis();
    shake_until = died_at + 400;
    lap_tone(0, 1, 0, LAP_WAVE_SINE);
    lap_tone(392, 110, 50, LAP_WAVE_SQUARE);
    lap_tone(330, 110, 50, LAP_WAVE_SQUARE);
    lap_tone(262, 260, 50, LAP_WAVE_SQUARE);
    lap_vibrate(220, 80);
    if (score > best) { best = score; dirty_best = true; new_best = true; }
}

static void advance(void)
{
    if (nqueued) { dir = queued[0]; queued[0] = queued[1]; nqueued--; }
    const int nx = body[0].x + DX[dir], ny = body[0].y + DY[dir];
    const bool eat = nx == food.x && ny == food.y, gold = star_on && nx == star.x && ny == star.y;
    if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || on_snake(nx, ny, 0) ) {
        /* The tail moves out of the way this step unless we are growing. */
        if (!(nx >= 0 && ny >= 0 && nx < gw && ny < gh && body[len - 1].x == nx && body[len - 1].y == ny && !eat)) { die(); return; }
    }
    memcpy(prev, body, sizeof(pt) * (size_t)len);
    if ((eat || gold) && len < MAXLEN) { prev[len] = body[len - 1]; body[len] = body[len - 1]; len++; }
    memmove(body + 1, body, sizeof(pt) * (size_t)(len - 1));
    body[0].x = (int8_t)nx; body[0].y = (int8_t)ny;
    if (eat) {
        score += 10 * level;
        ate_at = lap_millis();
        char t[6];
        snprintf(t, sizeof(t), "+%d", 10 * level);
        spark(fx0 + nx * T, fy0 + ny * T, 0, -22, 900, LC_BR_YELLOW, t);
        burst(nx, ny, LC_BR_RED, 10);
        food = free_tile();
        lap_tone(0, 1, 0, LAP_WAVE_SINE);
        lap_tone(880, 35, 40, LAP_WAVE_SQUARE);
        lap_tone(1320, 50, 40, LAP_WAVE_SQUARE);
        lap_haptic(LAP_HAPTIC_TICK);
        level = 1 + score / 120;
        step_ms = level >= 9 ? 70 : 150 - (uint32_t)level * 9;
        if (!star_on && (lap_random() % 4) == 0) { star = free_tile(); star_on = true; star_until = lap_millis() + 6000; }
    }
    if (gold) {
        score += 50;
        star_on = false;
        spark(fx0 + nx * T, fy0 + ny * T, 0, -22, 1100, LC_BR_YELLOW, "+50");
        burst(nx, ny, LC_BR_YELLOW, 18);
        lap_tone(0, 1, 0, LAP_WAVE_SINE);
        for (int i = 0; i < 4; i++) lap_tone(1047 + i * 262, 45, 40, LAP_WAVE_TRIANGLE);
        lap_haptic(LAP_HAPTIC_CONFIRM);
    }
}

/* ---- the picture -------------------------------------------------------- */

static void layout(void)
{
    if (!lap_canvas_init(&cv, lap_cols(), lap_rows())) return;
    hud_h = 18;
    const bool wide = cv.w > cv.h;
    int field_w, field_h;
    if (wide) {
        /* Landscape: the pad on the right. */
        const int pad_w = cv.h - hud_h < cv.w * 2 / 5 ? cv.h - hud_h : cv.w * 2 / 5;
        field_w = cv.w - pad_w - 4; field_h = cv.h - hud_h - 2;
        pad_x0 = cv.w - pad_w; pad_x1 = cv.w; pad_y0 = hud_h; pad_y1 = cv.h;
    } else {
        const int pad_h = cv.h * 34 / 100;
        field_w = cv.w - 4; field_h = cv.h - hud_h - pad_h - 4;
        pad_x0 = 0; pad_x1 = cv.w; pad_y0 = cv.h - pad_h; pad_y1 = cv.h - 6;   /* clear of the round corners */
    }
    gw = field_w / T; gh = field_h / T;
    if (gw > 60) gw = 60;
    if (gh > 60) gh = 60;
    fx0 = wide ? 2 : (cv.w - gw * T) / 2;
    fy0 = hud_h + (field_h - gh * T) / 2 + 1;
    pad_cx = (pad_x0 + pad_x1) / 2; pad_cy = (pad_y0 + pad_y1) / 2;
    const int pw = pad_x1 - pad_x0, ph = pad_y1 - pad_y0;
    pad_r = (pw < ph ? pw : ph) / 2 - 3;
}

/* Which quarter of the pad (x, y in dots) is in: the four triangles the
   diagonals cut, so every point of the pad steers somewhere. */
static int pad_dir(int x, int y)
{
    if (x < pad_x0 || x >= pad_x1 || y < pad_y0 || y >= pad_y1) return -1;
    const int dx = x - pad_cx, dy = (y - pad_cy);
    if (abs(dx) > abs(dy)) return dx > 0 ? RIGHT : LEFT;
    return dy > 0 ? DOWN : UP;
}

static void draw_field(float frac)
{
    /* The ground: soft checks, darker at the edges. */
    lc_fill_rect(&cv, fx0 - 2, fy0 - 2, gw * T + 4, gh * T + 4, LC_GREY);
    for (int ty = 0; ty < gh; ty++)
        for (int tx = 0; tx < gw; tx++) {
            const bool odd = (tx + ty) & 1;
            for (int y = 0; y < T; y++)
                for (int x = 0; x < T; x++)
                    lc_pset(&cv, fx0 + tx * T + x, fy0 + ty * T + y, (x == 0 && y == 0) ? (odd ? LC_BLUE : LC_GREY) : LC_BLACK);
        }
    const uint32_t t = lap_millis();
    /* The apple: it breathes. */
    {
        const int cx = fx0 + food.x * T + T / 2, cy = fy0 + food.y * T + T / 2 + 1;
        const int r = T / 2 - 1 + ((t / 220) % 2);
        static const uint8_t APPLE[4] = { LC_RED, LC_RED, LC_BR_RED, LC_BR_WHITE };
        lc_ball(&cv, cx, cy, r, APPLE);
        lc_pset(&cv, cx, cy - r - 1, LC_YELLOW);
        lc_pset(&cv, cx + 1, cy - r - 2, LC_BR_GREEN); lc_pset(&cv, cx + 2, cy - r - 2, LC_GREEN);
    }
    /* The star: it spins and blinks out when its time is nearly up. */
    if (star_on && (star_until - t > 1500 || (t / 120) % 2)) {
        const int cx = fx0 + star.x * T + T / 2, cy = fy0 + star.y * T + T / 2;
        const float a0 = t / 300.0f;
        for (int k = 0; k < 5; k++) {
            const float a = a0 + k * 1.2566f;
            lc_line(&cv, cx, cy, cx + (int)roundf(cosf(a) * (T / 2 + 1)), cy + (int)roundf(sinf(a) * (T / 2 + 1)), LC_BR_YELLOW);
        }
        lc_fill_circle(&cv, cx, cy, 1, LC_BR_WHITE);
    }
    /* The snake, each segment slid between where it was and where it is. */
    const int fall = dead ? (int)((t - died_at) / 45) : -1;
    for (int i = len - 1; i >= 0; i--) {
        if (dead && i < fall) continue;                     /* falling apart from the head back */
        const float sx = prev[i].x + (body[i].x - prev[i].x) * frac, sy = prev[i].y + (body[i].y - prev[i].y) * frac;
        const int cx = fx0 + (int)roundf(sx * T) + T / 2, cy = fy0 + (int)roundf(sy * T) + T / 2;
        const bool head = i == 0;
        uint8_t ramp[4] = { LC_BLACK, LC_GREEN, LC_BR_GREEN, LC_BR_WHITE };
        if ((i / 2) % 2 && !head) { ramp[1] = LC_GREEN; ramp[2] = LC_GREEN; }      /* stripes */
        if (dead && (t / 90) % 2) { ramp[1] = LC_GREY; ramp[2] = LC_WHITE; }
        /* Fill toward the next segment so the body is one tube. */
        if (i + 1 < len && !(dead && i + 1 < fall)) {
            const float nx = prev[i + 1].x + (body[i + 1].x - prev[i + 1].x) * frac, ny = prev[i + 1].y + (body[i + 1].y - prev[i + 1].y) * frac;
            const int mx = fx0 + (int)roundf((sx + nx) / 2 * T) + T / 2, my = fy0 + (int)roundf((sy + ny) / 2 * T) + T / 2;
            lc_ball(&cv, mx, my, T / 2 - 1, ramp);
        }
        const int r = head ? T / 2 : i > len - 4 ? T / 2 - 1 - (i - (len - 4)) / 2 : T / 2 - 1;
        lc_ball(&cv, cx, cy, r > 1 ? r : 1, ramp);
        if (head && !dead) {
            /* Eyes ahead, and the tongue now and then. */
            const int ax = DX[dir], ay = DY[dir], px = -ay, py = ax;
            for (int e = -1; e <= 1; e += 2) {
                const int ex = cx + ax + px * e * 2, ey = cy + ay + py * e * 2;
                lc_pset(&cv, ex, ey, LC_BR_WHITE);
                lc_pset(&cv, ex + ax, ey + ay, LC_BLACK);
            }
            if ((t / 140) % 9 < 2) {
                lc_pset(&cv, cx + ax * (r + 1), cy + ay * (r + 1), LC_BR_RED);
                lc_pset(&cv, cx + ax * (r + 2) + px, cy + ay * (r + 2) + py, LC_BR_RED);
                lc_pset(&cv, cx + ax * (r + 2) - px, cy + ay * (r + 2) - py, LC_BR_RED);
            }
        }
    }
}

static void draw_sparks(float dt)
{
    const uint32_t t = lap_millis();
    for (int i = 0; i < SPARKS; i++) {
        spark_t *s = &sparks[i];
        if (s->until <= t) continue;
        s->x += s->vx * dt; s->y += s->vy * dt;
        if (!s->text[0]) s->vy += 60 * dt;
        if (s->text[0]) lc_text_outlined(&cv, (int)s->x - lc_text_w(s->text, 1) / 2, (int)s->y, s->text, 1, s->col, LC_BLACK);
        else lc_pset(&cv, (int)s->x, (int)s->y, (t / 60 + i) % 3 ? s->col : LC_BR_WHITE);
    }
}

static void draw_pad(void)
{
    const uint32_t t = lap_millis();
    lc_fill_rect(&cv, pad_x0, pad_y0, pad_x1 - pad_x0, pad_y1 - pad_y0, LC_BLACK);
    const int r = pad_r;
    /* A big disc with a cross cut in; the pressed quarter lights up. */
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            const int d2 = x * x + y * y;
            if (d2 > r * r) continue;
            const int q = abs(x) > abs(y) ? (x > 0 ? RIGHT : LEFT) : (y > 0 ? DOWN : UP);
            const bool lit = pressed == q && t - pressed_ms < 160;
            const bool groove = abs(abs(x) - abs(y)) <= 1 || d2 < (r / 4) * (r / 4);
            uint8_t c = groove ? LC_BLACK : lit ? LC_BR_GREEN : d2 > (r - 2) * (r - 2) ? LC_GREY : lc_mix(LC_GREY, LC_WHITE, 0.25f - (float)y / r * 0.2f, x, y);
            lc_pset(&cv, pad_cx + x, pad_cy + y, c);
        }
    /* Arrows. */
    const int a = r * 2 / 3, s = r / 5 > 2 ? r / 5 : 2;
    for (int k = 0; k <= s; k++) {
        lc_fill_rect(&cv, pad_cx - k, pad_cy - a - s + k, 2 * k + 1, 1, LC_BR_WHITE);   /* up */
        lc_fill_rect(&cv, pad_cx - k, pad_cy + a + s - k, 2 * k + 1, 1, LC_BR_WHITE);   /* down */
        lc_fill_rect(&cv, pad_cx - a - s + k, pad_cy - k, 1, 2 * k + 1, LC_BR_WHITE);   /* left */
        lc_fill_rect(&cv, pad_cx + a + s - k, pad_cy - k, 1, 2 * k + 1, LC_BR_WHITE);   /* right */
    }
}

static void draw_hud(void)
{
    lc_fill_rect(&cv, 0, 0, cv.w, hud_h, LC_BLACK);
    char t[24];
    snprintf(t, sizeof(t), "%d", score);
    const bool pop = lap_millis() - ate_at < 150;
    lc_text(&cv, 2, 3 - (pop ? 1 : 0), t, 2, pop ? LC_BR_YELLOW : LC_BR_WHITE);
    snprintf(t, sizeof(t), "BEST %d", best);
    lc_text(&cv, cv.w - 14 - lc_text_w(t, 1), 3, t, 1, LC_BR_CYAN);     /* clear of the close button */
    snprintf(t, sizeof(t), "LEVEL %d", level);
    lc_text(&cv, cv.w - 14 - lc_text_w(t, 1), 10, t, 1, LC_GREEN);
}

static void banner(const char *big, const char *small, const char *small2, uint8_t col)
{
    int k = 4;
    while (k > 1 && lc_text_w(big, k) + 12 > cv.w - 6) k--;
    const int w = lc_text_w(big, k) + 12, h = 5 * k + 26, x = (cv.w - w) / 2;
    const int y = fy0 + (gh * T - h) / 2;
    lc_fill_rect(&cv, x, y, w, h, LC_BLACK);
    lc_rect(&cv, x, y, w, h, col);
    lc_rect(&cv, x + 1, y + 1, w - 2, h - 2, LC_GREY);
    lc_text(&cv, x + 6, y + 5, big, k, col);
    if (small) lc_text(&cv, (cv.w - lc_text_w(small, 1)) / 2, y + 5 * k + 9, small, 1, LC_BR_WHITE);
    if (small2) lc_text(&cv, (cv.w - lc_text_w(small2, 1)) / 2, y + 5 * k + 17, small2, 1, (lap_millis() / 400) % 2 ? LC_BR_WHITE : LC_GREY);
}

/* ---- the app -------------------------------------------------------------- */

static void enter(void)
{
    lap_fullscreen(true);
    lap_keep_awake(true);
    best = 0;
    load_best();
    gw = 0;
    playing = false;
    last_ms = lap_millis();
}

static void leave(void)
{
    if (dirty_best) save_best();
    lap_canvas_free(&cv);
}

static void start(void)
{
    reset();
    playing = true;
    lap_tone(0, 1, 0, LAP_WAVE_SINE);
    lap_tone(523, 60, 40, LAP_WAVE_SQUARE);
    lap_tone(784, 90, 40, LAP_WAVE_SQUARE);
}

/* The title's demo snake: it chases its tail around the field. */
static void demo(void)
{
    const uint32_t t = lap_millis();
    if (t - last_step < 110) return;
    last_step = t;
    const int nx = body[0].x + DX[dir], ny = body[0].y + DY[dir];
    if (nx < 1 || ny < 1 || nx >= gw - 1 || ny >= gh - 1) dir = (dir + 1) % 4;
    memcpy(prev, body, sizeof(pt) * (size_t)len);
    memmove(body + 1, body, sizeof(pt) * (size_t)(len - 1));
    body[0].x = (int8_t)(body[0].x + DX[dir]); body[0].y = (int8_t)(body[0].y + DY[dir]);
}

static void draw(void)
{
    if (!cv.d || cv.cw != lap_cols() || cv.ch != lap_rows()) {
        layout();
        if (!cv.d) return;
        reset();
        len = 10;
        for (int i = 0; i < len; i++) { body[i].x = (int8_t)(gw / 2 - i); body[i].y = 1; prev[i] = body[i]; }
    }
    const uint32_t t = lap_millis();
    float dt = (t - last_ms) / 1000.0f;
    last_ms = t;
    if (dt > 0.1f) dt = 0.1f;

    /* The finger: pressing a quarter of the pad steers at once; dragging
       across the field steers the way it goes. */
    lap_touch_t tc;
    int cw = 10, ch = 17;
    lap_cell_px(&cw, &ch);
    if (lap_touch(&tc) && tc.down) {
        const int x = tc.x * 2 / cw, y = tc.y * 3 / ch;
        int d = pad_dir(x, y);
        if (d < 0) {
            const int dx = tc.x - tc.x0, dy = tc.y - tc.y0;
            if (abs(dx) > cw * 2 || abs(dy) > ch) d = abs(dx) * ch > abs(dy) * cw ? (dx > 0 ? RIGHT : LEFT) : (dy > 0 ? DOWN : UP);
        }
        if (d >= 0 && d != finger_dir) {
            finger_dir = d;
            if (pad_dir(x, y) >= 0) { pressed = d; pressed_ms = t; lap_haptic(LAP_HAPTIC_TICK); }
            steer(d);
        }
    } else finger_dir = -1;
    const int sw = lap_swipe();
    if (sw) steer(sw == LAP_SWIPE_UP ? UP : sw == LAP_SWIPE_DOWN ? DOWN : sw == LAP_SWIPE_LEFT ? LEFT : RIGHT);

    float frac = 1;
    if (playing && !dead) {
        while (t - last_step >= step_ms) { last_step += step_ms; advance(); if (dead) break; }
        frac = dead ? 1 : (float)(t - last_step) / step_ms;
    } else if (!playing) {
        demo();
        frac = (float)(t - last_step) / 110;
    }
    if (star_on && t > star_until) star_on = false;
    if (frac > 1) frac = 1;

    lc_clear(&cv, LC_BLACK);
    draw_field(frac);
    draw_sparks(dt);
    if (!playing) banner("SNAKE", "TAP THE PAD TO PLAY", best ? "BEST SCORE" : "SWIPE OR TAP TO STEER", LC_BR_GREEN);
    else if (dead && t - died_at > 700) {
        char s[32];
        snprintf(s, sizeof(s), new_best ? "NEW BEST: %d" : "SCORE %d", score);
        banner("GAME OVER", s, "TAP TO PLAY AGAIN", new_best ? LC_BR_YELLOW : LC_BR_RED);
    }
    draw_hud();
    draw_pad();

    /* A shake on a crash: the whole field jolts. */
    int ox = 0, oy = 0;
    if (t < shake_until) { ox = (int)(lap_random() % 3) - 1; oy = (int)(lap_random() % 3) - 1; }
    lap_canvas_show(&cv, ox, oy);
}

static bool touch(int col, int row)
{
    const uint32_t t = lap_millis();
    if (!playing || (dead && t - died_at > 700)) { start(); return true; }
    (void)col; (void)row;
    return true;                         /* steering is done on the way down, in draw */
}

static bool key(lap_key_t k, char ch)
{
    if (!playing || dead) {
        if (k == LAP_KEY_ENTER || (k == LAP_KEY_CHAR && ch == ' ')) { if (!dead || lap_millis() - died_at > 700) start(); return true; }
    }
    switch (k) {
    case LAP_KEY_UP: steer(UP); return true;
    case LAP_KEY_DOWN: steer(DOWN); return true;
    case LAP_KEY_LEFT: steer(LEFT); return true;
    case LAP_KEY_RIGHT: steer(RIGHT); return true;
    case LAP_KEY_CHAR:
        if (ch == 'w') { steer(UP); return true; }
        if (ch == 's') { steer(DOWN); return true; }
        if (ch == 'a') { steer(LEFT); return true; }
        if (ch == 'd') { steer(RIGHT); return true; }
        return false;
    default: return false;
    }
}

/* The tile icon, in colour dots. */
LAP_ICON_PX(
    "....................",
    "....................",
    "....................",
    "aaaa2222aaaaaa0a...9",
    "aaaa2222aaaaaafa999.",
    "aaaa2222aaaaaaaa...9",
    "2222................",
    "2222................",
    "2222................",
    "aaaa2222aaaa........",
    "aaaa2222aaaa.......a",
    "aaaa2222aaaa......3.",
    "........2222.....f9.",
    "........2222....9999",
    "........2222....9999",
    "aaaa2222aaaa....1111",
    "aaaa2222aaaa.....11.",
    "aaaa2222aaaa........");

LAP_APP(.id = "snake", .name = "SNAKE", .sub = "eat & grow", .version = "2.0",
        .author = "LakeShark", .color = LAP_GREEN,
        .icon_art = { "  ____    ", " / o  \\_  ", " \\___  \\  ", "  ___)  ) ", " (_____/ *", "  SNAKE   " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
