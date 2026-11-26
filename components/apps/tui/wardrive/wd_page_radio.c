/* WARDRIVE's RADIO page (wd_page.h): the radio jobs.

   One instrument per radio, each drawn as the thing it is: the LoRa chip as
   a package on its coil antenna, an RTL-SDR as a USB stick with its whip, a
   HackRF as its board with the LED row. Beside it the job it runs, where it
   listens and how long the dwell has left; under it the sub-GHz band with
   every job's frequency marked and the tuner's place on it, the finds of the
   last ten minutes as braille in the colour of the job that made them, each
   job's record (finds, rate, last find, dry dwells, the scheduler's score),
   the scheduler's plan as a strip over the next ten minutes, and SWAP: a big
   key per job to pin the radio to it for the drive, AUTO to let the
   scheduler choose again.

   Nothing moves unless the radios say so: the antennas' arcs are the finds
   of the last ten seconds, the lamp lights when a find lands, the dwell bar
   is the scheduler's clock.

   Keys: UP/DOWN pick a radio, LEFT/RIGHT (or ENTER) step its pin, A AUTO. */
#include "wd_page.h"
#include "wd_radio.h"
#include "../ls_tui_ui.h"
#include "../ls_tui.h"
#include "../ls_braille.h"
#include "../ls_plate.h"
#include "../ls_calls.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

/* everything on the plates' ground (ls_plate.h) */
#define INK(c)  ls_plate_at((uint8_t)(c))
#define LIT(c)  INK((c) | TUI_BRIGHT)
#define DIM     ls_plate_at(TUI_WHITE)
#define FAINT   ls_plate_at(TUI_BLUE)
#define TEXT    ls_plate_at(TUI_WHITE | TUI_BRIGHT)
#define STRONG  ls_plate_at(TUI_WHITE | TUI_BRIGHT)
#define LAMP_MS 2000u

static EXT_RAM_BSS_ATTR wd_radio_state_t V;
static int s_sel;
static uint16_t s_seen_hits[WD_RJ_N];
static uint32_t s_lamp_ms[WD_RJ_N];
typedef struct { tui_rect r; int8_t radio, job; } hit_t;      /* job -1: AUTO */
static hit_t s_hit[2 * (WD_JOBS + 1)];
static int s_nhit;

static uint8_t job_hue(int j)
{
    static const uint8_t H[WD_JOBS] = { TUI_YELLOW, TUI_GREEN, TUI_CYAN, TUI_MAGENTA };
    return j >= 0 && j < WD_JOBS ? H[j] : TUI_WHITE;
}
static char job_letter(int j) { return j >= 0 && j < WD_JOBS ? "TIAM"[j] : '.'; }

static void put(tui_surface *sf, tui_rect c, int x, int y, const char *s, uint8_t at) { tui_put_str(sf, c, x, y, s, at); }
static void putc_(tui_surface *sf, tui_rect c, int x, int y, char ch, uint8_t at) { tui_put_char(sf, c, x, y, ch, at); }

static void ago(char *out, size_t n, uint32_t now, uint32_t then)
{
    if (!then) { snprintf(out, n, "never"); return; }
    const uint32_t s = (now - then) / 1000;
    if (s < 60) snprintf(out, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um%02us", (unsigned)(s / 60), (unsigned)(s % 60));
    else snprintf(out, n, "%uh", (unsigned)(s / 3600));
}

static int recent(const wd_radio_view_t *v) { return v->trace[WD_TRACE - 1] + v->trace[WD_TRACE - 2]; }

/* ------------------------------------------------------------ the art */

/* The LoRa chip: a QFN on its board with the coil antenna; the arcs off the
   coil are the last finds, the die lights while a session runs. */
static void art_chip(tui_surface *sf, tui_rect c, int x, int y, const wd_radio_view_t *v, uint32_t now, int r)
{
    const bool run = v->state == WD_RS_RUN, lamp = now - s_lamp_ms[r] < LAMP_MS;
    const uint8_t body = v->present ? INK(TUI_MAGENTA) : FAINT, pin = v->present ? LIT(TUI_YELLOW) : FAINT;
    const uint8_t coil = v->present ? LIT(TUI_WHITE) : FAINT;
    put(sf, c, x + 3, y, "_/\\/\\/\\/\\_", coil);
    put(sf, c, x + 3, y + 1, "|", coil);
    const int arcs = recent(v) > 3 ? 3 : recent(v);
    for (int i = 0; i < 3; i++) putc_(sf, c, x + 14 + i * 2, y, ')', i < arcs ? LIT(job_hue(v->job)) : FAINT);
    put(sf, c, x, y + 2, " .", body); put(sf, c, x + 15, y + 2, ".", body);
    for (int i = 0; i < 6; i++) putc_(sf, c, x + 3 + i * 2, y + 2, '|', pin), putc_(sf, c, x + 4 + i * 2, y + 2, '-', body);
    putc_(sf, c, x + 1, y + 3, '=', pin); putc_(sf, c, x + 2, y + 3, '|', body);
    char nm[12];
    snprintf(nm, sizeof(nm), "%-10.10s", v->name);
    put(sf, c, x + 4, y + 3, nm, v->present ? STRONG : FAINT);
    putc_(sf, c, x + 15, y + 3, '|', body); putc_(sf, c, x + 16, y + 3, '=', pin);
    putc_(sf, c, x + 1, y + 4, '=', pin); putc_(sf, c, x + 2, y + 4, '|', body);
    put(sf, c, x + 4, y + 4, "[", body);
    for (int i = 0; i < 8; i++)
        putc_(sf, c, x + 5 + i, y + 4, ':', run ? (lamp ? LIT(TUI_WHITE) : INK(job_hue(v->job))) : FAINT);
    put(sf, c, x + 13, y + 4, "]", body);
    putc_(sf, c, x + 15, y + 4, '|', body); putc_(sf, c, x + 16, y + 4, '=', pin);
    put(sf, c, x + 1, y + 5, " '", body); put(sf, c, x + 15, y + 5, "'", body);
    for (int i = 0; i < 6; i++) putc_(sf, c, x + 3 + i * 2, y + 5, '|', pin), putc_(sf, c, x + 4 + i * 2, y + 5, '-', body);
}

/* An RTL-SDR stick: the whip and its arcs, the lamp, the USB plug. */
static void art_rtl(tui_surface *sf, tui_rect c, int x, int y, const wd_radio_view_t *v, uint32_t now, int r)
{
    const bool lamp = now - s_lamp_ms[r] < LAMP_MS, run = v->state == WD_RS_RUN;
    const uint8_t body = v->present ? INK(TUI_CYAN) : FAINT, whip = v->present ? LIT(TUI_WHITE) : FAINT;
    putc_(sf, c, x + 6, y, 'o', whip);
    put(sf, c, x + 6, y + 1, "|", whip);
    put(sf, c, x + 6, y + 2, "|", whip);
    const int arcs = recent(v) > 3 ? 3 : recent(v);
    for (int i = 0; i < 3; i++) putc_(sf, c, x + 8 + i * 2, y + 1, ')', i < arcs ? LIT(job_hue(v->job)) : FAINT);
    put(sf, c, x + 1, y + 3, ".----#-----------.", body);
    put(sf, c, x + 1, y + 4, "|", body);
    put(sf, c, x + 3, y + 4, "RTL-SDR", v->present ? STRONG : FAINT);
    putc_(sf, c, x + 12, y + 4, lamp ? '*' : 'o', lamp ? LIT(TUI_GREEN) : run ? INK(TUI_GREEN) : FAINT);
    put(sf, c, x + 18, y + 4, "|==", body);
    put(sf, c, x + 1, y + 5, "'--R820T2--------'==", body);
}

/* A HackRF One: two SMA posts, the LED row (power, RX, the find lamp). */
static void art_hackrf(tui_surface *sf, tui_rect c, int x, int y, const wd_radio_view_t *v, uint32_t now, int r)
{
    const bool lamp = now - s_lamp_ms[r] < LAMP_MS, run = v->state == WD_RS_RUN;
    const uint8_t body = v->present ? INK(TUI_BLUE | TUI_BRIGHT) : FAINT, post = v->present ? LIT(TUI_YELLOW) : FAINT;
    put(sf, c, x + 3, y, "|", LIT(TUI_WHITE));
    const int arcs = recent(v) > 3 ? 3 : recent(v);
    for (int i = 0; i < 3; i++) putc_(sf, c, x + 5 + i * 2, y, ')', i < arcs ? LIT(job_hue(v->job)) : FAINT);
    put(sf, c, x + 3, y + 1, "#", post); put(sf, c, x + 15, y + 1, "#", post);
    put(sf, c, x + 1, y + 2, ".-#-----------#-.", body);
    put(sf, c, x + 1, y + 3, "|", body); put(sf, c, x + 17, y + 3, "|", body);
    static const char *const LED = "UPRTAB";
    for (int i = 0; i < 6; i++) {
        const bool on = v->present && (i < 2 || (i == 2 && run) || (i == 5 && lamp));
        putc_(sf, c, x + 3 + i * 2, y + 3, on ? 'o' : '.', on ? LIT(i == 5 ? TUI_GREEN : i == 2 ? TUI_CYAN : TUI_YELLOW) : FAINT);
        (void)LED;
    }
    put(sf, c, x + 1, y + 4, "|", body); put(sf, c, x + 17, y + 4, "|", body);
    put(sf, c, x + 3, y + 4, "HackRF One", v->present ? STRONG : FAINT);
    put(sf, c, x + 1, y + 5, "'---------------'", body);
}

/* ------------------------------------------------------------ the parts */

/* The state as a chip for the plate's top edge. */
static const char *state_word(const wd_radio_view_t *v, uint8_t *hue)
{
    static const char *const W[] = { "NOT FITTED", "IDLE", "ASKING", "LISTENING", "LENT OUT" };
    static const uint8_t H[] = { TUI_RED, TUI_WHITE, TUI_YELLOW, TUI_GREEN, TUI_RED };
    const int s = v->state < 5 ? v->state : 1;
    *hue = H[s];
    return v->pin >= 0 && s == WD_RS_RUN ? "PINNED" : W[s];
}

/* The headline beside the art: the job in big letters, where, how many. */
static void headline(tui_surface *sf, tui_rect c, int x, int y, int w, const wd_radio_view_t *v)
{
    char t[48];
    if (v->job < 0) {
        put(sf, c, x, y + 1, V.on ? "no job right now" : "START the drive", TEXT);
        if (v->why[0]) put(sf, c, x, y + 3, v->why, v->state == WD_RS_YIELD || v->state == WD_RS_ABSENT ? LIT(TUI_RED) : DIM);
        return;
    }
    const char *name = wd_radio_job_name(v->job);
    if (ls_calls_big_width(name) <= w) ls_calls_big_text(sf, c, x, y, name, LIT(job_hue(v->job)));
    else put(sf, c, x, y + 1, name, LIT(job_hue(v->job)));
    if (v->freq_khz) {
        snprintf(t, sizeof(t), "%u.%03u MHz", (unsigned)(v->freq_khz / 1000), (unsigned)(v->freq_khz % 1000));
        put(sf, c, x, y + 3, t, STRONG);
    }
    snprintf(t, sizeof(t), "%u found", (unsigned)v->hits);
    put(sf, c, x, y + 4, t, v->hits ? LIT(TUI_GREEN) : DIM);
    const int wl = (int)strlen(wd_radio_job_what(v->job));
    if (x + (int)strlen(t) + 3 + wl <= c.x + c.w) put(sf, c, x + (int)strlen(t) + 3, y + 4, wd_radio_job_what(v->job), DIM);
    if (v->why[0]) put(sf, c, x, y + 5, v->why, v->state == WD_RS_YIELD ? LIT(TUI_RED) : DIM);
}

/* The turn: how much of this dwell is gone, as one thick bar. */
static void turn(tui_surface *sf, tui_rect c, int x, int y, int w, const wd_radio_view_t *v, uint32_t now)
{
    char t[16];
    put(sf, c, x, y, "TURN", DIM);
    if (v->job < 0 || v->until_ms <= v->since_ms) { ls_plate_meter(sf, c, x + 6, y, w - 12, 0, TUI_WHITE); return; }
    const uint32_t len = v->until_ms - v->since_ms, gone = now > v->since_ms ? now - v->since_ms : 0;
    ls_plate_meter(sf, c, x + 6, y, w - 12, gone >= len ? 1.0f : (float)gone / (float)len, job_hue(v->job));
    const uint32_t left = v->until_ms > now ? (v->until_ms - now) / 1000 : 0;
    if (left >= 60) snprintf(t, sizeof(t), "%u:%02u", (unsigned)(left / 60), (unsigned)(left % 60));
    else snprintf(t, sizeof(t), "%us", (unsigned)left);
    put(sf, c, x + w - (int)strlen(t), y, t, STRONG);
}

/* Finds per ten seconds, newest at the right: one solid column a bucket,
   half-cell steps, in the colour of the job that ran; a run that found
   nothing leaves its tinted floor. */
static void graph(tui_surface *sf, tui_rect c, int x, int y, int w, int rows, const wd_radio_view_t *v)
{
    char t[24];
    const int cols = w - 5 < WD_TRACE ? w - 5 : WD_TRACE;
    if (cols < 8 || rows < 2) return;
    int top = 1;
    for (int i = WD_TRACE - cols; i < WD_TRACE; i++) if (v->trace[i] > top) top = v->trace[i];
    const int levels = rows * 2;
    for (int k = 0; k < cols; k++) {
        const int b = WD_TRACE - cols + k, jj = v->trace_job[b];
        const int h = v->trace[b] ? 1 + (v->trace[b] * (levels - 1) + top - 1) / top : 0;
        for (int row = 0; row < rows; row++) {
            const int above = h - row * 2;
            const int yy = y + rows - 1 - row;
            if (above >= 1) ls_plate_col(sf, c, x + k, yy, above, LIT(job_hue(jj)));
            else if (!row && jj >= 0) putc_(sf, c, x + k, yy, '_', INK(job_hue(jj)));     /* ran, found nothing */
            else if (!row) putc_(sf, c, x + k, yy, '_', FAINT);
        }
    }
    snprintf(t, sizeof(t), "%d", top);
    put(sf, c, x + cols + 1, y, t, STRONG);
    put(sf, c, x + cols + 1, y + 1, "per", DIM);
    put(sf, c, x + cols + 1, y + 2 < y + rows ? y + 2 : y + 1, "10s", DIM);
    snprintf(t, sizeof(t), "-%dm", cols * (int)(WD_TRACE_MS / 1000) / 60);
    put(sf, c, x, y + rows, t, DIM);
    put(sf, c, x + cols - 3, y + rows, "now", DIM);
}

/* One row a job: its name on its colour when it is the one running, what it
   found, how fast, when last, and how much the scheduler wants it. */
static int jobs(tui_surface *sf, tui_rect c, int x, int y, int w, int rows, const wd_radio_view_t *v, uint32_t now)
{
    int n = 0;
    char t[48], a[12];
    for (int j = 0; j < WD_JOBS && n < rows; j++) {
        const wd_job_view_t *J = &v->jobs[j];
        if (!J->can && !J->runs) continue;
        const bool cur = v->job == j;
        snprintf(t, sizeof(t), " %-5s ", wd_radio_job_name(j));
        put(sf, c, x, y + n, t, cur ? TUI_ATTR(TUI_BLACK, job_hue(j) | TUI_BRIGHT) : J->can ? LIT(job_hue(j)) : FAINT);
        ago(a, sizeof(a), now, J->last_hit_ms);
        snprintf(t, sizeof(t), "%4u  %4.1f/m  %-6s", (unsigned)J->hits, (double)J->rate, a);
        put(sf, c, x + 8, y + n, t, J->can ? (J->hits ? STRONG : TEXT) : FAINT);
        const int bx = x + 8 + (int)strlen(t) + 1, bw = x + w - bx;
        if (J->dry && bw >= 6) put(sf, c, bx, y + n, "DRY", LIT(TUI_RED));
        n++;
    }
    return n;
}

/* The radio's newest finds, newest first, on their job's colour mark. */
static int finds(tui_surface *sf, tui_rect c, int x, int y, int w, int rows, const wd_radio_view_t *v)
{
    int n = 0;
    char t[40];
    for (int i = 0; i < WD_LAST && n < rows; i++) {
        if (!v->last[i][0]) break;
        putc_(sf, c, x, y + n, job_letter(v->last_job[i]), LIT(job_hue(v->last_job[i])));
        snprintf(t, sizeof(t), "%.*s", w - 3 > 34 ? 34 : w - 3, v->last[i]);
        put(sf, c, x + 2, y + n, t, i ? TEXT : STRONG);
        n++;
    }
    return n;
}

/* What the scheduler does next, in words. */
static void next_line(tui_surface *sf, tui_rect c, int x, int y, const wd_radio_view_t *v)
{
    char t[64];
    put(sf, c, x, y, "NEXT", DIM);
    if (!V.on || !v->plan_n) { put(sf, c, x + 6, y, v->present ? "plans once the drive starts" : "", DIM); return; }
    if (v->pin >= 0) {
        snprintf(t, sizeof(t), "%s all drive, pinned", wd_radio_job_name(v->pin));
        put(sf, c, x + 6, y, t, LIT(job_hue(v->pin)));
        return;
    }
    if (v->plan_n > 1) {
        const wd_plan_step_t *p = &v->plan[1];
        snprintf(t, sizeof(t), "%s", wd_radio_job_name(p->job));
        put(sf, c, x + 6, y, t, LIT(job_hue(p->job)));
        snprintf(t, sizeof(t), "in %u:%02u for %u:%02u", (unsigned)(p->at_s / 60), (unsigned)(p->at_s % 60),
                 (unsigned)(p->for_s / 60), (unsigned)(p->for_s % 60));
        put(sf, c, x + 12, y, t, TEXT);
    } else put(sf, c, x + 6, y, "stays on this job", DIM);
}

/* SWAP: AUTO and a key a job, three rows tall, heavy frames. */
static void swap(tui_surface *sf, tui_rect c, int x, int y, int w, int h, int r, const wd_radio_view_t *v, bool focus)
{
    const int n = WD_JOBS + 1, gap = 1, bw = (w - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++) {
        const int j = i - 1;
        const bool can = j < 0 || v->jobs[j].can || v->pin == j;
        const bool on = (j < 0 && v->pin < 0) || (j >= 0 && v->pin == j);
        const tui_rect b = tui_rect_make(x + i * (bw + gap), y, i == n - 1 ? x + w - (x + i * (bw + gap)) : bw, h);
        ls_plate_key(sf, c, b, j < 0 ? "AUTO" : wd_radio_job_name(j), j < 0 ? TUI_WHITE : job_hue(j), on, can);
        if (focus && on && b.w > 2) putc_(sf, c, b.x + 1, b.y + (b.h - 1) / 2, '>', TUI_ATTR(TUI_BLACK, (j < 0 ? TUI_WHITE : job_hue(j)) | TUI_BRIGHT));
        if (s_nhit < (int)(sizeof(s_hit) / sizeof(s_hit[0])))
            s_hit[s_nhit++] = (hit_t){ b, (int8_t)r, (int8_t)j };
    }
}

/* One radio's plate in a. */
static void panel(tui_surface *sf, tui_rect a, int r, uint32_t now)
{
    const wd_radio_view_t *v = &V.r[r];
    char title[32];
    uint8_t sh;
    const char *sw = state_word(v, &sh);
    snprintf(title, sizeof(title), "%s%s %s", s_sel == r ? "> " : "", r == WD_RJ_LORA ? "LORA CHIP" : "USB SDR",
             v->present ? v->name : "");
    const tui_rect c = ls_plate(sf, a, a, r == WD_RJ_LORA ? TUI_MAGENTA : TUI_CYAN, title, sw, sh);
    if (c.w < 20 || c.h < 6) return;
    const int x = c.x, w = c.w, bottom = c.y + c.h;
    int y = c.y + 1;
    /* the radio as it is, and beside it the job in big letters */
    if (r == WD_RJ_LORA) art_chip(sf, c, x, y, v, now, r);
    else if (!strcmp(v->name, "HackRF")) art_hackrf(sf, c, x, y, v, now, r);
    else art_rtl(sf, c, x, y, v, now, r);
    headline(sf, c, x + 21, y, w - 21, v);
    y += 7;
    if (!v->present) {
        put(sf, c, x, y, r == WD_RJ_SDR ? "No SDR on the USB port." : "No LoRa chip answered.", STRONG);
        if (r == WD_RJ_SDR) put(sf, c, x, y + 1, "On battery the V1.0 board has no 5 V for it.", TEXT);
        return;
    }
    /* from the bottom up: SWAP, NEXT; then what fits between */
    const int key_h = bottom - y >= 13 ? 3 : 1;
    const int keys_y = bottom - key_h;
    next_line(sf, c, x, keys_y - 2, v);
    int njobs = 0, nlast = 0;
    for (int j = 0; j < WD_JOBS; j++) njobs += v->jobs[j].can || v->jobs[j].runs;
    for (int i = 0; i < WD_LAST; i++) nlast += v->last[i][0] != 0;
    int room = keys_y - 3 - y;                         /* rows above NEXT, keeping one blank */
    turn(sf, c, x, y, w, v, now);
    y += 2; room -= 2;
    int g_rows = room - njobs - 2 >= 6 ? 4 : room - njobs - 2 >= 4 ? 3 : 2;
    if (room >= g_rows + 2) { graph(sf, c, x, y, w, g_rows, v); y += g_rows + 2; room -= g_rows + 2; }
    const int jr = room < njobs ? room : njobs;
    if (jr > 0) { y += jobs(sf, c, x, y, w, jr, v, now); room -= jr; }
    if (room >= 3 && nlast) finds(sf, c, x, y + 1, w, room - 1 < 3 ? room - 1 : 3, v);
    swap(sf, c, x, keys_y, w, key_h, r, v, s_sel == r);
}

static void header(tui_surface *sf, tui_rect a, int y)
{
    static const char *const MV[] = { "STANDING", "SLOW", "DRIVING", "SPEED ?" };
    char t[96];
    tui_fill(sf, tui_rect_make(a.x, y, a.w, 1), ' ', ls_plate_at(TUI_WHITE));
    int x = a.x + ls_plate_tab(sf, a, a.x, y, "RADIO JOBS", TUI_YELLOW) + 1;
    if (!V.on) { put(sf, a, x, y, "START the drive to run them", LIT(TUI_YELLOW)); return; }
    snprintf(t, sizeof(t), "%s%s", wd_radio_region_name(V.region), V.region_fix ? "" : "?");
    put(sf, a, x, y, t, LIT(TUI_CYAN)); x += (int)strlen(t) + 2;
    const int kmh = V.speed >= 0 ? (int)lroundf(V.speed * 3.6f) : -1;
    if (kmh >= 0) snprintf(t, sizeof(t), "%s %d km/h", MV[V.motion < 4 ? V.motion : 3], kmh);
    else snprintf(t, sizeof(t), "%s", MV[3]);
    put(sf, a, x, y, t, V.motion == WD_MOVE_DRIVE ? LIT(TUI_YELLOW) : TEXT); x += (int)strlen(t) + 2;
    if (V.passive && x + 14 < a.x + a.w) {
        snprintf(t, sizeof(t), "+%u from apps", (unsigned)V.passive);
        put(sf, a, x, y, t, DIM);
    }
}

static void draw(tui_surface *sf, tui_rect a, uint32_t now)
{
    wd_radio_view(&V);
    for (int r = 0; r < WD_RJ_N; r++) {
        if (V.r[r].hits != s_seen_hits[r]) { if (V.r[r].hits > s_seen_hits[r]) s_lamp_ms[r] = now ? now : 1; s_seen_hits[r] = V.r[r].hits; }
    }
    s_nhit = 0;
    header(sf, a, a.y);
    const tui_rect body = tui_rect_make(a.x, a.y + 2, a.w, a.h - 2);
    const bool wide = a.w >= 2 * 54;
    if (wide) {
        tui_rect right = tui_rect_make(body.x + body.w / 2 + 1, body.y, body.w - body.w / 2 - 1, body.h);
        tui_rect hole;
        if (ls_tui_cutout(&hole) && hole.x > right.x && hole.x < right.x + right.w) right.w = hole.x - 1 - right.x;
        panel(sf, tui_rect_make(body.x, body.y, body.w / 2, body.h), WD_RJ_LORA, now);
        panel(sf, right, WD_RJ_SDR, now);
    } else {
        /* an absent radio takes only what it needs to say so */
        const int small = 11;
        int h0 = (body.h - 1) / 2, h1 = body.h - 1 - h0;
        if (!V.r[WD_RJ_SDR].present) { h1 = small; h0 = body.h - 1 - h1; }
        else if (!V.r[WD_RJ_LORA].present) { h0 = small; h1 = body.h - 1 - h0; }
        panel(sf, tui_rect_make(body.x, body.y, body.w, h0), WD_RJ_LORA, now);
        panel(sf, tui_rect_make(body.x, body.y + h0 + 1, body.w, h1), WD_RJ_SDR, now);
    }
}

/* Step radio r's pin through AUTO and the jobs it can run. */
static void step_pin(int r, int dir)
{
    int p = V.r[r].pin;
    for (int k = 0; k < WD_JOBS + 1; k++) {
        p += dir;
        if (p < -1) p = WD_JOBS - 1;
        if (p >= WD_JOBS) p = -1;
        if (p < 0 || V.r[r].jobs[p].can) break;
    }
    wd_radio_pin(r, p);
    V.r[r].pin = (int8_t)p;
}

static bool key(ls_tk_t k, char ch, uint32_t now)
{
    (void)now;
    if (k == LS_TK_UP || k == LS_TK_DOWN) { s_sel ^= 1; return true; }
    if (k == LS_TK_LEFT) { step_pin(s_sel, -1); return true; }
    if (k == LS_TK_RIGHT || k == LS_TK_ENTER) { step_pin(s_sel, +1); return true; }
    if (ch == 'a' || ch == 'A') { wd_radio_pin(s_sel, -1); V.r[s_sel].pin = -1; return true; }
    return false;
}

static bool touch(int x, int y, uint32_t now)
{
    (void)now;
    for (int i = 0; i < s_nhit; i++) {
        const tui_rect b = s_hit[i].r;
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
            s_sel = s_hit[i].radio;
            const int j = s_hit[i].job;
            if (j >= 0 && !V.r[s_sel].jobs[j].can && V.r[s_sel].pin != j) return true;
            wd_radio_pin(s_sel, j);
            V.r[s_sel].pin = (int8_t)j;
            return true;
        }
    }
    return false;
}

const wd_page_t wd_page_radio = { draw, key, touch, NULL };
