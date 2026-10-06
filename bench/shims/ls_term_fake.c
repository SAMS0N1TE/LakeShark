/* TERMINAL's command runner for the simulator and the screen tests: a few
   commands answer with canned text at once, everything else is unknown.
   The scrollback starts with a short session so the screen has something to
   draw (LSSIM_TERM_EMPTY=1 starts it empty). Bench only. */

#include "ls_term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ls_term_scroll_t s_scroll;
static bool             s_on;

static const char *const CMDS[] = {
    "crash", "crumb", "date", "heap", "help", "mode", "rtc", "safemode", "tel", "trail", "tui", "version",
};
#define N_CMDS ((int)(sizeof(CMDS) / sizeof(CMDS[0])))

static void out(const char *t) { ls_term_scroll_put(&s_scroll, t, strlen(t)); }

__attribute__((constructor)) static void fake_term_start(void)
{
    if (getenv("LSSIM_TERM_EMPTY")) return;
    out(LS_TERM_PROMPT "crumb\n");
    out("no panic recorded before this boot\n");
    out("errors saved: 2 ('crumb log' prints them, 'crumb clear' empties the log)\n");
    out(LS_TERM_PROMPT "heap\n");
    out("internal : free=45915 largest=14336 min_ever=36759\n");
    out("dma      : free=23755 largest=13824 min_ever=14599\n");
    out("psram    : free=22794840 largest=22544384 min_ever=22793188\n");
    out("cpu busy : core0=31% core1=8%\n");
    out(LS_TERM_PROMPT "frob\n");
    out("unknown command ('help' lists them)\n");
}

bool ls_term_start(void) { s_on = true; return true; }
void ls_term_stop(void) { s_on = false; }
bool ls_term_running(void) { return s_on; }
bool ls_term_busy(void) { return false; }

bool ls_term_submit(const char *line)
{
    if (!s_on || !line) return false;
    if (!strcmp(line, "help")) {
        for (int i = 0; i < N_CMDS; i++) { out(CMDS[i]); out("\n"); }
    } else if (!strncmp(line, "crumb", 5)) {
        out("no panic recorded before this boot\nerrors saved: 0\n");
    } else if (!strcmp(line, "heap")) {
        out("internal : free=45915 largest=14336 min_ever=36759\n");
    } else {
        out("unknown command ('help' lists them)\n");
    }
    return true;
}

void ls_term_out(const char *text) { if (text) out(text); }
void ls_term_clear(void) { ls_term_scroll_reset(&s_scroll); }
uint32_t ls_term_gen(void) { return s_scroll.gen; }
size_t ls_term_text(char *o, size_t n) { return ls_term_scroll_copy(&s_scroll, o, n); }

int ls_term_complete(const char *line, char *o, size_t n, char *list, size_t list_n)
{
    if (o && n) o[0] = '\0';
    if (list && list_n) list[0] = '\0';
    if (!line || strchr(line, ' ')) return 0;
    int count = 0;
    size_t k = 0;
    for (int i = 0; i < N_CMDS; i++) {
        if (strncmp(CMDS[i], line, strlen(line))) continue;
        count++;
        if (o && n) snprintf(o, n, "%s ", CMDS[i]);
        if (list && k + 1 < list_n) {
            const int w = snprintf(list + k, list_n - k, "%s%s", k ? " " : "", CMDS[i]);
            if (w > 0) k = (size_t)w >= list_n - k ? list_n - 1 : k + (size_t)w;
        }
    }
    if (count != 1 && o && n) o[0] = '\0';
    return count;
}
