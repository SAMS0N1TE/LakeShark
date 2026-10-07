/* See ls_experiments.h. The list, the one experiment that runs, and the
   `exp` console command. The board's half - the worker task and the LoRa
   socket - is ls_experiments_hw.c. */
#include "ls_experiments.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)

static EXT_RAM_BSS_ATTR const ls_experiment_t *s_list[LS_EXP_MAX];
static int s_n;

/* What was asked for, and how far the worker has got: s_done catches up
   with s_asked once the request s_asked numbers has been carried out. */
static const ls_experiment_t *s_want;
static uint32_t s_asked, s_done;

/* The worker's: what runs, since when, and whether the socket is held. */
static const ls_experiment_t *s_run;
static int64_t s_since_us;
static bool s_held;

/* The last start that failed, and why. */
static const ls_experiment_t *s_failed;
static EXT_RAM_BSS_ATTR char s_why[64];

/* ------------------------------------------------------------- the list -- */

void ls_exp_register(const ls_experiment_t *e)
{
    if (!e || !e->id || !e->name) return;
    LOCK();
    bool known = false;
    for (int i = 0; i < s_n && !known; i++)
        known = s_list[i] == e || !strcmp(s_list[i]->id, e->id);
    if (!known && s_n < LS_EXP_MAX) s_list[s_n++] = e;
    UNLOCK();
}

int ls_exp_count(void) { return s_n; }

const ls_experiment_t *ls_exp_at(int i)
{
    return i >= 0 && i < s_n ? s_list[i] : NULL;
}

const ls_experiment_t *ls_exp_find(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_list[i]->id, id)) return s_list[i];
    return NULL;
}

const char *ls_exp_maturity_name(ls_exp_maturity_t m)
{
    switch (m) {
    case LS_EXP_IDEA:   return "IDEA";
    case LS_EXP_TRYING: return "TRYING";
    case LS_EXP_WORKS:  return "WORKS";
    }
    return "?";
}

int ls_exp_read_lines(const ls_experiment_t *e, char (*out)[LS_EXP_LINE], int max)
{
    if (!e || !e->lines || !out || max <= 0) return 0;
    for (int i = 0; i < max; i++) out[i][0] = 0;
    int n = e->lines(out, max);
    if (n < 0) n = 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i][LS_EXP_LINE - 1] = 0;
    return n;
}

size_t ls_exp_note_text(const ls_experiment_t *e, const char *stamp, const char *state,
                        const char (*lines)[LS_EXP_LINE], int n,
                        char *title, size_t tcap, char *body, size_t bcap)
{
    if (title && tcap) title[0] = 0;
    if (body && bcap) body[0] = 0;
    if (!e || !title || !tcap || !body || !bcap) return 0;
    char when[24] = "";
    if (stamp && stamp[0]) {
        /* 2026-10-06T14:21:09Z -> 2026-10-06 14:21; "up 12s" and the like stay whole. */
        if (strlen(stamp) >= 16 && stamp[10] == 'T') {
            snprintf(when, sizeof(when), "%.10s %.5s", stamp, stamp + 11);
        } else {
            snprintf(when, sizeof(when), "%s", stamp);
        }
    }
    snprintf(title, tcap, "%s%s%s", e->name ? e->name : "Experiment", when[0] ? " " : "", when);
    size_t len = (size_t)snprintf(body, bcap, "> %s %s %s  %s\n", e->no_radio ? "EXPERIMENT" : "RADIO",
                                  e->name ? e->name : "", ls_exp_maturity_name(e->maturity),
                                  state && state[0] ? state : "stopped");
    if (len >= bcap) { len = bcap - 1; body[len] = 0; return len; }
    if (n < 0 || !lines) n = 0;
    if (n == 0) {
        len += (size_t)snprintf(body + len, bcap - len, "(no readout yet)\n");
    }
    for (int i = 0; i < n; i++) {
        char line[LS_EXP_LINE];
        memcpy(line, lines[i], LS_EXP_LINE);
        line[LS_EXP_LINE - 1] = 0;
        const size_t need = strlen(line) + 1;
        if (len + need >= bcap) break;
        len += (size_t)snprintf(body + len, bcap - len, "%s\n", line);
    }
    if (len >= bcap) len = bcap - 1;
    return len;
}

/* ------------------------------------------------------------ running -- */

static void ask(const ls_experiment_t *e)
{
    LOCK();
    s_want = e;
    s_asked++;
    if (e && s_failed == e) s_failed = NULL;
    UNLOCK();
    ls_exp_hw_wake();
}

void ls_exp_start(const ls_experiment_t *e) { if (e) ask(e); }
void ls_exp_stop(void) { ask(NULL); }

const ls_experiment_t *ls_exp_running(void)
{
    LOCK();
    const ls_experiment_t *e = s_run;
    UNLOCK();
    return e;
}

const ls_experiment_t *ls_exp_wanted(void)
{
    LOCK();
    const ls_experiment_t *e = s_want;
    UNLOCK();
    return e;
}

bool ls_exp_busy(void)
{
    LOCK();
    const bool busy = s_asked != s_done;
    UNLOCK();
    return busy;
}

bool ls_exp_settle(int timeout_ms)
{
    const bool worker = ls_exp_hw_wake();
    /* Counted in waits rather than read off a clock, so a clock that does
       not move cannot hold this forever. */
    for (int waited = 0;; waited += 10) {
        if (!ls_exp_busy()) return true;
        if (!worker) { ls_exp_service(); continue; }
        if (waited >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void fail(const ls_experiment_t *e, const char *why)
{
    LOCK();
    s_failed = e;
    snprintf(s_why, sizeof(s_why), "%s", why && why[0] ? why : "would not start");
    UNLOCK();
}

/* Stop what runs. With `keep` the socket stays held for the experiment
   about to start, so MeshCore is not handed it for a moment in between. */
static void halt(bool keep)
{
    const ls_experiment_t *e = s_run;
    if (e->stop) e->stop();
    const bool give = s_held && !keep;
    LOCK();
    s_run = NULL;
    if (give) s_held = false;
    UNLOCK();
    if (give) ls_exp_hw_radio_give();
}

static void begin(const ls_experiment_t *e)
{
    if (!e->no_radio) {
        const char *busy = ls_exp_hw_radio_busy();
        if (busy) {
            if (s_held) { ls_exp_hw_radio_give(); s_held = false; }
            fail(e, busy);
            return;
        }
        if (!s_held && !ls_exp_hw_radio_take()) {
            fail(e, "MeshCore would not release the LoRa radio");
            return;
        }
        s_held = true;
    } else if (s_held) {
        ls_exp_hw_radio_give();
        s_held = false;
    }
    char why[64] = "";
    if (e->start && !e->start(why, sizeof(why))) {
        if (s_held) { ls_exp_hw_radio_give(); s_held = false; }
        fail(e, why);
        return;
    }
    LOCK();
    s_run = e;
    s_since_us = esp_timer_get_time();
    if (s_failed == e) s_failed = NULL;
    UNLOCK();
}

bool ls_exp_service(void)
{
    LOCK();
    const uint32_t asked = s_asked;
    const ls_experiment_t *want = s_want;
    UNLOCK();
    if (asked != s_done) {
        if (s_run) halt(want && !want->no_radio);
        if (want) begin(want);
        else if (s_held) { ls_exp_hw_radio_give(); s_held = false; }
        LOCK();
        s_done = asked;
        UNLOCK();
    }
    const ls_experiment_t *run = s_run;
    if (run && run->poll) run->poll();
    bool upkeep = false;
    const int count = ls_exp_count();
    for (int i = 0; i < count; i++) {
        const ls_experiment_t *e = ls_exp_at(i);
        if (e && e->maintenance) upkeep |= e->maintenance();
    }
    return run != NULL || ls_exp_busy() || upkeep;
}

void ls_exp_forget(void)
{
    LOCK();
    s_n = 0;
    s_want = s_run = s_failed = NULL;
    s_asked = s_done = 0;
    s_held = false;
    s_since_us = 0;
    s_why[0] = 0;
    UNLOCK();
}

static void duration(int64_t us, char *out, size_t n)
{
    const long s = us > 0 ? (long)(us / 1000000) : 0;
    if (s < 60)        snprintf(out, n, "%lds", s);
    else if (s < 3600) snprintf(out, n, "%ldm%02lds", s / 60, s % 60);
    else               snprintf(out, n, "%ldh%02ldm", s / 3600, (s / 60) % 60);
}

void ls_exp_state_line(const ls_experiment_t *e, char *out, size_t n)
{
    if (!out || !n) return;
    LOCK();
    const ls_experiment_t *run = s_run, *want = s_want, *failed = s_failed;
    const bool pending = s_asked != s_done;
    const int64_t since = s_since_us;
    char why[sizeof(s_why)];
    memcpy(why, s_why, sizeof(why));
    UNLOCK();
    if (!e) { snprintf(out, n, "stopped"); return; }
    if (pending && want == e && run == e) { snprintf(out, n, "restarting"); return; }
    if (pending && want == e)             { snprintf(out, n, "starting"); return; }
    if (pending && run == e)              { snprintf(out, n, "stopping"); return; }
    if (run == e) {
        char t[16];
        duration(esp_timer_get_time() - since, t, sizeof(t));
        snprintf(out, n, "running %s", t);
        return;
    }
    if (failed == e) { snprintf(out, n, "%s", why); return; }
    snprintf(out, n, "stopped");
}

/* ------------------------------------------------------------- console -- */

#define CONSOLE_LINES 24

static void print_lines(const ls_experiment_t *e)
{
    char (*buf)[LS_EXP_LINE] = heap_caps_malloc(CONSOLE_LINES * LS_EXP_LINE, MALLOC_CAP_SPIRAM);
    if (!buf) { printf("exp: no memory\n"); return; }
    const int n = ls_exp_read_lines(e, buf, CONSOLE_LINES);
    for (int i = 0; i < n; i++) printf("  %s\n", buf[i]);
    heap_caps_free(buf);
}

static void print_state(const ls_experiment_t *e)
{
    char state[80];
    ls_exp_state_line(e, state, sizeof(state));
    printf("exp: %s %s\n", e->id, state);
}

int ls_exp_console(int argc, char **argv)
{
    if (argc < 2) {
        printf("exp: %d experiment%s; 'exp <id> start [args]', 'exp <id> stop', 'exp <id>'\n",
               s_n, s_n == 1 ? "" : "s");
        for (int i = 0; i < s_n; i++) {
            char state[80];
            ls_exp_state_line(s_list[i], state, sizeof(state));
            printf("  %-10s %-6s %s%s\n", s_list[i]->id, ls_exp_maturity_name(s_list[i]->maturity), state,
                   s_list[i]->lr2021_only ? "  [LR2021 only]" : "");
        }
        return 0;
    }
    if (!strcmp(argv[1], "stop")) {
        const ls_experiment_t *run = ls_exp_running();
        ls_exp_stop();
        const bool settled = ls_exp_settle(3000);
        if (run) print_state(run);
        else printf("exp: nothing was running\n");
        return settled ? 0 : 1;
    }
    const ls_experiment_t *e = ls_exp_find(argv[1]);
    if (!e) { printf("exp: no experiment '%s'; 'exp' lists them\n", argv[1]); return 1; }
    if (argc == 2) {
        printf("exp: %s, %s - %s\n", e->name, ls_exp_maturity_name(e->maturity), e->sub ? e->sub : "");
        if (e->lr2021_only) printf("exp: LR2021 radios only\n");
        if (e->needs) printf("exp: needs %s\n", e->needs);
        print_lines(e);
        print_state(e);
        return 0;
    }
    if (!strcmp(argv[2], "start")) {
        if (argc > 3) {
            if (!e->configure) { printf("exp: %s takes no arguments\n", e->id); return 1; }
            char why[96] = "";
            if (!e->configure(argc - 3, argv + 3, why, sizeof(why))) {
                printf("exp: %s: %s\n", e->id, why[0] ? why : "bad arguments");
                return 1;
            }
        }
        ls_exp_start(e);
        /* The mesh may take a second to let go, and the start its own time
           after that. */
        const bool settled = ls_exp_settle(3000);
        print_state(e);
        return settled && ls_exp_running() == e ? 0 : 1;
    }
    if (!strcmp(argv[2], "stop")) {
        if (ls_exp_running() != e) { printf("exp: %s is not running\n", e->id); return 1; }
        ls_exp_stop();
        const bool settled = ls_exp_settle(3000);
        print_state(e);
        return settled ? 0 : 1;
    }
    printf("exp: '%s' is not start or stop\n", argv[2]);
    return 1;
}
