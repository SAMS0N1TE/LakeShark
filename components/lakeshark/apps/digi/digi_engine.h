/* DIGITAL MODES runtime: the audio sink a receiver feeds, slot timing from
   GPS / NTP / RTC, the decode worker, the decode list and the SD log.

   Audio in: any task with demodulated SSB (USB) audio calls digi_sink_push
   with int16 mono PCM at its own rate and the dial frequency it is tuned to.
   The call is non-blocking and cheap (resample to 12 kHz into a PSRAM ring);
   nothing is queued while DIGI is stopped, and digi_sink_wanted() lets a
   producer skip even that. A receiver that can be tuned registers a
   digi_tuner_t so DIGI can move it to a band's dial frequency.

   Time: slot boundaries come from UTC. With a current GPS fix the RMC time is
   used (whole seconds stamped on arrival, less a configurable sentence lag);
   otherwise the system clock, labelled NTP when SNTP set it and RTC when the
   PCF8563 did. The median DT of recent decodes is shown as the clock offset
   (as WSJT-X does) and can be applied as a trim.

   Worker: one task, PSRAM stack, priority below audio, created on
   digi_start and joined on digi_stop. It never touches NVS. Decoding runs
   at each slot end: FT8 ~15 s, FT4 7.5 s, WSPR at the even minute.

   UI: digi_view copies a snapshot under a short lock; nothing here blocks the
   UI task. */
#ifndef DIGI_ENGINE_H
#define DIGI_ENGINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DIGI_MODE_FT8 = 0, DIGI_MODE_FT4, DIGI_MODE_WSPR, DIGI_MODE_COUNT } digi_mode_t;
typedef enum { DIGI_TIME_NONE = 0, DIGI_TIME_RTC, DIGI_TIME_NTP, DIGI_TIME_GPS } digi_time_src_t;
typedef enum { DIGI_SRC_NONE = 0, DIGI_SRC_RADIO, DIGI_SRC_TEST, DIGI_SRC_FILE } digi_source_t;

/* ---- audio sink (for the HF / SSB receiver) ------------------------------ */

/* True while DIGI wants audio. Cheap: one load. */
bool digi_sink_wanted(void);
/* PCM from the demodulator: mono int16 at rate_hz (8000..48000), the dial
   (suppressed-carrier) frequency in Hz, USB assumed. Safe from any task;
   never blocks; drops (and counts) when the ring is full. */
void digi_sink_push(const int16_t *pcm, int n, int rate_hz, uint64_t dial_hz);

/* A receiver that DIGI may retune. tune() is called from the UI task when the
   user picks a band and must not block; name() labels the source. */
typedef struct {
    bool (*tune)(uint64_t dial_hz, bool usb);
    const char *(*name)(void);
} digi_tuner_t;
void digi_set_tuner(const digi_tuner_t *t);

/* ---- bands --------------------------------------------------------------- */

typedef struct {
    const char *band;       /* "20m" */
    uint64_t ft8_hz, ft4_hz, wspr_hz;   /* 0: none on this band */
} digi_band_t;
#define DIGI_BAND_COUNT 12
extern const char *const digi_band_names[DIGI_BAND_COUNT];   /* "160m" .. "2m" */
int digi_band_count(void);
const digi_band_t *digi_band(int i);
uint64_t digi_band_dial(int i, digi_mode_t m);

/* ---- control --------------------------------------------------------------- */

bool digi_start(void);
void digi_stop(void);
bool digi_running(void);
void digi_set_mode(digi_mode_t m);
digi_mode_t digi_mode(void);
/* Picks the band and asks the tuner (if any) to go there. */
void digi_set_band(int i);
int digi_band_index(void);
/* Synthetic FT8/FT4 traffic fed through the sink in real time, so the screen
   and the timing can be checked without a receiver. Off by default. */
void digi_set_test_source(bool on);
bool digi_test_source(void);
/* Seconds added to the clock before slotting (-2..+2). */
void digi_set_time_trim(float s);
float digi_time_trim(void);
/* GPS sentence lag in ms subtracted from RMC time (0..900). */
void digi_set_gps_lag_ms(int ms);
int digi_gps_lag_ms(void);
void digi_set_logging(bool on);
bool digi_logging(void);
void digi_clear(void);

/* ---- view ---------------------------------------------------------------- */

#define DIGI_VIEW_ROWS   48
#define DIGI_SPEC_CELLS  96     /* band activity, 0..3000 Hz audio        */
#define DIGI_SLOT_HIST   12     /* decodes per recent slot, newest last   */

typedef struct {
    uint32_t seq;           /* rises by one per decode, never reused        */
    uint32_t at_ms;         /* uptime when it landed, for the arrival pulse */
    uint32_t slot_utc;      /* UTC seconds at the slot start                */
    digi_mode_t mode;
    int8_t snr;
    float dt, freq;
    char text[40];
    char call[14];          /* the sender, where the message has one        */
    char to[14];
    char grid[7];
    bool cq, to_me;
    bool has_pos;           /* grid and our GPS position both known         */
    float km, bearing;
} digi_row_t;

typedef struct {
    bool running;
    digi_mode_t mode;
    digi_source_t source;
    int band;
    uint64_t dial_hz;
    char tuner[16];
    /* time */
    digi_time_src_t tsrc;
    int64_t utc_ms;         /* now, from tsrc                               */
    bool time_ok;
    float trim_s;
    float clock_off_s;      /* median DT of recent decodes                  */
    int clock_off_n;
    int slot_ms;            /* 15000 / 7500 / 120000                        */
    int slot_pos_ms;        /* into the current slot                        */
    bool capturing;         /* a slot is being recorded                     */
    bool decoding;
    /* audio */
    float level_db;         /* dBFS of the last half second                 */
    int64_t audio_age_ms;   /* since the last sample arrived, -1 never      */
    uint32_t drops;
    uint8_t spec[DIGI_SPEC_CELLS];   /* 0..255 power, newest half second    */
    uint8_t spec_peak[DIGI_SPEC_CELLS];
    /* decodes */
    uint32_t total, slots;
    uint8_t slot_hist[DIGI_SLOT_HIST];
    int last_decodes;
    uint32_t last_cost_ms;  /* worker time for the last slot's decode       */
    int n;
    digi_row_t row[DIGI_VIEW_ROWS];  /* newest first                        */
    /* where we are */
    bool have_pos;
    char my_grid[7];
    /* log */
    bool logging;
    uint32_t logged;
    char log_err[24];
} digi_view_t;

/* Copy the current state. `out` is large (a few kB): keep it static. */
void digi_view(digi_view_t *out);

const char *digi_mode_name(digi_mode_t m);
const char *digi_time_name(digi_time_src_t t);

/* Maidenhead to the centre of the square (4 or 6 characters). */
bool digi_grid_to_ll(const char *grid, double *lat, double *lon);
void digi_ll_to_grid(double lat, double lon, char out[7]);
/* Bearing (degrees true) and great-circle distance (km) between two points. */
void digi_bearing_range(double la0, double lo0, double la1, double lo1, float *brg, float *km);

/* Console: digi [status|start|stop|mode|band|test|selftest|wav|log]. */
void digi_console_register(void);

/* Host and simulator only: put a decode in the list as the worker would. */
void digi_host_add(const digi_row_t *r);
void digi_host_set(const digi_view_t *v);

#ifdef __cplusplus
}
#endif
#endif
