#ifndef LS_CELL_MONITOR_H
#define LS_CELL_MONITOR_H
#include "cell_core.h"
#ifdef __cplusplus
extern "C" {
#endif
/* One cell heard on one carrier. rsrp10 is an RSRP-like level in tenths of
 * dBFS (CELL_NO_POWER when not measured); n_rb is 0 until a MIB decodes. */
#define CELL_MAX_CELLS 32
#define CELL_NO_POWER (-32768)
enum { CELL_MIB_NONE, CELL_MIB_FAILED, CELL_MIB_OK };
typedef struct {
    uint32_t hz;
    uint16_t earfcn, pci;
    int16_t cfo, rsrp10, sfn;
    uint8_t hits, n_rb, ports, mib;
    float pss, sss;
} cell_lte_t;
enum { CELL_PH_IDLE, CELL_PH_OCCUPANCY, CELL_PH_SYNC, CELL_PH_MIB };
typedef struct {
    bool busy, learning, baseline, located, quiet, sd, external;
    bool manual_site, compared, baseline_saved;
    bool receiver_missing, needs_relearn;
    bool lte;
    unsigned lte_count, lte_attempted, lte_usable, lte_discarded;
    /* Latest cell found, for the LoRa report. The list is cell_monitor_cells. */
    struct {uint32_t hz;int pci;} last;
    /* LTE scan plan and progress: phase, plateaus the occupancy pass found,
     * raster points chosen for PSS/SSS, MIB decodes tried and confirmed. */
    unsigned phase, plateaus, candidates, mib_tried, mib_done;
    bool gated, mib_on;                  /* settings in force */
    bool ppm_known, ppm_applied;
    float ppm, ppm_spread;               /* dongle clock error from cells heard */
    unsigned ppm_cells;
    int ppm_value;                       /* what the tuner is already corrected by */
    uint32_t scan_ms;                    /* since the search began */
    unsigned band, tune, tunes, passes, changed, rows;
    uint32_t frequency, elapsed_ms;
    struct { uint32_t hz; int power, delta; bool flagged; } row[8];
    unsigned spectrum_count;
    int8_t spectrum[96], reference[96]; /* peak held columns from a complete pass */
    /* Per column of the LTE search: bit 0 occupied, 1 raster point searched,
     * 2 cell found. */
    uint8_t mark[96];
    char message[80];
} cell_status_t;
enum { CELL_WATCH=1, CELL_LEARN, CELL_LOAD, CELL_LTE, CELL_PPM_ZERO };
void cell_monitor_init(void);
bool cell_monitor_request(int command, unsigned band, bool manual_site);
void cell_monitor_stop(void);
void cell_monitor_wait_stopped(void);
void cell_monitor_get(cell_status_t *out);
/* The cells found by the last LTE search, in the order found. */
unsigned cell_monitor_cells(cell_lte_t *out, unsigned max);
/* LTE search settings, kept until the next boot. Gated searches only the
 * occupied carriers; full tries every 100 kHz raster point. */
void cell_monitor_set_full_scan(bool full);
void cell_monitor_set_mib(bool on);
/* Forget the dongle's measured clock error and take the tuner correction back
 * to zero, as a short job on the worker. False while a job runs. */
bool cell_monitor_clear_ppm(void);
#ifdef __cplusplus
}
#endif
#endif
