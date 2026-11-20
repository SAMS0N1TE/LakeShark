/* PINBALL 2.0: two physical tables rendered by firmware lap3d (API 1.2).
   Table-space collision geometry is shared with the raised 3D parts.
   Hold either side to flip; hold right and release to shoot. Tilt and
   shake still use the board motion sensor. */
#define LAP_TARGET_API_1   /* API 1.3: runs on 2.19 firmware and later */
#include "lakeshark_app.h"
#include "lap_canvas.h"
#include "lap3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
/* Host timing hooks (bench/tools/pinball_perf.c); nothing on the board. */
#ifndef PB_MARK
#define PB_MARK(stage) ((void)0)
#endif

/* The table in its own units: 100 wide, 182 tall, y down. */
#define TW 100.0f
#define TH 182.0f
#define BALL_R 3.0f
#define GRAV 165.0f                     /* down the table, units/s^2 */
#define FL_LEN 15.0f
#define FL_REST 0.50f                   /* radians below level */
#define FL_UP (-0.48f)
#define LANE_U 92.0f
#define LANE_V 173.0f                   /* the ball at rest on the plunger */

typedef struct { float x, y; } v2;

/* ---- the table ---------------------------------------------------------- */

enum { K_WALL, K_RUBBER, K_KICK, K_GATE, K_DROP, K_MESH, K_SPINNER };
typedef struct { float ax, ay, bx, by; uint8_t kind, idx; } seg_t;
#define MAXSEG 48
static seg_t segs[MAXSEG];
static int nseg;

typedef struct { float x, y, r; uint32_t lit; } bumper_t;
enum { REEF, ION, MACHINES };
static int machine;
static const char *const MACHINE_NAME[MACHINES] = { "REEF CLUB", "ION STORM" };
static bumper_t bumpers[4];
static int bumper_count;
static uint32_t spinner_at;
static float spinner_angle, spinner_speed;
static int spinner_hits;

typedef struct { float px, py, ang, w; int side; bool held; } flipper_t;
static flipper_t flip[2];

static const float DROP_V[3] = { 70, 82, 94 };
static const float MESH_V[4] = { 74, 86, 98, 110 };
static const float LANE_X[3] = { 34, 46, 58 };
static bool drop_up[3], mesh_lit[4], lane_lit[3];
static uint32_t sling_lit[2], drop_reset_ms, mesh_reset_ms;

/* ---- the game ----------------------------------------------------------- */

enum { ST_TITLE, ST_PLAY, ST_DRAIN, ST_OVER };
static int state;
static v2 ball, vel, trail[4];
static bool in_lane, launched, tilted, charging, new_best;
static float power, lean;
static int score, ball_no, mult;

/* Difficulty: the same table run slower or at full pace, with more balls and
   a longer ball save when it is gentler. Slowing time keeps every shot the
   shape it was tuned to. */
enum { CHILL, NORMAL, ARCADE, LEVELS };
static const char *const LEVEL_NAME[LEVELS] = { "CHILL", "NORMAL", "ARCADE" };
static const char *const LEVEL_NOTE[LEVELS] = { "SLOW BALL - 5 BALLS - LONG SAVE", "3 BALLS - 8 SECOND SAVE", "FULL SPEED - 3 BALLS" };
static const float LEVEL_SPEED[LEVELS] = { 0.55f, 0.75f, 1.0f };
static const int LEVEL_BALLS[LEVELS] = { 5, 3, 3 };
static const uint32_t LEVEL_SAVE[LEVELS] = { 15000, 8000, 5000 };
static int level = NORMAL, records[MACHINES][LEVELS];
#define bests records[machine]
#define BALLS LEVEL_BALLS[level]
#define SAVE_MS LEVEL_SAVE[level]
static uint32_t last_ms, state_ms, save_until, nudge_at[3], jolt_ms, slow_since, msg_until;
static uint32_t key_l, key_r;
static bool show_perf;          /* P: frame interval, engine time and dots redrawn */
static float frame_ms;
static char msg[16];
static bool dirty_best;

typedef struct { float u, v; uint32_t t0; uint8_t col; char s[8]; } pop_t;
#define POPS 8
static pop_t pops[POPS];

/* ---- the screen ---------------------------------------------------------- */

static lap_canvas_t cv;
static l3_scene *scene;
static bool view_dirty = true;
static int camera_mode; /* 0 perspective, 1 aiming, 2 original 2D */
static uint8_t *bg_2d;
static bool bg_2d_dirty=true;
static uint16_t *rgb;
static uint32_t image_serial;
static float S, OX, OY;
static int hud_h;

static inline int X(float u) { return (int)(OX + u * S + 0.5f); }
static inline int Y(float v) { return (int)(OY + v * S + 0.5f); }

/* Small words go in the board's own font, over the picture once it is shown;
   the 3 x 5 dot letters are for headlines. Positions are cells. */
typedef struct { int col, row; uint8_t attr; char s[34]; } label_t;
#define LABELS 32
static label_t labels[LABELS];
static int nlabels;
static void put(int col, int row, const char *s, uint8_t attr)
{
    if (nlabels >= LABELS) return;
    label_t *l = &labels[nlabels++];
    *l = (label_t){ col, row, attr, "" };
    snprintf(l->s, sizeof(l->s), "%s", s);
}
static void put_c(int row, const char *s, uint8_t attr) { put((lap_cols() - (int)strlen(s)) / 2, row, s, attr); }
static void put_r(int row, const char *s, uint8_t attr) { put(lap_cols() - 1 - (int)strlen(s), row, s, attr); }
#define FELT(fg) LAP_ATTR(fg, LAP_BLUE)
#define INK(fg) LAP_ATTR(fg, LAP_BLACK)

/* ---- the best score ------------------------------------------------------- */

/* Keep the old best.txt untouched as a migration source. Each machine and
   pace has a separate record; the new file also remembers the selection. */
static void load_best(void)
{
    memset(records, 0, sizeof(records));
    int fd = lap_file_open("machines.txt", "r");
    if (fd >= 0) {
        char b[128] = {0};
        lap_file_read(fd, b, sizeof(b)-1);
        lap_file_close(fd);
        int v[9] = {0}, n = 0;
        for (const char *c=b; *c && n<9;) {
            while (*c==' ' || *c=='\n' || *c=='\r') c++;
            if (*c<'0' || *c>'9') break;
            v[n++] = atoi(c);
            while (*c>='0' && *c<='9') c++;
        }
        if (n>=8) {
            for(int m=0;m<MACHINES;m++) for(int l=0;l<LEVELS;l++) records[m][l]=v[m*LEVELS+l];
            if(v[6]<MACHINES) machine=v[6];
            if(v[7]<LEVELS) level=v[7];
            if(n==9 && v[8]<3) camera_mode=v[8];
            return;
        }
    }
    fd = lap_file_open("best.txt", "r");
    if(fd<0) return;
    char b[64]={0};
    lap_file_read(fd,b,sizeof(b)-1); lap_file_close(fd);
    int v[4]={0,0,0,NORMAL}, n=0;
    for(const char *c=b;*c && n<4;) {
        while(*c==' ' || *c=='\n' || *c=='\r') c++;
        if(*c<'0' || *c>'9') break;
        v[n++]=atoi(c); while(*c>='0' && *c<='9') c++;
    }
    if(n==1) records[REEF][ARCADE]=v[0];
    else if(n>=3) for(int i=0;i<LEVELS;i++) records[REEF][i]=v[i];
    if(n==4 && v[3]<LEVELS) level=v[3];
}
static void save_best(void)
{
    dirty_best = true;
    int fd=lap_file_open("machines.txt","w");
    if(fd<0) return;
    char b[128];
    int n=snprintf(b,sizeof(b),"%d %d %d %d %d %d %d %d %d\n",
        records[0][0],records[0][1],records[0][2],
        records[1][0],records[1][1],records[1][2],machine,level,camera_mode);
    int written=lap_file_write(fd,b,(size_t)n);
    lap_file_close(fd);
    if(written==n) dirty_best=false;
}

/* ---- sound ---------------------------------------------------------------- */

/* Arcade sounds cut each other off rather than queue up behind each other. */
static void sfx(const int *hz, int n, int ms, int vol, int wave)
{
    lap_tone_stop();
    for (int i = 0; i < n; i++) lap_tone(hz[i], ms, vol, wave);
}
#define SFX(ms, vol, wave, ...) do { static const int h_[] = { __VA_ARGS__ }; sfx(h_, (int)(sizeof(h_) / sizeof(h_[0])), ms, vol, wave); } while (0)

/* ---- building the table ---------------------------------------------------- */

static void seg(float ax, float ay, float bx, float by, int kind, int idx)
{
    if (nseg < MAXSEG) segs[nseg++] = (seg_t){ ax, ay, bx, by, (uint8_t)kind, (uint8_t)idx };
}

static void build_table(void)
{
    nseg = 0;
    bg_2d_dirty=true;
    static const bumper_t reef[] = { {32,54,6,0}, {60,54,6,0}, {46,76,6,0} };
    static const bumper_t ion[] = { {30,50,6,0}, {62,50,6,0}, {30,79,6,0}, {62,79,6,0} };
    bumper_count = machine==ION ? 4 : 3;
    memset(bumpers,0,sizeof(bumpers));
    memcpy(bumpers,machine==ION ? ion : reef,(size_t)bumper_count*sizeof(bumper_t));
    if(machine==ION) seg(39,105,53,105,K_SPINNER,0);
    /* The arch over the top, from the left rail round to the right one. */
    const int N = 20;
    v2 prev = { 4, 50 };
    for (int i = 1; i <= N; i++) {
        const float a = PI + PI * i / N;
        const v2 p = { 50 + 46 * cosf(a), 50 + 46 * sinf(a) };
        seg(prev.x, prev.y, p.x, p.y, K_WALL, 0);
        prev = p;
    }
    seg(4, 50, 4, 136, K_WALL, 0);              /* left rail */
    seg(4, 136, 21, 153, K_WALL, 0);            /* left inlane, down to the flipper */
    seg(96, 50, 96, 176, K_WALL, 0);            /* outer right rail */
    seg(88, 62, 88, 176, K_WALL, 0);            /* the shooter lane's wall */
    seg(88, 176, 96, 176, K_WALL, 0);           /* the plunger */
    seg(88, 136, 71, 153, K_WALL, 0);           /* right inlane */
    seg(88, 62, 96, 55, K_GATE, 0);             /* one way: out of the lane, never back in */
    /* Slingshots: rubber all round, the face toward the play kicks. */
    seg(8, 116, 8, 139, K_RUBBER, 0);   seg(8, 139, 19, 147, K_RUBBER, 0);   seg(8, 116, 19, 147, K_KICK, 0);
    seg(84, 116, 84, 139, K_RUBBER, 1); seg(84, 139, 73, 147, K_RUBBER, 1); seg(84, 116, 73, 147, K_KICK, 1);
    /* Posts between the rollover lanes. */
    for (int i = 0; i < 4; i++) seg(28 + 12 * i, 18, 28 + 12 * i, 28, K_RUBBER, 0);
    for (int i = 0; i < 3; i++) seg(8, DROP_V[i] - 4, 8, DROP_V[i] + 4, K_DROP, (uint8_t)i);
    for (int i = 0; i < 4; i++) seg(85, MESH_V[i] - 4, 85, MESH_V[i] + 4, K_MESH, (uint8_t)i);
    flip[0] = (flipper_t){ 24, 156, FL_REST, 0, +1, false };
    flip[1] = (flipper_t){ 68, 156, FL_REST, 0, -1, false };
}

/* ---- scoring ---------------------------------------------------------------- */

static void pop(float u, float v, const char *s, uint8_t col)
{
    int k = 0;
    for (int i = 1; i < POPS; i++) if (pops[i].t0 < pops[k].t0) k = i;
    pops[k] = (pop_t){ u, v, lap_millis() | 1, col, "" };
    snprintf(pops[k].s, sizeof(pops[k].s), "%s", s);
}

static void add(int pts, float u, float v)
{
    if (tilted) return;
    score += pts * mult;
    char t[8];
    snprintf(t, sizeof(t), "%d", pts * mult);
    pop(u, v, t, LC_BR_WHITE);
}

static void say(const char *s, int ms)
{
    snprintf(msg, sizeof(msg), "%s", s);
    msg_until = lap_millis() + (uint32_t)ms;
}

static void reset_ball(void)
{
    in_lane = true;
    launched = false;
    charging = false;
    power = 0;
    tilted = false;
    ball = (v2){ LANE_U, LANE_V };
    vel = (v2){ 0, 0 };
    for (int i = 0; i < 4; i++) trail[i] = ball;
    save_until = 0;
}

static void start(void)
{
    score = 0;
    ball_no = 1;
    mult = 1;
    new_best = false;
    memset(pops,0,sizeof(pops));
    memset(nudge_at,0,sizeof(nudge_at));
    memset(sling_lit,0,sizeof(sling_lit));
    for(int i=0;i<bumper_count;i++) bumpers[i].lit=0;
    spinner_hits=0; spinner_at=0; spinner_speed=0; spinner_angle=0;
    key_l=key_r=msg_until=jolt_ms=slow_since=0;
    flip[0].held=flip[1].held=false;
    flip[0].ang=flip[1].ang=FL_REST;
    for (int i = 0; i < 3; i++) drop_up[i] = true, lane_lit[i] = false;
    for (int i = 0; i < 4; i++) mesh_lit[i] = false;
    drop_reset_ms = mesh_reset_ms = 0;
    reset_ball();
    state = ST_PLAY;
    state_ms = lap_millis();
    SFX(70, 45, LAP_WAVE_SQUARE, 523, 659, 784, 1047);
    lap_haptic(LAP_HAPTIC_CONFIRM);
}

static void launch(float pw)
{
    in_lane = false;
    charging = false;
    vel = (v2){ 0, -(330 + 470 * pw) };
    if (!launched) save_until = lap_millis() + SAVE_MS;
    launched = true;
    SFX(14, 50, LAP_WAVE_NOISE, 300, 450, 650, 900);
    lap_vibrate(40, 60);
}

/* ---- physics ---------------------------------------------------------------- */

static bool touching(float ax, float ay, float bx, float by, float r, v2 *n, float *pen, float *t_out)
{
    const float dx = bx - ax, dy = by - ay;
    float t = ((ball.x - ax) * dx + (ball.y - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float ex = ball.x - (ax + t * dx), ey = ball.y - (ay + t * dy);
    const float d2 = ex * ex + ey * ey;
    if (d2 >= r * r || d2 < 1e-8f) return false;
    const float d = sqrtf(d2);
    *n = (v2){ ex / d, ey / d };
    *pen = r - d;
    if (t_out) *t_out = t;
    return true;
}

/* Push the ball out along n and bounce it; returns the closing speed. */
static float bounce(v2 n, float pen, float e)
{
    ball.x += n.x * pen;
    ball.y += n.y * pen;
    const float vn = vel.x * n.x + vel.y * n.y;
    if (vn < 0) {
        vel.x -= (1 + e) * vn * n.x;
        vel.y -= (1 + e) * vn * n.y;
    }
    return -vn;
}

static void hit_seg(const seg_t *s, uint32_t now)
{
    v2 n;
    float pen;
    if (s->kind == K_GATE && vel.y <= 0) return;
    if (s->kind == K_DROP && !drop_up[s->idx]) return;
    if (!touching(s->ax, s->ay, s->bx, s->by, BALL_R, &n, &pen, NULL)) return;
    /* A spinner swings through the ball, rather than bouncing it back. */
    if (s->kind == K_SPINNER) {
        if (!tilted && (!spinner_at || now-spinner_at>180)) {
            spinner_at=now; spinner_speed=28;
            spinner_hits++;
            add(150,46,105);
            if(spinner_hits%5==0) { add(2500,46,105); say("ION SURGE",1200); }
            vel.y*=.96f;
            SFX(18,40,LAP_WAVE_TRIANGLE,1480,1760);
        }
        return;
    }
    const float e = s->kind == K_WALL || s->kind == K_GATE ? 0.4f : s->kind == K_DROP ? 0.3f : 0.65f;
    const float hit = bounce(n, pen, e);
    switch (s->kind) {
    case K_KICK:
        if (hit > 25 && !tilted) {
            vel.x += n.x * 230;
            vel.y += n.y * 230;
            sling_lit[s->idx] = now | 1;
            add(10, ball.x, ball.y - 6);
            SFX(16, 60, LAP_WAVE_NOISE, 220, 160);
            lap_haptic(LAP_HAPTIC_TICK);
        }
        break;
    case K_RUBBER:
        if (hit > 80) SFX(10, 30, LAP_WAVE_SQUARE, 880);
        break;
    case K_DROP:
        if (hit > 15) {
            drop_up[s->idx] = false;
            add(500, 14, DROP_V[s->idx]);
            SFX(30, 55, LAP_WAVE_SQUARE, 660, 330);
            if (!drop_up[0] && !drop_up[1] && !drop_up[2]) {
                add(5000, 30, 88);
                say("JACKPOT", 1500);
                SFX(70, 55, LAP_WAVE_SQUARE, 784, 988, 1175, 1568, 1175, 1568);
                lap_haptic(LAP_HAPTIC_CONFIRM);
                drop_reset_ms = now + 1500;
            }
        }
        break;
    case K_MESH:
        if (hit > 15 && !mesh_lit[s->idx]) {
            mesh_lit[s->idx] = true;
            add(300, 76, MESH_V[s->idx]);
            SFX(25, 50, LAP_WAVE_TRIANGLE, 1319, 1760);
            if (mesh_lit[0] && mesh_lit[1] && mesh_lit[2] && mesh_lit[3]) {
                add(3000, 60, 92);
                if (mult < 5) mult++;
                say("MESH UP", 1500);
                save_until = now + SAVE_MS;     /* and the ball save comes back on */
                SFX(60, 55, LAP_WAVE_TRIANGLE, 659, 880, 1109, 1319, 1760);
                lap_haptic(LAP_HAPTIC_CONFIRM);
                mesh_reset_ms = now + 1500;
            }
        }
        break;
    default:
        break;
    }
}

static void hit_bumpers(uint32_t now)
{
    for (int i = 0; i < bumper_count; i++) {
        bumper_t *b = &bumpers[i];
        const float dx = ball.x - b->x, dy = ball.y - b->y, r = b->r + BALL_R;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= r * r || d2 < 1e-6f) continue;
        const float d = sqrtf(d2);
        const v2 n = { dx / d, dy / d };
        bounce(n, r - d, 0.5f);
        if (!tilted && now - b->lit > 60) {
            vel.x += n.x * 190;
            vel.y += n.y * 190;
            b->lit = now | 1;
            add(100, b->x, b->y - 9);
            static const int tone[4][2] = { { 1047, 784 }, { 1175, 880 }, { 988, 740 }, { 1397, 1047 } };
            sfx(tone[i], 2, 16, 55, LAP_WAVE_SQUARE);
        }
    }
}

static void hit_flipper(flipper_t *f)
{
    const float c = cosf(f->ang), sn = sinf(f->ang);
    const float bx = f->px + f->side * c * FL_LEN, by = f->py + sn * FL_LEN;
    const float dx = bx - f->px, dy = by - f->py;
    float t = ((ball.x - f->px) * dx + (ball.y - f->py) * dy) / (FL_LEN * FL_LEN);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float rf = 3.2f - 1.6f * t, r = BALL_R + rf;
    const float ex = ball.x - (f->px + t * dx), ey = ball.y - (f->py + t * dy);
    const float d2 = ex * ex + ey * ey;
    if (d2 >= r * r || d2 < 1e-8f) return;
    const float d = sqrtf(d2);
    const v2 n = { ex / d, ey / d };
    ball.x += n.x * (r - d);
    ball.y += n.y * (r - d);
    /* The bat's own speed where the ball touches it is what throws the ball. */
    const float s = t * FL_LEN;
    const v2 vs = { s * f->w * -f->side * sn, s * f->w * c };
    const float vn = (vel.x - vs.x) * n.x + (vel.y - vs.y) * n.y;
    if (vn < 0) {
        vel.x -= 1.3f * vn * n.x;
        vel.y -= 1.3f * vn * n.y;
    }
}

static void move_flippers(float h)
{
    for (int i = 0; i < 2; i++) {
        flipper_t *f = &flip[i];
        const float target = f->held && !tilted ? FL_UP : FL_REST;
        const float sp = f->held ? 26.0f : 16.0f;
        float d = target - f->ang;
        const float mx = sp * h;
        d = d > mx ? mx : d < -mx ? -mx : d;
        f->w = d / h;
        f->ang += d;
    }
}

static void drain(uint32_t now)
{
    if (now < save_until && !tilted) {
        say("BALL SAVED", 1200);
        reset_ball();
        launched = true;                /* no second save */
        SFX(60, 50, LAP_WAVE_TRIANGLE, 988, 1319);
        return;
    }
    state = ST_DRAIN;
    state_ms = now;
    SFX(120, 50, LAP_WAVE_TRIANGLE, 392, 330, 262, 196);
    lap_haptic(LAP_HAPTIC_WARN);
}

static void physics(float dt, uint32_t now)
{
    const float sp = sqrtf(vel.x * vel.x + vel.y * vel.y);
    int n = (int)ceilf(sp * dt / 1.2f);
    const int nmin = (int)ceilf(dt / 0.004f);
    if (n < nmin) n = nmin;
    if (n > 60) n = 60;
    const float h = dt / n;
    for (int k = 0; k < n; k++) {
        move_flippers(h);
        if (in_lane) continue;
        const float py = ball.y;
        vel.x += lean * h;
        vel.y += GRAV * h;
        /* Rolling resistance, applied per substep so frame rate does not
           change the loss. The steel ball still gets energy from contacts. */
        const float roll = 1.0f / (1.0f + .075f*h);
        vel.x *= roll; vel.y *= roll;
        ball.x += vel.x * h;
        ball.y += vel.y * h;
        for (int i = 0; i < nseg; i++) hit_seg(&segs[i], now);
        hit_bumpers(now);
        hit_flipper(&flip[0]);
        hit_flipper(&flip[1]);
        /* Rolling over a lane at the top. */
        if ((py > 27) != (ball.y > 27))
            for (int i = 0; i < 3; i++)
                if (fabsf(ball.x - LANE_X[i]) < 5 && !lane_lit[i] && !tilted) {
                    lane_lit[i] = true;
                    add(250, LANE_X[i], 36);
                    SFX(40, 45, LAP_WAVE_TRIANGLE, 1568);
                    if (lane_lit[0] && lane_lit[1] && lane_lit[2]) {
                        lane_lit[0] = lane_lit[1] = lane_lit[2] = false;
                        if (mult < 5) mult++;
                        add(1000, 46, 40);
                        char t[8];
                        snprintf(t, sizeof(t), "%dX", mult);
                        say(t, 1200);
                        SFX(60, 50, LAP_WAVE_SQUARE, 784, 988, 1175, 1568);
                    }
                }
        const float s2 = vel.x * vel.x + vel.y * vel.y;
        if (s2 > 950.0f * 950.0f) { const float f = 950.0f / sqrtf(s2); vel.x *= f; vel.y *= f; }
    }
    if (in_lane) return;
    /* Back in the shooter lane, too weak to make the arch: shoot again. */
    if (ball.x > 88 && ball.y > 150 && sp < 20) {
        in_lane = true;
        ball = (v2){ LANE_U, LANE_V };
        vel = (v2){ 0, 0 };
        return;
    }
    /* Somewhere the numbers can't be: back to the lane. */
    if (ball.x < 0 || ball.x > TW || ball.y < 0) { reset_ball(); launched = true; return; }
    if (ball.y > TH + 4) { drain(now); return; }
    /* Dead still away from the flippers for a while: a little shove. */
    if (sp < 6 && ball.y < 140) {
        if (!slow_since) slow_since = now;
        else if (now - slow_since > 2500) { vel.y = -90; vel.x = (lc_frand() - .5f) * 80; slow_since = 0; }
    } else
        slow_since = 0;
}

/* ---- the picture ---------------------------------------------------------- */

#include "table3d.h"
#include "table2d.h"

/* The 3D machine owns its score display. Only the two glass controls
   float outside the cabinet. The original 2D view keeps its flat HUD. */
static void draw_hud(uint32_t now)
{
    lc_fill_rect(&cv,0,0,cv.w,hud_h,LC_BLACK);
    const char *view=camera_mode==2?"[2D]":camera_mode==1?"[TOP]":"[3D]";
    put(1,1,view,INK(LAP_CYAN|LAP_BRIGHT));
    put_r(1,"EXIT",INK(LAP_WHITE|LAP_BRIGHT));
    if(show_perf) {
        char p[34];l3_stats_t st={0};if(scene && camera_mode!=2) l3_stats(scene,&st);
        snprintf(p,sizeof(p),"%2dms l3 %d.%d %u",(int)(frame_ms+.5f),(int)(st.us_total/1000),(int)(st.us_total/100%10),(unsigned)st.dots_shaded);
        put(7,1,p,INK(LAP_GREEN|LAP_BRIGHT));
    }
    if(camera_mode!=2) return;
    char t[34];snprintf(t,sizeof(t),"%d",score);
    lc_text(&cv,2,13,t,2,LC_BR_WHITE);
    snprintf(t,sizeof(t),"BALL %d/%d  %dX",ball_no,BALLS,mult);
    put_r(5,t,INK(LAP_CYAN|LAP_BRIGHT));
    put_r(7,LEVEL_NAME[level],INK(LAP_YELLOW|LAP_BRIGHT));
    if(tilted && (now/150)%2) put_c(5,"TILT",INK(LAP_RED|LAP_BRIGHT));
}

/* A panel over the table, on whole cells so the words inside sit clean. */
static int panel_w;
static void panel(int row, int rows)
{
    const int x = 6, y = row * 3, h = rows * 3;
    panel_w = cv.w - 12;
    lc_fill_rect(&cv, x, y, panel_w, h, LC_BLACK);
    lc_rect(&cv, x, y, panel_w, h, LC_BR_RED);
    lc_rect(&cv, x + 1, y + 1, panel_w - 2, h - 2, LC_RED);
}

static void headline(int y, const char *s, int k, uint8_t col, uint8_t edge)
{
    while (k > 1 && lc_text_w(s, k) > cv.w - 20) k--;
    lc_text_outlined(&cv, (cv.w - lc_text_w(s, k)) / 2, y, s, k, col, edge);
}

/* The level buttons on the title, in dots, for the finger. */
typedef struct { int x, y, w, h; } box_t;
static box_t level_btn[LEVELS], machine_btn[MACHINES];

static void draw_levels(int row)
{
    const int gap = 2, w = ((panel_w - 8 - 2 * gap) / 3) & ~1, x0 = ((cv.w - (3 * w + 2 * gap)) / 2) & ~1;
    for (int i = 0; i < LEVELS; i++) {
        box_t *b = &level_btn[i];
        *b = (box_t){ x0 + i * (w + gap), row * 3, w, 9 };
        const bool on = i == level;
        lc_fill_rect(&cv, b->x, b->y, b->w, b->h, on ? LC_BR_YELLOW : LC_GREY);
        lc_rect(&cv, b->x, b->y, b->w, b->h, on ? LC_BR_WHITE : LC_WHITE);
        const int n = (int)strlen(LEVEL_NAME[i]);
        put(b->x / 2 + (b->w / 2 - n) / 2, row + 1, LEVEL_NAME[i],
            on ? LAP_ATTR(LAP_BLACK, LAP_YELLOW | LAP_BRIGHT) : LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK | LAP_BRIGHT));
    }
}

static void draw_overlay(uint32_t now)
{
    char t[34];
    if (state == ST_TITLE) {
        const int row = (cv.h-90)/3, rows=30;
        panel(row, rows);
        headline(row*3+4,"PINBALL",2,LC_BR_YELLOW,LC_RED);
        int r=row+6;
        for(int i=0;i<MACHINES;i++) {
            box_t *b=&machine_btn[i];
            int w=((cv.w-20)/2)&~1;
            *b=(box_t){10+i*w,r*3,w-2,12};
            lc_rect(&cv,b->x,b->y,b->w,b->h,i==machine ? LC_BR_YELLOW : LC_GREY);
            put(b->x/2+(b->w/2-(int)strlen(MACHINE_NAME[i]))/2,r+1,
                MACHINE_NAME[i],INK(i==machine ? LAP_YELLOW|LAP_BRIGHT : LAP_WHITE));
        }
        r+=5;
        put_c(r++,machine==ION ? "4 POPS + SPINNER / 5 HITS = SURGE" : "3 POPS / DROP BANK = JACKPOT",INK(LAP_CYAN|LAP_BRIGHT));
        draw_levels(++r); r+=4;
        snprintf(t,sizeof(t),"BEST %d",bests[level]); put_c(r++,t,INK(LAP_YELLOW|LAP_BRIGHT));
        put_c(r++,LEVEL_NOTE[level],INK(LAP_WHITE));
        put_c(r++,"HOLD SIDE: FLIP / RIGHT: PLUNGER",INK(LAP_WHITE));
        put_c(r++,"SHAKE: NUDGE / VIEW: 3D OR 2D",INK(LAP_WHITE));
        if((now/400)%2) put_c(row+rows-2,"TAP GLASS TO PLAY",INK(LAP_GREEN|LAP_BRIGHT));
    } else if (state == ST_OVER) {
        const int row = Y(44) / 3, rows = 22;
        panel(row, rows);
        headline(row * 3 + 5, "GAME OVER", 2, LC_BR_RED, LC_BLACK);
        snprintf(t, sizeof(t), "%d", score);
        headline(row * 3 + 19, t, 3, new_best ? LC_BR_YELLOW : LC_BR_WHITE, LC_RED);
        int r = (row * 3 + 21 + 15) / 3 + 1;
        if (new_best) put_c(r++, "NEW BEST!", (now / 200) % 2 ? INK(LAP_YELLOW | LAP_BRIGHT) : INK(LAP_YELLOW));
        else { snprintf(t, sizeof(t), "BEST %d", bests[level]); put_c(r++, t, INK(LAP_WHITE)); }
        put_c(r++, LEVEL_NAME[level], INK(LAP_CYAN | LAP_BRIGHT));
        if (now - state_ms > 800 && (now / 400) % 2) put_c(row + rows - 2, "TAP TO GO ON", INK(LAP_GREEN | LAP_BRIGHT));
    } else if (state == ST_DRAIN) {
        if (ball_no >= BALLS) snprintf(t, sizeof(t), "GAME OVER");
        else if (ball_no + 1 == BALLS) snprintf(t, sizeof(t), "LAST BALL");
        else snprintf(t, sizeof(t), "BALL %d", ball_no + 1);
        headline(Y(84), t, 3, LC_BR_WHITE, LC_RED);
    }
    if (now < msg_until && state == ST_PLAY) {
        const uint32_t left = msg_until - now;
        headline(Y(84), msg, 3 + (left > 1100 ? 1 : 0), (now / 120) % 2 ? LC_BR_YELLOW : LC_BR_WHITE, LC_RED);
    }
}

/* ---- the app ---------------------------------------------------------------- */

static void layout(void)
{
    if(!lap_canvas_init(&cv,lap_cols(),lap_rows())) return;
    free(rgb);rgb=malloc((size_t)cv.w*cv.h*sizeof(*rgb));
    free(bg_2d);bg_2d=malloc((size_t)cv.w*cv.h);
    if(!bg_2d || !rgb) {lap_canvas_free(&cv);return;}
    memset(rgb,0,(size_t)cv.w*cv.h*sizeof(*rgb));image_serial++;
    bg_2d_dirty=true;
    l3_destroy(scene);
    scene=l3_create(lap_cols(),lap_rows());
    if(!scene) { lap_canvas_free(&cv); return; }
    setup_3d();
    hud_h=camera_mode==2?27:10;
    S=fminf(cv.w/TW,(cv.h-hud_h-1)/TH);
    OX=(cv.w-TW*S)/2;
    OY=hud_h+(cv.h-hud_h-TH*S)/2;
    view_dirty=true;
}

static void enter(void)
{
    lap_immersive(true);
    lap_hold_rotation(true);
    lap_keep_awake(true);
    load_best();
    build_table();
    view_dirty=true;
    state = ST_TITLE;
    mult = 1;
    for (int i = 0; i < 3; i++) drop_up[i] = true;
    reset_ball();
    last_ms = lap_millis();
}

static void leave(void)
{
    if (dirty_best) save_best();
    l3_destroy(scene);
    scene = NULL;
    free(rgb);rgb=NULL;
    free(bg_2d);bg_2d=NULL;
    lap_canvas_free(&cv);
}

/* The sensor: the board's lean rolls the ball, a jolt nudges the table. */
static void sense(uint32_t now)
{
    lap_motion_t m;
    lean = 0;
    if (!lap_motion(&m)) return;
    lean = lc_clampf(-m.ax * 70, -45, 45);
    const float a = sqrtf(m.ax * m.ax + m.ay * m.ay + m.az * m.az);
    if (fabsf(a - 1) < 0.55f || now - nudge_at[0] < 350 || state != ST_PLAY || in_lane || tilted) return;
    nudge_at[2] = nudge_at[1];
    nudge_at[1] = nudge_at[0];
    nudge_at[0] = now;
    vel.y -= 70;
    vel.x += lc_clampf(-m.ax * 120, -60, 60) + (lc_frand() - .5f) * 30;
    jolt_ms = now | 1;
    lap_vibrate(40, 50);
    if (nudge_at[2] && now - nudge_at[2] < 4000) {
        tilted = true;
        say("TILT", 2000);
        SFX(400, 60, LAP_WAVE_SAW, 110);
        lap_haptic(LAP_HAPTIC_ALERT);
    } else if (nudge_at[1] && now - nudge_at[1] < 4000) {
        say("DANGER", 900);
        SFX(80, 45, LAP_WAVE_SQUARE, 220, 0, 220);
    }
}

/* Fingers: the left half holds the left flipper, the right half the right
   one - or, with the ball in the shooter lane, the plunger. */
/* A tap on the title or the game-over panel, in dots. Both the finger and
   the tap callback report the same tap, so the second within 400 ms is it
   again. */
static void menu_tap(int x, int y)
{
    static uint32_t last;
    const uint32_t now = lap_millis();
    if(x>=cv.w-22 && y<10) { lap_exit(); return; }
    if (now - last < 400) return;
    if(x<22 && y<10) {
        camera_mode=(camera_mode+1)%3;view_dirty=true;last=now;save_best();return;
    }
    if(state==ST_PLAY) return;
    if (state != ST_TITLE && state != ST_OVER) return;
    last = now;
    if (state == ST_OVER) {
        if (now - state_ms > 800) { state = ST_TITLE; state_ms = now; }
        return;
    }
    for(int i=0;i<MACHINES;i++) {
        const box_t *b=&machine_btn[i];
        if(x>=b->x && x<b->x+b->w && y>=b->y && y<b->y+b->h) {
            if(machine!=i) { machine=i; build_table(); reset_ball(); view_dirty=true; save_best(); }
            lap_haptic(LAP_HAPTIC_TICK); return;
        }
    }
    for (int i = 0; i < LEVELS; i++) {
        const box_t *b = &level_btn[i];
        if (x >= b->x - 1 && x < b->x + b->w + 1 && y >= b->y - 3 && y < b->y + b->h + 3) {
            if (level != i) { level = i; save_best(); }
            const int hz = 660 + 220 * i;
            sfx(&hz, 1, 30, 40, LAP_WAVE_SQUARE);
            lap_haptic(LAP_HAPTIC_TICK);
            return;
        }
    }
    start();
}

static void fingers(uint32_t now)
{
    lap_touch_t t;
    int cw = 1, ch = 1;
    lap_cell_px(&cw, &ch);
    const int half = lap_cols() * cw / 2;
    const bool any = lap_touch(&t) && t.down;
    /* On the title and after a game, a finger lifting is a tap there. */
    static bool was_down;
    static int lx, ly;
    if (any) { lx = t.x * 2 / (cw > 0 ? cw : 1); ly = t.y * 3 / (ch > 0 ? ch : 1); }
    if (was_down && !any) menu_tap(lx, ly);
    was_down = any;
    const bool down = any && state == ST_PLAY && ly>=hud_h;
    bool l = down && t.x < half, r = down && t.x >= half;
    if (in_lane && r) {
        if (!charging) { charging = true; lap_tone(0, 1, 0, LAP_WAVE_SINE); }
        power = fminf(1, t.held_ms / 900.0f);
        r = false;
    } else if (charging) {
        launch(power);
        power = 0;
    }
    l |= now < key_l;
    r |= now < key_r;
    for (int i = 0; i < 2; i++) {
        const bool h = i ? r : l;
        if (h && !flip[i].held && !tilted) {
            /* A flip clacks, and moves the lit lanes along, as real tables do. */
            lap_tone(120, 12, 35, LAP_WAVE_NOISE);
            lap_haptic(LAP_HAPTIC_TICK);
            const bool a = lane_lit[0], c = lane_lit[2];
            if (i == 0) { lane_lit[0] = lane_lit[1]; lane_lit[1] = c; lane_lit[2] = a; }
            else { lane_lit[2] = lane_lit[1]; lane_lit[1] = a; lane_lit[0] = c; }
        }
        flip[i].held = h;
    }
}

static void draw(void)
{
    if (cv.cw != lap_cols() || cv.ch != lap_rows() || !cv.d || hud_h!=(camera_mode==2?27:10)) layout();
    if (!cv.d) { lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK)); lap_put_str(1, 1, "not enough memory", LAP_ATTR(LAP_RED, LAP_BLACK)); return; }
    PB_MARK(0);
    const uint32_t now = lap_millis();
    float dt = (now - last_ms) / 1000.0f;
    frame_ms += ((float)(now - last_ms) - frame_ms) * 0.1f;
    last_ms = now;
    if (dt > 0.05f) dt = 0.05f;
    if (dt <= 0) dt = 0.001f;

    /* The spinner's plate hangs under its wire: it swings back to rest and
       stops there, rather than creeping forever (a part that moves a
       hair every frame is redrawn every frame). */
    if (spinner_speed != 0 || spinner_angle != 0) {
        spinner_speed += -30.0f * sinf(2 * spinner_angle) * dt;
        spinner_speed *= 1.0f/(1.0f+2.5f*dt);
        spinner_angle += spinner_speed*dt;
        if (fabsf(spinner_speed) < 0.05f && fabsf(sinf(spinner_angle)) < 0.02f) {
            spinner_speed = 0;
            spinner_angle = 0;
        }
    }
    nlabels = 0;
    sense(now);
    fingers(now);
    if(hud_h!=(camera_mode==2?27:10)) layout();
    if(!cv.d) return;
    if (state == ST_PLAY) {
        for (int i = 3; i > 0; i--) trail[i] = trail[i - 1];
        trail[0] = ball;
        physics(dt * LEVEL_SPEED[level], now);
    } else
        move_flippers(dt * LEVEL_SPEED[level]);
    if (state == ST_DRAIN && now - state_ms > 1500) {
        if (ball_no < BALLS) {
            ball_no++;
            reset_ball();
            state = ST_PLAY;
        } else {
            state = ST_OVER;
            state_ms = now;
            if (score > bests[level]) { bests[level] = score; new_best = true; save_best(); }
        }
    }
    if (drop_reset_ms && now > drop_reset_ms) { drop_up[0] = drop_up[1] = drop_up[2] = true; drop_reset_ms = 0; }
    if (mesh_reset_ms && now > mesh_reset_ms) { memset(mesh_lit, 0, sizeof(mesh_lit)); mesh_reset_ms = 0; }
    PB_MARK(1);

    if(camera_mode==2) {
        if(bg_2d_dirty) {
            build_bg_2d();memcpy(bg_2d,cv.d,(size_t)cv.w*cv.h);bg_2d_dirty=false;
        }
        draw_table_2d(now);
    } else draw_table(now);
    if (state == ST_TITLE || state == ST_OVER) nlabels = 0;     /* the panel covers the table's words */
    draw_hud(now);
    draw_overlay(now);
    /* A nudge jolts the picture a cell. */
    const int ox = jolt_ms && now - jolt_ms < 90 ? ((now - jolt_ms) / 45 ? -1 : 1) : 0;
    if (ox) lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    PB_MARK(4);
    if(camera_mode==2) lap_canvas_show(&cv,ox,0);
    else {
        /* Preserve all the renderer's colours per dot. Packing six dots
           into a two-colour terminal cell erases small chrome details. */
        uint16_t palette[16];for(int c=0;c<16;c++) palette[c]=lap_palette(c);
        /* A new serial only when a pixel really changed: the firmware then
           pushes just the changed cells, and an unchanged frame none. */
        bool changed=false;
        for(int i=0;i<cv.w*cv.h;i++) {
            const uint16_t v=palette[cv.d[i]&15];
            if(rgb[i]!=v) { rgb[i]=v; changed=true; }
        }
        if(changed) image_serial++;
        PB_MARK(5);
        lap_image(ox,0,lap_cols(),lap_rows(),rgb,cv.w,cv.h,image_serial);
    }
    PB_MARK(6);
    for (int i = 0; i < nlabels; i++) lap_put_str(labels[i].col, labels[i].row, labels[i].s, labels[i].attr);
    PB_MARK(7);
}

static bool touch(int col, int row)
{
    menu_tap(col * 2 + 1, row * 3 + 1);
    return true;
}

static bool key(lap_key_t k, char ch)
{
    const uint32_t now = lap_millis();
    if(k==LAP_KEY_CHAR && (ch=='v' || ch=='V')) {
        camera_mode=(camera_mode+1)%3; view_dirty=true; save_best(); return true;
    }
    if(k==LAP_KEY_CHAR && (ch=='p' || ch=='P')) { show_perf=!show_perf; return true; }
    const bool go = k == LAP_KEY_ENTER || (k == LAP_KEY_CHAR && ch == ' ');
    if (state == ST_TITLE) {
        if(k==LAP_KEY_UP || k==LAP_KEY_DOWN || (k==LAP_KEY_CHAR && (ch=='1' || ch=='2'))) {
            machine=(k==LAP_KEY_CHAR) ? ch-'1' : (machine+1)%MACHINES;
            build_table(); reset_ball(); view_dirty=true; save_best(); return true;
        }
        if (k == LAP_KEY_LEFT || k == LAP_KEY_RIGHT) {
            level = (level + (k == LAP_KEY_RIGHT ? 1 : LEVELS - 1)) % LEVELS;
            save_best();
            return true;
        }
        if (go) start();
        return go;
    }
    if (state == ST_OVER) {
        if (go && now - state_ms > 800) { state = ST_TITLE; state_ms = now; }
        return go;
    }
    if (go && in_lane && state == ST_PLAY) { launch(0.75f + lc_frand() * 0.25f); return true; }
    if (k == LAP_KEY_LEFT || (k == LAP_KEY_CHAR && (ch == 'z' || ch == 'a'))) { key_l = now + 160; return true; }
    if (k == LAP_KEY_RIGHT || (k == LAP_KEY_CHAR && (ch == 'm' || ch == 'l'))) { key_r = now + 160; return true; }
    return false;
}

/* The tile icon, in colour dots. */
LAP_ICON_PX(
    "cccccccccccccccccccc",
    "c..................c",
    "c.............d5...c",
    "c..9191......ddd5..c",
    "c.9f9991....dfddd5.c",
    "c.9f9911....5d5d55.c",
    "c.999191.....5d55..c",
    "c.191911......55...c",
    "c..1911.fff7.......c",
    "c......fff7f7......c",
    "c......ffff77......c",
    "c......f7f777......c",
    "c.......f7f7.......c",
    "c.ff............ff.c",
    "c.ffff........ffff.c",
    "c...ffff....ffff...c",
    "c.....ff....ff.....c",
    "c..................c");

LAP_APP(.id = "pinball", .name = "PINBALL", .sub = "two tables / 2D + 3D", .version = "2.1",
        .author = "LakeShark", .color = LAP_MAGENTA,
        .icon_art = { "|  .--.  |", "| ( () ) |", "|  '--'  |", "|  o     |", "|\\_    _/|", "  \\    /  " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
