#ifndef LS_CELL_CORE_H
#define LS_CELL_CORE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CELL_MAX_BINS 1400
#define CELL_RATE 256000u
#define CELL_BIN 25000u
#define CELL_GAIN 297
#define CELL_BASELINE_VERSION 2u
#define CELL_RECEIVER_RTL 1u
#define CELL_CALIBRATION 1u
#define CELL_MISSING (-128)
/* LTE FDD downlink bands, TS 36.101 tables 5.5-1 and 5.7.3-1. `band` is the
 * E-UTRA band number and `offs` its N_offs-DL; the downlink carrier of
 * EARFCN n is low + 100 kHz * (n - offs). A row is one survey window: a band
 * wider than a baseline holds (CELL_MAX_BINS x CELL_BIN, 35 MHz) is two rows
 * with the same band number. The first cell_band_count rows are inside the
 * RTL-SDR tuner and keep their positions, because saved baselines name a
 * window by index; the rest need the HackRF. */
typedef struct { const char *name; uint32_t low, high; uint16_t band; uint32_t offs; } cell_band_t;
extern const cell_band_t cell_bands[];
extern const unsigned cell_band_count;   /* RTL-SDR scannable */
extern const unsigned cell_band_total;   /* plus HackRF-only */
#define CELL_RASTER_HZ 100000u
bool cell_earfcn_to_hz(unsigned band, unsigned earfcn, uint32_t *hz);
/* False when hz is outside the band or off the 100 kHz channel raster. */
bool cell_hz_to_earfcn(unsigned band, uint32_t hz, unsigned *earfcn);
void cell_band_earfcns(unsigned band, unsigned *first, unsigned *last);
/* First row holding hz, or -1. */
int cell_band_for_hz(uint32_t hz);
/* The rows whose downlink holds hz, one per band number: `selected` first
 * when it holds hz, then the table's order. -1 selects nothing. Returns how
 * many went into out. Bands overlap across regions (758-768 is B14 in the US
 * and B28 in APT 700 countries), so the first answer alone can be wrong. */
unsigned cell_bands_at(uint32_t hz, int selected, unsigned *out, unsigned max);
/* "B28 / EARFCN 9260" by the selected window when it holds hz; otherwise
 * every band that does, up to two, each with its EARFCN: "B14 5330/B28 9260".
 * Without the EARFCN off the raster ("B2/B25"). False, and "" in out,
 * outside every band. */
bool cell_band_label(uint32_t hz, int selected, char *out, size_t n);

/* HIGH RATE presets: downlink carrier centres for the HackRF, which AUTO
 * steps through. US ones first, then Argentina's B28 carriers. */
#define CELL_HRF_CHANNELS 9
extern const uint32_t cell_hrf_centers[CELL_HRF_CHANNELS];

/* RTL gains a survey can run at, tenths of dB, all R820T steps; CELL_GAIN is
 * one of them. A baseline holds the gain it was learned at. */
#define CELL_GAINS 6
extern const int16_t cell_gains[CELL_GAINS];
bool cell_gain_valid(int tenths);
/* A tune is clipped when more than one I/Q byte in a thousand sits on a rail
 * of the converter (0 or 255): its levels, and the images clipping folds into
 * other bins, belong to the receiver, not the air. */
#define CELL_RAIL_PER_MILLE 1
bool cell_overloaded(uint32_t rails, uint32_t samples);
typedef struct {
    uint32_t magic, version, band, count, rate, gain, passes;
    int32_t lat_tile, lon_tile;
    uint32_t located, antenna, created;
    uint32_t receiver, calibration;
    int8_t power[CELL_MAX_BINS];
    uint32_t checksum;
} cell_baseline_t;
bool cell_gps_valid(bool fix, int64_t timestamp, int64_t now, double lat, double lon, float hdop);
bool cell_coverage_complete(unsigned planned, unsigned attempted, unsigned usable, unsigned discarded);
uint32_t cell_checksum(const cell_baseline_t *b);
/* `gain` is the one the survey runs at now: a baseline learned at another
 * gain reads every level differently and is not this site's. */
bool cell_baseline_valid(const cell_baseline_t *b, unsigned band, int gain, int lat, int lon,
                         bool located, bool antenna);
/* Returns persistent changed-bin count. Missing data and motion reset streaks. */
unsigned cell_compare(const int8_t *base, const int8_t *now, uint8_t *streak,
                      unsigned n, bool comparable);
bool cell_motion_quiet(float ax, float ay, float az, float gx, float gy, float gz);

/* Stillness judged against the gyro's own resting reading, which on these
   boards is up to ~12 deg/s on one axis: a fixed limit on the raw rate reads
   every board as moving. The resting reading is followed slowly, so a turn
   reads as movement and a still board settles within a few seconds. */
typedef struct { float rest[3]; bool seeded; } cell_motion_t;
void cell_motion_reset(cell_motion_t *m);
bool cell_motion_step(cell_motion_t *m, float ax, float ay, float az,
                      float gx, float gy, float gz);

/* Dongle clock error from the cells it hears. An eNodeB carrier is within
 * about 0.05 ppm of nominal, so the offset measured on a synchronised cell is
 * the receiver's own crystal. ppm is positive when the dongle runs fast. One
 * observation per (carrier, PCI): hearing a cell again replaces it. */
#define CELL_PPM_MAX 24
#define CELL_PPM_AGREE 2.5f
typedef struct {
    uint32_t hz[CELL_PPM_MAX]; int16_t pci[CELL_PPM_MAX]; float ppm[CELL_PPM_MAX];
    unsigned n, next;
} cell_ppm_t;
typedef struct { float ppm, spread; unsigned n, agree; } cell_ppm_est_t;
void cell_ppm_reset(cell_ppm_t *p);
/* cfo_hz is the carrier offset the sync measured while `applied_ppm` was in
 * force. False for a reading that no crystal explains (beyond +-150 ppm). */
bool cell_ppm_add(cell_ppm_t *p, uint32_t carrier_hz, int pci, int cfo_hz, float applied_ppm);
/* Median over the observations; agree counts those within CELL_PPM_AGREE of
 * it, spread is their range. False with no observation. */
bool cell_ppm_estimate(const cell_ppm_t *p, cell_ppm_est_t *out);
/* PSS search window (centre and half width, Hz) for a carrier: the whole
 * +-40 kHz while the clock is unknown, +-5 kHz around the expected offset once
 * known. est_ppm is the dongle's error, applied_ppm what the tuner already
 * corrects. */
#define CELL_CFO_STEP 5000
void cell_cfo_window(bool known, float est_ppm, float applied_ppm, uint32_t carrier_hz,
                     int *centre, int *span);

/* Occupancy gating. A plateau is a run of 25 kHz bins held above the noise
 * floor of the pass: an LTE carrier shows as a flat top 1.08/2.7/4.5/9/13.5/18
 * MHz wide. Candidates are the 100 kHz raster points worth a PSS search. */
#define CELL_PLATEAU_MAX 16
#define CELL_GATE_DB 5
typedef struct { uint32_t lo, hi; int8_t level; } cell_plateau_t;
/* The noise floor of a pass: the fifth percentile of its bins, each smoothed
 * over 125 kHz. CELL_MISSING when fewer than eight bins have a reading within
 * two bins of them. */
int cell_floor(const int8_t *db, unsigned n);
/* Fills out (ascending frequency) and returns the count; *floor_db is the
 * noise floor used (cell_floor). Bins at CELL_MISSING are ignored. */
unsigned cell_plateaus(const int8_t *db, unsigned n, uint32_t start_hz, uint32_t bin_hz,
                       cell_plateau_t *out, unsigned max, int *floor_db);
/* A pass as the screen draws it: ncol columns, each the strongest bin under
 * it (CELL_MISSING where none), and bit 0 of mark on every column a carrier
 * plateau covers; the other mark bits are cleared. Fills pl as cell_plateaus
 * does and returns its count. */
unsigned cell_columns(const int8_t *db, unsigned n, uint32_t start_hz, uint32_t bin_hz,
                      int8_t *col, uint8_t *mark, unsigned ncol,
                      cell_plateau_t *pl, unsigned max, int *floor_db);
/* Raster points in [first, last] inside plateaus, ascending, deduplicated,
 * strongest plateaus first when max cuts the list. */
unsigned cell_candidates(const cell_plateau_t *p, unsigned np, uint32_t first, uint32_t last,
                         uint32_t *out, unsigned max);
#endif
