#include "ls_fox_run.h"
#include "ls_rfsim.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ls_time.h"
#include "ls_textpack.h"

#ifndef LS_FOX_DIR
#define LS_FOX_DIR "/sdcard/lakeshark/fox"
#endif
#define PENDING 8

static StaticSemaphore_t s_lock_mem;
static SemaphoreHandle_t s_lock;
static unsigned s_lock_once;
static void lock(void)
{
    /* The UI and the worker can both be first here. */
    unsigned expect = 0;
    if (__atomic_compare_exchange_n(&s_lock_once, &expect, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
        __atomic_store_n(&s_lock_once, 2, __ATOMIC_RELEASE);
    } else while (__atomic_load_n(&s_lock_once, __ATOMIC_ACQUIRE) != 2) vTaskDelay(1);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

EXT_RAM_BSS_ATTR static ls_fox_hunt_t s_hunt;
EXT_RAM_BSS_ATTR static ls_fox_fix_t s_fix;
EXT_RAM_BSS_ATTR static struct { ls_df_bearing_t b; uint32_t utc; } s_pending[PENDING];
static int s_npending;
static bool s_new_file = true, s_rewrite, s_reload;
static uint32_t s_fix_serial;
static char s_status[80] = "No bearings yet", s_file[48];

static void refit(void)
{
    if (s_fix_serial == s_hunt.serial) return;
    s_fix_serial = s_hunt.serial;
    if (!ls_fox_fix(s_hunt.b, s_hunt.n, &s_fix)) memset(&s_fix, 0, sizeof(s_fix));
}

void ls_fox_keep(const ls_df_bearing_t *b)
{
    if (!b) return;
    const uint32_t utc = ls_time_is_synced() ? (uint32_t)time(NULL) : 0;
    lock();
    ls_fox_hunt_add(&s_hunt, b, utc);
    if (s_npending < PENDING) { s_pending[s_npending].b = *b; s_pending[s_npending].utc = utc; s_npending++; }
    else s_rewrite = true;              /* the queue is full: write the whole hunt again */
    refit();
    unlock();
}

bool ls_fox_undo(void)
{
    lock();
    const bool ok = ls_fox_hunt_drop_last(&s_hunt);
    if (ok) { s_rewrite = true; s_npending = 0; }
    refit();
    unlock();
    return ok;
}

void ls_fox_new(void)
{
    lock();
    ls_fox_hunt_clear(&s_hunt);
    s_npending = 0; s_new_file = true; s_rewrite = false;
    s_file[0] = 0;
    LS_SNPUTS(t_ls_fox_run_new_hunt_take_a, s_status, sizeof(s_status), "New hunt: take a bearing");
    refit();
    unlock();
}

void ls_fox_reload(void) { lock(); s_reload = true; unlock(); }

void ls_fox_get(ls_fox_hunt_t *out) { lock(); *out = s_hunt; unlock(); }
int ls_fox_count(void) { lock(); const int n = s_hunt.n; unlock(); return n; }

bool ls_fox_fix_now(ls_fox_fix_t *out)
{
    lock(); refit(); *out = s_fix; unlock();
    return out->valid;
}

uint32_t ls_fox_serial(void) { return s_hunt.serial; }
const char *ls_fox_status(void) { return s_status; }
const char *ls_fox_file(void) { return s_file; }

/* -------------------------------------------------------------- worker -- */

/* Every directory down to `dir`. */
static void mkdirs(const char *dir)
{
    char p[128];
    snprintf(p, sizeof(p), "%s", dir);
    for (char *c = p + 1; ; c++) {
        if (*c == '/' || !*c) {
            const char keep = *c; *c = 0;
#ifdef _WIN32
            mkdir(p);
#else
            mkdir(p, 0777);
#endif
            *c = keep;
            if (!keep) break;
        }
    }
}

static void dirs(void) { mkdirs(LS_FOX_DIR); }

static void load_newest(void)
{
    dirs();
    char best[48] = "";
    DIR *d = opendir(LS_FOX_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const size_t n = strlen(e->d_name);
            if (n > 4 && n < sizeof(best) && !strcmp(e->d_name + n - 4, ".csv") && strcmp(e->d_name, best) > 0)
                snprintf(best, sizeof(best), "%s", e->d_name);
        }
        closedir(d);
    }
    if (!best[0]) { lock(); LS_SNPUTS(t_ls_fox_run_no_hunts_on_the, s_status, sizeof(s_status), "No hunts on the card"); unlock(); return; }
    char path[192];
    snprintf(path, sizeof(path), LS_FOX_DIR "/%s", best);
    FILE *f = fopen(path, "rb");
    if (!f) { lock(); LS_SNPRINTF(t_ls_fox_run_could_not_open_40s, s_status, sizeof(s_status), "Could not open %.40s", best); unlock(); return; }
    EXT_RAM_BSS_ATTR static ls_fox_hunt_t h;
    ls_fox_hunt_clear(&h);
    char line[200];
    while (fgets(line, sizeof(line), f)) {
        ls_df_bearing_t b; uint32_t utc;
        if (ls_fox_csv_parse(line, &b, &utc)) ls_fox_hunt_add(&h, &b, utc);
    }
    fclose(f);
    lock();
    const uint32_t serial = s_hunt.serial;
    s_hunt = h; s_hunt.serial = serial + 1;
    s_npending = 0; s_new_file = false; s_rewrite = false;
    snprintf(s_file, sizeof(s_file), "%s", best);
    LS_SNPRINTF(t_ls_fox_run_loaded_d_bearings_from, s_status, sizeof(s_status), "Loaded %d bearings from %.40s", h.n, best);
    refit();
    unlock();
}

void ls_fox_io_step(void)
{
    lock();
    const bool reload = s_reload; s_reload = false;
    unlock();
    if (reload) { load_newest(); return; }
    lock();
    if (!s_npending && !s_rewrite) { unlock(); return; }
    EXT_RAM_BSS_ATTR static ls_fox_hunt_t all;
    EXT_RAM_BSS_ATTR static struct { ls_df_bearing_t b; uint32_t utc; } q[PENDING];
    const bool rewrite = s_rewrite, fresh = s_new_file || !s_file[0];
    const int nq = s_npending;
    memcpy(q, s_pending, sizeof(q[0]) * nq);
    all = s_hunt;
    s_npending = 0; s_rewrite = false;
    if (fresh) {
        char base[40];
        ls_time_render_filename(base, sizeof(base), "hunt");
        snprintf(s_file, sizeof(s_file), "%.40s.csv", base);
        s_new_file = false;
    }
    char path[192];
    snprintf(path, sizeof(path), LS_FOX_DIR "/%s", s_file);
    unlock();

    dirs();
    FILE *f = fopen(path, rewrite || fresh ? "wb" : "ab");
    char line[200];
    bool ok = f != NULL;
    if (ok && (rewrite || fresh)) {
        LS_FPUTS(t_ls_fox_run_lakeshark_fox_hunt_bearings, "# LakeShark FOX HUNT bearings, true degrees; spread is two sigma\n", f);
        fwrite(line, 1, ls_fox_csv_header(line, sizeof(line)), f);
    }
    if (ok && rewrite) for (int i = 0; i < all.n; i++) fwrite(line, 1, ls_fox_csv_line(&all.b[i], all.utc[i], line, sizeof(line)), f);
    else if (ok) for (int i = 0; i < nq; i++) fwrite(line, 1, ls_fox_csv_line(&q[i].b, q[i].utc, line, sizeof(line)), f);
    if (f) { ok = !ferror(f); fclose(f); }
    lock();
    snprintf(s_status, sizeof(s_status), ok ? "%d bearings on the card" : "Card write failed: bearings kept in RAM", s_hunt.n);
    unlock();
}

/* ------------------------------------------------------------- console -- */

int ls_fox_command(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "new")) { ls_fox_new(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "undo")) return ls_fox_undo() ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "load")) { ls_fox_reload(); return 0; }
    if (argc >= 3 && !strcmp(argv[1], "add")) {
        /* fox add BEARING [SPREAD]: a bearing from where ls_rf_where says. */
        ls_df_bearing_t b = { .bearing = (float)atof(argv[2]), .spread = argc > 3 ? (float)atof(argv[3]) : 8.0f,
                              .time_us = esp_timer_get_time() };
        uint16_t fl;
        if (!ls_rf_where(&b.lat, &b.lon, &fl, NULL)) { printf("fox: no position (GPS fix or rfsim here)\n"); return 1; }
        snprintf(b.source, sizeof(b.source), "console");
        ls_fox_keep(&b);
    } else if (argc != 1) {
        printf("fox | fox add BEARING [SPREAD] | undo | new | load\n");
        return 1;
    }
    ls_fox_hunt_t h; ls_fox_get(&h);
    ls_fox_fix_t f; const bool ok = ls_fox_fix_now(&f);
    for (int i = 0; i < h.n; i++)
        LS_PRINTF(t_ls_fox_run_2d_6f_6f_5, "  %2d  %.6f, %.6f  %5.1f +/-%4.1f  %6.1f  %s\n", i + 1, h.b[i].lat, h.b[i].lon, h.b[i].bearing, h.b[i].spread, h.b[i].level, h.b[i].source);
    if (ok) LS_PRINTF(t_ls_fox_run_fox_fix_6f_6f, "fox: FIX %.6f, %.6f  ellipse %.0f x %.0f m at %.0f  rms %.0f m  %d used, %d behind  %.0f m at %.0f from the last place\n",
                   f.lat, f.lon, f.major_m, f.minor_m, f.major_deg, f.rms_m, f.used, f.behind, f.range_m, f.bearing);
    else LS_PRINTF(t_ls_fox_run_fox_no_fix_yet, "fox: no fix yet (%d bearings; need two that cross at 15 degrees or more)\n", h.n);
    printf("fox: %s %s\n", ls_fox_status(), ls_fox_file());
    return 0;
}
