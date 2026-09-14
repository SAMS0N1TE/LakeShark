#ifndef LS_CELL_MONITOR_H
#define LS_CELL_MONITOR_H
#include "cell_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool busy, learning, baseline, located, quiet, sd, external;
    bool manual_site, compared;
    bool receiver_missing, needs_relearn;
    bool lte;
    unsigned lte_count;
    struct {uint32_t hz;int pci,hits,cfo;float pss,sss;} cell[8];
    unsigned band, tune, tunes, passes, changed, rows;
    uint32_t frequency, elapsed_ms;
    struct { uint32_t hz; int power, delta; bool flagged; } row[8];
    unsigned spectrum_count;
    int8_t spectrum[96], reference[96]; /* peak held columns from a complete pass */
    char message[80];
} cell_status_t;
enum { CELL_WATCH=1, CELL_LEARN, CELL_LOAD, CELL_LTE };
void cell_monitor_init(void);
bool cell_monitor_request(int command, unsigned band, bool manual_site);
void cell_monitor_stop(void);
void cell_monitor_wait_stopped(void);
void cell_monitor_get(cell_status_t *out);
#ifdef __cplusplus
}
#endif
#endif
