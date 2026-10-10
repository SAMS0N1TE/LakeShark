#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
/* Radio side of the search. See ls_search.h. */

#include "ls_search.h"
#include "ls_sweep.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app_registry.h"
#include "ls_sdcard.h"
#include "ls_time.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ls_search";

/* LS_SEARCH_STRONG_DB is worked out for this many averaged frames per tune. */
_Static_assert(LS_SWEEP_DEFAULT_DWELL_FFTS == 8,
               "re-derive LS_SEARCH_STRONG_DB for the new averaging");

#define WORKER_STACK        8192
#define WORKER_PRIO         2

/* Between passes the receiver is free. A pass lasts seconds, so this is where
   an app that is starting gets its turn. */
#define PASS_GAP_MS         50

/* Passes that fail in a row before the search gives up and says why. */
#define MAX_FAILS           3

#define TOP_LINES           16

/* Events only; the closing summary is always written. About 400 kB. */
#define LOG_MAX_EVENTS      4000

#define LOG_DIR             "/sdcard/lakeshark/scan"

typedef struct {
    ls_sweep_plan_t plan;
    ls_search_t     core;
    int8_t         *dbfs;      /* the pass being measured */
    int8_t         *last;      /* the last whole pass, for the dump */
    void           *mem;       /* the core's per-bin state */
    FILE           *log;
    uint32_t        log_events;
    char            log_path[96];
} job_t;

EXT_RAM_BSS_ATTR static job_t s_job;
static SemaphoreHandle_t s_lock;           /* guards s_job.core and the status */
static TaskHandle_t      s_task;
static StaticTask_t      s_tcb;

/* s_busy: a job owns the buffers and the worker is not parked. */
static volatile bool     s_busy;
static volatile bool     s_stop_req;

static uint64_t          s_lo_hz, s_hi_hz;
static uint32_t          s_pass_ms;
static bool              s_ever;
static const char       *s_why = "";

static uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

static void *big_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}

static ls_radio_requirements_t requirements_for(const ls_sweep_plan_t *plan)
{
    /* The same window ls_sweep_run_cancelable asks for. */
    ls_radio_requirements_t r = {
        .required_caps  = LS_RADIO_RX_IQ_U8,
        .min_hz         = plan->start_hz > plan->half_span_hz
                              ? plan->start_hz - plan->half_span_hz : 0,
        .max_hz         = plan->stop_hz + plan->half_span_hz,
        .sample_rate_hz = plan->sample_rate_hz,
        .iq_format      = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
    };
    return r;
}

static bool cancelled(void *user)
{
    (void)user;
    /* An app switch is the next owner arriving; get out of its way. */
    return s_stop_req || app_switch_in_progress();
}

/* ------------------------------------------------------------------ log */

static void log_open(job_t *j)
{
    j->log = NULL;
    j->log_events = 0;
    j->log_path[0] = 0;
    if (!ls_sdcard_mounted()) return;

    if ((mkdir("/sdcard/lakeshark", 0775) != 0 && errno != EEXIST) ||
        (mkdir(LOG_DIR, 0775) != 0 && errno != EEXIST))
        return;

    snprintf(j->log_path, sizeof(j->log_path),
             LOG_DIR "/search_%08llx_%012llx.jsonl",
             (unsigned long long)(uint64_t)time(NULL),
             (unsigned long long)(uint64_t)(esp_timer_get_time() / 1000));
    j->log = fopen(j->log_path, "ab");
    if (!j->log) j->log_path[0] = 0;
}

/* One line. A write that fails ends the log and nothing else: the search
   does not depend on the card. */
static void log_line(job_t *j, const char *line)
{
    if (!j->log) return;
    if (fputs(line, j->log) < 0 || fputc('\n', j->log) < 0 ||
        fflush(j->log) != 0) {
        fclose(j->log);
        j->log = NULL;
    }
}

static void log_hit(job_t *j, const char *ev, const ls_search_hit_t *h,
                    float duty)
{
    char line[240];
    snprintf(line, sizeof(line),
             "{\"ev\":\"%s\",\"f_hz\":%lu,\"db\":%.1f,\"path\":\"%s\","
             "\"const\":%s,\"count\":%lu,\"duty\":%.2f,\"first_s\":%lu,"
             "\"last_s\":%lu,\"unix\":%lld}",
             ev, (unsigned long)h->freq_hz, (double)h->max_db,
             ls_search_path_name(h->path), h->constant ? "true" : "false",
             (unsigned long)h->count, (double)duty,
             (unsigned long)h->first_s, (unsigned long)h->last_s,
             (long long)(ls_time_is_synced() ? time(NULL) : 0));
    log_line(j, line);
}

/* ----------------------------------------------------------------- hits */

static void on_hit(const ls_search_hit_t *h, ls_search_hit_kind_t kind,
                   void *user)
{
    job_t *j = (job_t *)user;
    if (kind == LS_SEARCH_HIT_NEW) {
        printf("search: %.4f MHz  +%.0f dB  %s%s  pass %lu\n", h->freq_hz / 1e6,
               (double)h->max_db, ls_search_path_name(h->path),
               h->constant ? " const" : "", (unsigned long)j->core.pass);
        fflush(stdout);
    }
    if (j->log_events < LOG_MAX_EVENTS) {
        j->log_events++;
        log_hit(j, kind == LS_SEARCH_HIT_NEW ? "new" : "again", h,
                ls_search_duty(&j->core, h));
    }
}

/* ---------------------------------------------------------------- worker */

static void run_job(void)
{
    job_t *j = &s_job;
    unsigned fails = 0;
    const char *why = "stopped";

    log_open(j);
    if (j->log) {
        char line[160];
        snprintf(line, sizeof(line),
                 "{\"ev\":\"start\",\"lo_hz\":%llu,\"hi_hz\":%llu,"
                 "\"bin_hz\":%u,\"gain_tenths\":%d,\"unix\":%lld}",
                 (unsigned long long)j->plan.start_hz,
                 (unsigned long long)j->plan.stop_hz,
                 (unsigned)j->plan.bin_hz, LS_SEARCH_GAIN_TENTHS,
                 (long long)(ls_time_is_synced() ? time(NULL) : 0));
        log_line(j, line);
    }

    while (!s_stop_req) {
        if (app_switch_in_progress()) { why = "an app is starting"; break; }

        int64_t t0 = esp_timer_get_time();
        ls_radio_err_t e = ls_sweep_run_cancelable(
            &j->plan, LS_SEARCH_GAIN_TENTHS, LS_SWEEP_DEFAULT_DWELL_FFTS,
            false, j->dbfs, NULL,
            cancelled, NULL);

        if (e == LS_RADIO_OK) {
            fails = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memcpy(j->last, j->dbfs, j->plan.n_bins);
            ls_search_feed(&j->core, j->dbfs, uptime_s(), on_hit, j);
            s_pass_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            xSemaphoreGive(s_lock);
        } else if (e == LS_RADIO_ERR_STOPPED) {
            why = s_stop_req ? "stopped"
                : app_switch_in_progress() ? "an app is starting"
                : "receiver stopped";
            break;
        } else if (e == LS_RADIO_ERR_BUSY) {
            /* Somebody took the receiver in the gap between passes. */
            why = "an app took the receiver";
            break;
        } else if (++fails >= MAX_FAILS) {
            why = ls_radio_err_name(e);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PASS_GAP_MS));
    }

    /* Hand the table to the log and the console. The per-bin state stays
       until the next start, so `search dump` can still read it. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (j->log) {
        uint32_t idx[LS_SEARCH_MAX_HITS];
        uint32_t n = ls_search_top(&j->core, idx, LS_SEARCH_MAX_HITS);
        for (uint32_t i = 0; i < n; i++)
            log_hit(j, "end", &j->core.hit[idx[i]],
                    ls_search_duty(&j->core, &j->core.hit[idx[i]]));
        char line[120];
        snprintf(line, sizeof(line),
                 "{\"ev\":\"stop\",\"reason\":\"%s\",\"passes\":%lu,"
                 "\"hits\":%lu}", why, (unsigned long)j->core.pass,
                 (unsigned long)j->core.n_hits);
        log_line(j, line);
    }
    if (j->log) { fclose(j->log); j->log = NULL; }

    printf("search: stopped - %s  %lu passes  %lu hits\n", why,
           (unsigned long)j->core.pass, (unsigned long)j->core.n_hits);

    s_why = why;
    xSemaphoreGive(s_lock);
}

static void worker(void *unused)
{
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run_job();
        s_busy = false;
    }
}

/* ------------------------------------------------------------------ API */

static ls_radio_err_t run_start_serialized(uint64_t lo_hz, uint64_t hi_hz)
{
    if (s_busy) return LS_RADIO_ERR_EXISTS;

    ls_sweep_plan_t plan;
    if (!ls_search_plan(lo_hz, hi_hz, &plan)) return LS_RADIO_ERR_INVALID;

    /* Take the receiver once, here, so that an app owning it is an answer at
       the console and not a line printed later by the worker. */
    ls_radio_requirements_t req = requirements_for(&plan);
    ls_radio_session_t *probe = NULL;
    ls_radio_err_t e = ls_radio_acquire("search", &req, &probe);
    if (e != LS_RADIO_OK) return e;
    ls_radio_release(probe);

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return LS_RADIO_ERR_NO_MEMORY;
    }
    if (!s_task) {
        StackType_t *stack = heap_caps_malloc(
            WORKER_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!stack) return LS_RADIO_ERR_NO_MEMORY;
        s_task = xTaskCreateStaticPinnedToCore(worker, "ls_search",
                                               WORKER_STACK, NULL, WORKER_PRIO,
                                               stack, &s_tcb, tskNO_AFFINITY);
        if (!s_task) {
            heap_caps_free(stack);
            return LS_RADIO_ERR_NO_MEMORY;
        }
    }

    job_t *j = &s_job;

    /* The last search's state, kept for the dump, makes way for this one. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    j->core.floor = NULL;
    free(j->dbfs); free(j->last); free(j->mem);
    j->dbfs = NULL; j->last = NULL; j->mem = NULL;
    j->plan = plan;
    j->dbfs = big_alloc(plan.n_bins);
    j->last = big_alloc(plan.n_bins);
    j->mem  = big_alloc((size_t)plan.n_bins * LS_SEARCH_BYTES_PER_BIN);
    if (!j->dbfs || !j->last || !j->mem) {
        free(j->dbfs); free(j->last); free(j->mem);
        j->dbfs = NULL; j->last = NULL; j->mem = NULL;
        xSemaphoreGive(s_lock);
        return LS_RADIO_ERR_NO_MEMORY;
    }
    memset(j->last, LS_SWEEP_NO_DATA, plan.n_bins);

    ls_search_init(&j->core, plan.start_hz, plan.bin_hz, plan.n_bins, j->mem);
    s_lo_hz = plan.start_hz;
    s_hi_hz = plan.stop_hz;
    s_pass_ms = 0;
    s_why = "";
    s_ever = true;
    s_stop_req = false;
    s_busy = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "search %.4f-%.4f MHz, %u bins, %u tunes",
             plan.start_hz / 1e6, plan.stop_hz / 1e6,
             (unsigned)plan.n_bins, (unsigned)plan.n_tunes);

    xTaskNotifyGive(s_task);
    return LS_RADIO_OK;
}

/* Only one starter can initialize the worker or replace retained buffers. */
ls_radio_err_t ls_search_run_start(uint64_t lo_hz, uint64_t hi_hz)
{
    static bool starting;
    bool expected = false;
    if (!__atomic_compare_exchange_n(&starting, &expected, true, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return LS_RADIO_ERR_EXISTS;
    ls_radio_err_t result = run_start_serialized(lo_hz, hi_hz);
    __atomic_store_n(&starting, false, __ATOMIC_RELEASE);
    return result;
}

void ls_search_run_stop(void)
{
    if (!s_busy) return;
    s_stop_req = true;
    /* The stop is seen between reads and retunes; a retune is the long one. */
    for (int i = 0; i < 300 && s_busy; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_busy) printf("search: still handing the receiver back\n");
}

void ls_search_run_clear(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ls_search_reset(&s_job.core);
    xSemaphoreGive(s_lock);
}

bool ls_search_run_active(void) { return s_busy; }

void ls_search_run_report(void)
{
    if (!s_ever) {
        printf("search: not run yet.  `search start [lo_MHz hi_MHz]`, "
               "default 136-174\n");
        return;
    }

    ls_search_hit_t top[TOP_LINES];
    float duty[TOP_LINES];
    uint32_t n = 0, pass = 0, n_hits = 0, n_const = 0, pass_ms = 0, tunes = 0;
    uint64_t lo, hi;
    const char *why;
    bool running;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t idx[TOP_LINES];
    n = ls_search_top(&s_job.core, idx, TOP_LINES);
    for (uint32_t i = 0; i < n; i++) {
        top[i]  = s_job.core.hit[idx[i]];
        duty[i] = ls_search_duty(&s_job.core, &top[i]);
    }
    pass = s_job.core.pass;
    n_hits = s_job.core.n_hits;
    n_const = ls_search_n_constant(&s_job.core);
    tunes = s_job.plan.n_tunes;
    pass_ms = s_pass_ms;
    lo = s_lo_hz; hi = s_hi_hz;
    why = s_why;
    running = s_busy;
    xSemaphoreGive(s_lock);

    if (running)
        printf("search: running %.4f-%.4f MHz  pass %lu  %.1f s/pass  "
               "%lu tunes  gain %d.%d dB%s\n", lo / 1e6, hi / 1e6,
               (unsigned long)pass, pass_ms / 1000.0, (unsigned long)tunes,
               LS_SEARCH_GAIN_TENTHS / 10, LS_SEARCH_GAIN_TENTHS % 10,
               pass <= LS_SEARCH_WARMUP ? "  WARMING UP, no hits yet" : "");
    else
        printf("search: stopped (%s)  %.4f-%.4f MHz  %lu passes\n",
               why, lo / 1e6, hi / 1e6, (unsigned long)pass);
    printf("search: %lu hits, %lu constant (always there: spurs, steady "
           "carriers)\n", (unsigned long)n_hits, (unsigned long)n_const);

    if (!n) return;
    printf("  %10s  %5s  %5s  %5s  %6s  %5s  %8s  %8s\n", "MHz", "+dB", "seen",
           "duty", "path", "const", "first s", "last s");
    for (uint32_t i = 0; i < n; i++)
        printf("  %10.4f  %5.1f  %5lu  %4.0f%%  %6s  %5s  %8lu  %8lu\n",
               top[i].freq_hz / 1e6, (double)top[i].max_db,
               (unsigned long)top[i].count, (double)(duty[i] * 100.0f),
               ls_search_path_name(top[i].path), top[i].constant ? "yes" : "-",
               (unsigned long)top[i].first_s, (unsigned long)top[i].last_s);
}

/* ----------------------------------------------------------------- dump */

/* A line per tune's worth of bins, each taken under the lock so a pass
   cannot land in the middle of it, printed outside it so a slow console does
   not hold the search up. */
void ls_search_run_dump(void)
{
    if (!s_ever) {
        printf("search: nothing to dump, `search start` first\n");
        return;
    }

    ls_sweep_plan_t plan;
    uint32_t pass;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    plan = s_job.plan;
    pass = s_job.core.pass;
    xSemaphoreGive(s_lock);

    const uint32_t per_line = ls_search_dump_per_line(&plan);   /* a tune's worth */
    const size_t   cap      = ls_search_dump_cap(per_line);
    char *line = malloc(cap);
    if (!line) { printf("search: out of memory for the dump\n"); return; }

    printf("SEARCH-DUMP v1 lo=%llu hi=%llu bin=%u hop=%u dc=%u tunes=%u "
           "pass=%lu gain=%d enc=(hex/2)-100 00=none\n",
           (unsigned long long)plan.start_hz, (unsigned long long)plan.stop_hz,
           (unsigned)plan.bin_hz, (unsigned)plan.hop_hz,
           (unsigned)plan.dc_guard_hz, (unsigned)plan.n_tunes,
           (unsigned long)pass, LS_SEARCH_GAIN_TENTHS);

    uint32_t cursor = 0;
    for (;;) {
        size_t len = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_job.core.floor && s_job.last)
            len = ls_search_dump_next(&s_job.core, s_job.last, per_line,
                                      &cursor, line, cap);
        xSemaphoreGive(s_lock);
        if (!len) break;
        fputs(line, stdout);
        fputc('\n', stdout);
        vTaskDelay(1);
    }
    free(line);
    printf("SEARCH-DUMP end\n");
}

/* ------------------------------------------------------------- console */

/* MHz by default; a k, M or G suffix says otherwise. */
static bool parse_hz(const char *text, uint64_t *out)
{
    if (!text || !*text) return false;
    char *end = NULL;
    double v = strtod(text, &end);
    if (end == text || v <= 0) return false;
    double mult = 1e6;
    if (*end == 'k' || *end == 'K') mult = 1e3;
    else if (*end == 'g' || *end == 'G') mult = 1e9;
    *out = (uint64_t)(v * mult + 0.5);
    return true;
}

static const char *refusal(ls_radio_err_t e)
{
    switch (e) {
    case LS_RADIO_ERR_BUSY:        return "an app owns the receiver, go to HOME first";
    case LS_RADIO_ERR_UNAVAILABLE: return "no receiver that covers that range";
    case LS_RADIO_ERR_INVALID:     return "that range cannot be planned, need lo < hi and under 327 MHz wide";
    case LS_RADIO_ERR_EXISTS:      return "already running, `search stop` first";
    case LS_RADIO_ERR_NO_MEMORY:   return "out of memory";
    default:                       return ls_radio_err_name(e);
    }
}

int ls_search_command(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "start")) {
        uint64_t lo = LS_SEARCH_DEFAULT_LO_HZ, hi = LS_SEARCH_DEFAULT_HI_HZ;
        if (argc == 4) {
            if (!parse_hz(argv[2], &lo) || !parse_hz(argv[3], &hi)) {
                printf("SEARCH refused: could not read the frequency range\n");
                return 1;
            }
        } else if (argc != 2) {
            printf("SEARCH refused: usage: search start [lo_MHz hi_MHz]\n");
            return 1;
        }
        ls_radio_err_t e = ls_search_run_start(lo, hi);
        if (e != LS_RADIO_OK) {
            printf("SEARCH refused: %s\n", refusal(e));
            return 1;
        }
        printf("SEARCH started %.6g-%.6g MHz\n", lo / 1e6, hi / 1e6);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "stop"))  { ls_search_run_stop();  return 0; }
    if (argc >= 2 && !strcmp(argv[1], "dump"))  { ls_search_run_dump();  return 0; }
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        ls_search_run_clear();
        printf("search: floors and hits cleared\n");
        return 0;
    }
    if (argc == 1) { ls_search_run_report(); return 0; }

    printf("usage: search [start [lo_MHz hi_MHz] | stop | clear | dump]\n"
           "       search start            136-174 MHz\n");
    return 1;
}
