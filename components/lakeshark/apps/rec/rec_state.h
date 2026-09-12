#ifndef REC_STATE_H
#define REC_STATE_H

/**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iq_app_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REC_RTL_RATE      256000u
#define REC_DEFAULT_FREQ  433920000UL
#define REC_DEFAULT_GAIN  400

#define REC_MAX_EDGES     4096
#define REC_MIN_PULSE_US  40
#define REC_GAP_END_US    30000
#define REC_MIN_EDGES     6
#define REC_MAX_SPAN_US   8000000

typedef enum {
    REC_IDLE = 0,
    REC_ARMED,
    REC_CAPTURING,
    REC_DONE,
} rec_phase_t;

/**/
typedef enum {
    REC_END_NONE = 0,
    REC_END_GAP,
    REC_END_SPAN,
    REC_END_EDGES,
} rec_end_t;

typedef struct {
    rec_phase_t phase;
    uint32_t    freq_hz;
    int         gain_tenths;
    uint64_t    effective_freq_hz;
    int         effective_gain_tenths;
    bool        effective_freq_known;
    bool        effective_gain_known;
    ls_iq_result_state_t tune_state;
    ls_iq_result_state_t gain_state;
    ls_radio_err_t tune_error;
    ls_radio_err_t gain_error;
    bool        receiver_streaming;
    ls_radio_err_t receiver_error;
    int         edges;
    uint32_t    span_us;
    int         mag_now;
    int         mag_floor;
    int         mag_thresh;
    int         thresh_fixed;
    int         gap_ms;
    uint32_t    captures;
    uint32_t    bytes_sec;
    char        last_file[64];
    /**/
    uint32_t    bw_hz;
    uint32_t    min_pulse_us;
    uint32_t    max_span_us;
    int         min_edges;
    /**/
    int         end_reason;
    uint32_t    min_mark_us;
    uint32_t    max_mark_us;
    uint32_t    baud_est;
    /**/
    int         mag_peak;             /* peak seen across the last capture */
    int         mag_floor_at_start;   /* floor when the capture started */
    /**/

    uint64_t    bytes_free;
} rec_status_t;

/* Lightweight read-only state for always-on presentation consumers. Unlike
   rec_get_status(), this does not probe capture-volume free space. */
typedef struct {
    rec_phase_t phase;
    uint32_t    freq_hz;
    int         edges;
    int         mag_now;
    int         mag_thresh;
    uint32_t    bytes_sec;
    uint32_t    captures;
    bool        receiver_streaming;
} rec_hub_status_t;

int  rec_app_register(void);

void rec_get_status(rec_status_t *out);
void rec_get_hub_status(rec_hub_status_t *out);
void rec_get_receiver_status(ls_iq_control_status_t *out);
void rec_set_freq(uint32_t hz);
uint32_t rec_get_freq(void);
void rec_set_gain(int tenths);
/**/
void rec_set_thresh(int absolute);
int  rec_get_thresh(void);
/**/
void rec_set_gap_ms(int ms);
int  rec_get_gap_ms(void);

/**/
void     rec_set_bw(uint32_t hz);
uint32_t rec_get_bw(void);
void     rec_set_min_pulse(uint32_t us);
uint32_t rec_get_min_pulse(void);
void     rec_set_max_span(uint32_t us);
uint32_t rec_get_max_span(void);
void     rec_set_min_edges(int n);
int      rec_get_min_edges(void);

/**/
const char *rec_end_reason_name(int reason);

/**/
/* The SCOUT view. When enabled, the rx task folds IQ into an FFT and
   publishes a normalised 0..1 spectrum resampled to `n` bins across the
   REC_SCOUT_SPAN_HZ window centred on rec_get_freq().  The bins are the
   fftshifted MAX-per-column reading over the last few accumulations, so a
   narrow carrier does not get averaged away against its silent neighbours.
   Capture continues to run underneath - the FFT is a fold, not a retune -
   so ARMED and CAPTURING remain honest with the scout tab up. */
#define REC_SCOUT_SPAN_HZ  200000u

void     rec_scout_enable(bool on);
bool     rec_scout_enabled(void);
/* Copy the current spectrum out.  Returns false when the average is
   empty (freshly enabled or just reset).  out is normalised 0..1 where
   1.0 corresponds to REC_SCOUT_TOP_DB and 0.0 to REC_SCOUT_FLOOR_DB. */
bool     rec_scout_read(float *out, int n);
/* Peak within the last published spectrum, in Hz absolute and normalised
   0..1 level.  Returns 0 in hz_out if no peak has been observed yet. */
void     rec_scout_peak(uint32_t *hz_out, float *level_out);
/* Sweep counter - the number of complete spectra published since scout
   was enabled.  Used by the GUI to drive the waterfall and to measure
   frame rate. */
uint32_t rec_scout_sweeps(void);

bool rec_active(void);
void rec_arm(void);
/**/
void rec_arm_request(void);
void rec_disarm(void);

/**/
uint32_t rec_bytes_sec(void);

/**/
int rec_edge_count(void);
int rec_edges_copy(int from, int32_t *out, int max);

/**/
/* Return the free-byte count of the volume rec_dir() points at, or
   UINT64_MAX if the probe (statvfs) failed - which the caller must treat
   as "do not write".  Cheap enough to call from the REC timer tick and
   the console; no cache. */
uint64_t rec_dir_free_bytes(void);

/* Save as <name>_<wall-clock>.sub, or <name>_up-<seconds>s.sub before time
   sync. A numeric collision suffix is added without modifying old files. */
int  rec_save(const char *name, char *path_out, size_t path_len);

/**/

int  rec_list(char *out, size_t len, bool *out_truncated);
/**/
const char *rec_dir(void);
/**/
int  rec_file_info(int index, char *name, size_t nlen, uint32_t *freq_hz, long *size);
int  rec_load(int index);
int  rec_dump(const char *name, void (*emit)(const char *line, void *ctx), void *ctx);
int  rec_remove(const char *name);

#ifdef __cplusplus
}
#endif

#endif
