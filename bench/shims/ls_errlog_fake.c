/* The error log's chip side for the simulator and the screen tests: the
   records live in memory, and two sample ones are there from the start so
   DIAG's ERROR LOG page has something to draw (LSSIM_ERRLOG_EMPTY=1 starts
   it empty). Bench only. */

#include "ls_errlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ls_errlog_rec_t s_slot[LS_ERRLOG_RECORDS];
static bool            s_full[LS_ERRLOG_RECORDS];
static uint32_t        s_seq;

static bool st_load(int slot, ls_errlog_rec_t *out)
{
    if (!s_full[slot]) return false;
    *out = s_slot[slot];
    return true;
}
static bool st_save(int slot, const ls_errlog_rec_t *rec) { s_slot[slot] = *rec; s_full[slot] = true; return true; }
static bool st_erase(int slot) { s_full[slot] = false; return true; }
static uint32_t st_get_seq(void) { return s_seq; }
static bool st_set_seq(uint32_t seq) { s_seq = seq; return true; }

static const ls_errlog_store_t s_store = { st_load, st_save, st_erase, st_get_seq, st_set_seq };

static void sample(const char *reason, int64_t wall, const char *crumb, const char *trail,
                   const char *tail)
{
    ls_errlog_rec_t r;
    memset(&r, 0, sizeof(r));
    r.wall = wall;
    snprintf(r.fw, sizeof(r.fw), "2.8.2-bench");
    snprintf(r.reason, sizeof(r.reason), "%s", reason);
    snprintf(r.crumb, sizeof(r.crumb), "%s", crumb);
    snprintf(r.trail, sizeof(r.trail), "%s", trail);
    snprintf(r.tail, sizeof(r.tail), "%s", tail);
    ls_errlog_save(&r);
}

__attribute__((constructor)) static void fake_errlog_start(void)
{
    ls_errlog_init(&s_store);
    if (getenv("LSSIM_ERRLOG_EMPTY")) return;
    sample("brownout", 1791100000, "", "tui HOME 31.2s",
           "W (30120) gauge: battery 3.31 V\n");
    sample("task watchdog", 1791174684,
           "core1 int mcause 8000001e pc 4ff0a1c4 tval 0 ra 4ff0a1b0 sp 4ff3c2f0\n"
           "ticks: core1 stopped first: 'p25_rx' at tick 2534990, core0 ran on 412 ticks ('IDLE0')",
           "tui P25 812.3s, link p25 800.1s",
           "W (811020) p25: stream quiet 5 s\n"
           "W (811900) usb: 3 transfers retried\n"
           "E (812350) usb: transfer stuck on ep 0x81\n");
}

void ls_errlog_early(void) {}
void ls_errlog_boot(bool panicked, const char *crumb) { (void)panicked; (void)crumb; }

size_t ls_errlog_live(char *out, size_t n)
{
    static const char LIVE[] = "W (4210) ls_wifi: no saved network\nW (9031) p25: stream quiet 5 s\n";
    if (!out || !n) return 0;
    snprintf(out, n, "%s", LIVE);
    return strlen(out);
}

void ls_errlog_print_all(void) {}
